/*
 * ccsds_xtea — XTEA block cipher (see ccsds_xtea.h for the CTF weakness note).
 */

#include "ccsds_xtea.h"

/*
 * INTENTIONAL WEAKNESS (CTF): a single hard-coded 128-bit key for the whole
 * mission. The bytes spell ASCII ("PwnCubeSatLoRaKy"), so it stands out
 * verbatim in a flash dump obtained through the FLASH telecommand (APID 0x07).
 * Real flight software derives per-session keys and never ships them in
 * .rodata; here it is fixed on purpose so the "encrypted" uplink can be broken.
 */
const uint32_t CCSDS_TC_KEY[CCSDS_XTEA_KEYWORDS] = {
    0x50776E43U,  /* "PwnC" */
    0x75626553U,  /* "ubeS" */
    0x61744C6FU,  /* "atLo" */
    0x52614B79U   /* "RaKy" */
};

#define XTEA_DELTA  0x9E3779B9U

void ccsds_xtea_encrypt_block(uint32_t v[2], const uint32_t key[4])
{
    uint32_t v0 = v[0], v1 = v[1], sum = 0U;
    for (unsigned i = 0; i < CCSDS_XTEA_ROUNDS; i++) {
        v0  += (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3U]);
        sum += XTEA_DELTA;
        v1  += (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3U]);
    }
    v[0] = v0;
    v[1] = v1;
}

void ccsds_xtea_decrypt_block(uint32_t v[2], const uint32_t key[4])
{
    uint32_t v0 = v[0], v1 = v[1];
    uint32_t sum = XTEA_DELTA * CCSDS_XTEA_ROUNDS;
    for (unsigned i = 0; i < CCSDS_XTEA_ROUNDS; i++) {
        v1  -= (((v0 << 4) ^ (v0 >> 5)) + v0) ^ (sum + key[(sum >> 11) & 3U]);
        sum -= XTEA_DELTA;
        v0  -= (((v1 << 4) ^ (v1 >> 5)) + v1) ^ (sum + key[sum & 3U]);
    }
    v[0] = v0;
    v[1] = v1;
}

/* Big-endian word (de)serialisation so the wire format is CPU-independent. */
static uint32_t be32_load(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8)  |  (uint32_t)p[3];
}

static void be32_store(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

void ccsds_xtea_encrypt(uint8_t *buf, size_t len, const uint32_t key[4])
{
    for (size_t off = 0; off + CCSDS_XTEA_BLOCK <= len; off += CCSDS_XTEA_BLOCK) {
        uint32_t v[2] = { be32_load(buf + off), be32_load(buf + off + 4) };
        ccsds_xtea_encrypt_block(v, key);
        be32_store(buf + off,     v[0]);
        be32_store(buf + off + 4, v[1]);
    }
}

void ccsds_xtea_decrypt(uint8_t *buf, size_t len, const uint32_t key[4])
{
    for (size_t off = 0; off + CCSDS_XTEA_BLOCK <= len; off += CCSDS_XTEA_BLOCK) {
        uint32_t v[2] = { be32_load(buf + off), be32_load(buf + off + 4) };
        ccsds_xtea_decrypt_block(v, key);
        be32_store(buf + off,     v[0]);
        be32_store(buf + off + 4, v[1]);
    }
}
