/*
 * CubeSat — CommandService: uplink RX (radio0) + TC dispatch + rpmsg control.
 *
 * Patron identico a radio_service.c / sensor_service.c:
 *   - Endpoint rpmsg 0x4008 "rpmsg-command"
 *   - RX continua en radio0 (uplink) para recibir comandos TC via LoRa
 *   - Desempaca SPP, despacha por APID, responde por radio1 (downlink)
 *   - Linux controla start/stop/status/config via rpmsg
 *   - Evento EVT_TC_RX (0xE3) hacia Linux en cada TC recibido
 *
 * Diag markers:
 *   0xff6ff870  last cmd / event
 *   0xff6ff874  last error
 *   0xff6ff878  TC counter
 */

#include <rthw.h>
#include <rtthread.h>

#include "ipc_test_cfg.h"

#if defined(RT_USING_RPMSG_LITE) && !(defined(IPC_RAW_MBOX_TEST) && (IPC_RAW_MBOX_TEST == 1))

#include <string.h>
#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "spp.h"
#include "mission.h"
#include "sx1262_port.h"
#include "sx1262_regs.h"

/* ----------------------------------------------------------------- */
/*  rpmsg endpoint                                                    */
/* ----------------------------------------------------------------- */

#define CMD_EPT_ADDR   (0x4008U)
#define CMD_EPT_NAME   "rpmsg-command"

/* Commands from Linux */
#define CMD_CMD_PING        (0x01U)
#define CMD_CMD_START       (0x02U)
#define CMD_CMD_STOP        (0x03U)
#define CMD_CMD_STATUS      (0x04U)
#define CMD_CMD_CONFIG      (0x05U)
#define CMD_CMD_TC_SEND     (0x10U)

/* Events to Linux */
#define CMD_EVT_TC_RX       (0xE3U)

/* Errors */
#define CMD_ERR_IO          (0x10U)
#define CMD_ERR_TC          (0x12U)
#define CMD_ERR_BAD_ARG     (0x11U)

/* ----------------------------------------------------------------- */
/*  External (radio_service.c exports)                                */
/* ----------------------------------------------------------------- */

extern struct sx1262_device s_radio[];
extern int radio_tx(int inst, const uint8_t *data, uint8_t len);
extern int radio_ensure_ready(int inst, uint32_t freq_hz);
extern int radio_config_lora(int inst, uint32_t freq_hz, uint8_t sf,
                              uint32_t bw, uint8_t cr);
extern int radio_stop_rx(int inst);
extern bool radio_rx_owned_by_host(int inst);

/* ----------------------------------------------------------------- */
/*  Diagnostics                                                      */
/* ----------------------------------------------------------------- */

#define MARK(addr, val)   (*(volatile unsigned int *)(addr) = (unsigned int)(val))

/* ----------------------------------------------------------------- */
/*  State                                                             */
/* ----------------------------------------------------------------- */

static struct rpmsg_lite_instance   *s_inst;
static struct rpmsg_lite_endpoint   *s_ept;
static uint32_t                      s_host_addr;
static uint8_t                       s_rsp_buf[24];
static uint32_t                      s_rsp_len;
static volatile int                  s_rsp_pending;

static bool                          s_uplink_ready;
static bool                          s_uplink_rx_active;

static uint32_t                      s_ul_freq_hz;
static uint8_t                       s_ul_sf;
static uint32_t                      s_ul_bw;
static uint8_t                       s_ul_cr;

static packet_counter_t              s_cnt;
static uint32_t                      s_tc_count;

/* Thruster simulated */
#define THRUSTER_STATE_IDLE     0U
#define THRUSTER_STATE_RUNNING  1U

static uint8_t s_thruster_power[2];
static uint8_t s_thruster_state[2];

/* Beacon */
static uint32_t s_beacon_interval_ms;

/* Event buffer for EVT_TC_RX */
static uint8_t  s_evt_buf[256];
static uint32_t s_evt_len;
static volatile int s_evt_pending;

/* ----------------------------------------------------------------- */
/*  CRC-8                                        */
/* ----------------------------------------------------------------- */

static uint8_t crc8_compute(const uint8_t *data, uint32_t len)
{
    uint8_t crc = 0x00;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            if (crc & 0x80)
                crc = (uint8_t)((crc << 1) ^ 0x07);
            else
                crc <<= 1;
        }
    }
    return crc;
}

/* ----------------------------------------------------------------- */
/*  TX response on downlink (radio1)                                   */
/* ----------------------------------------------------------------- */

static int tx_on_downlink(const uint8_t *frame, uint8_t len)
{
    return radio_tx(1, frame, len);
}

static int tx_tm_response(uint16_t apid, const uint8_t *payload,
                           uint16_t payload_len)
{
    space_packet_t pkt;
    int err = spp_tm_build_packet(&pkt, &s_cnt,
                                   SPP_GROUP_FLAG_UNSEGMENTED,
                                   SPP_SECHEAD_FLAG_NOPRESENT, 0,
                                   apid, payload, payload_len);
    if (err != SPP_ERROR_NONE)
        return err;

    uint16_t total = spp_total_length(&pkt);
    return tx_on_downlink((uint8_t *)&pkt, (uint8_t)total);
}

/* ----------------------------------------------------------------- */
/*  Software reset                                                    */
/* ----------------------------------------------------------------- */

#include "hal_base.h"

static void software_reset(void)
{
    HAL_CRU_SetGlbSrst(GLB_SRST_FST);
    while (1) { }
}

/* ----------------------------------------------------------------- */
/*  FLASH image data                                                  */
/* ----------------------------------------------------------------- */

#define FLASH_DATA_LEN  255
#define CHUNK_SIZE      16

static const uint8_t s_flash_data[FLASH_DATA_LEN] = {
    0x00, 0x1F, 0x04, 0x20, 0xEB, 0x00, 0x00, 0x00, 0x35, 0x00,
    0x00, 0x00, 0x31, 0x00, 0x00, 0x00, 0x4D, 0x75, 0x01, 0x03,
    0x7A, 0x00, 0xC4, 0x00, 0x1D, 0x00, 0x00, 0x00, 0x00, 0x23,
    0x02, 0x88, 0x9A, 0x42, 0x03, 0xD0, 0x43, 0x88, 0x04, 0x30,
    0x91, 0x42, 0xF7, 0xD1, 0x18, 0x1C, 0x70, 0x47, 0x30, 0xBF,
    0xFD, 0xE7, 0xF4, 0x46, 0x00, 0xF0, 0x05, 0xF8, 0xA7, 0x48,
    0x00, 0x21, 0x01, 0x60, 0x41, 0x60, 0xE7, 0x46, 0xA5, 0x48,
    0x00, 0x21, 0xC9, 0x43, 0x01, 0x60, 0x41, 0x60, 0x70, 0x47,
    0xCA, 0x9B, 0x0D, 0x5B, 0xF9, 0x1D, 0x00, 0x00, 0x28, 0x43,
    0x29, 0x20, 0x32, 0x30, 0x32, 0x30, 0x20, 0x46, 0x6F, 0x6C,
    0x6C, 0x6F, 0x20, 0x54, 0x68, 0x65, 0x20, 0x57, 0x68, 0x74,
    0x65, 0x20, 0x52, 0x61, 0x62, 0x69, 0x74, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2D, 0x03,
    0x4C, 0x33, 0x57, 0x03, 0x54, 0x33, 0x8F, 0x03, 0x4D, 0x53,
    0xB9, 0x26, 0x53, 0x34, 0xAD, 0x26, 0x4D, 0x43, 0x1D, 0x26,
    0x43, 0x34, 0x05, 0x26, 0x55, 0x42, 0x91, 0x25, 0x44, 0x54,
    0xA9, 0x01, 0x44, 0x45, 0xAF, 0x01, 0x57, 0x56, 0x45, 0x01,
    0x49, 0x46, 0x91, 0x24, 0x45, 0x58, 0xE5, 0x23, 0x52, 0x45,
    0x6D, 0x23, 0x52, 0x50, 0xB5, 0x23, 0x46, 0x43, 0x51, 0x23,
    0x43, 0x58, 0x21, 0x23, 0x00, 0x00, 0x47, 0x52, 0x50, 0x00,
    0x43, 0x52, 0x58, 0x00, 0x53, 0x46, 0xCC, 0x01, 0x53, 0x44,
    0x4C, 0x02, 0x46, 0x5A, 0xCA, 0x01, 0x46, 0x53, 0x34, 0x27,
    0x46, 0x45, 0x28, 0x2E, 0x44, 0x53, 0x30, 0x2E, 0x44, 0x45,
    0xA4, 0x3D, 0x00, 0x00, 0x7D, 0x48, 0x01, 0x68, 0x00, 0x29,
    0x28, 0xD1, 0xFF, 0xF7, 0x9F, 0xFF, 0x7B, 0x49, 0x0A, 0x68,
    0x53, 0x0E, 0x01, 0xD3, 0x0A,
};

static void telemetry_spp_transmit_flash(void)
{
    const uint32_t total_chunks = (FLASH_DATA_LEN + CHUNK_SIZE - 1) / CHUNK_SIZE;

    for (uint32_t i = 0; i < total_chunks; i++) {
        uint32_t offset = i * CHUNK_SIZE;
        uint32_t remaining = FLASH_DATA_LEN - offset;
        uint32_t size = (remaining >= CHUNK_SIZE) ? CHUNK_SIZE : remaining;
        uint8_t buffer[MAX_PAYLOAD_CHUNK];
        uint16_t boff = 0;
        space_packet_t pkt;

        memset(buffer, 0, sizeof(buffer));
        buffer[boff++] = SPACECRAFT_ID;
        buffer[boff++] = (uint8_t)(i & 0xFF);
        buffer[boff++] = (uint8_t)((i >> 8) & 0xFF);
        buffer[boff++] = (uint8_t)(offset & 0xFF);
        buffer[boff++] = (uint8_t)((offset >> 8) & 0xFF);
        buffer[boff++] = (uint8_t)(remaining & 0xFF);
        buffer[boff++] = (uint8_t)((remaining >> 8) & 0xFF);
        if (boff + size + 2 > MAX_PAYLOAD_CHUNK)
            return;
        memcpy(&buffer[boff], &s_flash_data[offset], size);
        boff += size;
        buffer[boff++] = crc8_compute(&s_flash_data[offset], size);
        buffer[boff++] = '\0';

        uint8_t flag = (i == 0) ? SPP_GROUP_FLAG_START
                      : (i == total_chunks - 1) ? SPP_GROUP_FLAG_END
                      : SPP_GROUP_FLAG_CONT;

        int err = spp_tm_build_packet(&pkt, &s_cnt, flag,
                                       SPP_SECHEAD_FLAG_PRESENT, 6,
                                       SPP_APID_TM_FLASH,
                                       buffer, boff);
        if (err != SPP_ERROR_NONE)
            return;

        uint16_t total = spp_total_length(&pkt);
        (void)tx_on_downlink((uint8_t *)&pkt, (uint8_t)total);
        rt_thread_mdelay(100);
    }
}

/* ----------------------------------------------------------------- */
/*  Command dispatch                                                   */
/* ----------------------------------------------------------------- */

static void command_apid_handler(space_packet_t *pkt)
{
    /* header fields hold the BE wire value (see spp.c) — swap to read */
    uint16_t apid = spp_be16_to_host(pkt->header.identification) & 0x07FFU;
    uint8_t resp[MAX_PAYLOAD_CHUNK];
    uint16_t resp_len = 0;
    int err;

    MARK(0xff6ff870, 0xCC000000U | apid);

    if (apid == SPP_APID_TC_PING) {
        resp[0] = SPACECRAFT_ID;
        resp[1] = 0x41; resp[2] = 0x43; resp[3] = 0x4B; resp[4] = 0x00;
        resp_len = 5;
        tx_tm_response(SPP_APID_TM_PING, resp, resp_len);

    } else if (apid == SPP_APID_TC_RESETC) {
        rt_thread_mdelay(200);
        software_reset();

    } else if (apid == SPP_APID_TC_SEND_FW) {
        resp[0] = SPACECRAFT_ID;
        resp[1] = FW_PATCH;
        resp[2] = FW_MINOR;
        resp[3] = FW_MAJOR;
        resp[4] = 0x00;
        resp_len = 5;
        tx_tm_response(SPP_APID_TM_SEND_FW, resp, resp_len);

    } else if (apid == SPP_APID_TC_SET_THRUSTER) {
        uint8_t tid = pkt->data[0];
        uint8_t pwr = pkt->data[1];
        if (tid < 2) {
            s_thruster_power[tid] = pwr;
            s_thruster_state[tid] = (pwr > 0) ? THRUSTER_STATE_RUNNING
                                               : THRUSTER_STATE_IDLE;
        }
        resp[0] = tid;
        resp[1] = s_thruster_power[tid];
        resp_len = 2;
        tx_tm_response(SPP_APID_TM_SET_THRUSTER, resp, resp_len);

    } else if (apid == SPP_APID_TC_SET_BEACON_RATE) {
        uint8_t sec = pkt->data[0];
        if (sec > 0 && sec <= 10)
            s_beacon_interval_ms = (uint32_t)sec * 1000U;
        resp[0] = (uint8_t)(s_beacon_interval_ms / 1000);
        resp_len = 1;
        tx_tm_response(SPP_APID_TM_SET_BEACON_RATE, resp, resp_len);

    } else if (apid == SPP_APID_TC_BROADCAST_MSG) {
        uint16_t freq_mhz = ((uint16_t)pkt->data[0] << 8) | pkt->data[1];
        uint16_t pdlen = spp_be16_to_host(pkt->header.length) + 1;
        uint16_t mlen = (pdlen > 2) ? pdlen - 2 : 0;
        uint8_t mbuf[SPP_MAX_PAYLOAD_CHUNK];
        space_packet_t rpkt;

        if (mlen > SPP_MAX_PAYLOAD_CHUNK)
            mlen = SPP_MAX_PAYLOAD_CHUNK;
        memcpy(mbuf, pkt->data + 2, mlen);

        err = spp_tm_build_packet(&rpkt, &s_cnt,
                                   SPP_GROUP_FLAG_UNSEGMENTED,
                                   SPP_SECHEAD_FLAG_NOPRESENT, 0,
                                   SPP_APID_TM_BROADCAST_MSG,
                                   mbuf, mlen);
        if (err == SPP_ERROR_NONE) {
            uint16_t total = spp_total_length(&rpkt);
            radio_config_lora(1, (uint32_t)freq_mhz * 1000000U,
                              DOWNLINK_SF, DOWNLINK_BW, DOWNLINK_CR);
            (void)tx_on_downlink((uint8_t *)&rpkt, (uint8_t)total);
            radio_config_lora(1, DOWNLINK_FREQ,
                              DOWNLINK_SF, DOWNLINK_BW, DOWNLINK_CR);
        }

    } else if (apid == SPP_APID_TC_FLASH) {
        telemetry_spp_transmit_flash();

    } else {
        const uint8_t err_msg[] = {
            0x45, 0x72, 0x72, 0x6f, 0x72, 0x20, 0x55, 0x6e,
            0x6b, 0x6e, 0x6f, 0x77, 0x6e, 0x20, 0x41, 0x50,
            0x49, 0x44, 0x00
        };
        tx_tm_response(SPP_APID_TM_UNKNOWN, err_msg, sizeof(err_msg));
    }
}

/* ----------------------------------------------------------------- */
/*  Process received uplink packet                                    */
/* ----------------------------------------------------------------- */

static void process_rx_packet(const uint8_t *buf, uint8_t len)
{
    space_packet_t pkt;

    if (spp_unpack_packet(&pkt, buf, len) != SPP_ERROR_NONE)
        return;
    if (((spp_be16_to_host(pkt.header.identification) >> 12) & 0x01) != SPP_PTYPE_TC)
        return;

    s_tc_count++;

    /* Dispatch on MCU */
    command_apid_handler(&pkt);

    /* Push EVT_TC_RX to Linux */
    if (s_evt_pending == 0) {
        uint16_t apid = spp_be16_to_host(pkt.header.identification) & 0x07FF;
        uint32_t eo = 0;
        s_evt_buf[eo++] = CMD_EVT_TC_RX;
        s_evt_buf[eo++] = (uint8_t)(s_tc_count >> 24);
        s_evt_buf[eo++] = (uint8_t)(s_tc_count >> 16);
        s_evt_buf[eo++] = (uint8_t)(s_tc_count >> 8);
        s_evt_buf[eo++] = (uint8_t)s_tc_count;
        s_evt_buf[eo++] = (uint8_t)(apid >> 8);
        s_evt_buf[eo++] = (uint8_t)apid;
        if (eo + len <= sizeof(s_evt_buf)) {
            memcpy(s_evt_buf + eo, buf, len);
            eo += len;
        }
        s_evt_len = eo;
        s_evt_pending = 1;
    }
}

/* Forward declarations */
void command_service_init(void);

/* ----------------------------------------------------------------- */
/*  Uplink radio poll                                                 */
/* ----------------------------------------------------------------- */

void command_service_poll(void)
{
    struct sx1262_device *d;
    uint16_t irq = 0;
    static bool s_deferred;   /* host owned radio0; must re-arm on release */

    if (!s_uplink_rx_active)
        return;

    /* A Linux client (radio_test rx) armed radio0: hands off — reading the
     * IRQ here would steal its packets (RadioService polls earlier in the
     * loop, but packets landing mid-loop would hit us first). When the host
     * releases the radio, restore the uplink config (the host may have
     * changed freq/mod) and re-arm continuous RX. */
    if (radio_rx_owned_by_host(0)) {
        s_deferred = true;
        return;
    }
    if (s_deferred) {
        s_deferred = false;
        s_uplink_ready = false;
        command_service_init();
        return;
    }

    d = &s_radio[0];

    if (!sx1262_dio1_is_high(d))
        return;
    if (sx1262_get_irq_status(d, &irq) != 0)
        return;

    if (irq & SX1262_IRQ_RX_DONE) {
        uint8_t len = 0, off = 0;
        static uint8_t buf[256];

        sx1262_get_rx_buffer_status(d, &len, &off);
        if (len > 200)
            len = 200;
        sx1262_read_buffer(d, off, buf, len);
        sx1262_clear_irq_status(d, SX1262_IRQ_ALL);

        process_rx_packet(buf, len);

        sx1262_set_rx(d, 0);
    }
    else if (irq & SX1262_IRQ_TIMEOUT) {
        sx1262_clear_irq_status(d, SX1262_IRQ_ALL);
        sx1262_set_rx(d, 0);
    }
}

/* ----------------------------------------------------------------- */
/*  Deferred flush                                                    */
/* ----------------------------------------------------------------- */

void command_service_poll_flush(void)
{
    if (s_rsp_pending) {
        (void)rpmsg_lite_send(s_inst, s_ept, s_host_addr,
                              (char *)s_rsp_buf, s_rsp_len, RL_DONT_BLOCK);
        s_rsp_pending = 0;
    }
    if (s_evt_pending) {
        (void)rpmsg_lite_send(s_inst, s_ept, s_host_addr,
                              (char *)s_evt_buf, s_evt_len, RL_DONT_BLOCK);
        s_evt_pending = 0;
    }
}

/* ----------------------------------------------------------------- */
/*  rpmsg callback                                                    */
/* ----------------------------------------------------------------- */

static int32_t command_rx(void *payload, uint32_t payload_len,
                           uint32_t src, void *priv)
{
    const uint8_t *req = (const uint8_t *)payload;
    uint8_t rsp[24];
    uint32_t rsp_len = 0;

    (void)priv;
    if (payload_len < 1U) return RL_RELEASE;

    s_host_addr = src;
    rsp[0] = req[0];

    switch (req[0]) {

    case CMD_CMD_PING:
        rsp[1] = 0;
        rsp[2] = 'C'; rsp[3] = 'M'; rsp[4] = 'D'; rsp[5] = 'S';
        rsp[6] = 0x01;
        rsp_len = 7;
        break;

    case CMD_CMD_START:
        if (payload_len < 4U) { rsp[1] = CMD_ERR_BAD_ARG; rsp_len = 2; break; }
        {
            uint32_t freq = ((uint32_t)req[2] << 24) | ((uint32_t)req[3] << 16) |
                            ((uint32_t)req[4] << 8)  | req[5];
            s_ul_freq_hz = freq;
            s_uplink_rx_active = false;
            s_uplink_ready = false;
            command_service_init();
            if (s_uplink_rx_active) {
                rsp[1] = 0; rsp_len = 2;
            } else {
                rsp[1] = CMD_ERR_IO; rsp_len = 2;
            }
        }
        break;

    case CMD_CMD_STOP:
        s_uplink_rx_active = false;
        s_uplink_ready = false;
        radio_stop_rx(0);
        rsp[1] = 0;
        rsp_len = 2;
        break;

    case CMD_CMD_STATUS:
        rsp[1]  = 0;
        rsp[2]  = s_uplink_rx_active ? 1 : 0;
        rsp[3]  = s_thruster_power[0];
        rsp[4]  = s_thruster_power[1];
        rsp[5]  = (uint8_t)(s_beacon_interval_ms >> 8);
        rsp[6]  = (uint8_t)s_beacon_interval_ms;
        rsp[7]  = (uint8_t)(s_tc_count >> 24);
        rsp[8]  = (uint8_t)(s_tc_count >> 16);
        rsp[9]  = (uint8_t)(s_tc_count >> 8);
        rsp[10] = (uint8_t)s_tc_count;
        rsp_len = 11;
        break;

    case CMD_CMD_CONFIG:
        if (payload_len < 10U) { rsp[1] = CMD_ERR_BAD_ARG; rsp_len = 2; break; }
        {
            uint32_t freq = ((uint32_t)req[2] << 24) | ((uint32_t)req[3] << 16) |
                            ((uint32_t)req[4] << 8)  | req[5];
            uint8_t  sf = req[6];
            uint32_t bw = ((uint32_t)req[7] << 8) | req[8];
            uint8_t  cr = req[9];
            s_ul_freq_hz = freq;
            s_ul_sf      = sf;
            s_ul_bw      = bw;
            s_ul_cr      = cr;
            s_uplink_ready = false;
            s_uplink_rx_active = false;
            rsp[1] = 0; rsp_len = 2;
        }
        break;

    case CMD_CMD_TC_SEND:
        if (payload_len < 2U) { rsp[1] = CMD_ERR_BAD_ARG; rsp_len = 2; break; }
        process_rx_packet(&req[1], (uint8_t)(payload_len - 1U));
        rsp[1] = 0;
        rsp_len = 2;
        break;

    default:
        rsp[1] = 0xEE;
        rsp_len = 2;
        break;
    }

    MARK(0xff6ff870, 0xCC100000U | (rsp_len >= 2 ? rsp[1] : 0xFF));
    memcpy(s_rsp_buf, rsp, rsp_len);
    s_rsp_len = rsp_len;
    s_rsp_pending = 1;
    return RL_RELEASE;
}

/* ----------------------------------------------------------------- */
/*  Attach                                                            */
/* ----------------------------------------------------------------- */

int command_service_attach(struct rpmsg_lite_instance *inst)
{
    s_inst = inst;
    s_ept = rpmsg_lite_create_ept(inst, CMD_EPT_ADDR, command_rx, RT_NULL);
    if (s_ept == RT_NULL) return -1;
    rpmsg_ns_announce(inst, s_ept, CMD_EPT_NAME, RL_NS_CREATE);
    MARK(0xff6ff870, 0xCC200001U);
    return 0;
}

/* ----------------------------------------------------------------- */
/*  Init                                                              */
/* ----------------------------------------------------------------- */

void command_service_init(void)
{
    int err;
    struct sx1262_device *d = &s_radio[0];

    if (s_uplink_ready)
        return;

    err = radio_ensure_ready(0, s_ul_freq_hz);
    if (err) {
        MARK(0xff6ff874, 0xCC300000U | (uint8_t)(-err));
        return;
    }

    radio_stop_rx(0);

    sx1262_set_output_power(d, 20);   /* PA+OCP+clamp consistent (RX radio) */
    radio_config_lora(0, s_ul_freq_hz, s_ul_sf, s_ul_bw, s_ul_cr);

    /* No CRC on uplink */
    sx1262_set_packet_params(d, 8, 0, 0xFF, 0, 0);

    sx1262_clear_irq_status(d, SX1262_IRQ_ALL);
    err = sx1262_set_rx(d, 0);
    if (err) {
        MARK(0xff6ff874, 0xCC310000U | (uint8_t)(-err));
        return;
    }

    s_uplink_rx_active = true;
    s_uplink_ready = true;

    MARK(0xff6ff874, 0xCC400001U);
}

/* ----------------------------------------------------------------- */
/*  Default init (called once at boot)                                */
/* ----------------------------------------------------------------- */

static void default_init(void)
{
    s_ul_freq_hz = UPLINK_FREQ;
    s_ul_sf      = UPLINK_SF;
    s_ul_bw      = UPLINK_BW;
    s_ul_cr      = UPLINK_CR;

    s_thruster_power[0] = 10;
    s_thruster_state[0] = THRUSTER_STATE_RUNNING;
    s_thruster_power[1] = 10;
    s_thruster_state[1] = THRUSTER_STATE_RUNNING;

    s_beacon_interval_ms = BEACON_INTERVAL_MS;

    spp_counters_init(&s_cnt);
    s_tc_count = 0;
}

int command_service_init_default(void)
{
    default_init();
    command_service_init();
    return 0;
}

/* ----------------------------------------------------------------- */
/*  Accessors for telemetry_service                                    */
/* ----------------------------------------------------------------- */

uint8_t command_get_thruster0(void)       { return s_thruster_power[0]; }
uint8_t command_get_thruster1(void)       { return s_thruster_power[1]; }
uint32_t command_get_beacon_interval_ms(void) { return s_beacon_interval_ms; }

#endif /* RT_USING_RPMSG_LITE && !IPC_RAW_MBOX_TEST */
