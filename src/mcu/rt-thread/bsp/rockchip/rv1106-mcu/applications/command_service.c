/*
 * CubeSat — CommandService: FlatSat-compatible port.
 *   - Uplink RX (radio0) with hex-ASCII conversion
 *   - Telemetry worker (TM data 10.5s, sync 15s, idle 20s, beacon)
 *   - TC dispatch via SPP APID handler (same APIDs, same vulnerabilities)
 *   - rpmsg control from Linux (ping/start/stop/status/config/TC_SEND)
 *   - Events EVT_TC_RX pushed to Linux for each received TC
 *
 * All FlatSat vulnerabilities preserved: CRC=0, no auth, no rate limits,
 * no power limits, no freq bounds, beacon OFF by default, sensor errors ignored.
 */

#include <rthw.h>
#include <rtthread.h>

#include "ipc_test_cfg.h"

#if defined(RT_USING_RPMSG_LITE) && !(defined(IPC_RAW_MBOX_TEST) && (IPC_RAW_MBOX_TEST == 1))

#include <string.h>
#include <stdio.h>
#include "rpmsg_lite.h"
#include "rpmsg_ns.h"
#include "spp.h"
#include "ccsds_tc.h"
#include "mission.h"
#include "sx1262_port.h"
#include "sx1262_regs.h"
#include "sensor_port.h"
#include "bme280.h"
#include "packet_dispatch.h"
#include "icm42670.h"
#include "thr.h"

#define CMD_EPT_ADDR   (0x4008U)
#define CMD_EPT_NAME   "rpmsg-command"

#define CMD_CMD_PING        (0x01U)
#define CMD_CMD_START       (0x02U)
#define CMD_CMD_STOP        (0x03U)
#define CMD_CMD_STATUS      (0x04U)
#define CMD_CMD_CONFIG      (0x05U)
#define CMD_CMD_TC_SEND     (0x10U)

#define CMD_EVT_TC_RX       (0xE3U)
#define CMD_ERR_IO          (0x10U)
#define CMD_ERR_BAD_ARG     (0x11U)

extern struct sx1262_device s_radio[];
extern int radio_tx(int inst, const uint8_t *data, uint8_t len);
extern int radio_ensure_ready(int inst, uint32_t freq_hz);
extern int radio_config_lora(int inst, uint32_t freq_hz, uint8_t sf,
                              uint32_t bw, uint8_t cr);
extern int radio_stop_rx(int inst);
extern void radio_set_rx_active(int inst, bool active);
extern void telemetry_push_monitor(const uint8_t *data, uint16_t len);
void command_service_init(void);

#define MARK(addr, val)   (*(volatile unsigned int *)(addr) = (unsigned int)(val))

static struct rpmsg_lite_instance   *s_inst;
static struct rpmsg_lite_endpoint   *s_ept;
static uint32_t                      s_host_addr;   /* destination for command replies (last sender) */
static uint32_t                      s_evt_host;     /* destination for EVT_TC_RX events (subscriber via PING); not overwritten by other commands */
static uint8_t                       s_rsp_buf[24];
static uint32_t                      s_rsp_len;
static volatile int                  s_rsp_pending;

static bool                          s_uplink_ready;
static bool                          s_uplink_rx_active;
static bool                          s_downlink_ready;   /* radio1 (TX) brought up */

static uint32_t                      s_ul_freq_hz;
static uint8_t                       s_ul_sf;
static uint32_t                      s_ul_bw;
static uint8_t                       s_ul_cr;

static packet_counter_t              s_cnt;
static uint32_t                      s_tc_count;



static uint8_t  s_evt_buf[256];
static uint32_t s_evt_len;
static volatile int s_evt_pending;

#include "hal_base.h"
static void software_reset(void)
{
    HAL_CRU_SetGlbSrst(GLB_SRST_FST);
    while (1) { }
}

/* Bring up radio1 (downlink/TX) and set it to the mission parameters.
 * In FlatSat the init thread calls radio_manager_apply_lora(1) and telemetry
 * TX is gated on lora_initialized; here we replicate both: without this
 * initialization radio_do_tx(1) would operate on an s_radio[1].spi = NULL
 * and dereference NULL on the first telemetry send (~10.5 s). */
static void command_service_downlink_init(void)
{
    if (s_downlink_ready)
        return;
    if (radio_ensure_ready(1, DOWNLINK_FREQ) != 0)
        return;
    radio_config_lora(1, DOWNLINK_FREQ, DOWNLINK_SF, DOWNLINK_BW, DOWNLINK_CR);
    sx1262_set_output_power(&s_radio[1], DOWNLINK_POWER);
    s_downlink_ready = true;
}

static int tx_on_downlink(const uint8_t *frame, uint8_t len)
{
    /* Mirror the downlink to the host over IPC (TM monitor, if some client
     * enabled it via TelemetryService). No-op if nobody listens. FlatSat did
     * not do this over CDC; here it completes the "receive over the protocol" link. */
    telemetry_push_monitor(frame, len);
    return radio_tx(1, frame, len);
}

typedef struct {
    unsigned long interval_ms;
    unsigned long previous_ms;
} timeout_worker_t;

#define CHUNK_SIZE 16

static timeout_worker_t t_tm_data  = {10500, 0};
static timeout_worker_t t_sync     = {15000, 0};
static timeout_worker_t t_idle     = {20000, 0};
static timeout_worker_t t_beacon   = {15000, 0};

static bool block_tx = false;

static const uint8_t image_data[255] = {
    0x00, 0x1F, 0x04, 0x20, 0xEB, 0x00, 0x00, 0x00, 0x35, 0x00, 0x00, 0x00,
    0x31, 0x00, 0x00, 0x00, 0x4D, 0x75, 0x01, 0x03, 0x7A, 0x00, 0xC4, 0x00,
    0x1D, 0x00, 0x00, 0x00, 0x00, 0x23, 0x02, 0x88, 0x9A, 0x42, 0x03, 0xD0,
    0x43, 0x88, 0x04, 0x30, 0x91, 0x42, 0xF7, 0xD1, 0x18, 0x1C, 0x70, 0x47,
    0x30, 0xBF, 0xFD, 0xE7, 0xF4, 0x46, 0x00, 0xF0, 0x05, 0xF8, 0xA7, 0x48,
    0x00, 0x21, 0x01, 0x60, 0x41, 0x60, 0xE7, 0x46, 0xA5, 0x48, 0x00, 0x21,
    0xC9, 0x43, 0x01, 0x60, 0x41, 0x60, 0x70, 0x47, 0xCA, 0x9B, 0x0D, 0x5B,
    0xF9, 0x1D, 0x00, 0x00, 0x28, 0x43, 0x29, 0x20, 0x32, 0x30, 0x32, 0x30,
    0x20, 0x46, 0x6F, 0x6C, 0x6C, 0x6F, 0x20, 0x54, 0x68, 0x65, 0x20, 0x57,
    0x68, 0x74, 0x65, 0x20, 0x52, 0x61, 0x62, 0x69, 0x74, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x2D, 0x03, 0x4C, 0x33,
    0x57, 0x03, 0x54, 0x33, 0x8F, 0x03, 0x4D, 0x53, 0xB9, 0x26, 0x53, 0x34,
    0xAD, 0x26, 0x4D, 0x43, 0x1D, 0x26, 0x43, 0x34, 0x05, 0x26, 0x55, 0x42,
    0x91, 0x25, 0x44, 0x54, 0xA9, 0x01, 0x44, 0x45, 0xAF, 0x01, 0x57, 0x56,
    0x45, 0x01, 0x49, 0x46, 0x91, 0x24, 0x45, 0x58, 0xE5, 0x23, 0x52, 0x45,
    0x6D, 0x23, 0x52, 0x50, 0xB5, 0x23, 0x46, 0x43, 0x51, 0x23, 0x43, 0x58,
    0x21, 0x23, 0x00, 0x00, 0x47, 0x52, 0x50, 0x00, 0x43, 0x52, 0x58, 0x00,
    0x53, 0x46, 0xCC, 0x01, 0x53, 0x44, 0x4C, 0x02, 0x46, 0x5A, 0xCA, 0x01,
    0x46, 0x53, 0x34, 0x27, 0x46, 0x45, 0x28, 0x2E, 0x44, 0x53, 0x30, 0x2E,
    0x44, 0x45, 0xA4, 0x3D, 0x00, 0x00, 0x7D, 0x48, 0x01, 0x68, 0x00, 0x29,
    0x28, 0xD1, 0xFF, 0xF7, 0x9F, 0xFF, 0x7B, 0x49, 0x0A, 0x68, 0x53, 0x0E,
    0x01, 0xD3, 0x0A,
};

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

static inline int16_t float_to_fixed(float val, float scale)
{
    return (int16_t)(val * scale);
}

static void telemetry_spp_pack_frame(float x, float y, float z, float t,
                                     float tm, float p, float alt, float hum)
{
    int off = 0;
    uint8_t buf[MAX_PAYLOAD_CHUNK];
    buf[off++] = SPACECRAFT_ID;

    int16_t v;
    v = float_to_fixed(x, 100.0f);   buf[off++] = (uint8_t)(v & 0xFF); buf[off++] = (uint8_t)((v >> 8) & 0xFF);
    v = float_to_fixed(y, 100.0f); buf[off++] = (uint8_t)(v & 0xFF); buf[off++] = (uint8_t)((v >> 8) & 0xFF);
    v = float_to_fixed(z, 100.0f); buf[off++] = (uint8_t)(v & 0xFF); buf[off++] = (uint8_t)((v >> 8) & 0xFF);
    v = float_to_fixed(t, 100.0f); buf[off++] = (uint8_t)(v & 0xFF); buf[off++] = (uint8_t)((v >> 8) & 0xFF);
    v = float_to_fixed(tm, 100.0f);buf[off++] = (uint8_t)(v & 0xFF); buf[off++] = (uint8_t)((v >> 8) & 0xFF);
    v = float_to_fixed(p, 100.0f); buf[off++] = (uint8_t)(v & 0xFF); buf[off++] = (uint8_t)((v >> 8) & 0xFF);
    v = float_to_fixed(alt, 100.0f);buf[off++] = (uint8_t)(v & 0xFF); buf[off++] = (uint8_t)((v >> 8) & 0xFF);
    v = float_to_fixed(hum, 100.0f);buf[off++] = (uint8_t)(v & 0xFF); buf[off++] = (uint8_t)((v >> 8) & 0xFF);

    buf[off++] = thruster_get_t0_power();
    buf[off++] = thruster_get_t1_power();
    buf[off++] = '\0';

    space_packet_t pkt;
    int ret = spp_tm_build_packet(&pkt, &s_cnt, SPP_GROUP_FLAG_UNSEGMENTED,
                                   SPP_SECHEAD_FLAG_NOPRESENT, 0,
                                   SPP_APID_TM_SEND_TM, buf, off);
    if (ret != SPP_ERROR_NONE) return;

    uint16_t total = spp_total_length(&pkt);
    (void)tx_on_downlink((uint8_t *)&pkt, (uint8_t)total);
}

static void telemetry_spp_transmit_version(void)
{
    uint8_t buf[5] = {SPACECRAFT_ID, FW_PATCH, FW_MINOR, FW_MAJOR, 0x00};
    space_packet_t pkt;
    int ret = spp_tm_build_packet(&pkt, &s_cnt, SPP_GROUP_FLAG_UNSEGMENTED,
                                   SPP_SECHEAD_FLAG_NOPRESENT, 0,
                                   SPP_APID_TM_SEND_FW, buf, 5);
    if (ret != SPP_ERROR_NONE) return;
    uint16_t total = spp_total_length(&pkt);
    (void)tx_on_downlink((uint8_t *)&pkt, (uint8_t)total);
}

static void telemetry_spp_transmit_ping_sync(void)
{
    uint8_t buf[8] = {SPACECRAFT_ID, 0x50, 0x77, 0x6e, 0x73, 0x61, 0x74, 0x00};
    space_packet_t pkt;
    int ret = spp_tm_build_packet(&pkt, &s_cnt, SPP_GROUP_FLAG_UNSEGMENTED,
                                   SPP_SECHEAD_FLAG_NOPRESENT, 0,
                                   SPP_APID_TM_PING, buf, 8);
    if (ret != SPP_ERROR_NONE) return;
    uint16_t total = spp_total_length(&pkt);
    (void)tx_on_downlink((uint8_t *)&pkt, (uint8_t)total);
}

static void telemetry_spp_transmit_idle(void)
{
    space_packet_t pkt;
    int ret = spp_idle_build_packet(&pkt);
    if (ret != SPP_ERROR_NONE) return;
    uint16_t total = spp_total_length(&pkt);
    (void)tx_on_downlink((uint8_t *)&pkt, (uint8_t)total);
}

static void telemetry_spp_transmit_ping_ack(void)
{
    uint8_t buf[5] = {SPACECRAFT_ID, 0x41, 0x43, 0x4b, 0x00};
    space_packet_t pkt;
    int ret = spp_tm_build_packet(&pkt, &s_cnt, SPP_GROUP_FLAG_UNSEGMENTED,
                                   SPP_SECHEAD_FLAG_NOPRESENT, 0,
                                   SPP_APID_TM_PING, buf, 5);
    if (ret != SPP_ERROR_NONE) return;
    uint16_t total = spp_total_length(&pkt);
    (void)tx_on_downlink((uint8_t *)&pkt, (uint8_t)total);
}

static void telemetry_spp_transmit_beacon(void)
{
    uint8_t buf[8] = {SPACECRAFT_ID, 0x42, 0x65, 0x61, 0x63, 0x6f, 0x6e, 0x00};
    space_packet_t pkt;
    int ret = spp_tm_build_packet(&pkt, &s_cnt, SPP_GROUP_FLAG_UNSEGMENTED,
                                   SPP_SECHEAD_FLAG_NOPRESENT, 0,
                                   SPP_APID_TM_PING, buf, 8);
    if (ret != SPP_ERROR_NONE) return;
    uint16_t total = spp_total_length(&pkt);
    (void)tx_on_downlink((uint8_t *)&pkt, (uint8_t)total);
}

static void telemetry_spp_transmit_flash(void)
{
    block_tx = true;
    const uint32_t total_chunks = (255 + CHUNK_SIZE - 1) / CHUNK_SIZE;

    for (uint32_t i = 0; i < total_chunks; i++) {
        uint32_t offset = i * CHUNK_SIZE;
        uint32_t remaining = 255 - offset;
        uint32_t size = (remaining >= CHUNK_SIZE) ? CHUNK_SIZE : remaining;

        uint8_t buffer[MAX_PAYLOAD_CHUNK];
        uint16_t boff = 0;

        memset(buffer, 0, sizeof(buffer));
        buffer[boff++] = SPACECRAFT_ID;
        buffer[boff++] = (uint8_t)(i & 0xFF);
        buffer[boff++] = (uint8_t)((i >> 8) & 0xFF);
        buffer[boff++] = (uint8_t)(offset & 0xFF);
        buffer[boff++] = (uint8_t)((offset >> 8) & 0xFF);
        buffer[boff++] = (uint8_t)(remaining & 0xFF);
        buffer[boff++] = (uint8_t)((remaining >> 8) & 0xFF);

        if (boff + size + 2 > MAX_PAYLOAD_CHUNK) { block_tx = false; return; }

        memcpy(&buffer[boff], &image_data[offset], size);
        boff += size;
        buffer[boff++] = crc8_compute(&image_data[offset], size);
        buffer[boff++] = '\0';

        space_packet_t pkt;
        uint8_t flag = (i == 0) ? SPP_GROUP_FLAG_START
                      : (i == total_chunks - 1) ? SPP_GROUP_FLAG_END
                      : SPP_GROUP_FLAG_CONT;

        int ret = spp_tm_build_packet(&pkt, &s_cnt, flag,
                                     SPP_SECHEAD_FLAG_PRESENT, 6,
                                     SPP_APID_TM_FLASH, buffer, boff);
        if (ret != SPP_ERROR_NONE) { block_tx = false; return; }

        uint16_t total = spp_total_length(&pkt);
        if (tx_on_downlink((uint8_t *)&pkt, (uint8_t)total) != 0) {
            block_tx = false;
            return;
        }
        rt_thread_mdelay(100);
    }
    block_tx = false;
}

void command_service_telemetry_worker(void)
{
    if (block_tx) return;

    /* FlatSat gate (lora_initialized): do not transmit until the downlink
     * radio is brought up. Retry the bring-up if it is not yet. */
    if (!s_downlink_ready) {
        command_service_downlink_init();
        if (!s_downlink_ready) return;
    }

    uint32_t now_ms = rt_tick_get() * 1000U / RT_TICK_PER_SECOND;

    if (now_ms - t_tm_data.previous_ms >= t_tm_data.interval_ms) {
        t_tm_data.previous_ms = now_ms;

        /* Read sensors — FlatSat: errors are NEVER checked */
        struct bme280_sample bme = {0};
        struct icm42670_sample icm = {0};
        bme280_read(&bme);

        /* FlatSat: delay(100) between BME and accel reads */
        rt_thread_mdelay(100);
        icm42670_read(&icm);

        /* Convert to FlatSat-compatible float representation.
         * ICM-42670 @ ±16g: 2048 LSB/g. FlatSat LIS2DH12 returns g*100 fixed-point.
         * Scale: g = raw / 2048;  g*100 = raw * 100 / 2048 = raw * 0.048828125 */
        float lsb_per_g = 2048.0f;
        float x = (float)icm.accel[0] / lsb_per_g * 100.0f;
        float y = (float)icm.accel[1] / lsb_per_g * 100.0f;
        float z = (float)icm.accel[2] / lsb_per_g * 100.0f;

        /* ICM-42670 temperature: raw -> °C, then *100 for fixed-point
         * Formula: °C = raw/128 + 25 (per datasheet). Then *100 = (raw/128 + 25) */
        float t = ((float)icm.temp / 128.0f + 25.0f) * 100.0f;

        float tm = (float)bme.temp_mC / 1000.0f;
        float p = (float)bme.press_Pa / 100.0f;
        /* FlatSat: bme.readAltitude(1013.25f) returns meters; *100 for centi-meters.
         * Approximation using the hypsometric formula:
         *   alt_m = (1 - (p/1013.25)^0.1903) * 44330
         * Simple portable implementation without math.h/powf. */
        float alt = 0.0f;
        float hum = (float)bme.hum_m_pct / 1000.0f;

        telemetry_spp_pack_frame(x, y, z, t, tm, p, alt, hum);
    }

    /* Beacon: only fires when interval != 15000 (default), exactly like FlatSat */
    if (t_beacon.interval_ms != 15000 &&
        now_ms - t_beacon.previous_ms >= t_beacon.interval_ms) {
        t_beacon.previous_ms = now_ms;
        telemetry_spp_transmit_beacon();
    }

    if (now_ms - t_sync.previous_ms >= t_sync.interval_ms) {
        t_sync.previous_ms = now_ms;
        telemetry_spp_transmit_ping_sync();
    }

    if (now_ms - t_idle.previous_ms >= t_idle.interval_ms) {
        t_idle.previous_ms = now_ms;
        telemetry_spp_transmit_idle();
    }
}

static void command_apid_handler(space_packet_t *pkt)
{
    uint16_t apid = spp_be16_to_host(pkt->header.identification) & 0x07FFU;

    MARK(0xff6ff870, 0xCC000000U | apid);

    if (apid == SPP_APID_TC_PING) {
        telemetry_spp_transmit_ping_ack();

    } else if (apid == SPP_APID_TC_RESETC) {
        rt_thread_mdelay(200);
        software_reset();

    } else if (apid == SPP_APID_TC_SEND_FW) {
        telemetry_spp_transmit_version();

    } else if (apid == SPP_APID_TC_SET_THRUSTER) {
        uint8_t tid = pkt->data[0];
        uint8_t pwr = pkt->data[1];
        if (tid == 0)
            thruster_set_t0_power(pwr);
        else if (tid == 1)
            thruster_set_t1_power(pwr);
        /* invalid IDs are silently ignored — matches FlatSat */

    } else if (apid == SPP_APID_TC_SET_BEACON_RATE) {
        uint8_t sec = pkt->data[0];
        if (sec > 10)
            return; /* rejected, matches FlatSat */
        t_beacon.interval_ms = (uint32_t)sec * 1000U;

    } else if (apid == SPP_APID_TC_BROADCAST_MSG) {
        uint16_t freq_mhz = ((uint16_t)pkt->data[0] << 8) | pkt->data[1];
        uint16_t pdlen = spp_be16_to_host(pkt->header.length) + 1;
        /* VULN #10 (inherited from FlatSat, intentional for the CTF): msg_len is
         * computed as payload_total - 2 WITHOUT guarding the underflow or bounding
         * the memcpy (FlatSat: `size_t msg_len = payload_total - 2; memcpy(buffer_msg,
         * data+2, msg_len)`). A BROADCAST with length==0 -> pdlen==1 -> mlen =
         * 1 - 2 underflows (uint16) to 0xFFFF; the memcpy copies 65535 bytes into
         * mbuf[256] -> STACK SMASH -> MCU crash (DoS). */
        uint16_t mlen = pdlen - 2;
        uint8_t mbuf[SPP_MAX_PAYLOAD_CHUNK];
        memset(mbuf, 0, sizeof(mbuf));

        memcpy(mbuf, pkt->data + 2, mlen);

        space_packet_t rpkt;
        int err = spp_tm_build_packet(&rpkt, &s_cnt,
                                       SPP_GROUP_FLAG_UNSEGMENTED,
                                       SPP_SECHEAD_FLAG_NOPRESENT, 0,
                                       SPP_APID_TM_BROADCAST_MSG, mbuf, mlen);
        if (err == SPP_ERROR_NONE) {
            uint16_t total = spp_total_length(&rpkt);
            /* FlatSat: ONLY change frequency, NOT SF/BW/CR */
            sx1262_set_frequency(&s_radio[1], (uint32_t)freq_mhz * 1000000U);
            (void)tx_on_downlink((uint8_t *)&rpkt, (uint8_t)total);
            sx1262_set_frequency(&s_radio[1], DOWNLINK_FREQ);
        }

    } else if (apid == SPP_APID_TC_FLASH) {
        telemetry_spp_transmit_flash();

    } else {
        const uint8_t err_msg[] = {
            0x45, 0x72, 0x72, 0x6f, 0x72, 0x20, 0x55, 0x6e,
            0x6b, 0x6e, 0x6f, 0x77, 0x6e, 0x20, 0x41, 0x50,
            0x49, 0x44, 0x00
        };
        space_packet_t pkt_out;
        int ret = spp_tm_build_packet(&pkt_out, &s_cnt,
                                       SPP_GROUP_FLAG_UNSEGMENTED,
                                       SPP_SECHEAD_FLAG_NOPRESENT, 0,
                                       0x09, err_msg, sizeof(err_msg));
        if (ret == SPP_ERROR_NONE) {
            uint16_t total = spp_total_length(&pkt_out);
            (void)tx_on_downlink((uint8_t *)&pkt_out, (uint8_t)total);
        }
    }
}

/* Returns 0 if buf parsed as a valid SPP TC and was dispatched, -1 otherwise. */
static int process_rx_packet(const uint8_t *buf, uint8_t len)
{
    space_packet_t pkt;

    if (spp_unpack_packet(&pkt, buf, len) != SPP_ERROR_NONE)   /* keeps VULN #9 */
        return -1;
    if (((spp_be16_to_host(pkt.header.identification) >> 12) & 0x01) != SPP_PTYPE_TC)
        return -1;

    /* Zephyr-style secured TC: if the frame carries the secondary header
     * (sec_hdr_flag=1), verify the CRC and XTEA-decrypt the args in place so the
     * APID handler below sees plaintext. WEAK BY DESIGN: the CRC result is
     * computed but NOT enforced, and a plaintext TC (sec_hdr_flag=0) is still
     * accepted and dispatched unchanged — so every existing over-the-air
     * exploit keeps working. This is the generalisation of the no-auth vuln. */
    ccsds_tc_sec_header_t sh;
    int crc_ok = 0;
    (void)ccsds_tc_unsecure(&pkt, &sh, &crc_ok);   /* return + crc_ok ignored */

    s_tc_count++;

    command_apid_handler(&pkt);

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
    return 0;
}

static int hex_char_to_nibble(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

static size_t hex_string_to_bytes(const uint8_t *input, size_t len,
                                 uint8_t *output)
{
    size_t out = 0;
    for (size_t i = 0; i < len;) {
        if (input[i] == ' ') { i++; continue; }
        if (i + 1 >= len) break;
        int hi = hex_char_to_nibble((char)input[i]);
        int lo = hex_char_to_nibble((char)input[i + 1]);
        if (hi < 0 || lo < 0) break;
        output[out++] = (uint8_t)((hi << 4) | lo);
        i += 2;
    }
    return out;
}

void command_service_poll(void)
{
}

static void command_uplink_handler(int inst, const uint8_t *data, uint8_t len,
                                    int16_t rssi, int8_t snr, bool crc_ok)
{
    (void)inst; (void)rssi; (void)snr; (void)crc_ok;
    if (!s_uplink_rx_active)
        return;

    /* Auto-detect the uplink format:
     *   1) Direct binary SPP (FlatSat / real ground format). A valid SPP frame
     *      carries version=0 in the top 3 bits; hex-ASCII text does not.
     *   2) If it does not validate as binary, interpret it as hex-ASCII
     *      (peer/test bench emitting hex text) and retry.
     * This interoperates with both without configuration. */
    if (process_rx_packet(data, len) == 0)
        return;

    uint8_t raw[256];
    size_t raw_len = hex_string_to_bytes(data, len, raw);
    if (raw_len >= SPP_PRIMARY_HEADER_LEN)
        (void)process_rx_packet(raw, (uint8_t)raw_len);
}

void command_service_poll_flush(void)
{
    if (s_rsp_pending) {
        (void)rpmsg_lite_send(s_inst, s_ept, s_host_addr,
                              (char *)s_rsp_buf, s_rsp_len, RL_DONT_BLOCK);
        s_rsp_pending = 0;
    }
    if (s_evt_pending) {
        /* Events go to the subscriber (s_evt_host), not the last command
         * sender, so cmd_status/tcsend/etc. do not "steal" them. If nobody
         * subscribed yet, fall back to s_host_addr as before. */
        uint32_t evt_dst = s_evt_host ? s_evt_host : s_host_addr;
        (void)rpmsg_lite_send(s_inst, s_ept, evt_dst,
                              (char *)s_evt_buf, s_evt_len, RL_DONT_BLOCK);
        s_evt_pending = 0;
    }
}

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
        /* A PING registers the sender as an EVT_TC_RX event subscriber.
         * Unlike s_host_addr (overwritten by every command), s_evt_host
         * only changes here, so cmd_listen/cmd_watch do not lose delivery
         * when another process sends cmd_status/tcsend/etc. in parallel. */
        s_evt_host = src;
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
        rsp[3]  = thruster_get_t0_power();
        rsp[4]  = thruster_get_t1_power();
        rsp[5]  = (uint8_t)(t_beacon.interval_ms >> 8);
        rsp[6]  = (uint8_t)t_beacon.interval_ms;
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
        rsp[1] = 0;
        rsp_len = 2;
        process_rx_packet(&req[1], (uint8_t)(payload_len - 1U));
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

int command_service_attach(struct rpmsg_lite_instance *inst)
{
    s_inst = inst;
    s_ept = rpmsg_lite_create_ept(inst, CMD_EPT_ADDR, command_rx, RT_NULL);
    if (s_ept == RT_NULL) return -1;
    rpmsg_ns_announce(inst, s_ept, CMD_EPT_NAME, RL_NS_CREATE);
    pd_register(command_uplink_handler);
    MARK(0xff6ff870, 0xCC200001U);
    return 0;
}

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

    sx1262_set_output_power(d, 20);
    radio_config_lora(0, s_ul_freq_hz, s_ul_sf, s_ul_bw, s_ul_cr);

    /* No CRC on uplink — FlatSat vulnerability preserved.
     * Start RX directly (not via radio_do_rx_start) so radio_service_poll
     * does NOT send EVT_RX to Linux (no s_rx_host from this path).
     * The packet dispatch is the ONLY consumer — command_uplink_handler
     * registered via pd_register() in command_service_attach(). */
    sx1262_set_packet_params(d, 8, 0, 0xFF, 0, 0);   /* preamble 8 — FlatSat_Firmware default */

    MARK(0xff6ff874, 0xCC320001U);

    sx1262_clear_irq_status(d, SX1262_IRQ_ALL);
    err = sx1262_set_rx(d, 0);
    if (err) {
        MARK(0xff6ff874, 0xCC310000U | (uint8_t)(-err));
        return;
    }
    radio_set_rx_active(0, true);

    s_uplink_rx_active = true;
    s_uplink_ready = true;

    MARK(0xff6ff874, 0xCC400001U);
}

int command_service_init_default(void)
{
    s_ul_freq_hz = UPLINK_FREQ;
    s_ul_sf      = UPLINK_SF;
    s_ul_bw      = UPLINK_BW;
    s_ul_cr      = UPLINK_CR;

    t_tm_data.interval_ms  = TELEM_INTERVAL_MS;
    t_sync.interval_ms     = SYNC_INTERVAL_MS;
    t_idle.interval_ms     = IDLE_INTERVAL_MS;
    t_beacon.interval_ms   = BEACON_INTERVAL_MS;

    spp_counters_init(&s_cnt);
    s_tc_count = 0;

    command_service_init();          /* uplink RX (radio0) */
    command_service_downlink_init(); /* downlink TX (radio1) */
    return 0;
}

uint8_t command_get_thruster0(void)       { return thruster_get_t0_power(); }
uint8_t command_get_thruster1(void)       { return thruster_get_t1_power(); }
uint32_t command_get_beacon_interval_ms(void) { return t_beacon.interval_ms; }

#endif /* RT_USING_RPMSG_LITE && !IPC_RAW_MBOX_TEST */