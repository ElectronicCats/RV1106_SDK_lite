/*
 * CubeSat — RadioService client (SX1262 x2 on the RISC-V MCU, via /dev/rpmsg).
 *
 * The physical radio is controlled by the MCU's RT-Thread firmware; this tool
 * sends it commands over IPC (rpmsg). The channel is bound at boot (rcS).
 * See docs/migration/{design/40-migration-design,implementation/90-mcu-config-replication}.md.
 */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <sys/ioctl.h>

struct rpmsg_endpoint_info {
    char name[32];
    uint32_t src;
    uint32_t dst;
};
#define RPMSG_CREATE_EPT_IOCTL  _IOW(0xb5, 0x1, struct rpmsg_endpoint_info)
#define RPMSG_DESTROY_EPT_IOCTL _IO(0xb5, 0x2)

static uint32_t g_src;   /* unique per-process src, so we can find our endpoint */

#define RADIO_ERR_NOT_INITED 0x10   /* the radio is not configured */
#define EVT_RX          0xE0
#define EVT_RX_TIMEOUT  0xE1

static void usage(void)
{
    puts(
"radio_test — control de los SX1262 (radios 0 y 1) que posee el MCU RISC-V\n"
"\n"
"USO:  radio_test [-r N] <comando> [argumento]\n"
"      -r N   selecciona la radio: 0 (SPI0, por defecto) o 1 (SPI1).\n"
"\n"
"SECUENCIA TIPICA (el chip pierde su configuracion al apagar o resetear):\n"
"  radio_test init 915000000    # 1) configurar: calibra y fija 915 MHz, 14 dBm\n"
"  radio_test cw                # 2) portadora continua ON (espectrometro)\n"
"  radio_test stop              # 3) portadora OFF (standby)\n"
"\n"
"COMANDOS DE CONTROL:\n"
"  ping                Verifica que el servicio del MCU responde (id=RDIO).\n"
"  reset               Reset fisico del chip (pierde init; vuelve a standby).\n"
"  init <freq_hz> [param=valor ...]\n"
"                      Init + config individual de TX. Parametros nombrados en\n"
"                      cualquier orden (los no dados usan el default):\n"
"                        sf=5..12    bw=125|250|500   cr=1..4 (1=4/5..4=4/8)\n"
"                        power=-9..22  pre=preamble(sim)  crc=on|off  iq=std|inv\n"
"                        sync=pub|priv|<hex16>\n"
"                      Default: SF7 BW250 CR4/5 pre8 CRC on 20dBm sync privada\n"
"                      IQ std (OCP 140mA, LDRO auto, ganancia RX boosted).\n"
"                      Ej: init 915000000 sf=9 bw=250 crc=off iq=inv\n"
"                      Compat: init 915000000 7 125 1 20 (posicional sf bw cr pwr)\n"
"  freq <freq_hz>      Cambia solo la frecuencia (requiere init previo).\n"
"  power <dbm>         Potencia TX en dBm, -9 a 22 (requiere init previo).\n"
"  cw                  Portadora continua ON (requiere init). Espectrometro.\n"
"  stop                Standby: apaga portadora/RX. El init se conserva.\n"
"  status              Estado. mode: 2=standby, 4=FS, 5=RX, 6=TX.\n"
"  errors              Errores internos del chip (0x0000 = sano).\n"
"  reg <addr_hex>      Lee un registro. Ej: reg 0740 (sync MSB; default 0x14).\n"
"  wreg <addr_hex> <val> Escribe un registro. Ej: wreg 08E7 60 (OCP 60 mA).\n"
"  sync <pub|priv|hex4> Sync word LoRa: pub=0x3444 (publica/LoRaWAN),\n"
"                      priv=0x1424 (default), o hex de 16 bits (ej: 2B44).\n"
"                      Ambos extremos DEBEN coincidir. Requiere init previo\n"
"                      y se pierde con reset (re-aplicar tras init).\n"
"  antsw <0|1|2>       Switch de antena: 0=auto, 1=TX, 2=RX.\n"
"  mod <sf> <bw_khz> <cr> Configura modulacion LoRa: SF (5-12), BW en kHz\n"
"                      (ej: 250), CR (1=4/5..4=4/8). Requiere init previo.\n"
"  pkt <pre> <hdr> <plen> <crc> <iq> Configura params de paquete: preamble\n"
"                      (8), hdr (0=variable, 1=fija), payload_len, CRC\n"
"                      (0=off, 1=byte, 2=CCITT), IQ (0=std, 1=inv). El IQ queda\n"
"                      pegajoso: se re-aplica en cada TX y RX hasta reiniciar.\n"
"                      Ambos extremos deben usar la misma polaridad IQ.\n"
"  pkts                Muestra RSSI y SNR del ultimo paquete recibido.\n"
"  rssi                RSSI instantaneo (dBm).\n"
"\n"
"INYECCIÓN DE COMANDOS POR RPMsg (Equivalente a USB CDC de FlatSat):\n"
"  tcsend <apid_hex> [payload_hex]   Inyecta un TC (telecomando) directamente\n"
"                      al CommandService del MCU por rpmsg, saltÃ¡ndose la radio.\n"
"                      apid_hex = APID de 2-3 dÃ­gitos hex (ej: 02 = reset, 07 = flash).\n"
"                      payload_hex = opcional, bytes hex (ej: reset no necesita;\n"
"                      thruster: 00 0A = thruster0 power 10).\n"
"                      Ej: radio_test tcsend 02          # reset\n"
"                      Ej: radio_test tcsend 04 000A      # thruster0=10\n"
"                      Ej: radio_test tcsend 07           # flash\n"
"  cmd_ping            Ping al CommandService (responde \"CMDS\" + version).\n"
"  cmd_start <freq_hz> Inicia el CommandService (configura uplink).\n"
"  cmd_stop            Detiene el CommandService.\n"
"  cmd_status          Estado del CommandService: activo, thrusters, beacon, TC count.\n"
"  cmd_config <freq> [sf] [bw] [cr]  Reconfigura uplink (freq en Hz, opcionales).\n"
"  cmd_listen          Escucha eventos EVT_TC_RX del CommandService en tiempo\n"
"                      real (uplink recibido). Ctrl+C para salir.\n"
"  cmd_watch           Vista unificada: cada TC recibido + su efecto/respuesta\n"
"                      (downlink o cambio de estado). Ctrl+C para salir.\n"
"  tlm                 Monitor del downlink (TM/sync/idle/beacon/respuestas) via\n"
"                      IPC: imprime cada frame SPP que baja el MCU. Ctrl+C salir.\n"
"\n"
"PRUEBA BROADCAST CON FRECUENCIA ARBITRARIA:\n"
"  tcbroad <freq_hz> [txt]  EnvÃ­a TC_BROADCAST_MSG (APID 0x06) con frecuencia\n"
"                      y texto dados (bandas ISM limitadas a 430-960 MHz por el\n"
"                      firmware del MCU). Ej: radio_test tcbroad 868000000 \"test\"\n"
"\n"
"COMANDOS DE PAQUETES (LoRa, requieren init previo):\n"
"  tx <texto>          Transmite un paquete LoRa con <texto>. Bloquea hasta\n"
"                      TX_DONE. Ej: radio_test tx \"hola mundo\"\n"
"  ccsds <apid> [txt]  TX paquete CCSDS SPP (header 6 bytes + payload). Muestra\n"
"                      los bytes enviados para correlacionar con rx.\n"
"                      Ej: radio_test ccsds 001 \"ping\"\n"
"  rx [ms]             Escucha un paquete e imprime lo recibido (payload, RSSI,\n"
"                      SNR, CRC). ms = ventana de escucha (por defecto 10000;\n"
"                      0 = continuo). Ej: radio_test -r 1 rx 15000\n"
"  help                Muestra esta ayuda.\n"
"\n"
"PRUEBA LOOPBACK EN LA PLACA (radio 0 -> radio 1, acoplo cercano):\n"
"  loopback <freq_hz> <texto>   Test completo en un solo comando: inicializa\n"
"                      ambas radios, pone la 1 a escuchar y transmite desde la 0.\n"
"                      Ej: radio_test loopback 915000000 \"CubeSat!\"");
}

/* Read /sys/class/rpmsg/rpmsgN/src (the endpoint's local address), or -1. */
static long ept_src(int n)
{
    char lp[80], val[32];
    int f, r;

    snprintf(lp, sizeof(lp), "/sys/class/rpmsg/rpmsg%d/src", n);
    f = open(lp, O_RDONLY);
    if (f < 0) return -1;
    r = read(f, val, sizeof(val) - 1);
    close(f);
    if (r <= 0) return -1;
    val[r] = '\0';
    return strtol(val, NULL, 10);
}

/* rpmsg_ctrl index whose backing channel is "rpmsg-radio". The dst override is
 * ignored on a foreign channel's ctrl, so we must use the radio channel's own
 * ctrl (with two services bound, /dev/rpmsg0 may belong to the sensor). */
static int find_radio_ctrl(void)
{
    char lp[96], target[256];
    ssize_t r;
    int i;

    for (i = 0; i < 8; i++) {
        snprintf(lp, sizeof(lp), "/sys/class/rpmsg/rpmsg_ctrl%d/device", i);
        r = readlink(lp, target, sizeof(target) - 1);
        if (r > 0) {
            target[r] = '\0';
            if (strstr(target, "rpmsg-radio"))
                return i;
        }
    }
    return -1;
}

/* Create an endpoint (dst 0x4005) on the rpmsg-radio ctrl with a unique src,
 * then find our /dev/rpmsgN by matching that src in sysfs — race-free even with
 * concurrent clients (e.g. an RX listener) and leftover endpoint nodes. */
static int open_ept(void)
{
    struct rpmsg_endpoint_info ept = { .name = "radio", .dst = 0x4005 };
    int ctrl, cfd, t, n;
    char cp[64];

    ctrl = find_radio_ctrl();
    if (ctrl < 0) {
        fprintf(stderr, "error: no encuentro el canal rpmsg-radio (¿arranco el MCU?).\n"
                        "Revisa: ls /sys/bus/rpmsg/devices/\n");
        return -1;
    }
    g_src = 0x5000 + (uint32_t)(getpid() & 0x0FFF);
    ept.src = g_src;

    snprintf(cp, sizeof(cp), "/dev/rpmsg_ctrl%d", ctrl);
    cfd = open(cp, O_RDWR);
    if (cfd < 0) { perror(cp); return -1; }
    if (ioctl(cfd, RPMSG_CREATE_EPT_IOCTL, &ept) < 0) { perror("create ept"); close(cfd); return -1; }
    close(cfd);

    for (t = 0; t < 50; t++) {
        for (n = 0; n < 32; n++) {
            if (ept_src(n) == (long)g_src) {
                char np[64];
                int fd;
                snprintf(np, sizeof(np), "/dev/rpmsg%d", n);
                fd = open(np, O_RDWR);
                if (fd >= 0)
                    return fd;
            }
        }
        usleep(20000);
    }
    fprintf(stderr, "error: no aparecio el endpoint /dev/rpmsgN tras crearlo.\n");
    return -1;
}

/* Open an endpoint to a named MCU rpmsg service (e.g. "rpmsg-command" @ 0x4008,
 * "rpmsg-telemetry" @ 0x4007). Scans rpmsg_ctrl*, creates the endpoint with a
 * unique per-process src, and waits for /dev/rpmsgN to appear. */
/* Like open_named_ept() but with an explicit src address, so a single process
 * can hold several endpoints at once (each needs a UNIQUE src for rpmsg routing
 * — e.g. cmd_watch opens command + telemetry together). */
static int open_named_ept_src(const char *chan, uint32_t dst, uint32_t src)
{
    struct rpmsg_endpoint_info ept = { .name = "cli", .dst = dst };
    int cfd, t, n, ctrl;
    char cp[64];

    for (ctrl = 0; ctrl < 8; ctrl++) {
        snprintf(cp, sizeof(cp), "/sys/class/rpmsg/rpmsg_ctrl%d/device", ctrl);
        char target[256];
        ssize_t r = readlink(cp, target, sizeof(target) - 1);
        if (r > 0) {
            target[r] = '\0';
            if (strstr(target, chan))
                break;
        }
    }
    if (ctrl >= 8) {
        fprintf(stderr, "error: no encuentro el canal %s\n", chan);
        return -1;
    }
    ept.src = src;

    snprintf(cp, sizeof(cp), "/dev/rpmsg_ctrl%d", ctrl);
    cfd = open(cp, O_RDWR);
    if (cfd < 0) { perror(cp); return -1; }
    if (ioctl(cfd, RPMSG_CREATE_EPT_IOCTL, &ept) < 0) { perror("create ept"); close(cfd); return -1; }
    close(cfd);

    for (t = 0; t < 50; t++) {
        for (n = 0; n < 32; n++) {
            if (ept_src(n) == (long)src) {
                char np[64];
                int fd;
                snprintf(np, sizeof(np), "/dev/rpmsg%d", n);
                fd = open(np, O_RDWR);
                if (fd >= 0)
                    return fd;
            }
        }
        usleep(20000);
    }
    fprintf(stderr, "error: no aparecio el endpoint /dev/rpmsgN para %s.\n", chan);
    return -1;
}

/* Open an endpoint to a named MCU rpmsg service with the default per-process src. */
static int open_named_ept(const char *chan, uint32_t dst)
{
    g_src = 0x6000 + (uint32_t)(getpid() & 0x0FFF);
    return open_named_ept_src(chan, dst, g_src);
}

/* Endpoint to the CommandService (0x4008): direct TC injection + TC events. */
static int open_command_ept(void)
{
    return open_named_ept("rpmsg-command", 0x4008);
}

/* Endpoint to the TelemetryService (0x4007): downlink TM monitor (CDC-like). */
static int open_telemetry_ept(void)
{
    return open_named_ept("rpmsg-telemetry", 0x4007);
}

/* Tear down our endpoint so /dev/rpmsgN nodes don't leak across invocations. */
static void close_ept(int fd)
{
    if (fd >= 0) {
        (void)ioctl(fd, RPMSG_DESTROY_EPT_IOCTL, 0);
        close(fd);
    }
}

/* Read one rpmsg message with a timeout; returns length or <=0. */
static int rx_msg(int fd, uint8_t *buf, int max, int tmo_ms)
{
    struct pollfd pfd = { .fd = fd, .events = POLLIN };
    int n = poll(&pfd, 1, tmo_ms);

    if (n <= 0)
        return n;                 /* 0 = timeout, <0 = error */
    return read(fd, buf, max);
}

/* Send a request, wait for and read the (mirrored-cmd) reply. */
static int xfer(int fd, const uint8_t *req, int req_len, uint8_t *rsp, int rsp_max, int tmo_ms)
{
    if (write(fd, req, req_len) != req_len) { perror("write"); return -1; }
    return rx_msg(fd, rsp, rsp_max, tmo_ms);
}

static int need_arg(int have, const char *what)
{
    if (have)
        return 0;
    fprintf(stderr, "error: falta el argumento <%s>. Usa 'radio_test help'.\n", what);
    return 1;
}

static const char *radio_err_str(uint8_t e)
{
    switch (e) {
    case 0x00: return "OK";
    case 0x01: return "ERROR (SPI/comm)";
    case 0x02: return "ERROR (reset)";
    case 0x10: return "NOT_INITED (corre 'init' primero)";
    default:   return "ERROR (desconocido)";
    }
}

/* Human-readable name for an uplink TC APID (see mission.h). */
static const char *tc_apid_name(uint16_t apid)
{
    switch (apid) {
    case 0x01: return "PING";
    case 0x02: return "RESET";
    case 0x03: return "SEND_FW (pide version)";
    case 0x04: return "SET_THRUSTER";
    case 0x05: return "SET_BEACON_RATE";
    case 0x06: return "BROADCAST_MSG";
    case 0x07: return "FLASH (dump)";
    default:   return "APID desconocido";
    }
}

/* Human-readable name for a downlink TM APID (see mission.h). */
static const char *tm_apid_name(uint16_t apid)
{
    switch (apid) {
    case 0x01: return "PING/beacon";
    case 0x03: return "VERSION";
    case 0x06: return "BROADCAST";
    case 0x07: return "FLASH chunk";
    case 0x08: return "TM periodica";
    case 0x09: return "ERROR Unknown APID";
    default:   return "TM";
    }
}

/* Print the printable ASCII of a byte range as data="..." (non-printables as '.'). */
static void print_ascii_field(const uint8_t *b, int from, int to)
{
    int i;
    printf(" data=\"");
    for (i = from; i < to; i++)
        putchar((b[i] >= 0x20 && b[i] < 0x7f) ? b[i] : '.');
    printf("\"");
}

/* Wait for and print an EVT_RX or EVT_RX_TIMEOUT. Returns 0 for EVT_RX
 * with good CRC, 1 for bad CRC or timeout. Skips stray command replies. */
static int wait_rx_event(int fd, int tmo_ms)
{
    uint8_t ev[512];

    for (int tries = 0; tries < 10; tries++) {
        int n = rx_msg(fd, ev, sizeof(ev), tmo_ms);
        if (n <= 0) return 1;

        if (ev[0] == EVT_RX && n >= 7) {
            int rlen = ev[3];                       /* received payload length */
            int16_t rssi = (int16_t)(((uint16_t)ev[4] << 8) | ev[5]);
            int8_t snr = (int8_t)ev[6];
            uint8_t *pay = &ev[7];                  /* raw LoRa payload bytes */
            int i;

            /* Always print raw hex dump first */
            printf("rx: radio=%d raw[%d]=", ev[1], rlen);
            for (i = 0; i < rlen && 7 + i < n; i++)
                printf("%02x ", pay[i]);

            /* Detect CCSDS SPP: version must be 0 AND the length field must be
             * consistent with the LoRa frame (total = 6 + length_field + 1).
             * Without the length check, random payloads whose first byte is
             * < 0x20 get misdetected as SPP and print garbage. */
            {
                int dlen = (rlen >= 7) ? (((int)pay[4] << 8) | pay[5]) : -1;
                int is_spp = (rlen >= 7 && (pay[0] >> 5) == 0x00 &&
                              6 + dlen + 1 == rlen);

                if (is_spp) {
                    int type  = (pay[0] >> 4) & 0x01;
                    int apid  = ((pay[0] & 0x07) << 8) | pay[1];
                    int sflag = (pay[2] >> 6) & 0x03;
                    int scnt  = ((pay[2] & 0x3F) << 8) | pay[3];

                    /* SPP does not encode the secondary-header length in the
                     * packet, so the data field is shown from byte 6 as-is. */
                    printf(" SPP: %s apid=0x%03X seq=%d(0x%x) pay_len=%d data=\"",
                           type ? "TC" : "TM", apid, scnt, sflag, dlen + 1);
                    for (i = 6; i < rlen && i < 6 + 40 && 7 + i < n; i++)
                        putchar(isprint(pay[i]) ? pay[i] : '.');
                    printf("\"");
                } else {
                    printf(" txt=\"");
                    for (i = 0; i < rlen && i < 40 && 7 + i < n; i++)
                        putchar(isprint(pay[i]) ? pay[i] : '.');
                    printf("\"");
                }
            }
            printf(" rssi=%d snr=%d crc=%s\n",
                   rssi, snr, (ev[2] & 1) ? "ok" : "BAD");
            return (ev[2] & 1) ? 0 : 1;
        }
        if (ev[0] == EVT_RX_TIMEOUT) {
            printf("rx: radio=%d timeout (ventana agotada)\n",
                   n >= 2 ? ev[1] : -1);
            return 1;
        }
        /* Stray reply — skip and keep waiting */
    }
    return 1;
}

int main(int argc, char **argv)
{
    uint8_t req[512] = {0}, rsp[64];
    int fd, n, len = 2, tmo = 3000;
    int inst = 0;
    uint32_t freq;
    const char *cmd;

    /* optional  -r N  radio selector */
    if (argc >= 3 && !strcmp(argv[1], "-r")) {
        inst = atoi(argv[2]);
        if (inst < 0 || inst > 1) { fprintf(stderr, "error: radio debe ser 0 o 1.\n"); return 2; }
        argv += 2; argc -= 2;
    }

    if (argc < 2 || !strcmp(argv[1], "help") || !strcmp(argv[1], "-h") || !strcmp(argv[1], "--help")) {
        usage();
        return argc < 2 ? 2 : 0;
    }
    cmd = argv[1];
    req[1] = (uint8_t)inst;

    if (!strcmp(cmd, "ping"))        req[0] = 0x01;
    else if (!strcmp(cmd, "reset"))  req[0] = 0x02;
    else if (!strcmp(cmd, "reg")) {
        uint16_t a;
        if (need_arg(argc >= 3, "addr_hex")) return 2;
        a = (uint16_t)strtoul(argv[2], NULL, 16);
        req[0] = 0x03; req[2] = a >> 8; req[3] = a & 0xFF; len = 4;
    }
    else if (!strcmp(cmd, "wreg")) {
        uint16_t a, v;
        if (need_arg(argc >= 4, "addr_hex> <val")) return 2;
        a = (uint16_t)strtoul(argv[2], NULL, 16);
        v = (uint8_t)strtoul(argv[3], NULL, 0);
        req[0] = 0x04; req[2] = a >> 8; req[3] = a & 0xFF; req[4] = (uint8_t)v; len = 5;
    }
    else if (!strcmp(cmd, "init")) {
        /* Robust init: freq + individual TX params as key=value, any order:
         *   init <freq> [sf=N] [bw=N] [cr=N] [power=N] [pre=N] [crc=on|off] [iq=std|inv] [sync=pub|priv|hex]
         * Bare numbers after freq are still accepted positionally as sf bw cr power.
         * Unspecified params take the built-in defaults. Orchestrates
         * INIT (0x05) + SET_PKT_PARAMS (0x11, sticky pre/crc/iq) + SET_SYNC (0x14). */
        int sf = 7, bw = 250, cr = 1, pwr = 20, pre = 8, crc = 1, iq = 0;
        int sync_set = 0, pos = 0, i;
        uint16_t sw = 0x1424;
        uint8_t q[16];

        if (need_arg(argc >= 3, "freq_hz [sf=] [bw=] [cr=] [power=] [pre=] [crc=] [iq=] [sync=]")) return 2;
        freq = (uint32_t)strtoul(argv[2], NULL, 10);
        if (freq < 150000000U || freq > 960000000U) {
            fprintf(stderr, "error: frecuencia fuera de rango (150000000-960000000 Hz).\n");
            return 2;
        }
        for (i = 3; i < argc; i++) {
            char *a = argv[i], *eq = strchr(a, '=');
            if (eq) {
                char *v = eq + 1;
                *eq = '\0';
                if      (!strcmp(a, "sf"))                            sf  = atoi(v);
                else if (!strcmp(a, "bw"))                            bw  = atoi(v);
                else if (!strcmp(a, "cr"))                            cr  = atoi(v);
                else if (!strcmp(a, "power") || !strcmp(a, "pwr"))    pwr = atoi(v);
                else if (!strcmp(a, "pre")   || !strcmp(a, "preamble")) pre = atoi(v);
                else if (!strcmp(a, "crc"))  crc = (!strcmp(v, "on")  || !strcmp(v, "1")) ? 1 : 0;
                else if (!strcmp(a, "iq"))   iq  = (!strcmp(v, "inv") || !strcmp(v, "inverted") || !strcmp(v, "1")) ? 1 : 0;
                else if (!strcmp(a, "sync")) {
                    if      (!strcmp(v, "pub")  || !strcmp(v, "publica")) sw = 0x3444;
                    else if (!strcmp(v, "priv") || !strcmp(v, "privada")) sw = 0x1424;
                    else sw = (uint16_t)strtoul(v, NULL, 16);
                    sync_set = 1;
                } else { fprintf(stderr, "init: parametro desconocido '%s' (usa sf/bw/cr/power/pre/crc/iq/sync).\n", a); return 2; }
            } else {
                int val = atoi(a);
                if      (pos == 0) sf  = val;
                else if (pos == 1) bw  = val;
                else if (pos == 2) cr  = val;
                else if (pos == 3) pwr = val;
                pos++;
            }
        }
        if (sf < 5 || sf > 12)     { fprintf(stderr, "error: sf 5-12.\n"); return 2; }
        if (cr < 1 || cr > 4)      { fprintf(stderr, "error: cr 1-4 (1=4/5..4=4/8).\n"); return 2; }
        if (pwr < -9 || pwr > 22)  { fprintf(stderr, "error: power -9..22 dBm.\n"); return 2; }
        if (pre < 1 || pre > 65535){ fprintf(stderr, "error: preamble 1-65535.\n"); return 2; }
        if (bw != 125 && bw != 250 && bw != 500) { fprintf(stderr, "error: bw 125/250/500 kHz.\n"); return 2; }

        fd = open_ept();
        if (fd < 0) return 1;
        /* 1) base INIT (freq + sf/bw/cr/power) */
        q[0] = 0x05; q[1] = (uint8_t)inst;
        q[2] = freq >> 24; q[3] = freq >> 16; q[4] = freq >> 8; q[5] = (uint8_t)freq;
        q[6] = (uint8_t)sf; q[7] = (uint8_t)(bw >> 8); q[8] = (uint8_t)bw;
        q[9] = (uint8_t)cr; q[10] = (uint8_t)(int8_t)pwr;
        n = xfer(fd, q, 11, rsp, sizeof(rsp), 6000);
        if (n < 2 || rsp[1] != 0) {
            fprintf(stderr, "init: fallo (err=0x%02x)\n", n >= 2 ? rsp[1] : 0xFF);
            close_ept(fd); return 1;
        }
        /* 2) SET_PKT_PARAMS: preamble/CRC/IQ (sticky on the MCU, applied by TX+RX) */
        q[0] = 0x11; q[1] = (uint8_t)inst;
        q[2] = (uint8_t)(pre >> 8); q[3] = (uint8_t)pre;
        q[4] = 0x00; q[5] = 0xFF; q[6] = (uint8_t)crc; q[7] = (uint8_t)iq;
        n = xfer(fd, q, 8, rsp, sizeof(rsp), 3000);
        if (n < 2 || rsp[1] != 0)
            fprintf(stderr, "init: aviso, SET_PKT_PARAMS err=0x%02x\n", n >= 2 ? rsp[1] : 0xFF);
        /* 3) SET_SYNC only if requested (init already set private 0x1424) */
        if (sync_set) {
            q[0] = 0x14; q[1] = (uint8_t)inst;
            q[2] = (uint8_t)(sw >> 8); q[3] = (uint8_t)sw;
            n = xfer(fd, q, 4, rsp, sizeof(rsp), 3000);
        }
        close_ept(fd);
        printf("init     OK, freq=%lu Hz, SF=%d, BW=%d kHz, CR=4/%d, preamble=%d, power=%d dBm, CRC=%s, IQ=%s, sync=0x%04X (%s)\n",
               (unsigned long)freq, sf, bw, cr + 4, pre, pwr, crc ? "on" : "off",
               iq ? "inv" : "std", sw, sw == 0x3444 ? "pub" : sw == 0x1424 ? "priv" : "custom");
        return 0;
    }
    else if (!strcmp(cmd, "freq")) {
        if (need_arg(argc >= 3, "freq_hz")) return 2;
        freq = (uint32_t)strtoul(argv[2], NULL, 10);
        if (freq < 150000000U || freq > 960000000U) {
            fprintf(stderr, "error: frecuencia fuera de rango (150000000-960000000 Hz).\n");
            return 2;
        }
        req[0] = 0x06;
        req[2] = freq >> 24; req[3] = freq >> 16; req[4] = freq >> 8; req[5] = freq;
        len = 6; tmo = 6000;
    }
    else if (!strcmp(cmd, "power")) {
        int dbm;
        if (need_arg(argc >= 3, "dbm")) return 2;
        dbm = atoi(argv[2]);
        if (dbm < -9 || dbm > 22) { fprintf(stderr, "error: potencia -9..22 dBm.\n"); return 2; }
        req[0] = 0x07; req[2] = (uint8_t)(int8_t)dbm; len = 3;
    }
    else if (!strcmp(cmd, "cw"))     req[0] = 0x08;
    else if (!strcmp(cmd, "stop"))   req[0] = 0x09;
    else if (!strcmp(cmd, "status")) req[0] = 0x0A;
    else if (!strcmp(cmd, "errors")) req[0] = 0x0B;
    else if (!strcmp(cmd, "antsw")) {
        if (need_arg(argc >= 3, "0|1|2")) return 2;
        req[0] = 0x0C; req[2] = (uint8_t)atoi(argv[2]); len = 3;
    }
    else if (!strcmp(cmd, "mod")) {
        uint32_t bw_khz;
        if (need_arg(argc >= 5, "sf> <bw_khz> <cr")) return 2;
        req[2] = (uint8_t)atoi(argv[2]);   /* SF */
        bw_khz = (uint32_t)strtoul(argv[3], NULL, 10);
        req[3] = (uint8_t)((bw_khz >> 8) & 0xFF);
        req[4] = (uint8_t)(bw_khz & 0xFF);
        req[5] = (uint8_t)atoi(argv[4]);   /* CR */
        req[0] = 0x10; len = 6;
    }
    else if (!strcmp(cmd, "pkt")) {
        uint16_t pre;
        if (need_arg(argc >= 7, "pre> <hdr> <plen> <crc> <iq")) return 2;
        pre = (uint16_t)strtoul(argv[2], NULL, 10);
        req[2] = (uint8_t)(pre >> 8);
        req[3] = (uint8_t)pre;
        req[4] = (uint8_t)atoi(argv[3]);   /* header type */
        req[5] = (uint8_t)atoi(argv[4]);   /* payload len */
        req[6] = (uint8_t)atoi(argv[5]);   /* CRC */
        req[7] = (uint8_t)atoi(argv[6]);   /* IQ invert */
        req[0] = 0x11; len = 8;
    }
    else if (!strcmp(cmd, "pkts"))  req[0] = 0x12;
    else if (!strcmp(cmd, "rssi"))  req[0] = 0x13;
    else if (!strcmp(cmd, "sync")) {
        uint16_t sw;
        if (need_arg(argc >= 3, "pub|priv|hex16")) return 2;
        if (!strcmp(argv[2], "pub") || !strcmp(argv[2], "publica"))
            sw = 0x3444;                               /* LoRaWAN / public network */
        else if (!strcmp(argv[2], "priv") || !strcmp(argv[2], "privada"))
            sw = 0x1424;                               /* factory default */
        else
            sw = (uint16_t)strtoul(argv[2], NULL, 16);
        req[0] = 0x14; req[2] = (uint8_t)(sw >> 8); req[3] = (uint8_t)sw; len = 4;
    }
    else if (!strcmp(cmd, "tx")) {
        int plen;
        if (need_arg(argc >= 3, "texto")) return 2;
        plen = (int)strlen(argv[2]);
        if (plen > 250) plen = 250;
        req[0] = 0x0D; req[2] = (uint8_t)plen;
        memcpy(&req[3], argv[2], plen);
        len = 3 + plen; tmo = 6000;   /* handler blocks until TX_DONE */
    }
    else if (!strcmp(cmd, "tcsend")) {
        /* Direct TC injection via rpmsg to CommandService, bypassing RF.
         * Usage: radio_test tcsend <apid_hex> [payload_hex...]
         * Builds a CCSDS SPP packet and sends it via CMD_CMD_TC_SEND (0x10)
         * to the command_service rpmsg endpoint (0x4008).
         * Payload is raw hex bytes (e.g. "00 0A" for thruster0 power 10). */
        uint16_t apid;
        uint8_t  pkt[300];
        int      plen = 0, i;

        if (need_arg(argc >= 3, "apid_hex")) return 2;
        apid = (uint16_t)(strtoul(argv[2], NULL, 16) & 0x07FF);

        /* Parse optional hex payload args */
        for (i = 3; i < argc && plen < 250; i++) {
            pkt[plen++] = (uint8_t)strtoul(argv[i], NULL, 16);
        }

        /* Build SPP primary header: type=TC, seq_flags=unsegmented, seq=0 */
        uint16_t ident = (0x0 << 13) | (0x1 << 12) | (0x0 << 11) | apid;
        uint16_t seq   = (0x3 << 14) | 0x0000;
        uint16_t dlen  = (uint16_t)((plen > 0 ? plen : 1) - 1);

        uint8_t frame[260];
        int foff = 0;
        frame[foff++] = 0x10;  /* CMD_CMD_TC_SEND */
        /* SPP header (6 bytes big-endian) */
        frame[foff++] = (uint8_t)(ident >> 8);
        frame[foff++] = (uint8_t)ident;
        frame[foff++] = (uint8_t)(seq >> 8);
        frame[foff++] = (uint8_t)seq;
        frame[foff++] = (uint8_t)(dlen >> 8);
        frame[foff++] = (uint8_t)dlen;
        /* payload */
        if (plen > 0) { memcpy(frame + foff, pkt, plen); foff += plen; }
        else { frame[foff++] = 0x00; }  /* pad byte */

        fd = open_command_ept();
        if (fd < 0) return 1;

        n = xfer(fd, frame, foff, rsp, sizeof(rsp), 3000);
        close_ept(fd);

        if (n < 2) { fprintf(stderr, "error: respuesta invalida (n=%d)\n", n); return 1; }
        printf("tcsend: APID 0x%03X %s", apid, radio_err_str(rsp[1]));
        if (rsp[1] == 0) printf(" (TC inyectado: %d bytes payload hex)", plen);
        printf("\n");
        return rsp[1] ? 1 : 0;
    }
    else if (!strcmp(cmd, "tcbroad")) {
        /* Send TC_BROADCAST_MSG (APID 0x06) with given frequency and text.
         * Frequency in Hz (uint32_t, converted to MHz uint16_t).
         * Text appended as data after 2-byte freq.
         * Direct rpmsg injection. */
        uint16_t freq_mhz;
        const char *text;
        if (need_arg(argc >= 4, "freq_hz> <texto")) return 2;
        freq_mhz = (uint16_t)(strtoul(argv[2], NULL, 10) / 1000000);
        text = argv[3];
        int tlen = (int)strlen(text);
        if (tlen > 248) tlen = 248;

        /* Build payload: freq(2 bytes) + text */
        uint8_t pay[300];
        int plen = 0;
        pay[plen++] = (uint8_t)(freq_mhz >> 8);
        pay[plen++] = (uint8_t)freq_mhz;
        memcpy(pay + plen, text, tlen);
        plen += tlen;

        /* Build SPP TC frame */
        uint16_t ident = (0x0 << 13) | (0x1 << 12) | (0x0 << 11) | 0x06; /* TC BROADCAST */
        uint16_t seq   = (0x3 << 14) | 0x0000;
        uint16_t dlen  = (uint16_t)(plen - 1);

        uint8_t frame[350];
        int off = 0;
        frame[off++] = 0x10;  /* CMD_CMD_TC_SEND */
        frame[off++] = (uint8_t)(ident >> 8);
        frame[off++] = (uint8_t)ident;
        frame[off++] = (uint8_t)(seq >> 8);
        frame[off++] = (uint8_t)seq;
        frame[off++] = (uint8_t)(dlen >> 8);
        frame[off++] = (uint8_t)dlen;
        memcpy(frame + off, pay, plen);
        off += plen;

        fd = open_command_ept();
        if (fd < 0) return 1;

        n = xfer(fd, frame, off, rsp, sizeof(rsp), 3000);
        close_ept(fd);

        if (n < 2) { fprintf(stderr, "error: respuesta invalida (n=%d)\n", n); return 1; }
        printf("tcbroad: freq=%u MHz txt=\"%s\" %s\n",
               freq_mhz, text, radio_err_str(rsp[1]));
        return rsp[1] ? 1 : 0;
    }
    else if (!strcmp(cmd, "cmd_ping")) {
        uint8_t req[4] = {0x01};
        fd = open_command_ept();
        if (fd < 0) return 1;
        n = xfer(fd, req, 1, rsp, sizeof(rsp), 3000);
        close_ept(fd);
        if (n < 7) { fprintf(stderr, "error: respuesta invalida (n=%d)\n", n); return 1; }
        printf("cmd_ping: id=\"%c%c%c%c\" version=%d\n",
               rsp[2], rsp[3], rsp[4], rsp[5], rsp[6]);
        return rsp[1] ? 1 : 0;
    }
    else if (!strcmp(cmd, "cmd_start")) {
        uint32_t freq;
        if (need_arg(argc >= 3, "freq_hz")) return 2;
        freq = (uint32_t)strtoul(argv[2], NULL, 10);
        uint8_t req[6] = {0x02, 0,
                          (uint8_t)(freq >> 24), (uint8_t)(freq >> 16),
                          (uint8_t)(freq >> 8),  (uint8_t)freq};
        fd = open_command_ept();
        if (fd < 0) return 1;
        n = xfer(fd, req, 6, rsp, sizeof(rsp), 3000);
        close_ept(fd);
        if (n < 2) { fprintf(stderr, "error: respuesta invalida (n=%d)\n", n); return 1; }
        printf("cmd_start: %s\n", radio_err_str(rsp[1]));
        return rsp[1] ? 1 : 0;
    }
    else if (!strcmp(cmd, "cmd_stop")) {
        uint8_t req[2] = {0x03, 0};
        fd = open_command_ept();
        if (fd < 0) return 1;
        n = xfer(fd, req, 1, rsp, sizeof(rsp), 3000);
        close_ept(fd);
        if (n < 2) { fprintf(stderr, "error: respuesta invalida (n=%d)\n", n); return 1; }
        printf("cmd_stop: %s\n", radio_err_str(rsp[1]));
        return rsp[1] ? 1 : 0;
    }
    else if (!strcmp(cmd, "cmd_status")) {
        uint8_t req[2] = {0x04, 0};
        fd = open_command_ept();
        if (fd < 0) return 1;
        n = xfer(fd, req, 1, rsp, sizeof(rsp), 3000);
        close_ept(fd);
        if (n < 11) { fprintf(stderr, "error: respuesta invalida (n=%d)\n", n); return 1; }
        printf("cmd_status: active=%d thruster=[%d,%d] beacon=%dms tx_count=%u\n",
               rsp[2], rsp[3], rsp[4],
               ((uint16_t)rsp[5] << 8) | rsp[6],
               ((uint32_t)rsp[7] << 24) | ((uint32_t)rsp[8] << 16) |
               ((uint32_t)rsp[9] << 8)  | rsp[10]);
        return rsp[1] ? 1 : 0;
    }
    else if (!strcmp(cmd, "cmd_config")) {
        uint32_t freq;
        uint8_t sf, cr;
        uint32_t bw;
        if (need_arg(argc >= 4, "freq_hz [sf] [bw] [cr]")) return 2;
        freq = (uint32_t)strtoul(argv[2], NULL, 10);
        sf = (argc > 3) ? (uint8_t)strtoul(argv[3], NULL, 10) : 7;
        bw = (argc > 4) ? (uint32_t)strtoul(argv[4], NULL, 10) : 250000;
        cr = (argc > 5) ? (uint8_t)strtoul(argv[5], NULL, 10) : 1;
        uint8_t req[11] = {0x05, 0,
                           (uint8_t)(freq >> 24), (uint8_t)(freq >> 16),
                           (uint8_t)(freq >> 8),  (uint8_t)freq,
                           sf,
                           (uint8_t)(bw >> 8), (uint8_t)bw,
                           cr};
        fd = open_command_ept();
        if (fd < 0) return 1;
        n = xfer(fd, req, 10, rsp, sizeof(rsp), 3000);
        close_ept(fd);
        if (n < 2) { fprintf(stderr, "error: respuesta invalida (n=%d)\n", n); return 1; }
        printf("cmd_config: freq=%u sf=%u bw=%u cr=%u %s\n",
               freq, sf, bw, cr, radio_err_str(rsp[1]));
        return rsp[1] ? 1 : 0;
    }
    else if (!strcmp(cmd, "cmd_listen")) {
        /* Listen for EVT_TC_RX events from CommandService (like FlatSat CDC).
         * Opens the command endpoint and polls in a loop, printing each event. */
        fd = open_command_ept();
        if (fd < 0) return 1;

        printf("cmd_listen: escuchando eventos EVT_TC_RX... (Ctrl+C para salir)\n");
        /* Send a PING first so the MCU registers our host address for event delivery */
        {
            uint8_t ping_req[2] = {0x01, 0};
            (void)xfer(fd, ping_req, 2, rsp, sizeof(rsp), 2000);
        }
        for (;;) {
            n = xfer(fd, NULL, 0, rsp, sizeof(rsp), 2000);
            if (n > 0) {
                if (n >= 7 && rsp[0] == 0xE3) {
                    uint32_t tc_cnt = ((uint32_t)rsp[1] << 24) |
                                      ((uint32_t)rsp[2] << 16) |
                                      ((uint32_t)rsp[3] << 8)  | rsp[4];
                    uint16_t apid = ((uint16_t)rsp[5] << 8) | rsp[6];
                    printf("[EVT_TC_RX] TC #%u APID=0x%03X (%u bytes)\n",
                           tc_cnt, apid, n > 7 ? n - 7 : 0);
                    if (n > 7) {
                        printf("  Payload: ");
                        for (int i = 7; i < n; i++)
                            printf("%02X ", rsp[i]);
                        printf("\n");
                    }
                } else if (rsp[0] == 0xE0 || rsp[0] == 0xE1) {
                    printf("[EVT_RADIO] type=0x%02X len=%d\n", rsp[0], n);
                } else {
                    printf("[EVT_UNK] type=0x%02X len=%d\n", rsp[0], n);
                }
                fflush(stdout);
            }
        }
        close_ept(fd);
        return 0;
    }
    else if (!strcmp(cmd, "cmd_watch")) {
        /* Unified view: each received TC (EVT_TC_RX @0x4008) together with its
         * effect/response. ALSO opens the downlink monitor (@0x4007) to show
         * the response that comes down (PING ack, version, flash, broadcast,
         * error). For TCs that send NOTHING down (thruster/beacon), decode the
         * direct effect from the TC bytes. This is the host's "TC log". */
        int cfd = open_named_ept_src("rpmsg-command",   0x4008,
                                     0x6000 + (uint32_t)(getpid() & 0x0FFF));
        if (cfd < 0) return 1;
        int tfd = open_named_ept_src("rpmsg-telemetry", 0x4007,
                                     0x7000 + (uint32_t)(getpid() & 0x0FFF));
        if (tfd < 0) { close_ept(cfd); return 1; }

        printf("cmd_watch: TC recibidos + efecto/respuesta (Ctrl+C para salir)\n");
        { uint8_t p[2] = {0x01, 0};    (void)xfer(cfd, p, 2, rsp, sizeof(rsp), 2000); }
        { uint8_t m[2] = {0x10, 0x01}; (void)xfer(tfd, m, 2, rsp, sizeof(rsp), 2000); }

        struct pollfd pfds[2] = { { cfd, POLLIN, 0 }, { tfd, POLLIN, 0 } };
        for (;;) {
            if (poll(pfds, 2, 2000) <= 0) continue;

            if (pfds[0].revents & POLLIN) {          /* --- TC received --- */
                n = read(cfd, rsp, sizeof(rsp));
                if (n >= 7 && rsp[0] == 0xE3) {
                    uint32_t cnt  = ((uint32_t)rsp[1] << 24) | ((uint32_t)rsp[2] << 16) |
                                    ((uint32_t)rsp[3] << 8)  |  rsp[4];
                    uint16_t apid = ((uint16_t)rsp[5] << 8)  |  rsp[6];
                    printf("\n[TC #%u]  APID=0x%03X  %s\n", cnt, apid, tc_apid_name(apid));
                    if (n > 7) {
                        printf("   raw:");
                        for (int i = 7; i < n; i++) printf(" %02X", rsp[i]);
                        printf("\n");
                    }
                    /* Effect of TCs that generate no downlink (read from the TC). */
                    if (apid == 0x04 && n >= 15)
                        printf("   -> efecto: thruster%u potencia=%u (sin downlink; ver cmd_status)\n",
                               rsp[13], rsp[14]);
                    else if (apid == 0x05 && n >= 14)
                        printf("   -> efecto: beacon_rate=%us %s(sin downlink)\n",
                               rsp[13], rsp[13] > 10 ? "[RECHAZADO >10s] " : "");
                    else if (apid == 0x02)
                        printf("   -> efecto: RESET del satelite (sin downlink)\n");
                    fflush(stdout);
                }
            }

            if (pfds[1].revents & POLLIN) {          /* --- downlink response --- */
                n = read(tfd, rsp, sizeof(rsp));
                if (n == 2 && rsp[0] == 0x10) continue;   /* MONITOR echo */
                if (n >= 6) {
                    uint16_t apid = (((uint16_t)rsp[0] << 8) | rsp[1]) & 0x07FF;
                    printf("   <- respuesta: TM APID=0x%03X %s", apid, tm_apid_name(apid));
                    if (n > 6) print_ascii_field(rsp, 6, n);
                    printf("\n");
                    fflush(stdout);
                }
            }
        }
        close_ept(cfd);
        close_ept(tfd);
        return 0;
    }
    else if (!strcmp(cmd, "tlm")) {
        /* Downlink monitor (TM/sync/idle/beacon/TC responses) via IPC.
         * Replacement for the host's "serial monitor": opens the TelemetryService
         * (0x4007), enables the monitor and prints every SPP frame the MCU
         * transmits on the downlink. Equivalent to seeing the downlink without an SDR. */
        fd = open_telemetry_ept();
        if (fd < 0) return 1;
        printf("tlm: monitor de downlink habilitado... (Ctrl+C para salir)\n");
        {
            uint8_t mon_req[2] = {0x10, 0x01};   /* TELEM_CMD_MONITOR enable */
            (void)xfer(fd, mon_req, 2, rsp, sizeof(rsp), 2000);
        }
        for (;;) {
            n = rx_msg(fd, rsp, sizeof(rsp), 2000);
            if (n <= 0) continue;
            if (n == 2 && rsp[0] == 0x10) continue;   /* MONITOR command echo */
            if (n >= 6) {
                uint16_t ident = ((uint16_t)rsp[0] << 8) | rsp[1];
                uint16_t apid  = ident & 0x07FF;
                int      type  = (ident >> 12) & 0x01;
                uint16_t dlen  = (uint16_t)((((uint16_t)rsp[4] << 8) | rsp[5]) + 1);
                printf("[TM] APID=0x%03X %s frame=%dB data=%dB\n",
                       apid, type ? "TC" : "TM", n, dlen);
                printf("  ");
                for (int i = 0; i < n; i++) printf("%02X ", rsp[i]);
                printf("\n");
            } else {
                printf("[TM?] len=%d: ", n);
                for (int i = 0; i < n; i++) printf("%02X ", rsp[i]);
                printf("\n");
            }
            fflush(stdout);
        }
        close_ept(fd);
        return 0;
    }
    else if (!strcmp(cmd, "ccsds")) {
        /* Build and transmit a CCSDS SPP packet (6-byte header + payload).
         * Usage: radio_test ccsds <apid_hex> [texto]
         * Example: radio_test ccsds 001 "hola"
         * Sends: [ver=0,type=TC(1),sec=0,apid][seq_flags=3(unsg),seq=0][len-1][payload]
         */
        uint16_t apid;
        uint8_t  hdr[6];
        int      plen;
        uint16_t ident, seq, dlen_be;

        if (need_arg(argc >= 3, "apid_hex")) return 2;
        apid = (uint16_t)(strtoul(argv[2], NULL, 16) & 0x07FF);

        /* SPP data field must be >= 1 byte; with no text send one 0x00 pad */
        plen = (argc >= 4) ? (int)strlen(argv[3]) : 0;
        if (plen > 250) plen = 250;

        /* Build SPP primary header (big-endian) */
        ident  = (0x0 << 13) | (0x1 << 12) | (0x0 << 11) | apid;  /* TC type */
        seq    = (0x3 << 14) | 0x0000;                              /* unsegmented, count=0 */
        dlen_be = (uint16_t)((plen > 0 ? plen : 1) - 1);            /* CCSDS length = payload-1 */

        hdr[0] = (uint8_t)(ident >> 8);
        hdr[1] = (uint8_t)ident;
        hdr[2] = (uint8_t)(seq >> 8);
        hdr[3] = (uint8_t)seq;
        hdr[4] = (uint8_t)(dlen_be >> 8);
        hdr[5] = (uint8_t)dlen_be;

        memcpy(&req[3], hdr, 6);                       /* SPP header: req[3..8] */
        if (plen > 0)
            memcpy(&req[9], argv[3], plen);            /* payload AFTER the 6-byte header */
        else {
            req[9] = 0x00; plen = 1;                   /* pad byte, matches dlen_be = 0 */
        }
        req[0] = 0x0D;
        req[2] = (uint8_t)(6 + plen);                  /* total LoRa payload = header + text */
        len = 3 + 6 + plen; tmo = 6000;

        printf("ccsds: SPP header:");
        for (int i = 0; i < 6; i++) printf(" %02x", hdr[i]);
        if (plen) printf(" | payload=\"%s\"", argv[3]);
        printf(" (%d bytes total via LoRa)\n", 6 + plen);
    }
    else if (!strcmp(cmd, "rx")) {
        int rx_ms = (argc >= 3) ? atoi(argv[2]) : 10000;
        int total_timeout = (rx_ms == 0) ? 3600000 : rx_ms + 1500;
        req[0] = 0x0E; req[2] = (uint8_t)(rx_ms >> 8); req[3] = (uint8_t)rx_ms; len = 4;

        fd = open_ept();
        if (fd < 0) return 1;
        n = xfer(fd, req, len, rsp, sizeof(rsp), 3000);
        if (n < 2 || rsp[1] != 0) {
            if (n >= 2 && rsp[1] == RADIO_ERR_NOT_INITED)
                fprintf(stderr, "radio sin configurar. Corre primero: radio_test init 915000000\n");
            else
                fprintf(stderr, "rx_start fallo (n=%d err=0x%02x)\n", n, n >= 2 ? rsp[1] : 0xFF);
            close_ept(fd); return 1;
        }
        if (rx_ms)
            printf("rx: escuchando radio %d por hasta %d segundos...\n",
                   inst, (rx_ms + 999) / 1000);
        else
            printf("rx: escuchando radio %d (continuo, Ctrl-C para salir)...\n", inst);
        /* Loop: receive ALL packets within the window */
        for (;;) {
            int rc = wait_rx_event(fd, total_timeout);
            if (rc != 0) break;  /* timeout or bad CRC */
            /* got a good packet — keep listening for more */
        }
        /* If we get here, the MCU will timeout via s_rx_deadline and
         * send EVT_RX_TIMEOUT, or user Ctrl-C'd */
        close_ept(fd);
        return 0;
    }
    else if (!strcmp(cmd, "loopback")) {
        /* Self-contained on-board test: radio0 transmits, radio1 receives.
         * All I/O on one fd so replies and the RX event never race two procs. */
        uint32_t f;
        const char *text;
        uint8_t m[512];
        int i, plen, got_evt = 0, rc = 1;
        int txi = 0, rxi = 1;   /* optional: loopback <freq> <text> [txinst rxinst] */

        if (need_arg(argc >= 4, "freq_hz> <texto")) return 2;
        f = (uint32_t)strtoul(argv[2], NULL, 10);
        text = argv[3];
        if (argc >= 6) { txi = atoi(argv[4]); rxi = atoi(argv[5]); }
        if (txi < 0 || txi > 1 || rxi < 0 || rxi > 1 || txi == rxi) {
            fprintf(stderr, "error: tx/rx deben ser 0 y 1 distintos.\n"); return 2;
        }
        plen = (int)strlen(text); if (plen > 250) plen = 250;

        fd = open_ept();
        if (fd < 0) return 1;

        /* init both radios */
        for (i = 0; i < 2; i++) {
            uint8_t r[8] = { 0x05, (uint8_t)i, f>>24, f>>16, f>>8, f };
            n = xfer(fd, r, 6, m, sizeof(m), 6000);
            if (n < 2 || m[1] != 0) { fprintf(stderr, "init radio %d fallo (err=0x%02x)\n", i, n>=2?m[1]:0xFF); close_ept(fd); return 1; }
            printf("loopback: radio %d init @ %u Hz OK\n", i, f);
        }
        /* rx radio listens (8 s window) */
        { uint8_t r[4] = { 0x0E, (uint8_t)rxi, (8000>>8)&0xFF, 8000&0xFF };
          n = xfer(fd, r, 4, m, sizeof(m), 3000);
          if (n < 2 || m[1] != 0) { fprintf(stderr, "rx_start radio %d fallo\n", rxi); close_ept(fd); return 1; } }
        printf("loopback: radio %d escuchando; radio %d transmite \"%s\"...\n", rxi, txi, text);
        /* tx radio transmits (reply arrives after TX_DONE) */
        { uint8_t r[512] = { 0x0D, (uint8_t)txi, (uint8_t)plen }; memcpy(&r[3], text, plen);
          n = xfer(fd, r, 3 + plen, m, sizeof(m), 6000); }
        /* now demux messages: TX reply [0x0D] and RX event [0xE0] */
        for (i = 0; i < 6; i++) {
            if (n <= 0) { n = rx_msg(fd, m, sizeof(m), 9000); continue; }
            if (m[0] == 0x0D) printf("loopback: TX radio %d err=0x%02x\n", txi, n>=2?m[1]:0xFF);
            else if (m[0] == EVT_RX && n >= 7) {
                int L = m[3], j; int16_t rssi = (int16_t)(((uint16_t)m[4]<<8)|m[5]); int8_t snr=(int8_t)m[6];
                printf("loopback: RX radio %d len=%d rssi=%d dBm snr=%d crc=%s payload=\"",
                       m[1], L, rssi, snr, (m[2]&1)?"ok":"BAD");
                for (j = 0; j < L && 7+j < n; j++) putchar(isprint(m[7+j])?m[7+j]:'.');
                printf("\"\n");
                got_evt = 1; rc = (m[2]&1)?0:1; break;
            }
            else if (m[0] == EVT_RX_TIMEOUT) { printf("loopback: RX timeout (radio 1 no recibio)\n"); break; }
            n = rx_msg(fd, m, sizeof(m), 9000);
        }
        if (!got_evt && rc) fprintf(stderr, "loopback: sin evento de RX\n");
        close_ept(fd);
        return rc;
    }
    else {
        fprintf(stderr, "error: comando desconocido '%s'. Usa 'radio_test help'.\n", cmd);
        return 2;
    }

    fd = open_ept();
    if (fd < 0) return 1;

    n = xfer(fd, req, len, rsp, sizeof(rsp), tmo);
    close_ept(fd);
    if (n < 2) { fprintf(stderr, "error: respuesta invalida (n=%d)\n", n); return 1; }

    if (rsp[1] == RADIO_ERR_NOT_INITED) {
        fprintf(stderr, "radio sin configurar (la configuracion se pierde al apagar/reset).\n"
                        "Corre primero:  radio_test init 915000000\n");
        return 1;
    }

    printf("%-7s  %s", cmd, radio_err_str(rsp[1]));
    if (req[0] == 0x01 && n >= 6) printf(", id=%c%c%c%c v%d", rsp[2], rsp[3], rsp[4], rsp[5], rsp[6]);
    if (req[0] == 0x05 && rsp[1] == 0x00) {   /* init: show config details */
        uint32_t f = ((uint32_t)req[2] << 24) | ((uint32_t)req[3] << 16) |
                     ((uint32_t)req[4] << 8) | req[5];
        if (len >= 11)
            printf(", freq=%lu Hz, SF=%d, BW=%d kHz, CR=%d, power=%d dBm, CRC=off",
                   (unsigned long)f, req[6], ((int)req[7]<<8)|req[8], req[9], (int8_t)req[10]);
        else
            printf(", freq=%lu Hz, SF=7, BW=250 kHz, CR=4/5, preamble=8, "
                   "power=20 dBm, sync=privada 0x1424, CRC=on (defaults)",
                   (unsigned long)f);
    }
    if ((req[0] == 0x02 || req[0] == 0x0A) && n >= 3)
        printf(", status=0x%02x mode=%d%s", rsp[2], (rsp[2] >> 4) & 7,
               (((rsp[2] >> 4) & 7) == 6) ? " (TX)" :
               (((rsp[2] >> 4) & 7) == 5) ? " (RX)" :
               (((rsp[2] >> 4) & 7) == 2) ? " (standby)" : "");
    if (req[0] == 0x03 && n >= 3) printf(", val=0x%02x", rsp[2]);
    if (req[0] == 0x0B && n >= 4) printf(", dev_errors=0x%02x%02x%s", rsp[2], rsp[3],
                                          (rsp[2] | rsp[3]) ? " (ERROR)" : " (sano)");
    if (req[0] == 0x12 && n >= 5) printf(", rssi=%d dBm snr=%d", (int16_t)((rsp[2]<<8)|rsp[3]), (int8_t)rsp[4]);
    if (req[0] == 0x13 && n >= 4) printf(", rssi=%d dBm", (int16_t)((rsp[2]<<8)|rsp[3]));
    if (req[0] == 0x14 && rsp[1] == 0x00) {
        uint16_t sw = ((uint16_t)req[2] << 8) | req[3];
        printf(", sync=0x%04X (%s)", sw,
               sw == 0x3444 ? "publica/LoRaWAN" :
               sw == 0x1424 ? "privada, default" : "custom");
    }
    printf("\n");

    return rsp[1] ? 1 : 0;
}
