/*
 * ccsds_tc — Telecommand security layer (CCSDS-style secured TC).
 *
 * This is a "complete" telecommand frame in the style of FlatSat (ElectronicCats),
 * layered on top of the SPP primary header (spp.h). NOTE: it is a *variant*, not
 * a byte-exact clone of that firmware — see docs/flatsat-protocol-comparison.md
 * (this uses XTEA + a counter/func/key secondary header; ElectronicCats uses
 * AES-128-CTR/XOR + a timestamp secondary header):
 *
 *   +------------------+------------------------+---------------------+--------+
 *   | SPP primary (6)  | TC secondary header(4) | XTEA(user data)     | CRC(2) |
 *   +------------------+------------------------+---------------------+--------+
 *      type=TC, sec_hdr_flag=1        cmd_counter(2)   padded to 8-byte   CRC-16
 *                                     func_code(1)     XTEA-ECB blocks    CCITT
 *                                     key_id(1)
 *
 * The primary header's packet_data_length counts (sec-hdr + encrypted + CRC).
 * CRC-16-CCITT covers the primary header + secondary header + ciphertext.
 *
 * INTENTIONAL WEAKNESSES (CTF) — the frame is well-formed, the weaknesses are
 * by design and mirror the FlatSat exercise:
 *   - Fixed key (ccsds_xtea.c), ECB mode: confidentiality is breakable.
 *   - CRC-16 is an integrity CHECK, not a MAC: an attacker who tampers with the
 *     frame simply recomputes it, so it provides no authentication.
 *   - command_counter is monotonic and predictable, and is NOT checked against
 *     replay by the mission.
 *   - The receiver (command_service.c) does NOT require this layer: a plaintext
 *     TC with sec_hdr_flag=0 is still accepted and dispatched, so the secured
 *     path is optional — the generalisation of the "no authentication" vuln.
 */

#ifndef CCSDS_TC_H
#define CCSDS_TC_H

#include <stdint.h>
#include "spp.h"

#ifdef __cplusplus
extern "C" {
#endif

#define CCSDS_TC_SECHDR_LEN   4U   /* cmd_counter(2) + func_code(1) + key_id(1) */
#define CCSDS_TC_CRC_LEN      2U   /* CRC-16-CCITT trailer                       */

/* Return codes (in addition to the SPP_ERROR_* negatives from spp.h). */
#define CCSDS_TC_OK           0    /* secured TC parsed/decrypted                */
#define CCSDS_TC_ERR_NOSEC    1    /* plaintext TC (no secondary header)         */

typedef struct {
    uint16_t command_counter;  /* monotonic, predictable (no replay protection) */
    uint8_t  function_code;    /* command class / auth tag (unenforced)         */
    uint8_t  key_id;           /* selects the XTEA key (only one exists)        */
} ccsds_tc_sec_header_t;

/*
 * Build a secured TC into `pkt` from a plaintext `payload`. Fills the SPP
 * primary header (type=TC, sec_hdr_flag=1), writes the secondary header,
 * XTEA-encrypts the zero-padded payload, and appends the CRC-16. The tc counter
 * in `cnt` is advanced (and mirrored into the secondary header's
 * command_counter). The full on-wire length is returned via *out_total.
 * Returns SPP_ERROR_NONE, or a negative SPP_ERROR_* on bad args / overflow.
 */
int ccsds_tc_build(space_packet_t *pkt, packet_counter_t *cnt, uint16_t apid,
                   uint8_t function_code, uint8_t key_id,
                   const uint8_t *payload, uint16_t payload_len,
                   uint16_t *out_total);

/*
 * In-place "unsecure" of a received, already-SPP-unpacked TC.
 *
 * If the packet has sec_hdr_flag=1: reads the secondary header into *sh (if
 * non-NULL), verifies the CRC and reports the result via *crc_ok (if non-NULL;
 * the mission does NOT reject on mismatch — weak by design), XTEA-decrypts the
 * user data, and rewrites pkt->data so it holds ONLY the decrypted command
 * args (and fixes pkt->header.length). The caller's APID handler then reads the
 * plaintext args exactly as for an unsecured TC. Returns CCSDS_TC_OK.
 *
 * If sec_hdr_flag=0: leaves `pkt` untouched and returns CCSDS_TC_ERR_NOSEC.
 *
 * On a malformed secured frame (too short, non-block-aligned) returns a
 * negative SPP_ERROR_*.
 */
int ccsds_tc_unsecure(space_packet_t *pkt, ccsds_tc_sec_header_t *sh,
                      int *crc_ok);

#ifdef __cplusplus
}
#endif

#endif /* CCSDS_TC_H */
