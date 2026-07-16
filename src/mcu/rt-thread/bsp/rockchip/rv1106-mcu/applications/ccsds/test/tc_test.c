/* Host round-trip test for the CCSDS TC security library. */
#include <stdio.h>
#include <string.h>
#include "spp.h"
#include "ccsds_tc.h"
#include "ccsds_xtea.h"

static int fails = 0;
#define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); fails++; } \
                              else printf("ok:   %s\n", msg); } while (0)

int main(void)
{
    packet_counter_t cnt; spp_counters_init(&cnt);

    /* 1) Build a secured TC from a known plaintext, then unsecure it. */
    const uint8_t payload[] = { 0x01, 0xC8, 0x03, 0x94 };  /* e.g. thruster args */
    space_packet_t pkt; uint16_t total = 0;
    int r = ccsds_tc_build(&pkt, &cnt, 0x04 /*APID*/, 0x00 /*func*/, 0x01 /*key*/,
                           payload, sizeof(payload), &total);
    CHECK(r == SPP_ERROR_NONE, "ccsds_tc_build returns OK");
    /* total = 6 primary + 4 sechdr + 8 (4 padded to block) + 2 crc = 20 */
    CHECK(total == 20, "on-wire length is 20 bytes");

    /* sec_hdr_flag must be set in the primary header */
    uint16_t id = spp_be16_to_host(pkt.header.identification);
    CHECK(((id >> 11) & 1) == 1, "sec_hdr_flag set in built TC");
    CHECK(((id >> 12) & 1) == SPP_PTYPE_TC, "packet type is TC");

    /* ciphertext must differ from plaintext (encryption actually happened) */
    CHECK(memcmp(pkt.data + CCSDS_TC_SECHDR_LEN, payload, sizeof(payload)) != 0,
          "user data is encrypted (differs from plaintext)");

    /* 2) Unsecure a copy → recover plaintext, CRC valid */
    space_packet_t rx = pkt;
    ccsds_tc_sec_header_t sh; int crc_ok = -1;
    r = ccsds_tc_unsecure(&rx, &sh, &crc_ok);
    CHECK(r == CCSDS_TC_OK, "ccsds_tc_unsecure returns OK");
    CHECK(crc_ok == 1, "CRC verifies on an untampered frame");
    CHECK(sh.command_counter == cnt.tc, "secondary-header counter matches");
    CHECK(memcmp(rx.data, payload, sizeof(payload)) == 0,
          "decrypted args match original plaintext");

    /* 3) Tamper one ciphertext byte → CRC must fail (but still decrypts) */
    space_packet_t bad = pkt;
    bad.data[CCSDS_TC_SECHDR_LEN] ^= 0xFF;
    int crc_ok2 = -1;
    r = ccsds_tc_unsecure(&bad, NULL, &crc_ok2);
    CHECK(r == CCSDS_TC_OK, "unsecure still parses a tampered frame");
    CHECK(crc_ok2 == 0, "CRC mismatch flagged on tampered frame");

    /* 4) A plaintext TC (no secondary header) is left untouched */
    space_packet_t plain;
    uint8_t plain_args[] = { 0x00, 0x2A };
    spp_tc_build_packet(&plain, &cnt, SPP_GROUP_FLAG_UNSEGMENTED,
                        SPP_SECHEAD_FLAG_NOPRESENT, 0, 0x04, plain_args, 2);
    space_packet_t plain_copy = plain;
    r = ccsds_tc_unsecure(&plain, NULL, NULL);
    CHECK(r == CCSDS_TC_ERR_NOSEC, "plaintext TC reported as NOSEC");
    CHECK(memcmp(&plain, &plain_copy, sizeof(plain)) == 0,
          "plaintext TC left untouched (exploits still work)");

    printf(fails ? "\n%d CHECK(S) FAILED\n" : "\nALL CHECKS PASSED\n", fails);
    return fails ? 1 : 0;
}
