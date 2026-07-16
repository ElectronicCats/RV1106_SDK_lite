/*
 * ccsds_tc — Telecommand security layer implementation (see ccsds_tc.h).
 *
 * Built on the live SPP model (space_packet_t): a space_packet_t is a packed
 * 6-byte header immediately followed by data[], so (uint8_t*)pkt is the raw
 * on-wire frame and CRC/cipher can operate on it directly with no copies.
 */

#include "ccsds_tc.h"
#include "ccsds_crc.h"
#include "ccsds_xtea.h"
#include <string.h>

/* Round a length up to the XTEA block size. */
static uint16_t pad_to_block(uint16_t n)
{
    return (uint16_t)((n + (CCSDS_XTEA_BLOCK - 1U)) & ~(CCSDS_XTEA_BLOCK - 1U));
}

int ccsds_tc_build(space_packet_t *pkt, packet_counter_t *cnt, uint16_t apid,
                   uint8_t function_code, uint8_t key_id,
                   const uint8_t *payload, uint16_t payload_len,
                   uint16_t *out_total)
{
    if (pkt == NULL)
        return SPP_ERROR_INVALID_BUFFER;

    uint16_t enc_len    = pad_to_block(payload_len);          /* >= payload_len */
    uint16_t data_field = (uint16_t)(CCSDS_TC_SECHDR_LEN + enc_len + CCSDS_TC_CRC_LEN);
    if (data_field > SPP_MAX_PAYLOAD_CHUNK)
        return SPP_ERROR_PAYLOAD_LEN;

    /* Advance the (predictable) TC counter, same wrap rule as spp.c. */
    uint16_t counter = 0;
    if (cnt != NULL) {
        if (cnt->tc == 16383)
            cnt->tc = 0;
        cnt->tc++;
        counter = cnt->tc & 0x3FFFU;
    }

    /* Primary header: version 0, type TC, secondary-header flag set, APID. */
    uint16_t packet_id = 0;
    packet_id |= (CCSDS_SPP_VERSION & 0x07U) << 13;
    packet_id |= (SPP_PTYPE_TC & 0x01U) << 12;
    packet_id |= (SPP_SECHEAD_FLAG_PRESENT & 0x01U) << 11;
    packet_id |= (apid & 0x07FFU);

    uint16_t seq_ctrl = 0;
    seq_ctrl |= (SPP_GROUP_FLAG_UNSEGMENTED & 0x03U) << 14;
    seq_ctrl |= (counter & 0x3FFFU);

    pkt->header.identification = spp_host_to_be16(packet_id);
    pkt->header.sequence       = spp_host_to_be16(seq_ctrl);
    pkt->header.length         = spp_host_to_be16((uint16_t)(data_field - 1U));

    /* Secondary header. */
    pkt->data[0] = (uint8_t)(counter >> 8);
    pkt->data[1] = (uint8_t)(counter);
    pkt->data[2] = function_code;
    pkt->data[3] = key_id;

    /* User data: zero-pad then encrypt just the user-data region. */
    memset(pkt->data + CCSDS_TC_SECHDR_LEN, 0, enc_len);
    if (payload != NULL && payload_len > 0)
        memcpy(pkt->data + CCSDS_TC_SECHDR_LEN, payload, payload_len);
    ccsds_xtea_encrypt(pkt->data + CCSDS_TC_SECHDR_LEN, enc_len, CCSDS_TC_KEY);

    /* CRC-16-CCITT over primary header + secondary header + ciphertext, then
     * append it. (uint8_t*)pkt is contiguous [header(6)][data...]. */
    uint16_t crc_region = (uint16_t)(SPP_PRIMARY_HEADER_LEN + CCSDS_TC_SECHDR_LEN + enc_len);
    uint16_t crc = ccsds_crc16_ccitt((const uint8_t *)pkt, crc_region);
    pkt->data[CCSDS_TC_SECHDR_LEN + enc_len]      = (uint8_t)(crc >> 8);
    pkt->data[CCSDS_TC_SECHDR_LEN + enc_len + 1U] = (uint8_t)(crc);

    if (out_total != NULL)
        *out_total = (uint16_t)(SPP_PRIMARY_HEADER_LEN + data_field);
    return SPP_ERROR_NONE;
}

int ccsds_tc_unsecure(space_packet_t *pkt, ccsds_tc_sec_header_t *sh,
                      int *crc_ok)
{
    if (pkt == NULL)
        return SPP_ERROR_INVALID_BUFFER;

    uint16_t id       = spp_be16_to_host(pkt->header.identification);
    uint8_t  sec_flag = (uint8_t)((id >> 11) & 0x01U);
    if (!sec_flag)
        return CCSDS_TC_ERR_NOSEC;     /* plaintext TC — leave pkt untouched */

    uint16_t data_field = (uint16_t)(spp_be16_to_host(pkt->header.length) + 1U);
    if (data_field < (CCSDS_TC_SECHDR_LEN + CCSDS_TC_CRC_LEN))
        return SPP_ERROR_PAYLOAD_LEN;  /* too short to hold sec-hdr + CRC */

    uint16_t enc_len = (uint16_t)(data_field - CCSDS_TC_SECHDR_LEN - CCSDS_TC_CRC_LEN);
    if (enc_len & (CCSDS_XTEA_BLOCK - 1U))
        return SPP_ERROR_PAYLOAD_LEN;  /* ciphertext must be block-aligned */

    if (sh != NULL) {
        sh->command_counter = (uint16_t)(((uint16_t)pkt->data[0] << 8) | pkt->data[1]);
        sh->function_code   = pkt->data[2];
        sh->key_id          = pkt->data[3];
    }

    /* Verify CRC (computed, but the mission does NOT reject on mismatch). */
    uint16_t crc_region = (uint16_t)(SPP_PRIMARY_HEADER_LEN + CCSDS_TC_SECHDR_LEN + enc_len);
    uint16_t want = ccsds_crc16_ccitt((const uint8_t *)pkt, crc_region);
    uint16_t got  = (uint16_t)(((uint16_t)pkt->data[CCSDS_TC_SECHDR_LEN + enc_len] << 8) |
                                pkt->data[CCSDS_TC_SECHDR_LEN + enc_len + 1U]);
    if (crc_ok != NULL)
        *crc_ok = (want == got) ? 1 : 0;

    /* Decrypt the user-data region in place. */
    ccsds_xtea_decrypt(pkt->data + CCSDS_TC_SECHDR_LEN, enc_len, CCSDS_TC_KEY);

    /* Collapse the frame so pkt->data holds only the plaintext args, and fix
     * the length field, so the APID handler reads it like an unsecured TC. */
    memmove(pkt->data, pkt->data + CCSDS_TC_SECHDR_LEN, enc_len);
    pkt->header.length = spp_host_to_be16(enc_len ? (uint16_t)(enc_len - 1U) : 0U);
    return CCSDS_TC_OK;
}
