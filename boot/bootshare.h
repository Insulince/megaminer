/*
 * bootshare.h - what the bootloader (boot.c) and the programs agree on.
 *
 * Flash: the program lives in 0x00000-0x1DFFF (120 KB), the bootloader in the
 * 8 KB boot section 0x1E000-0x1FFFF (high fuse BOOTSZ = 00). With the BOOTRST
 * fuse programmed (high fuse 0x90) every reset starts the bootloader, which
 * normally jumps straight to the program. Only code in the boot section may
 * write flash, which is why the bootloader, not the program, takes in a new
 * firmware.
 *
 * Start-up button menu (in the bootloader, so it works even when the program
 * is broken): hold the button through a reset;
 * the traffic lights flash at the start and every 2 s after, and the luck
 * bar shows what a release will choose: 1 lit = clear the console password,
 * 2 = WiFi setup, 3 = stay in the bootloader for a firmware update; 0 lit =
 * nothing. The cycle repeats every 8 s.
 *
 * The HEARTBEAT tells the two apart: 0.2 s on / 0.2 s off in the bootloader,
 * a slow breathing glow in the program. In the bootloader the telnet LED shows WiFi
 * (blinking = joining, steady = joined) and the other six are one bar
 * (boot.c's header has the details).
 *
 * Hand-overs:
 *   program -> bootloader: the "update" command writes BOOT_REQ_UPDATE to
 *     BOOT_REQUEST_EE and restarts the chip (watchdog). The bootloader clears
 *     it on reading, so it works once.
 *   bootloader -> program: GPIOR0 = BOOT_HANDOFF | choice (a register: it
 *     survives the jump, a real reset clears it). GPIOR1 = the reset cause
 *     (MCUSR), which the bootloader has to clear.
 *
 * A firmware is "incomplete" from the first page erase until its read-back
 * CRC matches (BOOT_STATE_EE = BOOT_INCOMPLETE); the bootloader then never
 * starts the program, whatever happens in between (power cut, lost WiFi).
 */
#ifndef BOOTSHARE_H
#define BOOTSHARE_H

#define BOOT_START        0x1E000UL     /* bytes; = word address 0xF000 */
#define APP_MAX           BOOT_START    /* largest program: 122,880 bytes */

#define BOOT_REQUEST_EE   ((uint8_t *)0xFF0)    /* EEPROM, clear of settings (0) and best (0x200) */
#define BOOT_STATE_EE     ((uint8_t *)0xFF1)
#define BOOT_REQ_UPDATE   0xB0
#define BOOT_INCOMPLETE   0x1D

#define BOOT_HANDOFF      0xB0          /* GPIOR0 high nibble: the bootloader ran */
#define BOOT_CHOICE_NONE       0
#define BOOT_CHOICE_CLEAR_PASS 1
#define BOOT_CHOICE_WIFI_SETUP 2
#define BOOT_CHOICE_UPDATE     3        /* never reaches the program */

#define BOOT_HTTP_PORT    80            /* same address and port as the web page */

/* What the bootloader needs from the settings, in a fixed format of its own
 * (the bootloader never changes, the program's settings layout may). The
 * program keeps it equal to its settings: at start-up and at every save
 * (settings.c; eeprom_update_block writes only bytes that differ). */
#include <stdint.h>
#define BOOT_CREDS_EE     ((void *)0xF00)
#define BOOT_CREDS_MAGIC  0xB007
struct boot_creds {
    uint16_t magic;
    char     ssid[33];
    char     pass[65];
    char     console_pass[17];
    uint16_t sum;                       /* boot_sum() of ssid .. console_pass */
};

static inline uint16_t boot_sum(const uint8_t *p, uint16_t n)
{
    uint16_t s = BOOT_CREDS_MAGIC;

    while (n--)
        s = (uint16_t)(s * 31 + *p++);
    return s;
}
#define BOOT_CREDS_SUM(c) boot_sum((const uint8_t *)(c)->ssid, \
                                   (uint16_t)(sizeof (c)->ssid + sizeof (c)->pass + sizeof (c)->console_pass))

#endif
