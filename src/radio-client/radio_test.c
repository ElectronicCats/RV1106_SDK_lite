/*
 * CubeSat — cliente del RadioService (SX1262 ×2 en el MCU RISC-V, via /dev/rpmsg).
 *
 * La radio fisica la controla el firmware RT-Thread del MCU; esta herramienta
 * le manda comandos por IPC (rpmsg). El canal se liga en el arranque (rcS).
 * Ver docs/riscv-migration/{40-diseno-migracion,90-mcu-configuracion...}.md.
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

#define RADIO_ERR_NOT_INITED 0x10   /* la radio no esta configurada */
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
"  init <freq_hz>      Inicializacion completa: standby, DC-DC, LoRa SF7/BW125,\n"
"                      calibracion, frecuencia y 14 dBm. Ej: init 915000000\n"
"  freq <freq_hz>      Cambia solo la frecuencia (requiere init previo).\n"
"  power <dbm>         Potencia TX en dBm, -9 a 22 (requiere init previo).\n"
"  cw                  Portadora continua ON (requiere init). Espectrometro.\n"
"  stop                Standby: apaga portadora/RX. El init se conserva.\n"
"  status              Estado. mode: 2=standby, 4=FS, 5=RX, 6=TX.\n"
"  errors              Errores internos del chip (0x0000 = sano).\n"
"  reg <addr_hex>      Lee un registro. Ej: reg 0740 (debe dar 0x14).\n"
"  antsw <0|1|2>       Switch de antena: 0=auto, 1=TX, 2=RX.\n"
"\n"
"COMANDOS DE PAQUETES (LoRa, requieren init previo):\n"
"  tx <texto>          Transmite un paquete LoRa con <texto>. Bloquea hasta\n"
"                      TX_DONE. Ej: radio_test tx \"hola mundo\"\n"
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

/* Wait for and print an EVT_RX / EVT_RX_TIMEOUT, skipping any late command
 * reply that may still be queued ahead of the event. */
static int wait_rx_event(int fd, int tmo_ms)
{
    uint8_t ev[512];
    int deadline_loops = 0;

    for (;;) {
        int n = rx_msg(fd, ev, sizeof(ev), tmo_ms);
        if (n <= 0) { fprintf(stderr, "rx: sin evento (timeout).\n"); return 1; }

        if (ev[0] == EVT_RX && n >= 7) {
            int len = ev[3];
            int16_t rssi = (int16_t)(((uint16_t)ev[4] << 8) | ev[5]);
            int8_t snr = (int8_t)ev[6];
            int i;

            printf("rx: radio=%d len=%d rssi=%d dBm snr=%d crc=%s payload=\"",
                   ev[1], len, rssi, snr, (ev[2] & 1) ? "ok" : "BAD");
            for (i = 0; i < len && 7 + i < n; i++)
                putchar(isprint(ev[7 + i]) ? ev[7 + i] : '.');
            printf("\"\n");
            return (ev[2] & 1) ? 0 : 1;
        }
        if (ev[0] == EVT_RX_TIMEOUT) {
            printf("rx: radio=%d timeout (ventana de escucha agotada, sin paquete)\n",
                   n >= 2 ? ev[1] : -1);
            return 1;
        }
        /* Not our event (a stray reply) — keep waiting, but don't loop forever. */
        if (++deadline_loops > 4) { fprintf(stderr, "rx: evento inesperado 0x%02x\n", ev[0]); return 1; }
    }
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
    else if (!strcmp(cmd, "init") || !strcmp(cmd, "freq")) {
        if (need_arg(argc >= 3, "freq_hz")) return 2;
        freq = (uint32_t)strtoul(argv[2], NULL, 10);
        if (freq < 150000000U || freq > 960000000U) {
            fprintf(stderr, "error: frecuencia fuera de rango (150000000-960000000 Hz).\n");
            return 2;
        }
        req[0] = strcmp(cmd, "init") ? 0x06 : 0x05;
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
    else if (!strcmp(cmd, "tx")) {
        int plen;
        if (need_arg(argc >= 3, "texto")) return 2;
        plen = (int)strlen(argv[2]);
        if (plen > 250) plen = 250;
        req[0] = 0x0D; req[2] = (uint8_t)plen;
        memcpy(&req[3], argv[2], plen);
        len = 3 + plen; tmo = 6000;   /* handler blocks until TX_DONE */
    }
    else if (!strcmp(cmd, "rx")) {
        int rx_ms = (argc >= 3) ? atoi(argv[2]) : 10000;
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
        printf("rx: escuchando radio %d %s...\n", inst,
               rx_ms ? "" : "(continuo, Ctrl-C para salir)");
        if (rx_ms == 0) {
            /* continuous: the MCU re-arms after each packet; keep printing. */
            for (;;)
                (void)wait_rx_event(fd, 3600000);
        }
        n = wait_rx_event(fd, rx_ms + 1500);
        close_ept(fd);
        return n;
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

    printf("%s: err=0x%02x", cmd, rsp[1]);
    if (req[0] == 0x01 && n >= 6) printf(" id=%c%c%c%c v%d", rsp[2], rsp[3], rsp[4], rsp[5], rsp[6]);
    if ((req[0] == 0x02 || req[0] == 0x0A) && n >= 3)
        printf(" status=0x%02x mode=%d%s", rsp[2], (rsp[2] >> 4) & 7,
               (((rsp[2] >> 4) & 7) == 6) ? " (TX)" :
               (((rsp[2] >> 4) & 7) == 5) ? " (RX)" :
               (((rsp[2] >> 4) & 7) == 2) ? " (standby)" : "");
    if (req[0] == 0x03 && n >= 3) printf(" val=0x%02x", rsp[2]);
    if (req[0] == 0x0B && n >= 4) printf(" dev_errors=0x%02x%02x%s", rsp[2], rsp[3],
                                          (rsp[2] | rsp[3]) ? " (hay errores)" : " (sano)");
    printf("\n");

    return rsp[1] ? 1 : 0;
}
