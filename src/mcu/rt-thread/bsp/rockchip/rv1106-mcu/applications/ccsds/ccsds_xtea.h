/*
 * ccsds_xtea — XTEA block cipher for the TC security layer.
 *
 * XTEA (eXtended TEA): 64-bit block, 128-bit key, 32 Feistel rounds. Chosen
 * because it is tiny enough for the RISC-V MCU yet a real, published cipher —
 * close in spirit to the profile FlatSat (ElectronicCats) uses for its
 * "encrypted" telecommand link (that firmware uses AES-128-CTR/XOR — see
 * docs/flatsat-protocol-comparison.md; XTEA is a lighter, self-contained variant).
 *
 * INTENTIONAL WEAKNESS (CTF): the mission key CCSDS_TC_KEY is FIXED and compiled
 * into the firmware (see ccsds_xtea.c). It is recoverable from a flash image
 * dump (TC APID 0x07 leaks .rodata), and the cipher runs in ECB mode with no
 * per-packet nonce — identical plaintext blocks encrypt identically, so the
 * link is confidential in name only. The library is complete and correct as a
 * cipher; the weakness is deliberately in the key management and mode, matching
 * the "crypto present but breakable" theme of the exercise.
 */

#ifndef CCSDS_XTEA_H
#define CCSDS_XTEA_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CCSDS_XTEA_ROUNDS   32U
#define CCSDS_XTEA_BLOCK     8U   /* bytes per block (two 32-bit words)        */
#define CCSDS_XTEA_KEYWORDS  4U   /* 128-bit key = four 32-bit words           */

/* The fixed mission key. Defined in ccsds_xtea.c. Deliberately extractable. */
extern const uint32_t CCSDS_TC_KEY[CCSDS_XTEA_KEYWORDS];

/* Single-block primitives (v = two 32-bit words, host order). */
void ccsds_xtea_encrypt_block(uint32_t v[2], const uint32_t key[4]);
void ccsds_xtea_decrypt_block(uint32_t v[2], const uint32_t key[4]);

/* ECB over a byte buffer. `len` MUST be a multiple of CCSDS_XTEA_BLOCK; the
 * caller zero-pads. Words are serialised big-endian on the wire so the result
 * is independent of the CPU's byte order. Any trailing bytes (len % 8) are left
 * untouched. */
void ccsds_xtea_encrypt(uint8_t *buf, size_t len, const uint32_t key[4]);
void ccsds_xtea_decrypt(uint8_t *buf, size_t len, const uint32_t key[4]);

#ifdef __cplusplus
}
#endif

#endif /* CCSDS_XTEA_H */
