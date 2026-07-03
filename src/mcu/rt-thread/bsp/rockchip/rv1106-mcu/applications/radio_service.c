/*
 * CubeSat — RadioService (SX1262 ×2) over rpmsg. Driver migration (docs 40 §3.2,
 * 50, 90): the RISC-V MCU owns the two LoRa radios; Linux drives them by IPC.
 *
 * Channel: "rpmsg-radio" (ept 0x4005) on the shared rpmsg-lite instance. On
 * Linux, bind it to rpmsg_char (done automatically in rcS):
 *   /dev/rpmsg_ctrl0, RPMSG_CREATE_EPT_IOCTL(dst=0x4005) -> /dev/rpmsg0
 *
 * Protocol v1 — request [0]=cmd [1]=instance(0|1) [2..]=args; reply mirrors cmd
 * with [1]=err (0 ok; 0x10 = not initialized; 0xEE = unknown):
 *   0x01 SVC_PING              -> [0x01,0,'R','D','I','O',ver]
 *   0x02 RESET_STATUS          -> [0x02,err,status]      (port init + chip reset)
 *   0x03 READ_REG   a_hi a_lo  -> [0x03,err,val]
 *   0x04 WRITE_REG  a_hi a_lo v-> [0x04,err]
 *   0x05 INIT       f3..f0     -> [0x05,err]             (full proven init, 14 dBm)
 *   0x06 SET_FREQ   f3..f0     -> [0x06,err]
 *   0x07 SET_POWER  dbm(i8)    -> [0x07,err]
 *   0x08 SET_CW                -> [0x08,err]             (carrier ON)
 *   0x09 STANDBY               -> [0x09,err]             (carrier/RX OFF)
 *   0x0A GET_STATUS            -> [0x0A,err,status]
 *   0x0B GET_ERRORS           -> [0x0B,err,e_hi,e_lo]
 *   0x0C SET_ANTSW  mode       -> [0x0C,err]
 *   0x0D TX         len data.. -> [0x0D,err]             (send one LoRa packet,
 *                                                         blocks until TX_DONE)
 *   0x0E RX_START   t_hi t_lo  -> [0x0E,err]             (listen; t=ms, 0=cont.)
 *   0x0F RX_STOP               -> [0x0F,err]
 *
 * Unsolicited events (MCU -> host, pushed from the poll loop):
 *   0xE0 EVT_RX   [0xE0,inst,flags,len,rssi_hi,rssi_lo,snr, data..]
 *                 flags bit0 = CRC ok
 *   0xE1 EVT_RX_TIMEOUT [0xE1,inst]
 *
 * Threading: command handlers and the RX poll run in the single rpmsg poll
 * thread (ping_echo.c). TX blocks briefly (packet airtime). RX is NON-blocking:
 * RX_START arms the chip, the poll loop watches DIO1 and pushes EVT_RX on
 * RX_DONE — this is also the DIO1 event-push path (doc 90 §10).
 *
 * Diag markers: 0xff6ff858 = last cmd, 0xff6ff85c = last handler err,
 * 0xff6ff860 = last deferred-send rc, 0xff6ff864 = last RX event (0xE0/E1|len).
 */
#include <rthw.h>
#include <rtthread.h>

#include "ipc_test_cfg.h"

#if defined(RT_USING_RPMSG_LITE) && !(defined(IPC_RAW_MBOX_TEST) && (IPC_RAW_MBOX_TEST == 1))

#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "sx1262_port.h"
#include "sx1262_regs.h"

#define RADIO_EPT_ADDR   (0x4005U)
#define RADIO_EPT_NAME   "rpmsg-radio"

#define RADIO_CMD_SVC_PING      (0x01U)
#define RADIO_CMD_RESET_STATUS  (0x02U)
#define RADIO_CMD_READ_REG      (0x03U)
#define RADIO_CMD_WRITE_REG     (0x04U)
#define RADIO_CMD_INIT          (0x05U)
#define RADIO_CMD_SET_FREQ      (0x06U)
#define RADIO_CMD_SET_POWER     (0x07U)
#define RADIO_CMD_SET_CW        (0x08U)
#define RADIO_CMD_STANDBY       (0x09U)
#define RADIO_CMD_GET_STATUS    (0x0AU)
#define RADIO_CMD_GET_ERRORS    (0x0BU)
#define RADIO_CMD_SET_ANTSW     (0x0CU)
#define RADIO_CMD_TX            (0x0DU)   /* [len][payload...] */
#define RADIO_CMD_RX_START      (0x0EU)   /* [t_hi][t_lo] ms, 0=continuous */
#define RADIO_CMD_RX_STOP       (0x0FU)

#define RADIO_EVT_RX            (0xE0U)
#define RADIO_EVT_RX_TIMEOUT    (0xE1U)

/* err the client can explain: chip not configured (freq/PA lost on reset) */
#define RADIO_ERR_NOT_INITED    (0x10U)

#define N_RADIO   2
#define TX_TIMEOUT_MS  3000U      /* cap on the in-handler TX_DONE wait */
#define RX_PAYLOAD_MAX 250U       /* fits an rpmsg buffer (496B) with the header */

#define MARK(addr, val)  (*(volatile unsigned int *)(addr) = (unsigned int)(val))

static struct rpmsg_lite_instance *s_rs_inst;
static struct rpmsg_lite_endpoint *s_rs_ept;
static uint32_t s_host_addr;                 /* last host ept addr, for deferred replies */
static uint32_t s_rx_host[N_RADIO];          /* host that armed each radio's RX (event dst) */

static struct sx1262_device s_radio[N_RADIO];
static bool s_ready[N_RADIO];                /* port (SPI/pins) initialized */
static bool s_inited[N_RADIO];               /* sx1262_init() done */
static volatile bool s_rx_active[N_RADIO];   /* listening; poll watches DIO1 */
static volatile bool s_rx_cont[N_RADIO];     /* continuous RX: re-arm, don't stop */

/* Deferred command reply: the rx callback runs INSIDE the poll thread's vring
 * drain; sending from that context froze the drain, so we queue the reply and
 * radio_service_poll() flushes it after the drain returns. */
static uint8_t  s_rsp_buf[8];
static uint32_t s_rsp_len;
static volatile int s_rsp_pending;

/* Event buffer (RX payload push). Only touched by the poll thread. */
static uint8_t s_evt_buf[8 + RX_PAYLOAD_MAX];

static int radio_lazy_init(int inst)
{
    if (s_ready[inst])
        return 0;
    if (sx1262_port_init(&s_radio[inst], inst) != 0)
        return -1;
    if (sx1262_reset(&s_radio[inst]) != 0)
        return -2;
    s_ready[inst] = true;
    return 0;
}

/* Send one LoRa packet, blocking until TX_DONE (bounded). Returns 0 or <0. */
static int radio_do_tx(int inst, const uint8_t *data, uint8_t len)
{
    struct sx1262_device *d = &s_radio[inst];
    int err;

    s_rx_active[inst] = false;      /* TX preempts any listen */
    s_rx_cont[inst]   = false;

    /* LoRa TX transmits exactly SetPacketParams.payloadLength bytes. */
    err = sx1262_set_packet_params(d, 8, 0, len, 1, 0);
    if (err) return err;
    err = sx1262_write_buffer(d, 0x00, data, len);
    if (err) return err;
    sx1262_clear_irq_status(d, SX1262_IRQ_ALL);

    err = sx1262_set_tx(d, 0);      /* start TX, no chip timeout, returns now */
    if (err) return err;
    err = sx1262_poll_irq(d, SX1262_IRQ_TX_DONE, TX_TIMEOUT_MS);

    sx1262_clear_irq_status(d, SX1262_IRQ_ALL);
    sx1262_set_antsw(d, SX1262_ANTSW_AUTO);
    d->tx_in_progress = false;
    return err;
}

/* Arm continuous/one-shot RX. Non-blocking afterwards: the poll loop delivers. */
static int radio_do_rx_start(int inst, uint32_t timeout_ms)
{
    struct sx1262_device *d = &s_radio[inst];
    int err;

    /* Variable-length header, CRC on, max payload for RX. */
    err = sx1262_set_packet_params(d, 8, 0, 0xFF, 1, 0);
    if (err) return err;
    sx1262_clear_irq_status(d, SX1262_IRQ_ALL);
    err = sx1262_set_rx(d, timeout_ms);
    if (err == 0) {
        s_rx_cont[inst]   = (timeout_ms == 0);  /* 0 = continuous: keep listening */
        s_rx_active[inst] = true;
    }
    return err;
}

static void radio_do_rx_stop(int inst)
{
    struct sx1262_device *d = &s_radio[inst];

    s_rx_active[inst] = false;
    s_rx_cont[inst]   = false;
    sx1262_set_standby(d, SX1262_STANDBY_RC);
    sx1262_set_antsw(d, SX1262_ANTSW_AUTO);
}

/*
 * Poll loop tick (called each ~2 ms by ping_echo_thread, after the vring
 * drain). Flushes a pending command reply and services any listening radio:
 * DIO1 high -> read IRQ -> on RX_DONE push EVT_RX with the payload, on TIMEOUT
 * push EVT_RX_TIMEOUT. One-shot: RX disarms after delivering.
 */
void radio_service_poll(void)
{
    int inst;

    if (s_rsp_pending)
    {
        int32_t ret = rpmsg_lite_send(s_rs_inst, s_rs_ept, s_host_addr,
                                      (char *)s_rsp_buf, s_rsp_len, RL_DONT_BLOCK);
        MARK(0xff6ff860, 0xAD300000U | ((uint32_t)(-ret) & 0xFFFFU));
        s_rsp_pending = 0;
    }

    for (inst = 0; inst < N_RADIO; inst++)
    {
        struct sx1262_device *d = &s_radio[inst];
        uint16_t irq = 0;

        if (!s_rx_active[inst])
            continue;
        if (!sx1262_dio1_is_high(d))     /* cheap GPIO check, no SPI */
            continue;
        if (sx1262_get_irq_status(d, &irq) != 0)
            continue;

        if (irq & SX1262_IRQ_RX_DONE)
        {
            uint8_t len = 0, off = 0;
            int16_t rssi = 0;
            int8_t  snr = 0;

            sx1262_get_rx_buffer_status(d, &len, &off);
            if (len > RX_PAYLOAD_MAX)
                len = RX_PAYLOAD_MAX;
            sx1262_read_buffer(d, off, &s_evt_buf[7], len);
            sx1262_get_packet_status(d, &rssi, &snr);

            s_evt_buf[0] = RADIO_EVT_RX;
            s_evt_buf[1] = (uint8_t)inst;
            s_evt_buf[2] = (irq & SX1262_IRQ_CRC_ERR) ? 0x00U : 0x01U; /* CRC ok */
            s_evt_buf[3] = len;
            s_evt_buf[4] = (uint8_t)(rssi >> 8);
            s_evt_buf[5] = (uint8_t)rssi;
            s_evt_buf[6] = (uint8_t)snr;
            MARK(0xff6ff864, 0xE0000000U | ((uint32_t)inst << 16) | len);
            (void)rpmsg_lite_send(s_rs_inst, s_rs_ept, s_rx_host[inst],
                                  (char *)s_evt_buf, 7U + len, RL_DONT_BLOCK);
            sx1262_clear_irq_status(d, SX1262_IRQ_ALL);
            if (s_rx_cont[inst]) {
                /* continuous RX: chip stays in RX (0xFFFFFF timeout); just
                 * clear the IRQ and keep listening for the next packet. */
            } else {
                s_rx_active[inst] = false;
                sx1262_set_standby(d, SX1262_STANDBY_RC);
                sx1262_set_antsw(d, SX1262_ANTSW_AUTO);
            }
        }
        else if (irq & SX1262_IRQ_TIMEOUT)
        {
            uint8_t ev[2] = { RADIO_EVT_RX_TIMEOUT, (uint8_t)inst };

            MARK(0xff6ff864, 0xE1000000U | (uint32_t)inst);
            (void)rpmsg_lite_send(s_rs_inst, s_rs_ept, s_rx_host[inst],
                                  (char *)ev, sizeof(ev), RL_DONT_BLOCK);
            s_rx_active[inst] = false;
            s_rx_cont[inst] = false;
            sx1262_clear_irq_status(d, SX1262_IRQ_ALL);
            sx1262_set_antsw(d, SX1262_ANTSW_AUTO);
        }
    }
}

static int32_t radio_rx(void *payload, uint32_t payload_len, uint32_t src, void *priv)
{
    const uint8_t *req = (const uint8_t *)payload;
    uint8_t rsp[8];
    uint32_t rsp_len = 0;
    int inst;
    int err;

    (void)priv;
    if (payload_len < 2U)
        return RL_RELEASE;

    s_host_addr = src;   /* remember where to push events / replies */

    {
        static unsigned int s_dispatch_count;
        MARK(0xff6ff858, 0xAD000000U | ((++s_dispatch_count & 0xFFU) << 8) | req[0]);
    }

    inst = req[1];
    if (inst >= N_RADIO)
    {
        rsp[0] = req[0]; rsp[1] = 0xED;   /* bad instance */
        rsp_len = 2;
        goto reply;
    }

    rsp[0] = req[0];

    switch (req[0])
    {
    case RADIO_CMD_SVC_PING:
        rsp[1] = 0;
        rsp[2] = 'R'; rsp[3] = 'D'; rsp[4] = 'I'; rsp[5] = 'O';
        rsp[6] = 0x01;   /* protocol version */
        rsp_len = 7;
        break;

    case RADIO_CMD_RESET_STATUS:
    {
        uint8_t status = 0xFF;

        s_inited[inst] = false;
        s_rx_active[inst] = false;
        s_rx_cont[inst] = false;
        err = radio_lazy_init(inst);
        if (err == 0)
            err = sx1262_reset(&s_radio[inst]);
        if (err == 0)
            err = sx1262_get_status(&s_radio[inst], &status);
        rsp[1] = (uint8_t)(-err);
        rsp[2] = status;
        rsp_len = 3;
        break;
    }

    case RADIO_CMD_READ_REG:
    {
        uint8_t val = 0xFF;
        uint16_t addr;

        if (payload_len < 4U) return RL_RELEASE;
        addr = ((uint16_t)req[2] << 8) | req[3];
        err = radio_lazy_init(inst);
        if (err == 0)
            err = sx1262_read_register(&s_radio[inst], addr, &val);
        rsp[1] = (uint8_t)(-err);
        rsp[2] = val;
        rsp_len = 3;
        break;
    }

    case RADIO_CMD_WRITE_REG:
    {
        uint16_t addr;

        if (payload_len < 5U) return RL_RELEASE;
        addr = ((uint16_t)req[2] << 8) | req[3];
        err = radio_lazy_init(inst);
        if (err == 0)
            err = sx1262_write_register(&s_radio[inst], addr, req[4]);
        rsp[1] = (uint8_t)(-err);
        rsp_len = 2;
        break;
    }

    case RADIO_CMD_INIT:
    {
        uint32_t freq;

        if (payload_len < 6U) return RL_RELEASE;
        freq = ((uint32_t)req[2] << 24) | ((uint32_t)req[3] << 16) |
               ((uint32_t)req[4] << 8) | req[5];
        err = radio_lazy_init(inst);
        if (err == 0)
            err = sx1262_init(&s_radio[inst], freq);
        if (err == 0)
            s_inited[inst] = true;
        rsp[1] = (uint8_t)(-err);
        rsp_len = 2;
        break;
    }

    case RADIO_CMD_SET_FREQ:
    {
        uint32_t freq;

        if (payload_len < 6U) return RL_RELEASE;
        freq = ((uint32_t)req[2] << 24) | ((uint32_t)req[3] << 16) |
               ((uint32_t)req[4] << 8) | req[5];
        err = radio_lazy_init(inst);
        if (err == 0 && !s_inited[inst]) { rsp[1] = RADIO_ERR_NOT_INITED; rsp_len = 2; break; }
        if (err == 0)
            err = sx1262_set_frequency(&s_radio[inst], freq);
        rsp[1] = (uint8_t)(-err);
        rsp_len = 2;
        break;
    }

    case RADIO_CMD_SET_POWER:
        if (payload_len < 3U) return RL_RELEASE;
        err = radio_lazy_init(inst);
        if (err == 0 && !s_inited[inst]) { rsp[1] = RADIO_ERR_NOT_INITED; rsp_len = 2; break; }
        if (err == 0)
            err = sx1262_set_tx_params(&s_radio[inst], (int8_t)req[2], 0x04);
        rsp[1] = (uint8_t)(-err);
        rsp_len = 2;
        break;

    case RADIO_CMD_SET_CW:
        err = radio_lazy_init(inst);
        if (err == 0 && !s_inited[inst]) { rsp[1] = RADIO_ERR_NOT_INITED; rsp_len = 2; break; }
        if (err == 0)
            err = sx1262_set_tx_continuous_wave(&s_radio[inst]);
        rsp[1] = (uint8_t)(-err);
        rsp_len = 2;
        break;

    case RADIO_CMD_STANDBY:
        err = radio_lazy_init(inst);
        if (err == 0)
        {
            s_rx_active[inst] = false;
            s_rx_cont[inst] = false;
            err = sx1262_set_standby(&s_radio[inst], SX1262_STANDBY_RC);
            sx1262_set_antsw(&s_radio[inst], SX1262_ANTSW_AUTO);
        }
        rsp[1] = (uint8_t)(-err);
        rsp_len = 2;
        break;

    case RADIO_CMD_GET_STATUS:
    {
        uint8_t status = 0xFF;

        err = radio_lazy_init(inst);
        if (err == 0)
            err = sx1262_get_status(&s_radio[inst], &status);
        rsp[1] = (uint8_t)(-err);
        rsp[2] = status;
        rsp_len = 3;
        break;
    }

    case RADIO_CMD_GET_ERRORS:
    {
        uint16_t errs = 0xFFFF;

        err = radio_lazy_init(inst);
        if (err == 0)
            err = sx1262_get_dev_errors(&s_radio[inst], &errs);
        rsp[1] = (uint8_t)(-err);
        rsp[2] = (uint8_t)(errs >> 8);
        rsp[3] = (uint8_t)errs;
        rsp_len = 4;
        break;
    }

    case RADIO_CMD_SET_ANTSW:
        if (payload_len < 3U) return RL_RELEASE;
        err = radio_lazy_init(inst);
        if (err == 0)
        {
            s_radio[inst].ant_sw_mode = req[2];
            sx1262_set_antsw(&s_radio[inst], req[2]);
        }
        rsp[1] = (uint8_t)(-err);
        rsp_len = 2;
        break;

    case RADIO_CMD_TX:
    {
        uint8_t len;

        if (payload_len < 3U) return RL_RELEASE;
        len = req[2];
        if ((uint32_t)len + 3U > payload_len) len = (uint8_t)(payload_len - 3U);
        err = radio_lazy_init(inst);
        if (err == 0 && !s_inited[inst]) { rsp[1] = RADIO_ERR_NOT_INITED; rsp_len = 2; break; }
        if (err == 0)
            err = radio_do_tx(inst, &req[3], len);
        rsp[1] = (uint8_t)(-err);
        rsp_len = 2;
        break;
    }

    case RADIO_CMD_RX_START:
    {
        uint32_t tmo;

        if (payload_len < 4U) return RL_RELEASE;
        tmo = ((uint32_t)req[2] << 8) | req[3];
        err = radio_lazy_init(inst);
        if (err == 0 && !s_inited[inst]) { rsp[1] = RADIO_ERR_NOT_INITED; rsp_len = 2; break; }
        if (err == 0) {
            s_rx_host[inst] = src;   /* deliver this radio's RX events here */
            err = radio_do_rx_start(inst, tmo);
        }
        rsp[1] = (uint8_t)(-err);
        rsp_len = 2;
        break;
    }

    case RADIO_CMD_RX_STOP:
        err = radio_lazy_init(inst);
        if (err == 0)
            radio_do_rx_stop(inst);
        rsp[1] = (uint8_t)(-err);
        rsp_len = 2;
        break;

    default:
        rsp[1] = 0xEE;   /* unknown command */
        rsp_len = 2;
        break;
    }

reply:
    MARK(0xff6ff85c, 0xAD200000U | rsp[1]);
    memcpy(s_rsp_buf, rsp, rsp_len);
    s_rsp_len = rsp_len;
    s_rsp_pending = 1;
    return RL_RELEASE;
}

/* Called by ping_echo.c once the link is up and the instance exists. */
int radio_service_attach(struct rpmsg_lite_instance *inst)
{
    s_rs_inst = inst;
    s_rs_ept = rpmsg_lite_create_ept(inst, RADIO_EPT_ADDR, radio_rx, RT_NULL);
    if (s_rs_ept == RT_NULL)
        return -1;
    rpmsg_ns_announce(inst, s_rs_ept, RADIO_EPT_NAME, RL_NS_CREATE);
    MARK(0xff6ff858, 0xAD000001U);   /* service attached+announced */
    return 0;
}

#endif /* RT_USING_RPMSG_LITE && !IPC_RAW_MBOX_TEST */
