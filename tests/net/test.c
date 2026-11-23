/* PC test of net_winc.c's WiFi setup mode: net_winc.c compiled unchanged
 * against fake AVR headers, a fake module (this file) and fake settings.
 * The provisioning request is compared byte for byte with the one built
 * from Microchip's own 19.5.2 header (ref.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

uint8_t PORTA, DDRA, PORTB, DDRB, PINB, PORTD, PIND;
uint16_t SP = 0xffff;
uint8_t __heap_start;
uint8_t *fake_flash;

#include "settings.h"
struct settings settings;
static int saves;
uint8_t settings_changed;
static char saved_ssid[40], saved_pass[80];
uint8_t settings_command(const char *line)
{ if (strcmp(line, "save") == 0) { saves++; strcpy(saved_ssid, settings.ssid); strcpy(saved_pass, settings.pass); } return 1; }
/* settings.c's rule for wifi.ssid= and wifi.password=, in short */
static int wifi_checks;
uint8_t settings_wifi_ok(const char *ssid, const char *pass)
{
    size_t s = strlen(ssid), p = strlen(pass);
    wifi_checks++;
    for (const char *c = ssid; *c; c++) if (*c < ' ' || *c > '~') return 0;
    for (const char *c = pass; *c; c++) if (*c < ' ' || *c > '~') return 0;
    return s >= 1 && s <= 32 && (p == 0 || (p >= 8 && p <= 64));
}

/* uart */
static char out[20000]; static int out_n;
void uart_puts(const char *s) { while (*s && out_n < (int)sizeof out - 1) out[out_n++] = *s++; out[out_n] = 0; }
void uart_putc(char c) { char s[2] = { c, 0 }; uart_puts(s); }
void uart_put_u32(uint32_t v) { char s[12]; sprintf(s, "%lu", (unsigned long)v); uart_puts(s); }
uint8_t uart_inject_line(const char *s) { (void)s; return 1; }

/* board */
#include "board.h"
volatile uint8_t board_beat_due;
void board_beat_now(void) { board_beat_due = 0; }
void board_telnet(uint8_t on) { (void)on; }
static int light_states[4];
void board_setup_light(uint8_t state, uint8_t on) { (void)on; light_states[state]++; }

/* ---- the fake module ---- */
#include "winc.h"
const char *winc_error = ""; uint32_t winc_chip_id, winc_version; uint8_t winc_mac[6];
uint16_t winc_bus_errors, winc_send_no_room; uint32_t winc_last_value; uint8_t winc_seen[12], winc_seen_n;
static int starts, connects; static uint8_t prov_req[256]; static int prov_len, prov_op;
static uint8_t join_req[108];
struct m { uint8_t group, op; uint8_t b[128]; uint16_t len; };
static struct m queue[8]; static int q_head, q_tail;
static uint8_t msg[128]; static uint16_t msg_len;
static void enqueue(uint8_t g, uint8_t op, const void *b, uint16_t len)
{ struct m *x = &queue[q_tail++ % 8]; x->group = g; x->op = op; memcpy(x->b, b, len); x->len = len; }

/* what the "phone" types, round by round */
struct page { const char *ssid, *pass; uint8_t type, status; };
static const struct page pages[] = {
    { "NewNet", "wrongpass",  2, 0 },     /* typo: join fails -> nothing saved, page again */
    { "NewNet", "rightpass",  2, 1 },     /* the page itself reports a failure */
    { "OldWEP", "rightpass",  3, 0 },     /* WEP: refused without trying */
    { "Net\x1b[2J", "rightpass", 2, 0 },  /* a control character (escape sequence): refused without trying */
    { "NewNet", "rightpass",  2, 0 },     /* works -> saved */
};
static int page_no;
static char snap_ssid[40], snap_pass[80]; static int snap_saves = -1;


uint8_t winc_start(void) { starts++; q_head = q_tail = 0; return 1; }
uint8_t winc_send(uint8_t group, uint8_t op, const void *ctrl, uint16_t ctrl_len,
                  const void *data, uint16_t data_len, uint16_t data_offset)
{
    (void)data; (void)data_len; (void)data_offset;
    if (group == WINC_GROUP_WIFI && (op & 0x7f) == 8) {
        uint8_t info[100] = { 0 };
        if (page_no == 1) { strcpy(snap_ssid, settings.ssid); strcpy(snap_pass, settings.pass); snap_saves = saves; }
        const struct page *p = &pages[page_no++];
        memcpy(prov_req, ctrl, ctrl_len); prov_len = ctrl_len; prov_op = op;
        strcpy((char *)&info[0], p->ssid); strcpy((char *)&info[33], p->pass);
        info[98] = p->type; info[99] = p->status;
        enqueue(WINC_GROUP_WIFI, 9, info, 100);
    } else if (group == WINC_GROUP_WIFI && op == 40) {
        uint8_t st[4] = { 0 };
        connects++; memcpy(join_req, ctrl, 108);
        if (strcmp((char *)&join_req[70], "NewNet") == 0 && strcmp((char *)&join_req[0], "rightpass") == 0) {
            uint8_t dhcp[20] = { 192, 168, 1, 193, 192, 168, 1, 1 };
            st[0] = 1; enqueue(WINC_GROUP_WIFI, 44, st, 4); enqueue(WINC_GROUP_WIFI, 50, dhcp, 20);
        } else {
            st[0] = 0; st[1] = 3; enqueue(WINC_GROUP_WIFI, 44, st, 4);    /* authentication failed */
        }
    }
    return 1;
}
uint8_t winc_poll(winc_handler_t h)
{ if (q_head == q_tail) return 0; struct m *x = &queue[q_head++ % 8]; memcpy(msg, x->b, x->len); msg_len = x->len; h(x->group, x->op, x->len, 0); return 1; }
uint8_t winc_receive(uint32_t addr, void *buf, uint16_t n, uint8_t done)
{ (void)done; if (addr + n > msg_len) return 0; memcpy(buf, msg + addr, n); return 1; }
void winc_hold(void) {} void winc_release(void) {} uint8_t winc_holding(void) { return 0; }

#include "net_winc.c"

void ref_prov(unsigned char out[204]);
static int fails;
#define CHECK(what, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) fails++; } while (0)

int main(void)
{
    unsigned char ref[204];
    int r;

    strcpy(settings.ssid, "homenet"); strcpy(settings.pass, "oldpass"); strcpy(settings.worker, "megaminer");
    r = net_wifi_setup();

    ref_prov(ref);
    CHECK("returns 1 once the new details work", r == 1);
    CHECK("request = Microchip's tstrM2MProvisionModeConfig, byte for byte (204)", prov_len == 204 && memcmp(prov_req, ref, 204) == 0);
    CHECK("  sent as opcode 8 with the data-packet flag", prov_op == (8 | 0x80));
    CHECK("module restarted for each of the 5 rounds", starts == 5);
    CHECK("joins tried: the typo and the good one (not the page failure, WEP or the control character)", connects == 2);
    CHECK("details checked like wifi.ssid= / wifi.password= (4 rounds got that far)", wifi_checks == 4);
    CHECK("console said the control-character details were refused", strstr(out, "would refuse") != NULL);
    CHECK("  join request carries the new name, password, WPA2", strcmp((char *)&join_req[70], "NewNet") == 0 && strcmp((char *)&join_req[0], "rightpass") == 0 && join_req[65] == 2);
    CHECK("saved exactly once, after the good join", saves == 1 && strcmp(saved_ssid, "NewNet") == 0 && strcmp(saved_pass, "rightpass") == 0);
    CHECK("settings now hold the new details", strcmp(settings.ssid, "NewNet") == 0 && strcmp(settings.pass, "rightpass") == 0);
    CHECK("no reply buffer left pointing at the old stack", prov_info == 0);
    CHECK("lights: waited, tried, failed twice (typo, WEP), done once", light_states[BOARD_SETUP_WAITING] > 0 && light_states[BOARD_SETUP_TRYING] == 3 && light_states[BOARD_SETUP_FAILED] == 2 && light_states[BOARD_SETUP_DONE] == 1);
    CHECK("console said NOTHING SAVED after the typo", strstr(out, "NOTHING SAVED") != NULL);
    CHECK("console named WEP", strstr(out, "WEP") != NULL);
    CHECK("console said page failure", strstr(out, "reported a failure") != NULL);

    CHECK("after the typo (seen as round 2 starts): OLD details back, nothing saved",
          strcmp(snap_ssid, "homenet") == 0 && strcmp(snap_pass, "oldpass") == 0 && snap_saves == 0);
    printf("\n--- console output ---\n%s", out);
    printf("%s\n", fails ? "SOME FAILED" : "ALL OK");
    return fails != 0;
}
