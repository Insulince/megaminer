/*
 * miner_pc.c - a PC stand-in for the chip's hardware, so that the miners
 * themselves (miner.c with miner_sv2.c or miner_sv1.c, not copies) can be
 * run and tested on the PC:
 *
 *   make sim
 *   build/miner_sv2_sim HOST PORT WORKER SECONDS [console lines...]
 *   build/miner_sv1_sim HOST PORT WORKER SECONDS [console lines...]
 *
 * e.g.  build/miner_sv2_sim 127.0.0.1 34255 megaminer 20 sv2.authority_key=616f... save
 *
 * (build\miner_sv2_sim.exe on Windows.) It provides everything the miners expect
 * from uart.h, net.h, settings.h, entropy.h and the two platform_ functions,
 * using the PC's sockets (Winsock on Windows, BSD sockets elsewhere), clock
 * and random numbers. The extra arguments are fed in as if typed on the
 * serial console. The program exits after SECONDS.
 */

/* ---- the few things that differ between Windows and Linux/macOS ---- */
#ifdef _WIN32
#define _CRT_RAND_S                     /* before stdlib.h: declares rand_s */
#endif
#include <stdio.h>
#include <stdlib.h>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>

typedef SOCKET sock_t;
#define NO_SOCKET INVALID_SOCKET
#define close_socket closesocket

static void os_init(void) { WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa); }
static void pause_ms(unsigned ms) { Sleep(ms); }
static unsigned long os_millis(void) { return GetTickCount(); }
static unsigned char os_random_byte(void) { unsigned int r; rand_s(&r); return (unsigned char)r; }
static void receive_timeout(sock_t s, unsigned ms)
{
    DWORD t = ms;
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char *)&t, sizeof t);
}
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

typedef int sock_t;
#define NO_SOCKET (-1)
#define close_socket close

static FILE *urandom;
static void os_init(void)
{
    signal(SIGPIPE, SIG_IGN);           /* a closed connection = send() fails, not death */
    urandom = fopen("/dev/urandom", "rb");
}
static void pause_ms(unsigned ms) { usleep(ms * 1000u); }
static unsigned long os_millis(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (unsigned long)t.tv_sec * 1000ul + (unsigned long)(t.tv_nsec / 1000000);
}
static unsigned char os_random_byte(void)
{
    int c = urandom ? fgetc(urandom) : EOF;
    if (c == EOF) {
        printf("[sim] no /dev/urandom\n");
        exit(2);
    }
    return (unsigned char)c;
}
static void receive_timeout(sock_t s, unsigned ms)
{
    struct timeval t = { (time_t)(ms / 1000), (suseconds_t)((ms % 1000) * 1000) };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &t, sizeof t);
}
#endif

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "entropy.h"
#include "hashrate.h"
#include "net.h"

/* No lights, button or ADC on the PC. */
void board_init(void) {}
void board_lamp_test(void) {}
uint8_t board_recovery(void) { return 0; }
void watchdog_start(void) {}
void watchdog_report(void) {}
void watchdog_forget(void) {}
void watchdog_json(void) { printf(",\"restart\":\"unknown\",\"frozen\":0,\"stuck\":0"); }
void watchdog_restart(uint8_t why) { printf("(PC stand-in) watchdog restart, reason %u\n", why); exit(3); }
void board_setup_light(uint8_t state, uint8_t on) { (void)state; (void)on; }
uint8_t net_wifi_setup(void) { return 0; }
void board_luck(uint8_t zero_bits) { (void)zero_bits; }
void board_traffic(uint8_t colour) { (void)colour; }
void board_telnet(uint8_t on) { (void)on; }
void board_tick(void) {}
volatile uint8_t board_beat_due;
void board_beat_now(void) { board_beat_due = 0; }
uint16_t board_vcc_mv(void) { return 5000; }
#include "settings.h"
#include "uart.h"

struct settings settings;

static time_t  started;
static long    run_seconds;
static char  **typed;               /* remaining pretend console lines */
static int     typed_left;
static sock_t  sock = NO_SOCKET;
static uint8_t connected;

/* ---- platform ---- */

uint32_t platform_seconds(void)
{
    time_t now = time(NULL);

    if (now - started >= run_seconds) {
        printf("\n[sim] time is up\n");
        exit(0);
    }
    return (uint32_t)(now - started);
}

static unsigned long started_ms;

uint32_t platform_millis(void)
{
    if (!started_ms)
        started_ms = os_millis();
    return (uint32_t)(os_millis() - started_ms);
}

void platform_init(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    os_init();
}

/* ---- uart.h ---- */

void uart_init(void) {}
void (*uart_line_prefix)(void);
void uart_putc(char c)
{
    static int mid_line, in_prefix;

    if (!mid_line && !in_prefix && uart_line_prefix && c != '\r' && c != '\n') {
        in_prefix = 1;
        uart_line_prefix();
        in_prefix = 0;
    }
    if (!in_prefix)
        mid_line = (c != '\n');
    if (c != '\r')
        putchar(c);
}
void uart_puts(const char *s) { while (*s) uart_putc(*s++); }
void uart_put_u32(uint32_t n) { printf("%lu", (unsigned long)n); }
void uart_put_hex(const uint8_t *b, uint8_t len) { while (len--) printf("%02x", *b++); }
uint8_t uart_rx_errors(void) { return 0; }

const char *uart_line(void) { return typed_left ? *typed : 0; }
void uart_line_done(void) { typed++; typed_left--; }

/* ---- hashrate.h's storage: none on the PC ---- */

uint8_t platform_best_load(uint8_t out[HASHRATE_BEST_BYTES]) { (void)out; return 0; }
void platform_best_save(const uint8_t *best) { (void)best; }

/* ---- entropy.h ---- */

void (*entropy_idle)(void);

void entropy_start(void) {}
void entropy_raw(uint8_t out[ENTROPY_RAW_KEPT]) { memset(out, 0, ENTROPY_RAW_KEPT); }

void entropy_bytes(uint8_t out[32])
{
    for (int i = 0; i < 32; i++)
        out[i] = os_random_byte();
}

/* ---- settings.h (just enough) ---- */

void settings_load(void) {}

void settings_show(void)
{
    printf("  pool.address=%u.%u.%u.%u pool.port=%u pool.worker=%s sv2.check_cert=%u time_server=%u.%u.%u.%u\n",
           settings.server_ip[0], settings.server_ip[1], settings.server_ip[2],
           settings.server_ip[3], settings.server_port, settings.worker, settings.check_cert,
           settings.ntp_ip[0], settings.ntp_ip[1], settings.ntp_ip[2], settings.ntp_ip[3]);
}

static void parse_ip(const char *s, uint8_t out[4])
{
    unsigned a, b, c, d;

    if (sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d) != 4) {
        printf("[sim] bad address %s\n", s);
        exit(2);
    }
    out[0] = (uint8_t)a; out[1] = (uint8_t)b; out[2] = (uint8_t)c; out[3] = (uint8_t)d;
}

uint8_t settings_changed;

uint8_t settings_command(const char *line)
{
    printf("cmd: %s\n", line);
    if (strcmp(line, "save") == 0) {
        /* The sim's settings are only sv2.check_cert and time_server: pool settings. */
        settings_changed = SETTINGS_CHANGED_POOL;
        return SETTINGS_APPLY;
    }
    if (strncmp(line, "sv2.check_cert=", 15) == 0)
        settings.check_cert = (uint8_t)(line[15] == '1');
    else if (strncmp(line, "time_server=", 12) == 0)
        parse_ip(line + 12, settings.ntp_ip);
    else
        printf("[sim] unknown command\n");
    return SETTINGS_HANDLED;
}

/* ---- net.h ---- */

uint8_t net_init(void)
{
    if (sock != NO_SOCKET)
        close_socket(sock);
    sock = NO_SOCKET;
    connected = 0;
    return 1;
}

uint8_t net_link_up(void) { return 1; }
void net_service(void) { }
void net_report(void) { printf("net: (PC stand-in) socket %s\n", sock == NO_SOCKET ? "closed" : "open"); }
uint8_t net_connected(void) { return connected; }

void net_close(void)
{
    if (sock != NO_SOCKET)
        close_socket(sock);
    sock = NO_SOCKET;
    connected = 0;
}

void net_poll(void)
{
    struct sockaddr_in to = {0};

    if (sock != NO_SOCKET)
        return;
    connected = 0;
    to.sin_family = AF_INET;
    to.sin_port = htons(settings.server_port);
    memcpy(&to.sin_addr, settings.server_ip, 4);
    sock = socket(AF_INET, SOCK_STREAM, 0);
    if (connect(sock, (struct sockaddr *)&to, sizeof to) != 0) {
        close_socket(sock);
        sock = NO_SOCKET;
        pause_ms(500);
        return;
    }
    connected = 1;
}

uint16_t net_recv(uint8_t *buf, uint16_t max)
{
    fd_set set;
    struct timeval none = {0, 0};
    int n;

    if (sock == NO_SOCKET || max == 0)
        return 0;
    FD_ZERO(&set);
    FD_SET(sock, &set);
    if (select((int)sock + 1, &set, NULL, NULL, &none) <= 0)   /* Windows ignores the count */
        return 0;
    n = recv(sock, (char *)buf, max, 0);
    if (n <= 0) {                           /* the server closed the connection */
        net_close();
        return 0;
    }
    return (uint16_t)n;
}

uint8_t net_send(const uint8_t *buf, uint16_t len)
{
    while (len) {
        int n = sock == NO_SOCKET ? -1 : send(sock, (const char *)buf, len, 0);
        if (n <= 0)
            return 0;
        buf += n;
        len -= (uint16_t)n;
    }
    return 1;
}

uint16_t net_udp_exchange(const uint8_t ip[4], uint16_t port,
                          const uint8_t *req, uint16_t req_len,
                          uint8_t *resp, uint16_t resp_max)
{
    struct sockaddr_in to = {0};
    sock_t u = socket(AF_INET, SOCK_DGRAM, 0);
    int n;

    to.sin_family = AF_INET;
    to.sin_port = htons(port);
    memcpy(&to.sin_addr, ip, 4);
    receive_timeout(u, 2000);
    sendto(u, (const char *)req, req_len, 0, (struct sockaddr *)&to, sizeof to);
    n = recv(u, (char *)resp, resp_max, 0);
    close_socket(u);
    return n > 0 ? (uint16_t)n : 0;
}

/* ---- start: fill the settings, then run miner_sv2.c's main ---- */

int miner_main(void);

int main(int argc, char **argv)
{
    static const uint8_t ntp[4] = {216, 239, 35, 0};

    if (argc < 5) {
        printf("usage: miner_sv2_sim HOST PORT WORKER SECONDS [console lines...]\n");
        return 2;
    }
    parse_ip(argv[1], settings.server_ip);
    settings.server_port = (uint16_t)atoi(argv[2]);
    strncpy(settings.worker, argv[3], WORKER_MAX);
    run_seconds = atol(argv[4]);
    settings.check_cert = 1;
    memcpy(settings.ntp_ip, ntp, 4);
    typed = argv + 5;
    typed_left = argc - 5;
    started = time(NULL);
    return miner_main();
}
