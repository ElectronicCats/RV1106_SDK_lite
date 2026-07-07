/*
 * CubeSat — CCSDS telemetry service.
 *
 * CCSDS 133.0-B SPP packet format:
 * - CCSDS SPP primary header (6 bytes, big-endian), NO secondary header, NO CRC
 * - Telemetry payload (APID 0x08, 20 bytes, LE int16 fixed-point x100)
 * - Sync/Beacon (APID 0x01, 8 bytes: SC_ID + "Pwnsat\0" or "Beacon\0")
 * - Idle (APID 0x7FF, 14 bytes: "HackTheWorld!\0")
 * - Sequence counter increments per TM packet, wraps at 16383; idle uses 0
 *
 * Default LoRa: 916 MHz, SF7, BW250, CR 4/5 (mission downlink config)
 * Timers: telemetry 10.5s, sync 15s, idle 20s
 *
 * Controlled over rpmsg (endpoint 0x4007 "rpmsg-telemetry") from Linux.
 * Runs in the rpmsg poll thread (ping_echo.c).
 */

#include <rthw.h>
#include <rtthread.h>

#include "ipc_test_cfg.h"

#if defined(RT_USING_RPMSG_LITE) && !(defined(IPC_RAW_MBOX_TEST) && (IPC_RAW_MBOX_TEST == 1))

#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "sensor_port.h"
#include "bme280.h"
#include "icm42670.h"
#include "sx1262_port.h"
#include "spp.h"
#include "mission.h"
#include "command_service.h"

#define TELEM_EPT_ADDR   (0x4007U)
#define TELEM_EPT_NAME   "rpmsg-telemetry"

#define TELEM_CMD_PING      (0x01U)
#define TELEM_CMD_START     (0x02U)
#define TELEM_CMD_STOP      (0x03U)
#define TELEM_CMD_STATUS    (0x04U)
#define TELEM_CMD_CONFIG    (0x05U)
#define TELEM_CMD_MONITOR   (0x06U)

#define TELEM_EVT_TX        (0xE2U)

#define TELEM_ERR_IO        (0x10U)
#define TELEM_ERR_BAD_ARG   (0x11U)

#define MARK(addr, val)   (*(volatile unsigned int *)(addr) = (unsigned int)(val))

/* ------------------------------------------------------------------ */
/*  Defaults (downlink radio1, mission.h constants)                    */
/* ------------------------------------------------------------------ */

#define TELEM_DEFAULT_INST      1
#define TELEM_DEFAULT_INTERVAL  TELEM_INTERVAL_MS
#define TELEM_SYNC_INTERVAL     SYNC_INTERVAL_MS
#define TELEM_IDLE_INTERVAL     IDLE_INTERVAL_MS
#define TELEM_DEFAULT_FREQ      DOWNLINK_FREQ
#define TELEM_DEFAULT_SF        DOWNLINK_SF
#define TELEM_DEFAULT_BW        DOWNLINK_BW
#define TELEM_DEFAULT_CR        DOWNLINK_CR

#define SPP_TM_TYPE             (0U)
#define SPP_VER_SHIFT           (13U)
#define SPP_TYPE_SHIFT          (12U)
#define SPP_SECHDR_SHIFT        (11U)

#define APID_TM_SEND_TM         SPP_APID_TM_SEND_TM
#define APID_TM_PING            SPP_APID_TM_PING
#define APID_IDLE               SPP_APID_IDLE
#define SC_ID                   SPACECRAFT_ID

#define PAYLOAD_SIZE_TM         20
#define PAYLOAD_SIZE_SYNC       8
#define PAYLOAD_SIZE_IDLE       14
#define TM_FRAME_SIZE           (6 + PAYLOAD_SIZE_TM)
#define SYNC_FRAME_SIZE         (6 + PAYLOAD_SIZE_SYNC)
#define IDLE_FRAME_SIZE         (6 + PAYLOAD_SIZE_IDLE)

/* ------------------------------------------------------------------ */
/*  State                                                             */
/* ------------------------------------------------------------------ */

static bool                          s_inited;
static bool                          s_sensors_up;
static bool                          s_radio_up;
static bool                          s_telem_running;
static uint8_t                       s_telem_inst;
static uint32_t                      s_telem_interval_ms;
static uint32_t                      s_telem_freq_hz;
static uint8_t                       s_telem_sf;
static uint32_t                      s_telem_bw;
static uint8_t                       s_telem_cr;
static uint32_t                      s_last_tick_ms;
static uint16_t                      s_seq_count;
static uint32_t                      s_tx_count;
static uint32_t                      s_fail_count;

static uint32_t                      s_tm_prev;
static uint32_t                      s_sync_prev;
static uint32_t                      s_idle_prev;
static uint32_t                      s_beacon_prev;
static uint32_t                      s_beacon_interval_saved;

static struct rpmsg_lite_instance   *s_ts_inst;
static struct rpmsg_lite_endpoint   *s_ts_ept;
static uint32_t                      s_ts_host_addr;
static uint8_t                       s_ts_rsp[24];
static uint32_t                      s_ts_rsp_len;
static volatile int                  s_ts_rsp_pending;

static bool                          s_monitor_enabled;
static uint8_t                       s_monitor_buf[384];
static uint32_t                      s_monitor_len;
static volatile int                  s_monitor_pending;

extern struct sx1262_device s_radio[];
extern int radio_tx(int inst, const uint8_t *data, uint8_t len);
extern int radio_ensure_ready(int inst, uint32_t freq_hz);
extern int radio_config_lora(int inst, uint32_t freq_hz, uint8_t sf,
                              uint32_t bw, uint8_t cr);
extern int radio_stop_rx(int inst);

/* ------------------------------------------------------------------ */
/*  Helpers: LE for payload, BE for SPP header                        */
/* ------------------------------------------------------------------ */

static void put_i16_le(uint8_t *p, int16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

/* ------------------------------------------------------------------ */
/*  Build SPP primary header (6 bytes, big-endian)                     */
/* ------------------------------------------------------------------ */

static void build_spp_header(uint8_t *buf, uint8_t type, uint8_t sec_hdr,
                              uint16_t apid, uint8_t seq_flags,
                              uint16_t seq_count, uint16_t data_field_len)
{
    uint16_t packet_id = (0U << SPP_VER_SHIFT) |
                         ((uint16_t)(type & 1) << SPP_TYPE_SHIFT) |
                         ((uint16_t)(sec_hdr & 1) << SPP_SECHDR_SHIFT) |
                         (apid & 0x07FFU);
    uint16_t seq_ctrl = ((uint16_t)(seq_flags & 3) << 14) |
                         (seq_count & 0x3FFFU);
    uint16_t pkt_len  = data_field_len - 1;

    /* Big-endian wire order, MSB first. (The old host_to_be16(x)>>8 pattern
     * double-swapped and emitted the header little-endian on the air.) */
    buf[0] = (uint8_t)(packet_id >> 8);
    buf[1] = (uint8_t)packet_id;
    buf[2] = (uint8_t)(seq_ctrl >> 8);
    buf[3] = (uint8_t)seq_ctrl;
    buf[4] = (uint8_t)(pkt_len >> 8);
    buf[5] = (uint8_t)pkt_len;
}

/* ------------------------------------------------------------------ */
/*  Sensor helpers                                                     */
/* ------------------------------------------------------------------ */

static int ensure_sensors(void)
{
    if (s_sensors_up) return 0;
    if (sensor_i2c_init() != 0) return -1;
    if (bme280_init() != 0) return -2;
    if (icm42670_init() != 0) return -3;
    s_sensors_up = true;
    return 0;
}

static int ensure_radio(uint8_t inst)
{
    int err;
    if (s_radio_up) return 0;
    err = radio_ensure_ready(inst, s_telem_freq_hz);
    if (err) return err;
    radio_stop_rx(inst);
    sx1262_set_output_power(&s_radio[inst], 20);   /* PA+OCP+clamp consistent */
    radio_config_lora(inst, s_telem_freq_hz,
                      s_telem_sf, s_telem_bw, s_telem_cr);
    s_radio_up = true;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Build Telemetry packet (APID 0x08, 26 bytes) (CCSDS 133.0-B)     */
/* ------------------------------------------------------------------ */

static int build_tm_packet(uint8_t *buf, size_t buf_size, size_t *out_len)
{
    struct bme280_sample   bme = {0};
    struct icm42670_sample icm = {0};
    int err;

    if (buf_size < TM_FRAME_SIZE) return -1;

    err = bme280_read(&bme);
    if (err) return err;
    err = icm42670_read(&icm);
    if (err) return err;

    /* BME280 data */
    int16_t bme_temp_x100 = (int16_t)(bme.temp_mC / 10);            /* C x100 */
    float   press_hpa     = (float)bme.press_Pa / 100.0f;           /* hPa */
    int16_t press_x100    = (int16_t)(press_hpa * 100.0f);          /* hPa x100 (fixed-point hPa x100) */
    int16_t alt_x100      = 0;                                      /* no alt calc */
    int16_t hum_x100      = (int16_t)(bme.hum_m_pct / 10);         /* % x100 */

    /* ICM-42670 accel: mg -> m/s2 x100 */
    int16_t acc_x = (int16_t)((int32_t)icm.accel[0] * 9807 / 10000);
    int16_t acc_y = (int16_t)((int32_t)icm.accel[1] * 9807 / 10000);
    int16_t acc_z = (int16_t)((int32_t)icm.accel[2] * 9807 / 10000);
    int16_t acc_t = icm.temp;   /* ICM temp in C x100 */

    uint8_t payload[PAYLOAD_SIZE_TM];
    uint16_t off = 0;
    payload[off++] = SC_ID;
    put_i16_le(payload + off, acc_x); off += 2;
    put_i16_le(payload + off, acc_y); off += 2;
    put_i16_le(payload + off, acc_z); off += 2;
    put_i16_le(payload + off, acc_t); off += 2;
    put_i16_le(payload + off, bme_temp_x100); off += 2;
    put_i16_le(payload + off, press_x100);    off += 2;
    put_i16_le(payload + off, alt_x100);      off += 2;
    put_i16_le(payload + off, hum_x100);      off += 2;
    payload[off++] = command_get_thruster0();
    payload[off++] = command_get_thruster1();
    payload[off++] = 0;

    build_spp_header(buf, SPP_TM_TYPE, 0, APID_TM_SEND_TM, 3,
                     s_seq_count, PAYLOAD_SIZE_TM);
    memcpy(buf + 6, payload, PAYLOAD_SIZE_TM);
    *out_len = TM_FRAME_SIZE;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Build Sync packet (APID 0x01, 14 bytes): SC_ID + "Pwnsat\0"       */
/* ------------------------------------------------------------------ */

static int build_sync_packet(uint8_t *buf, size_t buf_size, size_t *out_len)
{
    if (buf_size < SYNC_FRAME_SIZE) return -1;
    uint8_t payload[PAYLOAD_SIZE_SYNC] = {
        SC_ID, 0x50, 0x77, 0x6e, 0x73, 0x61, 0x74, 0x00
    };
    build_spp_header(buf, SPP_TM_TYPE, 0, APID_TM_PING, 3,
                     s_seq_count, PAYLOAD_SIZE_SYNC);
    memcpy(buf + 6, payload, PAYLOAD_SIZE_SYNC);
    *out_len = SYNC_FRAME_SIZE;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Build Idle packet (APID 0x7FF, 20 bytes): "HackTheWorld!\0"       */
/*  seq_count = 0 (CCSDS idle packet, seq=0)              */
/* ------------------------------------------------------------------ */

static int build_idle_packet(uint8_t *buf, size_t buf_size, size_t *out_len)
{
    if (buf_size < IDLE_FRAME_SIZE) return -1;
    uint8_t payload[PAYLOAD_SIZE_IDLE] = {
        0x48, 0x61, 0x63, 0x6b, 0x54, 0x68, 0x65, 0x57,
        0x6f, 0x72, 0x6c, 0x64, 0x21, 0x00
    };
    build_spp_header(buf, SPP_TM_TYPE, 0, APID_IDLE, 3,
                     0, PAYLOAD_SIZE_IDLE);
    memcpy(buf + 6, payload, PAYLOAD_SIZE_IDLE);
    *out_len = IDLE_FRAME_SIZE;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  TX helper                                                         */
/* ------------------------------------------------------------------ */

static int tx_packet(uint8_t inst, const uint8_t *frame, size_t len,
                     uint8_t evt_buf[], uint32_t *evt_off)
{
    int err;
    sx1262_set_packet_params(&s_radio[inst], 8, 0,
                             (uint8_t)len, 1, 0);
    err = radio_tx(inst, frame, (uint8_t)len);
    if (err == 0 && s_monitor_enabled && evt_buf && evt_off) {
        if (*evt_off + 1 + len <= sizeof(s_monitor_buf)) {
            evt_buf[(*evt_off)++] = (uint8_t)len;
            memcpy(evt_buf + *evt_off, frame, len);
            *evt_off += len;
        }
    }
    return err;
}

/* ------------------------------------------------------------------ */
/*  Seq counter wrap (CCSDS sequence-count wrap)           */
/* ------------------------------------------------------------------ */

static void inc_seq(void)
{
    if (s_seq_count == 16383)
        s_seq_count = 0;
    else
        s_seq_count++;
}

/* ------------------------------------------------------------------ */
/*  Poll tick                                                         */
/* ------------------------------------------------------------------ */

void telemetry_service_poll(void)
{
    int err;
    uint8_t inst;
    uint32_t now_ms;

    if (!s_telem_running)
        return;

    if (!s_inited) {
        ensure_sensors();
        ensure_radio(s_telem_inst);
        now_ms = rt_tick_get() * 1000U / RT_TICK_PER_SECOND;
        s_tm_prev           = now_ms;
        s_sync_prev         = now_ms;
        s_idle_prev         = now_ms;
        s_beacon_prev       = now_ms;
        s_beacon_interval_saved = command_get_beacon_interval_ms();
        s_inited = true;
        MARK(0xff6ff890, 0xAF000001U);
    }

    if (!s_sensors_up || !s_radio_up) {
        s_fail_count++;
        MARK(0xff6ff894, 0xAF100000U | (s_fail_count & 0xFFFFU));
        if (s_fail_count > 100) { s_telem_running = false; s_inited = false; }
        return;
    }

    now_ms = rt_tick_get() * 1000U / RT_TICK_PER_SECOND;
    inst = s_telem_inst;

    radio_config_lora(inst, s_telem_freq_hz,
                      s_telem_sf, s_telem_bw, s_telem_cr);

    uint32_t evt_off = 0;
    if (s_monitor_enabled)
        s_monitor_buf[evt_off++] = TELEM_EVT_TX;

    err = 0;
    int sent = 0;   /* packets actually transmitted this tick */

    /* Telemetry data (10.5s) */
    if ((now_ms - s_tm_prev) >= s_telem_interval_ms) {
        s_tm_prev = now_ms;
        uint8_t frame[TM_FRAME_SIZE];
        size_t len = 0;
        err = build_tm_packet(frame, sizeof(frame), &len);
        if (err == 0) {
            err = tx_packet(inst, frame, len,
                            s_monitor_enabled ? s_monitor_buf : NULL, &evt_off);
            if (err == 0) { inc_seq(); sent++; }
        }
    }

    /* Sync (15s) */
    if ((now_ms - s_sync_prev) >= TELEM_SYNC_INTERVAL) {
        s_sync_prev = now_ms;
        uint8_t frame[SYNC_FRAME_SIZE];
        size_t len = 0;
        err = build_sync_packet(frame, sizeof(frame), &len);
        if (err == 0) {
            err = tx_packet(inst, frame, len,
                            s_monitor_enabled ? s_monitor_buf : NULL, &evt_off);
            if (err == 0) { inc_seq(); sent++; }
        }
    }

    /* Idle (20s) */
    if ((now_ms - s_idle_prev) >= TELEM_IDLE_INTERVAL) {
        s_idle_prev = now_ms;
        uint8_t frame[IDLE_FRAME_SIZE];
        size_t len = 0;
        err = build_idle_packet(frame, sizeof(frame), &len);
        if (err == 0) {
            err = tx_packet(inst, frame, len,
                            s_monitor_enabled ? s_monitor_buf : NULL, &evt_off);
            if (err == 0) sent++;
        }
    }

    /* Beacon (configurable via TC, default 15s) */
    {
        uint32_t bi = command_get_beacon_interval_ms();
        if (bi != s_beacon_interval_saved) {
            s_beacon_interval_saved = bi;
            s_beacon_prev = now_ms;
        }
        if (s_beacon_interval_saved > 0 &&
            (now_ms - s_beacon_prev) >= s_beacon_interval_saved) {
            s_beacon_prev = now_ms;
            uint8_t bframe[SYNC_FRAME_SIZE];
            uint8_t payload[8] = {SC_ID, 0x42, 0x65, 0x61, 0x63, 0x6f, 0x6e, 0x00};
            build_spp_header(bframe, SPP_TM_TYPE, 0, APID_TM_PING, 3,
                             s_seq_count, 8);
            memcpy(bframe + 6, payload, 8);
            err = tx_packet(inst, bframe, SYNC_FRAME_SIZE,
                            s_monitor_enabled ? s_monitor_buf : NULL, &evt_off);
            if (err == 0) { inc_seq(); sent++; }
        }
    }

    if (s_monitor_enabled && sent > 0) {
        s_monitor_len = evt_off;
        s_monitor_pending = 1;
    }

    if (err == 0 && sent > 0) {
        s_tx_count += (uint32_t)sent;
        MARK(0xff6ff898, s_tx_count);
        MARK(0xff6ff890, 0xAF010000U | (s_tx_count & 0xFFFFU));
    } else if (err != 0) {
        s_fail_count++;
        MARK(0xff6ff894, 0xAF300000U | ((uint32_t)(-err) & 0xFFU));
    }
}

/* ------------------------------------------------------------------ */
/*  Deferred-reply flush                                               */
/* ------------------------------------------------------------------ */

void telemetry_service_poll_flush(void)
{
    if (s_ts_rsp_pending) {
        (void)rpmsg_lite_send(s_ts_inst, s_ts_ept, s_ts_host_addr,
                              (char *)s_ts_rsp, s_ts_rsp_len, RL_DONT_BLOCK);
        s_ts_rsp_pending = 0;
    }
    if (s_monitor_pending) {
        (void)rpmsg_lite_send(s_ts_inst, s_ts_ept, s_ts_host_addr,
                              (char *)s_monitor_buf, s_monitor_len, RL_DONT_BLOCK);
        s_monitor_pending = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  rpmsg RX callback                                                  */
/* ------------------------------------------------------------------ */

static int32_t telemetry_rx(void *payload, uint32_t payload_len,
                             uint32_t src, void *priv)
{
    const uint8_t *req = (const uint8_t *)payload;
    uint8_t rsp[24];
    uint32_t rsp_len = 0;

    (void)priv;
    if (payload_len < 1U) return RL_RELEASE;

    s_ts_host_addr = src;
    MARK(0xff6ff89c, 0xAF400000U | req[0]);

    rsp[0] = req[0];

    switch (req[0]) {

    case TELEM_CMD_PING:
        rsp[1] = 0;
        rsp[2] = 'T'; rsp[3] = 'E'; rsp[4] = 'L'; rsp[5] = 'E';
        rsp[6] = 0x01;
        rsp_len = 7;
        break;

    case TELEM_CMD_START: {
        uint8_t  inst;
        uint32_t interval_ms;
        if (payload_len < 4U) { rsp[1] = TELEM_ERR_BAD_ARG; rsp_len = 2; break; }
        inst        = req[1];
        interval_ms = ((uint32_t)req[2] << 8) | req[3];
        if (inst > 1) { rsp[1] = TELEM_ERR_BAD_ARG; rsp_len = 2; break; }
        if (interval_ms < 1000U || interval_ms > 600000U) {
            rsp[1] = TELEM_ERR_BAD_ARG; rsp_len = 2; break;
        }
        s_telem_inst         = inst;
        s_telem_interval_ms  = interval_ms;
        s_telem_running      = true;
        s_inited             = false;
        s_seq_count          = 0;
        rsp[1] = 0;
        rsp_len = 2;
        break;
    }

    case TELEM_CMD_STOP:
        s_telem_running = false;
        s_inited = false;
        rsp[1] = 0;
        rsp_len = 2;
        break;

    case TELEM_CMD_STATUS:
        rsp[1]  = 0;
        rsp[2]  = s_telem_running ? 1 : 0;
        rsp[3]  = s_telem_inst;
        rsp[4]  = (uint8_t)(s_telem_interval_ms >> 8);
        rsp[5]  = (uint8_t)s_telem_interval_ms;
        rsp[6]  = (uint8_t)(s_seq_count >> 8);
        rsp[7]  = (uint8_t)s_seq_count;
        rsp[8]  = (uint8_t)(s_tx_count >> 24);
        rsp[9]  = (uint8_t)(s_tx_count >> 16);
        rsp[10] = (uint8_t)(s_tx_count >> 8);
        rsp[11] = (uint8_t)s_tx_count;
        rsp[12] = (uint8_t)(s_fail_count >> 24);
        rsp[13] = (uint8_t)(s_fail_count >> 16);
        rsp[14] = (uint8_t)(s_fail_count >> 8);
        rsp[15] = (uint8_t)s_fail_count;
        rsp_len = 16;
        break;

    case TELEM_CMD_CONFIG: {
        uint8_t  inst;
        uint32_t freq;
        uint8_t  sf;
        uint32_t bw;
        uint8_t  cr;
        if (payload_len < 9U) { rsp[1] = TELEM_ERR_BAD_ARG; rsp_len = 2; break; }
        inst  = req[1];
        freq  = ((uint32_t)req[2] << 24) | ((uint32_t)req[3] << 16) |
                ((uint32_t)req[4] << 8)  | req[5];
        sf    = req[6];
        bw    = ((uint32_t)req[7] << 8)  | req[8];
        cr    = req[9];
        if (inst > 1) { rsp[1] = TELEM_ERR_BAD_ARG; rsp_len = 2; break; }
        if (freq < 150000000U || freq > 960000000U) { rsp[1] = TELEM_ERR_BAD_ARG; rsp_len = 2; break; }
        if (sf < 5 || sf > 12) { rsp[1] = TELEM_ERR_BAD_ARG; rsp_len = 2; break; }
        s_telem_inst        = inst;
        s_telem_freq_hz     = freq;
        s_telem_sf          = sf;
        s_telem_bw          = bw;
        s_telem_cr          = cr;
        s_radio_up          = false;
        s_inited            = false;
        rsp[1] = 0;
        rsp_len = 2;
        break;
    }

    case TELEM_CMD_MONITOR: {
        uint8_t enable = (payload_len >= 2) ? req[1] : 1;
        s_monitor_enabled = enable ? true : false;
        rsp[1] = 0;
        rsp_len = 2;
        break;
    }

    default:
        rsp[1] = 0xEE;
        rsp_len = 2;
        break;
    }

    MARK(0xff6ff89c, 0xAF500000U | (rsp_len >= 2 ? rsp[1] : 0xFF));
    memcpy(s_ts_rsp, rsp, rsp_len);
    s_ts_rsp_len = rsp_len;
    s_ts_rsp_pending = 1;
    return RL_RELEASE;
}

/* ------------------------------------------------------------------ */
/*  Attach + Init                                                     */
/* ------------------------------------------------------------------ */

int telemetry_service_attach(struct rpmsg_lite_instance *inst)
{
    s_ts_inst = inst;
    s_ts_ept = rpmsg_lite_create_ept(inst, TELEM_EPT_ADDR, telemetry_rx, RT_NULL);
    if (s_ts_ept == RT_NULL) return -1;
    rpmsg_ns_announce(inst, s_ts_ept, TELEM_EPT_NAME, RL_NS_CREATE);
    MARK(0xff6ff89c, 0xAF600001U);
    return 0;
}

int telemetry_service_init(void)
{
    s_inited            = false;
    s_sensors_up        = false;
    s_radio_up          = false;
    s_telem_running     = false;
    s_telem_inst        = TELEM_DEFAULT_INST;
    s_telem_interval_ms = TELEM_DEFAULT_INTERVAL;
    s_telem_freq_hz     = TELEM_DEFAULT_FREQ;
    s_telem_sf          = TELEM_DEFAULT_SF;
    s_telem_bw          = TELEM_DEFAULT_BW;
    s_telem_cr          = TELEM_DEFAULT_CR;
    s_last_tick_ms      = 0;
    s_seq_count         = 0;
    s_tx_count          = 0;
    s_fail_count        = 0;
    return 0;
}

#endif /* RT_USING_RPMSG_LITE && !IPC_RAW_MBOX_TEST */
