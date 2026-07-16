# Comparación de protocolo FlatSat — ElectronicCats vs PWNSat

Dos firmwares de FlatSat distintos hablan dos perfiles CCSDS **diferentes y no
interoperables** sobre LoRa. Este documento fija exactamente dónde divergen los
dos formatos de wire, con el código que produce cada uno, y cómo se relaciona el
port RISC-V de pwncube (esta rama `ccsds-tc-library`) con ambos.

> **Nomenclatura.** Notas previas los llamaban "FlatSat Zephyr" y "FlatSat
> RadioLib". Este documento usa **FlatSat (ElectronicCats)** para el firmware
> Zephyr y **FlatSat (PWNSat)** para el firmware RadioLib que portea pwncube.

## Resumen

- **FlatSat (ElectronicCats)** — firmware Zephyr RTOS (`flat-sat-fw-interno`).
  CCSDS seguro: **secondary header de 4 B (timestamp) + cifrado de payload por
  niveles (ninguno / XOR / AES-128-CTR) + CRC-16-CCITT**. Rechaza cualquier TC
  sin secondary header.
- **FlatSat (PWNSat)** — firmware Arduino/RadioLib (`FlatSat_Firmware`), el que
  portea pwncube. **CCSDS pelado: primary header de 6 B + payload en claro.** Sin
  secondary header, sin CRC, sin cifrado.
- **No son compatibles a nivel de wire.** Un frame construido por uno es
  malinterpretado por el otro. (Ya verificado en hardware.)
- **Esta rama implementa ambos estilos del lado pwncube:** el camino en claro
  (compatible PWNSat, conserva las vulns del CTF) y una librería de *TC seguro*
  (`applications/ccsds/`) modelada sobre la idea de ElectronicCats — ver la última
  sección para saber qué tan fiel es.

## Formatos de frame, lado a lado

```
FlatSat (PWNSat)  — SPP en claro
┌───────────────────────┬──────────────────────┐
│  Primary header (6)   │  payload (en claro)   │
└───────────────────────┴──────────────────────┘

FlatSat (ElectronicCats) — SPP seguro
┌───────────────────────┬───────────────┬───────────────────────┬────────┐
│  Primary header (6)    │ sec-hdr (4)   │  payload               │ CRC(2) │
│  type=TC, sec_hdr=1    │ timestamp u32 │  ninguno/XOR/AES-128   │ CCITT  │
└───────────────────────┴───────────────┴───────────────────────┴────────┘
                                 └── se usa como IV de AES-CTR
```

Ambos comparten el **primary header CCSDS 133.0-B idéntico** (6 octetos,
big-endian: version, type, flag de sec-hdr, APID, seq flags, seq count,
packet-data-length). Todo lo que va **después** del primary header es donde
divergen.

## Dónde cambia el código

### 1. Primary header — idéntico

Ambos arman el mismo header de 6 B. PWNSat/pwncube:

```c
// src/mcu/.../applications/spp.c  (spp_build_packet)
packet_id |= (type & 0x01) << 12;          // TM/TC
packet_id |= (sec_header & 0x01) << 11;     // flag de secondary header
packet_id |= (apid & 0x07FF);
pkt->header.length = spp_host_to_be16(data_len + sec_header_len - 1);
```

ElectronicCats parsea el mismo layout de bits:

```c
// flat-sat-fw-interno/flatsat/src/main.c  (process_incoming_telecommand)
uint16_t packet_id  = (data[0] << 8) | data[1];
uint8_t  pkt_type   = (packet_id >> 12) & 0x1;
uint8_t  sec_hdr    = (packet_id >> 11) & 0x1;
uint16_t apid       = packet_id & 0x7FF;
```

### 2. Secondary header — timestamp (ElectronicCats) vs ninguno (PWNSat)

**PWNSat** nunca emite secondary header; el payload empieza en el byte 6.

**ElectronicCats** pone un timestamp de 4 B en los bytes 6–9, y **lo usa como IV
del cifrado**:

```c
// flat-sat-fw-interno/flatsat/src/main.c
// Extract timestamp from secondary header (big-endian)
uint32_t timestamp = (data[6] << 24) | (data[7] << 16) | (data[8] << 8) | data[9];
// Payload is from data[10] to data[total_len - 3]
uint16_t payload_len = total_len - 12;      // 6 primary + 4 sec-hdr + 2 CRC
```

### 3. Cifrado del payload — por niveles (ElectronicCats) vs ninguno (PWNSat)

**PWNSat** no tiene crypto: `command_apid_handler` lee `pkt->data[0]`,
`pkt->data[1]` directo como argumentos del comando.

**ElectronicCats** cifra el payload según un nivel de *dificultad* en runtime
(`flatsat_difficulty`, `0 = training`), con clave sobre el IV del timestamp:

```c
// flat-sat-fw-interno/flatsat/src/main.c
if (flatsat_difficulty == 2) {                      // cifrado XOR
    const char *xor_key = "PWNSAT";
    for (uint16_t i = 0; i < payload_len; i++)
        data[10 + i] ^= xor_key[i % 6];
} else if (flatsat_difficulty >= 3) {               // AES-128-CTR
    uint8_t iv[16] = {0};
    iv[0] = (timestamp >> 24) & 0xFF; /* ...iv[1..3] = timestamp... */
    uint8_t round_keys[176];
    aes_key_expansion((const uint8_t *)"PWNSAT_K3Y_2026!", round_keys);
    // por bloque: block_iv[12..15] = contador; keystream = AES(block_iv);
    //             data[10+i] ^= keystream[...]
}
```

- Nivel 0/1: en claro. Nivel 2: XOR con `"PWNSAT"`. Nivel ≥3: **AES-128-CTR**,
  clave `"PWNSAT_K3Y_2026!"`, IV = `timestamp || contador_de_bloque`.
- El núcleo AES-128 es una implementación desde cero en el mismo archivo
  (`aes_key_expansion`, `aes128_encrypt_block`, …).

### 4. CRC — presente sobre plaintext (ElectronicCats) vs ausente (PWNSat)

**PWNSat** no añade CRC (y el uplink de pwncube corre con el CRC del PHY LoRa
apagado también).

**ElectronicCats** añade un CRC-16-CCITT y lo verifica **después de descifrar**
(sobre el frame en claro):

```c
// flat-sat-fw-interno/flatsat/src/main.c
uint16_t expected_crc = ccsds_crc16(data, total_len - 2);   // sobre frame descifrado
uint16_t actual_crc   = (data[total_len - 2] << 8) | data[total_len - 1];
if (expected_crc != actual_crc) { flat_sat.error_count++; return; }
```

### 5. Enforcement — obligatorio (ElectronicCats) vs se acepta cualquiera (pwncube)

**ElectronicCats rechaza** cualquier TC que no sea un paquete seguro:

```c
// flat-sat-fw-interno/flatsat/src/main.c
// Telecommands must have pkt_type == 1 and sec_hdr == 1
if (pkt_type != 1 || sec_hdr != 1) { return; }
```

**pwncube NO exige** su capa segura — un TC en claro (`sec_hdr=0`) igual se
despacha, y un CRC malo se ignora (esta es la debilidad intencional de sin-auth
del CTF):

```c
// src/mcu/.../applications/command_service.c  (process_rx_packet)
ccsds_tc_sec_header_t sh; int crc_ok = 0;
(void)ccsds_tc_unsecure(&pkt, &sh, &crc_ok);   // return + crc_ok ignorados
command_apid_handler(&pkt);
```

## Qué añade esta rama, y qué tan fiel es

La librería de TC seguro de esta rama (`applications/ccsds/ccsds_tc.c` +
`ccsds_xtea.c`) implementa la *forma* del perfil ElectronicCats — un TC seguro
con secondary header, payload cifrado y CRC — pero es una **variante, no un clon
byte-a-byte**:

| Campo | FlatSat (ElectronicCats) | FlatSat (PWNSat) | lib segura pwncube (esta rama) |
|-------|--------------------------|------------------|--------------------------------|
| Primary header | 6 B CCSDS | 6 B CCSDS | 6 B CCSDS |
| Secondary header | 4 B **timestamp** (=IV) | ninguno | 4 B **cmd_counter+func_code+key_id** |
| Cifrado | ninguno / XOR `PWNSAT` / **AES-128-CTR** | ninguno | **XTEA-ECB** |
| Clave | `PWNSAT_K3Y_2026!` | — | `PwnCubeSatLoRaKy` |
| CRC-16-CCITT | sí, sobre **plaintext** | no | sí, sobre **ciphertext** |
| ¿Exige sec-hdr? | **sí** (rechaza planos) | n/a | no (acepta planos) |

Así que el camino seguro de pwncube **no** es interoperable con ElectronicCats
tal cual. Para que pwncube reciba y descifre los TC/TM nativos de ElectronicCats,
la librería necesitaría: reinterpretar el secondary header como timestamp,
cambiar el cifrado a AES-128-CTR (clave `PWNSAT_K3Y_2026!`, IV = timestamp), el
nivel XOR `"PWNSAT"`, y mover la verificación de CRC a *después* de descifrar. Es
un cambio deliberado y aparte; la librería actual mantiene XTEA a propósito (un
cifrado más ligero y autocontenido para el CTF).

## Parámetros de RF / PHY

| Parámetro | FlatSat (ElectronicCats) | FlatSat (PWNSat) / uplink pwncube |
|-----------|--------------------------|-----------------------------------|
| Frecuencia (default) | 915 MHz | 918 MHz |
| Ancho de banda | 125 kHz | 250 kHz |
| Spreading factor | 7 | 7 |
| Coding rate | 4/5 | 4/5 |
| Preamble | 12 | 8 |
| Sync word | private (0x1424) | private (0x1424) |
| CRC LoRa | on | off |

Los params de PHY también difieren, así que los dos ni siquiera se demodulan
hasta que un lado se reconfigura. ElectronicCats es totalmente reconfigurable
desde su `Cat-Shell` (`lora_freq/sf/bw/cr/preamble/syncword` + `lora_apply`); el
uplink de pwncube es 918/BW250/SF7/CR4-5 (`mission.h`). Alinear el PHY es
necesario pero no suficiente — los formatos de frame de arriba siguen difiriendo.

## Implicaciones de interop (qué significa "recibir" de verdad)

- **ElectronicCats ↔ PWNSat: no compatibles a nivel de wire.** Incluso en
  dificultad 0 (payload en claro), ElectronicCats igual emite un secondary header
  de 4 B (timestamp) y un CRC de 2 B que el parser PWNSat/pwncube no espera. Las
  dificultades más altas añaden XOR/AES encima.
- **pwncube recibe los frames de ElectronicCats a nivel PHY** (la radio funciona
  — verificado: RX en radio0, RSSI/SNR buenos) **pero los malinterpreta**: con
  `sec_hdr=1` corre `ccsds_tc_unsecure`, lee el timestamp como counter/func/key, e
  intenta XTEA-descifrar un payload AES/en-claro → argumentos basura (y, al no
  estar alineado a bloque, puede abortar antes de descifrar). El test RF por aire
  que *sí* funcionó usó el formato seguro **propio** de pwncube transmitido a mano
  por el pipe crudo `TX <hex>` del FlatSat — probó RF + la librería de pwncube, no
  interop nativa con ElectronicCats.

Ver también: `applications/ccsds/README.md` (la librería de TC seguro),
`docs/vulnerability-comparison.md` (vulns compartidas/porteadas),
`docs/flatsat-port-changes.md`.
