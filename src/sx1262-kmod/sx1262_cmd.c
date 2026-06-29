#include <linux/spi/spi.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/math64.h>
#include "sx1262.h"
#include "sx1262_regs.h"

extern int sx1262_spi_write(struct sx1262_device *dev, const uint8_t *data, size_t len);
extern int sx1262_spi_write_then_read(struct sx1262_device *dev,
                                        const uint8_t *tx, size_t tx_len,
                                        uint8_t *rx, size_t rx_len);
extern int sx1262_spi_transfer(struct sx1262_device *dev,
                                struct spi_transfer *xfers, unsigned int num);
extern int sx1262_wait_busy(struct sx1262_device *dev, unsigned long timeout_ms);
extern void sx1262_set_antsw(struct sx1262_device *dev, int mode);

/* Read single register: full-duplex single CS-asserted transaction.
 * Frame: cmd + addr_hi + addr_lo + NOP + NOP.
 * rx[4] = register value (after cmd, addr, and two dummies). */
int sx1262_read_register(struct sx1262_device *dev, uint16_t addr, uint8_t *val)
{
    uint8_t buf[5] = {
        SX1262_CMD_READ_REGISTER,
        (addr >> 8) & 0xFF,
        addr & 0xFF,
        SX1262_CMD_NOP,
        SX1262_CMD_NOP
    };
    uint8_t rx[5] = { 0 };
    struct spi_transfer xfer = {
        .tx_buf = buf,
        .rx_buf = rx,
        .len = 5,
    };
    int ret;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_transfer(dev, &xfer, 1);
    if (ret) return ret;

    *val = rx[4];
    return 0;
}

/* Write single register */
int sx1262_write_register(struct sx1262_device *dev, uint16_t addr, uint8_t val)
{
    uint8_t tx[4] = { SX1262_CMD_WRITE_REGISTER, (addr >> 8) & 0xFF, addr & 0xFF, val };
    int ret;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write(dev, tx, 4);
    if (ret) return ret;

    return sx1262_wait_busy(dev, 100);
}

/* Read multiple registers */
int sx1262_read_registers(struct sx1262_device *dev, uint16_t addr, uint8_t *buf, size_t len)
{
    uint8_t tx[3] = { SX1262_CMD_READ_REGISTER, (addr >> 8) & 0xFF, addr & 0xFF };
    uint8_t nop_len = len + 1; /* status byte + data */
    uint8_t rx[260];
    int ret;

    if (nop_len > sizeof(rx))
        return -EINVAL;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write_then_read(dev, tx, 3, rx, nop_len);
    if (ret) return ret;

    memcpy(buf, rx + 1, len);
    return 0;
}

/* Write multiple registers */
int sx1262_write_registers(struct sx1262_device *dev, uint16_t addr, const uint8_t *buf, size_t len)
{
    uint8_t tx[260];
    int ret;

    if (len + 3 > sizeof(tx))
        return -EINVAL;

    tx[0] = SX1262_CMD_WRITE_REGISTER;
    tx[1] = (addr >> 8) & 0xFF;
    tx[2] = addr & 0xFF;
    memcpy(tx + 3, buf, len);

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write(dev, tx, len + 3);
    if (ret) return ret;

    return sx1262_wait_busy(dev, 100);
}

/* Read FIFO buffer */
int sx1262_read_buffer(struct sx1262_device *dev, uint8_t offset, uint8_t *buf, size_t len)
{
    uint8_t tx[2] = { SX1262_CMD_READ_BUFFER, offset };
    uint8_t rx[260];
    int ret;

    if (len + 1 > sizeof(rx))
        return -EINVAL;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write_then_read(dev, tx, 2, rx, len + 1);
    if (ret) return ret;

    memcpy(buf, rx + 1, len);
    return 0;
}

/* Write FIFO buffer */
int sx1262_write_buffer(struct sx1262_device *dev, uint8_t offset, const uint8_t *buf, size_t len)
{
    uint8_t tx[260];
    int ret;

    if (len + 2 > sizeof(tx))
        return -EINVAL;

    tx[0] = SX1262_CMD_WRITE_BUFFER;
    tx[1] = offset;
    memcpy(tx + 2, buf, len);

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write(dev, tx, len + 2);
    if (ret) return ret;

    return sx1262_wait_busy(dev, 100);
}

/* NOTE: this board uses a 32 MHz crystal (no TCXO) and an external antenna
 * switch driven by GPIO (see sx1262_set_antsw), so SetDIO3asTcxoCtrl and
 * SetDIO2asRfSwitchCtrl are intentionally not used. */

/* Set sleep mode */
int sx1262_set_sleep(struct sx1262_device *dev, uint8_t sleep_cfg)
{
    uint8_t tx[2] = { SX1262_CMD_SET_SLEEP, sleep_cfg };
    return sx1262_spi_write(dev, tx, 2);
}

/* Set regulator mode: 0=LDO only, 1=DC-DC + LDO */
int sx1262_set_regulator_mode(struct sx1262_device *dev, uint8_t mode)
{
    uint8_t tx[2] = { SX1262_CMD_SET_REGULATOR_MODE, mode };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 2);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set standby mode */
int sx1262_set_standby(struct sx1262_device *dev, uint8_t standby_cfg)
{
    uint8_t tx[2] = { SX1262_CMD_SET_STANDBY, standby_cfg };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 2);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set packet type (LoRa or GFSK) */
int sx1262_set_packet_type(struct sx1262_device *dev, uint8_t pkt_type)
{
    uint8_t tx[2] = { SX1262_CMD_SET_PACKET_TYPE, pkt_type };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 2);
    if (ret) return ret;
    dev->packet_type = pkt_type;
    return sx1262_wait_busy(dev, 100);
}

/* Get packet type (debug/verify) */
int sx1262_get_packet_type(struct sx1262_device *dev, uint8_t *pkt_type)
{
    uint8_t tx[1] = { SX1262_CMD_GET_PACKET_TYPE };
    uint8_t rx[2] = { 0 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write_then_read(dev, tx, 1, rx, 2);
    if (ret) return ret;
    *pkt_type = rx[1];
    return 0;
}

/* Set RF frequency.
 * SetRfFrequency (0x86) takes FOUR Frf bytes, MSB first:
 * Frf[31:24], Frf[23:16], Frf[15:8], Frf[7:0]. Sending only 3 drops the MSB
 * and lands the carrier on the wrong frequency (e.g. 915 -> ~768 MHz). */
int sx1262_set_frequency(struct sx1262_device *dev, uint32_t freq_hz)
{
    uint64_t rf_freq = div_u64((uint64_t)freq_hz * (1 << 25), 32000000);
    uint8_t tx[5] = {
        SX1262_CMD_SET_RF_FREQUENCY,
        (rf_freq >> 24) & 0xFF,
        (rf_freq >> 16) & 0xFF,
        (rf_freq >> 8) & 0xFF,
        rf_freq & 0xFF
    };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 5);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set buffer base addresses for FIFO (TX and RX) */
int sx1262_set_buffer_base_address(struct sx1262_device *dev, uint8_t tx_base, uint8_t rx_base)
{
    uint8_t tx[3] = { SX1262_CMD_SET_BUFFER_BASE_ADDRESS, tx_base, rx_base };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 3);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set fallback mode after RX/TX completion */
int sx1262_set_rx_tx_fallback_mode(struct sx1262_device *dev, uint8_t mode)
{
    uint8_t tx[2] = { SX1262_CMD_SET_RX_TX_FALLBACK_MODE, mode };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 2);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set PA config (optimal for SX1262) */
int sx1262_set_pa_config(struct sx1262_device *dev)
{
    uint8_t tx[5] = { SX1262_CMD_SET_PA_CONFIG, 0x04, 0x07, 0x00, 0x01 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 5);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set TX parameters (power dBm, ramp time).
 * On the SX1262 the power field is the signed dBm value directly
 * (-9..+22 with the high-power PA), NOT an SX127x-style offset. */
int sx1262_set_tx_params(struct sx1262_device *dev, int8_t power_dbm, uint8_t ramp_time)
{
    uint8_t tx[3];
    int ret;

    if (power_dbm > 22)  power_dbm = 22;
    if (power_dbm < -9)  power_dbm = -9;

    tx[0] = SX1262_CMD_SET_TX_PARAMS;
    tx[1] = (uint8_t)power_dbm;   /* two's-complement dBm */
    tx[2] = ramp_time;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 3);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set modulation parameters (LoRa) */
int sx1262_set_modulation_params(struct sx1262_device *dev, uint8_t sf, uint32_t bw, uint8_t cr, bool ldro)
{
    int ret;
    uint8_t bw_bits;
    uint8_t tx[9] = { SX1262_CMD_SET_MODULATION_PARAMS };

    /* LoRa bandwidth enum values per SX1262 datasheet (SetModulationParams) */
    switch (bw) {
        case 7800:     bw_bits = 0x00; break;
        case 10400:    bw_bits = 0x08; break;
        case 15600:    bw_bits = 0x01; break;
        case 20800:    bw_bits = 0x09; break;
        case 31250:    bw_bits = 0x02; break;
        case 41700:    bw_bits = 0x0A; break;
        case 62500:    bw_bits = 0x03; break;
        case 125000:   bw_bits = 0x04; break;
        case 250000:   bw_bits = 0x05; break;
        case 500000:   bw_bits = 0x06; break;
        default:       bw_bits = 0x04; break;  /* 125 kHz */
    }

    tx[1] = sf;         /* Spreading Factor */
    tx[2] = bw_bits;     /* Bandwidth */
    tx[3] = cr;          /* Coding Rate: 1=4/5, 2=4/6, 3=4/7, 4=4/8 */
    tx[4] = ldro ? 1 : 0; /* Low Data Rate Optimization */

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    /* LoRa SetModulationParams takes exactly 4 parameter bytes */
    ret = sx1262_spi_write(dev, tx, 5);
    if (ret) return ret;
    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    /* Errata 15.1: Tx modulation quality. Reg 0x0889 (TxModulation):
     * clear bit 2 for BW=500 kHz, set it otherwise. */
    {
        uint8_t reg = 0;
        if (sx1262_read_register(dev, 0x0889, &reg) == 0) {
            if (bw == 500000)
                reg &= ~0x04;
            else
                reg |= 0x04;
            sx1262_write_register(dev, 0x0889, reg);
        }
    }
    return 0;
}

/* Set packet parameters (LoRa) — byte order matches SX1262 datasheet */
int sx1262_set_packet_params(struct sx1262_device *dev, uint16_t preamble_len,
                              uint8_t hdr_type, uint8_t payload_len,
                              uint8_t crc_type, uint8_t iq_inverted)
{
    int ret;
    uint8_t tx[7] = {
        SX1262_CMD_SET_PACKET_PARAMS,
        (preamble_len >> 8) & 0xFF,    /* PreambleLength MSB */
        preamble_len & 0xFF,            /* PreambleLength LSB */
        hdr_type,                       /* HeaderType: 0=variable, 1=fixed */
        payload_len,                    /* PayloadLength */
        crc_type,                       /* CrcType: 0=off, 1=on */
        iq_inverted ? 0x40 : 0x00      /* InvertIQ: bit 6 */
    };

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 7);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set DIO IRQ params via the SetDioIrqParams command (opcode 0x08).
 * Routes the requested IRQs to the global IRQ register and to DIO1. */
int sx1262_set_dio_irq_params(struct sx1262_device *dev, uint16_t irq_mask)
{
    int ret;
    uint8_t irq_h = (irq_mask >> 8) & 0xFF;
    uint8_t irq_l = irq_mask & 0xFF;
    uint8_t tx[9] = {
        SX1262_CMD_SET_DIO_IRQ_PARAMS,
        irq_h, irq_l,   /* IrqMask  */
        irq_h, irq_l,   /* DIO1Mask */
        0x00, 0x00,     /* DIO2Mask */
        0x00, 0x00,     /* DIO3Mask */
    };

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 9);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Get RX buffer status: payload length and start pointer of last packet. */
int sx1262_get_rx_buffer_status(struct sx1262_device *dev, uint8_t *payload_len,
                                 uint8_t *start_ptr)
{
    uint8_t tx[1] = { 0x13 };  /* GetRxBufferStatus */
    uint8_t rx[3] = { 0 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write_then_read(dev, tx, 1, rx, 3);
    if (ret) return ret;
    /* rx[0]=status, rx[1]=PayloadLengthRx, rx[2]=RxStartBufferPointer */
    if (payload_len) *payload_len = rx[1];
    if (start_ptr)   *start_ptr = rx[2];
    return 0;
}

/* Clear IRQ status */
int sx1262_clear_irq_status(struct sx1262_device *dev, uint16_t irq_mask)
{
    uint8_t tx[3] = {
        SX1262_CMD_CLR_IRQ_STATUS,
        (irq_mask >> 8) & 0xFF,
        irq_mask & 0xFF
    };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 3);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Get IRQ status */
int sx1262_get_irq_status(struct sx1262_device *dev, uint16_t *irq_status)
{
    uint8_t buf[4] = { SX1262_CMD_GET_IRQ_STATUS, SX1262_CMD_NOP, SX1262_CMD_NOP, SX1262_CMD_NOP };
    uint8_t rx[4] = { 0 };
    struct spi_transfer xfer = { .tx_buf = buf, .rx_buf = rx, .len = 4 };
    int ret = sx1262_wait_busy(dev, 10);
    if (ret) return ret;
    ret = sx1262_spi_transfer(dev, &xfer, 1);
    if (ret) return ret;
    *irq_status = (rx[2] << 8) | rx[3];
    return 0;
}

/* Poll IRQ status until a given flag is set (with timeout), or timeout_ms elapses */
int sx1262_poll_irq(struct sx1262_device *dev, uint16_t flag, unsigned long timeout_ms)
{
    unsigned long deadline = jiffies + msecs_to_jiffies(timeout_ms);
    uint16_t irq;
    int ret;

    do {
        ret = sx1262_get_irq_status(dev, &irq);
        if (ret) return ret;
        if (irq & flag)
            return 0;  /* success — do NOT clear; caller reads/clears the IRQ */
        if (time_after(jiffies, deadline))
            return -ETIMEDOUT;
        usleep_range(1000, 2000);
    } while (1);
}

/* Get device errors */
int sx1262_get_dev_errors(struct sx1262_device *dev, uint16_t *errors)
{
    uint8_t buf[4] = { SX1262_CMD_GET_DEVICE_ERRORS, SX1262_CMD_NOP, SX1262_CMD_NOP, SX1262_CMD_NOP };
    uint8_t rx[4] = { 0 };
    struct spi_transfer xfer = { .tx_buf = buf, .rx_buf = rx, .len = 4 };
    int ret = sx1262_wait_busy(dev, 10);
    if (ret) return ret;
    ret = sx1262_spi_transfer(dev, &xfer, 1);
    if (ret) return ret;
    *errors = (rx[2] << 8) | rx[3];
    return 0;
}

/* Clear device errors */
int sx1262_clear_dev_errors(struct sx1262_device *dev)
{
    uint8_t tx[1] = { SX1262_CMD_CLEAR_DEV_ERRORS };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 1);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Get chip status */
int sx1262_get_status(struct sx1262_device *dev, uint8_t *status)
{
    uint8_t tx[1] = { SX1262_CMD_GET_STATUS };
    uint8_t rx[2] = { 0 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write_then_read(dev, tx, 1, rx, 2);
    if (ret) return ret;
    *status = rx[1];
    return 0;
}

/* Get RSSI (instantaneous) */
int sx1262_get_rssi_inst(struct sx1262_device *dev, int16_t *rssi)
{
    uint8_t tx[1] = { SX1262_CMD_GET_RSSI_INST };
    uint8_t rx[2] = { 0 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write_then_read(dev, tx, 1, rx, 2);
    if (ret) return ret;
    *rssi = -(int16_t)(rx[1] / 2);
    return 0;
}


/* Calibrate */
int sx1262_calibrate(struct sx1262_device *dev, uint8_t calib_params)
{
    uint8_t tx[2] = { SX1262_CMD_CALIBRATE, calib_params };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 2);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 30000); /* calibration can take up to 30ms */
}

/* Calibrate image for given frequency band (start/end) */
int sx1262_calibrate_image(struct sx1262_device *dev, uint8_t freq_band_start, uint8_t freq_band_end)
{
    uint8_t tx[4] = { SX1262_CMD_CALIBRATE_IMAGE, freq_band_start, freq_band_end, 0x00 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 4);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 30000);
}

/* Calibrate image picking the band that contains freq_hz (datasheet Table 9-2) */
int sx1262_calibrate_image_for_freq(struct sx1262_device *dev, uint32_t freq_hz)
{
    uint8_t start, end;

    if (freq_hz >= 902000000)       { start = 0xE1; end = 0xE9; } /* 902-928 */
    else if (freq_hz >= 863000000)  { start = 0xD7; end = 0xDB; } /* 863-870 */
    else if (freq_hz >= 779000000)  { start = 0xC1; end = 0xC5; } /* 779-787 */
    else if (freq_hz >= 470000000)  { start = 0x75; end = 0x81; } /* 470-510 */
    else                            { start = 0x6B; end = 0x6F; } /* 430-440 */

    return sx1262_calibrate_image(dev, start, end);
}

/* Set TX (timeout in ms, 0 = instant/continuous).
 * Returns when TX completes (TX_DONE IRQ flag seen via polling) or error. */
int sx1262_set_tx(struct sx1262_device *dev, uint32_t timeout_ms)
{
    int ret;
    uint32_t timeout_raw = 0;
    uint8_t tx[4] = { SX1262_CMD_SET_TX };

    if (timeout_ms > 0) {
        timeout_raw = timeout_ms * 64U;
    }

    tx[1] = (timeout_raw >> 16) & 0xFF;
    tx[2] = (timeout_raw >> 8) & 0xFF;
    tx[3] = timeout_raw & 0xFF;

    if (dev->ant_sw_mode == SX1262_ANTSW_AUTO)
        sx1262_set_antsw(dev, SX1262_ANTSW_TX);

    reinit_completion(&dev->dio1_completion);
    atomic_set(&dev->irq_flags, 0);
    dev->tx_in_progress = true;
    dev->rx_in_progress = false;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_spi_write(dev, tx, 4);
    if (ret) return ret;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    /* Poll for TX_DONE (with chip timeout + 100ms margin) */
    if (timeout_ms > 0) {
        ret = sx1262_poll_irq(dev, SX1262_IRQ_TX_DONE, timeout_ms + 100);
        if (ret == -ETIMEDOUT)
            dev_dbg(&dev->spi->dev, "TX poll timeout\n");
        /* poll_irq no longer clears; clear TX_DONE here */
        sx1262_clear_irq_status(dev, SX1262_IRQ_ALL);
    }

    dev->tx_in_progress = false;
    return 0;
}

/* Start a continuous unmodulated carrier (for spectrum-analyzer / bring-up).
 * Stays on until SetStandby/SetRx/SetTx. ANT_SW is forced to TX. */
int sx1262_set_tx_continuous_wave(struct sx1262_device *dev)
{
    uint8_t tx[1] = { SX1262_CMD_SET_TX_CONTINUOUS };
    int ret;

    if (dev->ant_sw_mode == SX1262_ANTSW_AUTO)
        sx1262_set_antsw(dev, SX1262_ANTSW_TX);

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write(dev, tx, 1);
    if (ret) return ret;
    return sx1262_wait_busy(dev, 100);
}

/* Set RX (timeout in ms, 0 = continuous) */
int sx1262_set_rx(struct sx1262_device *dev, uint32_t timeout_ms)
{
    int ret;
    uint32_t timeout_raw;
    uint8_t tx[4] = { SX1262_CMD_SET_RX };
    uint8_t status;
    uint16_t err_bits;

    /* ms == 0 -> continuous RX (0xFFFFFF). Otherwise timeout in 15.625us steps. */
    if (timeout_ms == 0)
        timeout_raw = 0xFFFFFF;
    else
        timeout_raw = timeout_ms * 64U;

    tx[1] = (timeout_raw >> 16) & 0xFF;
    tx[2] = (timeout_raw >> 8) & 0xFF;
    tx[3] = timeout_raw & 0xFF;

    /* Set ANT_SW to RX if in auto mode */
    if (dev->ant_sw_mode == SX1262_ANTSW_AUTO)
        sx1262_set_antsw(dev, SX1262_ANTSW_RX);

    reinit_completion(&dev->dio1_completion);
    atomic_set(&dev->irq_flags, 0);
    dev->tx_in_progress = false;
    dev->rx_in_progress = true;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;

    ret = sx1262_get_status(dev, &status);
    dev_info(&dev->spi->dev, "set_rx: pre status=0x%02x mode=%d\n", status, (status >> 4) & 0x07);

    /* Check and clear device errors before SetRx */
    sx1262_get_dev_errors(dev, &err_bits);
    dev_info(&dev->spi->dev, "set_rx: dev_errors_pre=0x%04x\n", err_bits);
    sx1262_clear_dev_errors(dev);

    dev_info(&dev->spi->dev, "set_rx: cmd=%02x %02x %02x %02x\n",
             tx[0], tx[1], tx[2], tx[3]);
    ret = sx1262_spi_write(dev, tx, 4);
    if (ret) return ret;

    ret = sx1262_wait_busy(dev, 100);
    if (ret) {
        dev_err(&dev->spi->dev, "set_rx: BUSY timeout\n");
        return ret;
    }

    msleep(110);
    ret = sx1262_get_status(dev, &status);
    dev_info(&dev->spi->dev, "set_rx: after 110ms status=0x%02x mode=%d\n", status, (status >> 4) & 0x07);

    sx1262_get_dev_errors(dev, &err_bits);
    dev_info(&dev->spi->dev, "set_rx: dev_errors_post=0x%04x\n", err_bits);

    msleep(50);
    ret = sx1262_get_status(dev, &status);
    dev_info(&dev->spi->dev, "set_rx: after 50ms status=0x%02x mode=%d\n", status, (status >> 4) & 0x07);

    sx1262_clear_irq_status(dev, 0xFFFF);
    return 0;
}

/* Initialize device: full startup sequence matching RadioLib conventions */
int sx1262_init(struct sx1262_device *dev, uint32_t freq_hz)
{
    int ret;
    uint16_t errs;

    /* --- Step 1: Standby RC --- */
    ret = sx1262_set_standby(dev, SX1262_STANDBY_RC);
    if (ret) { dev_err(&dev->spi->dev, "Standby failed\n"); return ret; }

    /* --- Step 1a: Regulator mode DC-DC + LDO (RadioLib default for SX1262) ---
     * NOTE: these modules use a plain 32 MHz crystal (XOSC starts cleanly with
     * dev_errors=0). DIO3-as-TCXO control makes XOSC fail (XOSC_START_ERR 0x20),
     * so it is intentionally NOT configured. */
    ret = sx1262_set_regulator_mode(dev, 0x01);
    if (ret) { dev_err(&dev->spi->dev, "Regulator mode failed\n"); return ret; }

    /* NOTE: on this board the RF switch is NOT driven by the chip's DIO2; the
     * schematic's DIO2 net is driven by an RV1106 GPIO (named ant-sw1 here).
     * So we do NOT call SetDio2AsRfSwitchCtrl — the switch is toggled via the
     * ant-sw GPIOs in sx1262_set_antsw(). */

    /* --- Step 2: Set packet type (LoRa) --- */
    ret = sx1262_set_packet_type(dev, SX1262_PKT_TYPE_LORA);
    if (ret) { dev_err(&dev->spi->dev, "Set packet type failed\n"); return ret; }

    /* --- Step 3: Set fallback mode to STDBY_RC --- */
    ret = sx1262_set_rx_tx_fallback_mode(dev, SX1262_FALLBACK_RC);
    if (ret) { dev_err(&dev->spi->dev, "Fallback mode failed\n"); return ret; }

    /* --- Step 4: Calibrate all blocks --- */
    ret = sx1262_calibrate(dev, 0x7F);
    if (ret) { dev_err(&dev->spi->dev, "Calibrate failed\n"); return ret; }

    /* --- Step 5: Calibrate image for the operating band --- */
    ret = sx1262_calibrate_image_for_freq(dev, freq_hz);
    if (ret) { dev_err(&dev->spi->dev, "Calibrate image failed\n"); return ret; }

    /* --- Step 6: Set PA config for SX1262 (high-power PA) --- */
    ret = sx1262_set_pa_config(dev);
    if (ret) { dev_err(&dev->spi->dev, "PA config failed\n"); return ret; }

    /* Errata 15.2: better Tx resistance to antenna mismatch.
     * Reg 0x08D8 (TxClampConfig) |= 0x1E (set bits 1-4). */
    {
        uint8_t reg = 0, after = 0;
        if (sx1262_read_register(dev, 0x08D8, &reg) == 0) {
            dev_info(&dev->spi->dev, "CLAMP: read 0x08D8=0x%02x\n", reg);
            sx1262_write_register(dev, 0x08D8, reg | 0x1E);
            sx1262_read_register(dev, 0x08D8, &after);
            dev_info(&dev->spi->dev, "CLAMP: after write=0x%02x (expected 0x%02x)\n",
                     after, reg | 0x1E);
        }
    }

    /* --- Step 7: Set frequency --- */
    ret = sx1262_set_frequency(dev, freq_hz);
    if (ret) { dev_err(&dev->spi->dev, "Set frequency failed\n"); return ret; }

    /* --- Step 8: Set TX params (power dBm, ramp 0x00-0x07; 0x04 = 200us) --- */
    ret = sx1262_set_tx_params(dev, 14, 0x04);
    if (ret) { dev_err(&dev->spi->dev, "TX params failed\n"); return ret; }

    /* --- Step 9: Set modulation params (LoRa SF7 BW125 CR4/5) --- */
    ret = sx1262_set_modulation_params(dev, 7, 125000, 1, false);
    if (ret) { dev_err(&dev->spi->dev, "Modulation params failed\n"); return ret; }

    /* --- Step 10: Set buffer base addresses --- */
    ret = sx1262_set_buffer_base_address(dev, 0x00, 0x00);
    if (ret) { dev_err(&dev->spi->dev, "Buffer base addr failed\n"); return ret; }

    /* --- Step 11: Set packet params (preamble=8, variable, crc on) --- */
    ret = sx1262_set_packet_params(dev, 8, 0, 0xFF, 1, 0);
    if (ret) { dev_err(&dev->spi->dev, "Packet params failed\n"); return ret; }

    /* --- Step 12: Clear IRQ status and route TxDone/RxDone/Timeout to DIO1 --- */
    sx1262_clear_irq_status(dev, 0xFFFF);
    ret = sx1262_set_dio_irq_params(dev, SX1262_IRQ_TX_DONE | SX1262_IRQ_RX_DONE | SX1262_IRQ_TIMEOUT);
    if (ret) { dev_err(&dev->spi->dev, "DIO IRQ failed\n"); return ret; }

    /* Verify IRQ registers were written correctly */
    sx1262_read_register(dev, 0x01D4, (uint8_t *)&errs);
    dev_info(&dev->spi->dev, "IRQ: 01D4=0x%02x (expect 0x00)\n", (uint8_t)(errs & 0xFF));
    {
        uint8_t v;
        sx1262_read_register(dev, 0x01D5, &v);
        dev_info(&dev->spi->dev, "IRQ: 01D5=0x%02x (expect 0x83)\n", v);
        sx1262_read_register(dev, 0x01D7, &v);
        dev_info(&dev->spi->dev, "IRQ: 01D7=0x%02x (expect 0x83)\n", v);
    }

    /* --- Step 14: Clear any pending device errors from calibration --- */
    sx1262_get_dev_errors(dev, &errs);
    dev_info(&dev->spi->dev, "INIT: dev_errors=0x%04x\n", errs);
    sx1262_clear_dev_errors(dev);

    dev_info(&dev->spi->dev, "INIT: done\n");
    return 0;
}

/* Get packet status (RSSI, SNR, etc.) */
int sx1262_get_packet_status(struct sx1262_device *dev, int16_t *rssi_pkt, int8_t *snr)
{
    uint8_t tx[1] = { SX1262_CMD_GET_PKT_STATUS };
    uint8_t rx[4] = { 0 };
    int ret = sx1262_wait_busy(dev, 100);
    if (ret) return ret;
    ret = sx1262_spi_write_then_read(dev, tx, 1, rx, 4);
    if (ret) return ret;
    *rssi_pkt = -(int16_t)(rx[2] / 2);
    *snr = (int8_t)rx[3] / 4;
    return 0;
}
