# CCSDS SPP + secured TC library

A small, self-contained CCSDS library for the CubeSat firmware. It provides the
Space Packet Protocol (SPP) primary header codec and a **secured telecommand
(TC)** layer modelled on the Zephyr FlatSat — secondary header + XTEA encryption
+ CRC-16 — layered on top of the SPP frame.

> This is a **CTF exercise**. The library is a complete, correct implementation,
> but it is wired into the mission with **intentional weaknesses** so the secured
> link can be broken. See "Intentional weaknesses" below. Do not reuse as-is for
> a real flight system.

## Files

| File | Role |
|------|------|
| `../spp.h` / `../spp.c` | SPP primary header (6 B, big-endian), TM/TC/idle builders, `spp_unpack_packet` (parse). Carries the deliberate over-read (vuln #9). |
| `ccsds_crc.h` / `ccsds_crc.c` | CRC-16-CCITT (poly 0x1021, init 0xFFFF). |
| `ccsds_xtea.h` / `ccsds_xtea.c` | XTEA block cipher (64-bit block, 128-bit key, 32 rounds), ECB over a byte buffer. Holds the fixed mission key. |
| `ccsds_tc.h` / `ccsds_tc.c` | Secured TC: build (`ccsds_tc_build`) and in-place decrypt/verify (`ccsds_tc_unsecure`). |
| `ccsds_133_space_packet.*`, `ccsds_types.h` | Earlier standalone CCSDS model (unused by the mission; kept as reference). |

## Secured TC wire format

```
+------------------+------------------------+----------------------+--------+
| SPP primary (6)  | TC secondary header(4) | XTEA(user data)      | CRC(2) |
+------------------+------------------------+----------------------+--------+
   type=TC                cmd_counter (BE16)   plaintext zero-padded  CRC-16
   sec_hdr_flag=1         function_code (1)    to 8-byte blocks,      CCITT
   APID (11)              key_id (1)           XTEA-ECB encrypted     over
                                                                      hdr+sechdr
                                                                      +cipher
```

- The primary header's `packet_data_length` = `(4 + ciphertext + 2) − 1`.
- CRC-16-CCITT covers **primary header + secondary header + ciphertext** (the
  2-byte CRC trailer itself is excluded).
- The user data is padded with zeros up to a multiple of 8 before encryption.

## API

```c
/* Build a secured TC from a plaintext payload. */
int ccsds_tc_build(space_packet_t *pkt, packet_counter_t *cnt, uint16_t apid,
                   uint8_t function_code, uint8_t key_id,
                   const uint8_t *payload, uint16_t payload_len,
                   uint16_t *out_total);

/* Decrypt + verify a received TC in place. If sec_hdr_flag=1: reads the
 * secondary header (*sh), reports CRC validity (*crc_ok, NOT enforced),
 * XTEA-decrypts, and rewrites pkt->data to hold only the plaintext args.
 * If sec_hdr_flag=0: returns CCSDS_TC_ERR_NOSEC and leaves pkt untouched. */
int ccsds_tc_unsecure(space_packet_t *pkt, ccsds_tc_sec_header_t *sh, int *crc_ok);
```

`ccsds_tc_unsecure` rewrites `pkt->data` so a downstream APID handler reads the
decrypted command arguments exactly as it would for an unsecured TC — the
security layer is transparent to the dispatch code.

### Mission integration

`command_service.c :: process_rx_packet` calls `ccsds_tc_unsecure` right after
`spp_unpack_packet`, then dispatches by APID. The return value and `crc_ok` are
**ignored** on purpose (see below).

## Intentional weaknesses (CTF)

The cipher, CRC and secondary header are real and correctly implemented. The
exploitable weaknesses are in how they are keyed and enforced:

1. **Fixed key, in the clear.** `CCSDS_TC_KEY` (`ccsds_xtea.c`) is one 128-bit
   key compiled into the firmware. Its bytes are ASCII (`"PwnCubeSatLoRaKy"`), so
   it appears verbatim in a flash image obtained through the FLASH telecommand
   (APID 0x07). Recover the key → decrypt/forge any secured TC.
2. **ECB mode, no nonce.** Equal plaintext blocks encrypt to equal ciphertext, so
   the "encrypted" link leaks structure.
3. **CRC is a check, not a MAC.** An attacker who tampers with a frame recomputes
   the CRC-16 trivially — it provides integrity against noise, not authentication.
4. **Predictable counter, no replay check.** `command_counter` is monotonic and
   is never validated, so captured frames can be replayed.
5. **The receiver does not require the layer.** `process_rx_packet` accepts a
   plaintext TC (`sec_hdr_flag=0`) and never rejects on CRC mismatch, so every
   pre-existing over-the-air exploit still works. This is the generalisation of
   the no-authentication vulnerability.

Preserved from the base implementation: **#9** SPP unpack over-read
(`spp.c`), **#10** broadcast length underflow (`command_service.c`), **#6**
uninitialised secondary header on the FLASH TM path.

## Testing

`test/tc_test.c` is a host round-trip test: build a secured TC, unsecure it,
check the plaintext round-trips, the CRC validates, a tampered frame is flagged,
and a plaintext TC passes through untouched. It lives in `test/` so the mission
build (which globs `ccsds/*.c`, non-recursive) never pulls its `main()` in. Run
from this directory:

```sh
gcc -Wall -Wextra -I. -I.. -o /tmp/tc_test \
    test/tc_test.c ../spp.c ccsds_crc.c ccsds_xtea.c ccsds_tc.c && /tmp/tc_test
```

## Building a different secured command (example)

```c
packet_counter_t cnt; spp_counters_init(&cnt);
uint8_t args[] = { 0x00, 0xC8 };            /* thruster 0, power 200 */
space_packet_t pkt; uint16_t total;
ccsds_tc_build(&pkt, &cnt, SPP_APID_TC_SET_THRUSTER, 0x00, 0x01,
               args, sizeof(args), &total);
/* transmit the first `total` bytes of `pkt` on the uplink */
```
