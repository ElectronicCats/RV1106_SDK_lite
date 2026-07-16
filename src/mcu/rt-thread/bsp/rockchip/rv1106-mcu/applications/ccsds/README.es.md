# Librería CCSDS SPP + TC seguro

Librería CCSDS pequeña y autocontenida para el firmware del CubeSat. Provee el
codec del encabezado primario del Space Packet Protocol (SPP) y una capa de
**telecomando (TC) seguro** al estilo de **FlatSat (ElectronicCats)** —
secondary header + cifrado + CRC-16 — sobre el frame SPP.

> Es una **variante**, no un clon byte-a-byte de ese firmware: usa XTEA y un
> secondary header `counter/func/key`, mientras que FlatSat (ElectronicCats) usa
> AES-128-CTR/XOR y un secondary header `timestamp`. Para la comparación completa
> de los tres protocolos (ElectronicCats vs PWNSat vs esta librería) ver
> `docs/flatsat-protocol-comparison.md` (en la raíz del repo).

> Esto es un **ejercicio de CTF**. La librería es una implementación completa y
> correcta, pero se conecta a la misión con **debilidades intencionales** para
> que el enlace "seguro" se pueda romper. Ver "Debilidades intencionales" abajo.
> No reutilizar tal cual en un sistema de vuelo real.

## Archivos

| Archivo | Rol |
|------|------|
| `../spp.h` / `../spp.c` | Encabezado primario SPP (6 B, big-endian), builders TM/TC/idle, `spp_unpack_packet` (parse). Lleva el over-read deliberado (vuln #9). |
| `ccsds_crc.h` / `ccsds_crc.c` | CRC-16-CCITT (poly 0x1021, init 0xFFFF). |
| `ccsds_xtea.h` / `ccsds_xtea.c` | Cifrador de bloque XTEA (bloque 64-bit, clave 128-bit, 32 rondas), ECB sobre un buffer. Contiene la clave fija de la misión. |
| `ccsds_tc.h` / `ccsds_tc.c` | TC seguro: construir (`ccsds_tc_build`) y descifrar/verificar in-place (`ccsds_tc_unsecure`). |
| `ccsds_133_space_packet.*`, `ccsds_types.h` | Modelo CCSDS previo standalone (sin usar por la misión; referencia). |

## Formato de wire del TC seguro

```
+------------------+------------------------+----------------------+--------+
| SPP primary (6)  | TC secondary header(4) | XTEA(user data)      | CRC(2) |
+------------------+------------------------+----------------------+--------+
   type=TC                cmd_counter (BE16)   plaintext con zero-pad  CRC-16
   sec_hdr_flag=1         function_code (1)    a bloques de 8 B,       CCITT
   APID (11)              key_id (1)           cifrado XTEA-ECB        sobre
                                                                       hdr+sechdr
                                                                       +cifrado
```

- El `packet_data_length` del primario = `(4 + cifrado + 2) − 1`.
- El CRC-16-CCITT cubre **primario + secondary header + cifrado** (el propio
  trailer de 2 B del CRC se excluye).
- Los datos de usuario se rellenan con ceros hasta múltiplo de 8 antes de cifrar.

## API

```c
/* Construye un TC seguro a partir de un payload en texto plano. */
int ccsds_tc_build(space_packet_t *pkt, packet_counter_t *cnt, uint16_t apid,
                   uint8_t function_code, uint8_t key_id,
                   const uint8_t *payload, uint16_t payload_len,
                   uint16_t *out_total);

/* Descifra + verifica un TC recibido in-place. Si sec_hdr_flag=1: lee el
 * secondary header (*sh), reporta validez del CRC (*crc_ok, NO se exige),
 * descifra con XTEA, y reescribe pkt->data para que contenga solo los args en
 * claro. Si sec_hdr_flag=0: retorna CCSDS_TC_ERR_NOSEC y no toca pkt. */
int ccsds_tc_unsecure(space_packet_t *pkt, ccsds_tc_sec_header_t *sh, int *crc_ok);
```

`ccsds_tc_unsecure` reescribe `pkt->data` para que el handler de APID lea los
argumentos descifrados igual que en un TC sin cifrar — la capa de seguridad es
transparente para el código de dispatch.

### Integración en la misión

`command_service.c :: process_rx_packet` llama a `ccsds_tc_unsecure` justo
después de `spp_unpack_packet`, y luego despacha por APID. El valor de retorno y
`crc_ok` se **ignoran** a propósito (ver abajo).

## Debilidades intencionales (CTF)

El cifrado, el CRC y el secondary header son reales y están bien implementados.
Las debilidades explotables están en cómo se gestiona la clave y en que no se
exige nada:

1. **Clave fija, en claro.** `CCSDS_TC_KEY` (`ccsds_xtea.c`) es una sola clave de
   128 bits compilada en el firmware. Sus bytes son ASCII (`"PwnCubeSatLoRaKy"`),
   así que aparece literal en un dump de flash obtenido por el telecomando FLASH
   (APID 0x07). Recuperar la clave → descifrar/forjar cualquier TC seguro.
2. **Modo ECB, sin nonce.** Bloques de plaintext iguales cifran igual, así que el
   enlace "cifrado" filtra estructura.
3. **El CRC es un check, no un MAC.** Un atacante que altera un frame recalcula el
   CRC-16 trivialmente — da integridad contra ruido, no autenticación.
4. **Contador predecible, sin anti-replay.** `command_counter` es monótono y nunca
   se valida, así que los frames capturados se pueden reenviar.
5. **El receptor no exige la capa.** `process_rx_packet` acepta un TC en texto
   plano (`sec_hdr_flag=0`) y nunca rechaza por CRC — así que todos los exploits
   previos por RF siguen funcionando. Es la generalización de la vuln de
   sin-autenticación.

Preservadas de la base: **#9** over-read en el unpack SPP (`spp.c`), **#10**
underflow de longitud en broadcast (`command_service.c`), **#6** secondary header
sin inicializar en el path TM de FLASH.

## Pruebas

`test/tc_test.c` es un test host de round-trip: construye un TC seguro, lo
"unsecure", comprueba que el plaintext hace round-trip, que el CRC valida, que un
frame alterado se marca, y que un TC en texto plano pasa intacto. Vive en `test/`
para que el build de la misión (que hace glob de `ccsds/*.c`, no recursivo) nunca
arrastre su `main()`. Ejecutar desde este directorio:

```sh
gcc -Wall -Wextra -I. -I.. -o /tmp/tc_test \
    test/tc_test.c ../spp.c ccsds_crc.c ccsds_xtea.c ccsds_tc.c && /tmp/tc_test
```

## Construir otro comando seguro (ejemplo)

```c
packet_counter_t cnt; spp_counters_init(&cnt);
uint8_t args[] = { 0x00, 0xC8 };            /* thruster 0, potencia 200 */
space_packet_t pkt; uint16_t total;
ccsds_tc_build(&pkt, &cnt, SPP_APID_TC_SET_THRUSTER, 0x00, 0x01,
               args, sizeof(args), &total);
/* transmitir los primeros `total` bytes de `pkt` por el uplink */
```
