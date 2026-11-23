/* PC test of boot.c's HTTP upload path: boot.c is compiled unchanged against
 * fake chip headers (inc/), the WiFi module is played by this file. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
uint8_t PORTA, DDRA, PORTB, DDRB, PINB, PORTD, PIND = 0xff, GPIOR0, GPIOR1, MCUSR;
uint8_t *fake_flash; uint8_t fake_ee[4096];
static uint8_t fill_buf[256];
void fake_erase(uint32_t a) { memset(fake_flash + a, 0xff, 256); }
void fake_fill(uint32_t a, uint16_t w) { fill_buf[a & 255] = (uint8_t)w; fill_buf[(a & 255) + 1] = (uint8_t)(w >> 8); }
void fake_write(uint32_t a) { if (a >= 0x1E000) { printf("FAIL wrote into the boot section\n"); exit(1); } memcpy(fake_flash + a, fill_buf, 256); }
void fake_restart(void) { printf("FAIL restart called\n"); exit(1); }

/* the module: one message body at a time */
static uint8_t msg[4096];
#include "winc.h"
/* As strict as the real winc.c: no reading past the end of the message
 * (this is what the first chip run tripped over: a 16-byte read of a 4-byte
 * bind reply, refused). */
static uint16_t msg_len;
uint8_t winc_receive(uint32_t addr, void *buf, uint16_t n, uint8_t done)
{ (void)done; if (addr + n > msg_len) return 0; memcpy(buf, msg + addr, n); return 1; }
static char sent[2048]; static int sends; static uint8_t last_op, last_ctrl[16];
uint8_t winc_send(uint8_t g, uint8_t op, const void *ctrl, uint16_t cl, const void *data, uint16_t dl, uint16_t off)
{ (void)g; (void)off; sends++; last_op = op & 0x7f; memcpy(last_ctrl, ctrl, cl < 16 ? cl : 16);
  if ((op & 0x7f) == 0x45 && data) { memcpy(sent, data, dl); sent[dl] = 0; } return 1; }
uint8_t winc_start(void) { return 1; }
uint8_t winc_poll(winc_handler_t h) { (void)h; return 0; }
void winc_hold(void) {} void winc_release(void) {} uint8_t winc_holding(void) { return 0; }
const char *winc_error = "";

#define main boot_main
#define naked noinline
#define section(x) used
#define __asm__
#define __volatile__(x) (void)0
#include "boot.c"
#undef main
#undef section

static int fails;
#define CHECK(what, cond) do { int ok_ = (cond); printf("%s %s\n", ok_ ? "ok  " : "FAIL", what); if (!ok_) fails++; } while (0)

/* menu watching: one record per 100 ms step (PORTA as shown during it, and
 * whether the heartbeat was toggled at its start); release after N steps */
static int watching, steps, release_after; static uint8_t shown[400], beat[400];
void fake_delay(double ms)
{ if (!watching || ms < 100) return;
  shown[steps] = PORTA; beat[steps] = PINB != 0; PINB = 0;
  if (++steps >= release_after) PIND |= 4; }
static uint8_t menu(int hold_steps)
{ uint8_t c; steps = 0; release_after = hold_steps; PIND &= (uint8_t)~4; PINB = 0; watching = 1; c = button_menu(); watching = 0; return c; }

static uint32_t crc_ref(const uint8_t *p, uint32_t n)
{ uint32_t c = 0xffffffff; while (n--) { c ^= *p++; for (int i = 0; i < 8; i++) c = (c >> 1) ^ (0xEDB88320u & -(c & 1)); } return ~c; }

static void deliver(uint8_t group, uint8_t op, uint16_t len) { msg_len = len; on_message(group, op, len, 0); }

static void accept(uint8_t sock)
{ memset(msg, 0, 16); msg[8] = 0; msg[9] = sock; deliver(WINC_GROUP_IP, SOCKET_CMD_ACCEPT, 12); }

static void recv_piece(const uint8_t *data, int n)
{ memset(msg, 0, 16); msg[8] = (uint8_t)n; msg[9] = (uint8_t)(n >> 8); msg[10] = 16; msg[12] = (uint8_t)client;
  msg[14] = (uint8_t)client_session; msg[15] = (uint8_t)(client_session >> 8); memcpy(msg + 16, data, n);
  deliver(WINC_GROUP_IP, SOCKET_CMD_RECV, (uint16_t)(16 + n)); }

/* send a whole request in random pieces of 1..1400 bytes, as TCP may */
static void request(const uint8_t *req, int n)
{ int at = 0; while (at < n && h_state < H_VERIFY) { int k = 1 + rand() % 1400; if (k > n - at) k = n - at; recv_piece(req + at, k); at += k; }
  if (h_state == H_VERIFY) verify(); if (reply) send_reply(); }

static uint8_t *req;
static int build(const char *method_path, const char *pass, const uint8_t *img, uint32_t len, uint32_t crc, int lowercase)
{ int n = sprintf((char *)req, "%s HTTP/1.1\r\nHost: 192.168.1.193\r\n%s: %u\r\n%s: %08x\r\n%s%s%s\r\n",
      method_path, lowercase ? "content-length" : "Content-Length", (unsigned)len, lowercase ? "x-crc32" : "X-Crc32", (unsigned)crc,
      pass ? (lowercase ? "x-pass: " : "X-Pass: ") : "", pass ? pass : "", pass ? "\r\n" : "");
  memcpy(req + n, img, len); return n + (int)len; }

static void set_creds(const char *cp)
{ struct boot_creds c; memset(&c, 0, sizeof c); c.magic = BOOT_CREDS_MAGIC; strcpy(c.ssid, "homenet"); strcpy(c.pass, "x"); strcpy(c.console_pass, cp);
  c.sum = BOOT_CREDS_SUM(&c); memcpy(fake_ee + 0xF00, &c, sizeof c); memcpy(&creds, &c, sizeof c); }

int main(void)
{
  uint8_t *img = malloc(0x1E000); uint32_t len = 109649; int n;
  fake_flash = malloc(0x20000); req = malloc(200000);
  srand(7);
  for (uint32_t i = 0; i < 0x1E000; i++) img[i] = (uint8_t)rand();
  { uint32_t c = 0xffffffff; for (uint32_t i = 0; i < len; i++) c = crc32_update(c, img[i]); CHECK("nibble-table CRC32 = bitwise reference", (c ^ 0xffffffff) == crc_ref(img, len)); }
  memset(fake_flash, 0xAA, 0x20000);   /* "the old program" + bootloader */
  memset(fake_ee, 0xff, sizeof fake_ee);
  set_creds("sesame");

  /* joining and opening port 80, with replies of their real lengths */
  memset(msg, 0, 16); msg[0] = 1; deliver(WINC_GROUP_WIFI, WIFI_RESP_CON_STATE, 4);
  deliver(WINC_GROUP_WIFI, WIFI_REQ_DHCP_CONF, 20);
  CHECK("joined and has an address", joined && have_address);
  ask_listen();
  CHECK("asks to bind port 80", last_op == SOCKET_CMD_BIND && last_ctrl[2] == 0 && last_ctrl[3] == 80 && listen_asked);
  memset(msg, 0, 16); msg[0] = LISTEN_SOCK; msg[1] = 0; msg[2] = last_ctrl[10]; msg[3] = last_ctrl[11];
  deliver(WINC_GROUP_IP, SOCKET_CMD_BIND, 4);
  CHECK("4-byte bind reply understood -> asks to listen", last_op == SOCKET_CMD_LISTEN);
  deliver(WINC_GROUP_IP, SOCKET_CMD_LISTEN, 4);
  CHECK("4-byte listen reply understood -> listening (steady yellow)", listening == 1);
  memset(msg, 0, 16); msg[1] = 0xff; deliver(WINC_GROUP_IP, SOCKET_CMD_BIND, 4);
  CHECK("a refused bind -> will ask again", listen_asked == 0);
  listen_asked = 1;

  /* GETs */
  accept(3); n = sprintf((char *)req, "GET /boot HTTP/1.1\r\nHost: x\r\n\r\n"); request(req, n);
  CHECK("GET /boot -> 200 bootloader", strstr(sent, "HTTP/1.0 200 ") == sent && strstr(sent, "\r\n\r\nbootloader\n"));
  sent_reply = 0; memset(msg, 0, 16); deliver(WINC_GROUP_IP, SOCKET_CMD_SEND, 8);
  CHECK("8-byte send reply understood", sent_reply == 1);
  client = -1; accept(4); n = sprintf((char *)req, "GET /log?from=0 HTTP/1.1\r\n\r\n"); request(req, n);
  CHECK("GET /log -> 503", strncmp(sent, "HTTP/1.0 503 ", 13) == 0);

  /* wrong password: nothing touched */
  client = -1; accept(5); n = build("POST /flash", "nope", img, len, crc_ref(img, len), 0); request(req, n);
  CHECK("wrong password -> 403", strncmp(sent, "HTTP/1.0 403 ", 13) == 0);
  CHECK("  flash untouched, not marked incomplete", fake_flash[0] == 0xAA && fake_ee[0xFF1] == 0xff);
  /* missing password when one is set */
  client = -1; accept(5); n = build("POST /flash", NULL, img, len, crc_ref(img, len), 0); request(req, n);
  CHECK("no password -> 403", strncmp(sent, "HTTP/1.0 403 ", 13) == 0);
  /* too big */
  client = -1; accept(5); n = build("POST /flash", "sesame", img, 0x1E001, 0, 0); request(req, n);
  CHECK("122,881 bytes -> 400", strncmp(sent, "HTTP/1.0 400 ", 13) == 0 && fake_flash[0] == 0xAA);

  /* a spare idle connection is replaced by the real one */
  client = -1; accept(6); accept(2);
  CHECK("idle spare connection replaced by the newcomer", client == 2);

  /* the real upload (lower-case header names, as a browser may send) */
  n = build("POST /flash", "sesame", img, len, crc_ref(img, len), 1); request(req, n);
  CHECK("good upload -> 200", strncmp(sent, "HTTP/1.0 200 ", 13) == 0);
  CHECK("  every byte written", memcmp(fake_flash, img, len) == 0);
  CHECK("  rest of the last page is 0xff", fake_flash[len] == 0xff && fake_flash[(len | 255)] == 0xff);
  CHECK("  pages past the image untouched", fake_flash[(len | 255) + 1] == 0xAA);
  CHECK("  boot section untouched", fake_flash[0x1E000] == 0xAA && fake_flash[0x1FFFF] == 0xAA);
  CHECK("  marked complete, will restart into it", fake_ee[0xFF1] == 0xff && finished == 1 && !failed);

  /* corrupted in transit: wrong CRC claimed */
  client = -1; finished = 0; accept(1); n = build("POST /flash", "sesame", img, 5000, 0x12345678, 0); request(req, n);
  CHECK("CRC mismatch -> 500, stays incomplete, red", strncmp(sent, "HTTP/1.0 500 ", 13) == 0 && fake_ee[0xFF1] == BOOT_INCOMPLETE && failed && !finished);

  /* the sender gives up halfway (Ctrl-C): stays incomplete, red, nothing past the cut written */
  client = -1; failed = 0; finished = 0; accept(1); n = build("POST /flash", "sesame", img, len, crc_ref(img, len), 0);
  { int cut = n - 60000; int at = 0; while (at < cut) { int k = cut - at > 1400 ? 1400 : cut - at; recv_piece(req + at, k); at += k; } }
  CHECK("upload cut off midway -> still receiving", h_state == H_BODY && client == 1);
  memset(msg, 0, 16); msg[8] = 0; msg[12] = 1; msg[14] = (uint8_t)client_session; msg[15] = (uint8_t)(client_session >> 8);
  deliver(WINC_GROUP_IP, SOCKET_CMD_RECV, 16);   /* status 0: the sender closed */
  CHECK("  sender left -> red, stays incomplete, not finished", failed == 1 && fake_ee[0xFF1] == BOOT_INCOMPLETE && !finished && client == -1);
  /* and the empty-POST password check the ota tool now does: no flash touched */
  failed = 0; memset(fake_flash, 0xAA, 4096); client = -1; accept(2);
  n = sprintf((char *)req, "POST /flash HTTP/1.1\r\nContent-Length: 0\r\nX-Crc32: 00000000\r\nX-Pass: sesame\r\n\r\n"); request(req, n);
  CHECK("empty POST with the right password -> 400, flash untouched", strncmp(sent, "HTTP/1.0 400 ", 13) == 0 && fake_flash[0] == 0xAA);
  client = -1; accept(2);
  n = sprintf((char *)req, "POST /flash HTTP/1.1\r\nContent-Length: 0\r\nX-Crc32: 00000000\r\nX-Pass: nope\r\n\r\n"); request(req, n);
  CHECK("empty POST with a wrong password -> 403, flash untouched", strncmp(sent, "HTTP/1.0 403 ", 13) == 0 && fake_flash[0] == 0xAA);

  /* no console password set: none needed */
  set_creds(""); client = -1; accept(1); n = build("POST /flash", NULL, img, 3000, crc_ref(img, 3000), 0); request(req, n);
  CHECK("no consolepass set: upload without X-Pass -> 200", strncmp(sent, "HTTP/1.0 200 ", 13) == 0 && fake_ee[0xFF1] == 0xff);

  /* the button menu: 2 s steps, flash at every step start incl. the first,
   * no red counting, telnet dark, heartbeat every 0.2 s */
  CHECK("menu: release in the first 2 s -> cancel", menu(15) == 0);
  CHECK("  traffic flash right at the start (0-0.3 s), luck dark", shown[0] == 0x0e && shown[2] == 0x0e);
  CHECK("  then everything dark while cancel is offered (no red)", shown[3] == 0 && shown[10] == 0 && shown[14] == 0);
  { int ok = 1; for (int i = 0; i < 15; i++) ok &= beat[i] == (i % 2 == 0) && !(shown[i] & 1); CHECK("  heartbeat toggles every 0.2 s, telnet LED dark", ok); }
  CHECK("menu: release at 2.5 s -> 1 (clear password)", menu(25) == 1);
  CHECK("  flash + luck 1 at 2.0 s, luck 1 alone at 2.3 s", shown[20] == (0x0e | 0x10) && shown[23] == 0x10);
  CHECK("menu: release at 4.5 s -> 2 (WiFi setup)", menu(45) == 2 && shown[44] == 0x30);
  CHECK("menu: release at 7.9 s -> 3 (update)", menu(79) == 3 && shown[78] == 0x70 && shown[60] == 0x7e);
  CHECK("menu: release at 8.5 s -> wrapped to cancel, flash again at 8.0 s", menu(85) == 0 && shown[80] == 0x0e && shown[84] == 0);
  CHECK("menu: release at 10.5 s -> 1 again", menu(105) == 1);

  /* the progress bar macro: six lights, PA1..PA6 */
  CHECK("bar: BAR(0)=0, BAR(1)=green, BAR(3)=traffic, BAR(6)=all six", BAR(0) == 0 && BAR(1) == 0x02 && BAR(3) == 0x0e && BAR(6) == 0x7e);

  printf("%s\n", fails ? "SOME FAILED" : "ALL OK");
  return fails != 0;
}
