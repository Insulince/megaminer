# megaminer: an XBT miner on an ATmega1284P. Works on Windows (cmd.exe),
# Linux and macOS with GNU make.
#
#   make                      build build/miner_sv2.hex (or TARGET=miner_sv1, ...)
#   make size                 flash / RAM use of the program
#   make ota  [HOST=a.b.c.d]  send the program to the miner over WiFi
#   make flash                program + bootloader over a USBasp (first time only)
#   make check                every PC test, every firmware build, the checks
#                             of the hashing machine code and the bootloader
#   make test / test-sv2      the PC tests of the hashing and SV2 code alone
#   make sim                  the miners as PC programs (build/miner_sv2_sim, ..._sv1_sim)
#   make sig                  read the chip signature over the USBasp
#   make clean                remove build/, i.e. everything the build made
#
# Programs: miner_sv2 (Stratum V2), miner_sv1 (Stratum V1), the chip tests
# sv2_chip_test and winc_id, and from the Ethernet days miner_net /
# miner_uart (job bridge: cmd/bridge).
#
# No target here writes fuses or lock bits. Keep it that way: see the README.

TARGET := miner_sv2
MCU    := atmega1284p
F_CPU  := 20000000UL

# Everything the build makes goes in build/: programs, PC tests, generated
# sources (the fast hashing routine, the gzipped web page).
B := build

# ---- the few things that differ between Windows and everything else ----
# On Windows PC programs get .exe and run through cmd.exe, which needs
# backslashes in a command's path.
ifeq ($(OS),Windows_NT)
SHELL   := cmd.exe
EXE     := .exe
PYTHON  := python
NULL    := NUL
WINSOCK := -lws2_32
run      = $(subst /,\,$(1))
path     = $(subst /,\,$(1))
RM_DIR   = if exist $(1) rmdir /s /q $(1)
MKDIR    = if not exist $(1) mkdir $(1)
else
EXE     :=
PYTHON  := python3
NULL    := /dev/null
WINSOCK :=
run      = ./$(1)
path     = $(1)
RM_DIR   = rm -rf $(1)
MKDIR    = mkdir -p $(1)
endif

# ---- where the sources are ----
#   firmware/      the miners and everything they share
#   firmware/net/  the WiFi module (net.h is the interface any network chip implements)
#   boot/          the bootloader
#   ethernet/      the W5500 Ethernet backend and the early bridge-era programs
#   tests/chip/    programs that test the chip: sv2_chip_test (the SV2 crypto)
#                  and winc_id (is the WiFi module wired right?)
SRC_DIRS := firmware firmware/net boot ethernet tests/chip
vpath %.c $(SRC_DIRS)
vpath %.h $(SRC_DIRS)
# -I. for third_party/ (sv2curve.c compiles micro-ecc inside itself)
INCLUDES := $(addprefix -I,$(SRC_DIRS)) -I$(B) -I.

# ---- firmware version ----
# From git: "v0.1.0" at a tag, "v0.1.0-3-g1a2b3c4" after it, "-dirty" with
# uncommitted changes, "dev" outside git. See firmware/version.h.
VERSION := $(shell git describe --tags --always --dirty 2>$(NULL))
ifeq ($(VERSION),)
VERSION := dev
endif
VERSION_FLAGS = -DFIRMWARE_VERSION=\"$(VERSION)\" -DPROGRAM_NAME=\"$(TARGET)\"

# avrdude: this USBasp clone's firmware cannot set the ISP clock from software,
# so -B is rejected ("cannot set sck period") and is left off by default.
# The "cannot set sck period" line still prints; it is harmless.
# If flashing ever fails to verify, close the clone's slow-SCK jumper (JP3).
# With a programmer that supports it: make flash BITCLOCK="-B 32"
PROGRAMMER := usbasp
PART       := m1284p
BITCLOCK   :=
AVRDUDE    := avrdude -c $(PROGRAMMER) -p $(PART) $(BITCLOCK)

CC      := avr-gcc
OBJCOPY := avr-objcopy
SIZE    := avr-size
PCC     := gcc

# Flash savers (measured on miner_sv2: 122,130 -> 116,394 bytes):
#   -mrelax          the linker turns 4-byte calls/jumps into 2-byte relative
#                    ones where the target is near enough (also 1 cycle faster)
#   -mcall-prologues functions that save many registers share one copy of the
#                    save/restore code (a few cycles per such call; the hashing
#                    itself is assembly and unaffected)
# -flto would save ~1 KB more but is riskier with the inline assembly; not used.
CFLAGS  := -mmcu=$(MCU) -DF_CPU=$(F_CPU) -Os -std=gnu11 -Wall -Wextra \
           -Werror=misspelled-isr \
           -ffunction-sections -fdata-sections -mrelax -mcall-prologues
LDFLAGS := -mmcu=$(MCU) -Wl,--gc-sections -mrelax

PCFLAGS := -std=c11 -O2 -Wall -Wextra

.PHONY: all size flash sig clean check check-boot test test-sv2 test-tests \
        sim verify-fast ota FORCE

all: $(B)/$(TARGET).hex

$(B):
	$(call MKDIR,$(B))

# ---- PC tests ----

# BLAKE2b, the fast hashing routine's C model, hash-rate maths, DHCP.
test: | $(B)
	$(PCC) $(PCFLAGS) -Ifirmware -Itests/hash -o $(B)/test_blake2b$(EXE) tests/hash/test_blake2b.c firmware/blake2b.c
	$(call run,$(B)/test_blake2b$(EXE))
	$(PCC) $(PCFLAGS) -Ifirmware -o $(B)/test_blake2b_work$(EXE) tests/hash/test_blake2b_work.c firmware/blake2b_work.c firmware/blake2b.c
	$(call run,$(B)/test_blake2b_work$(EXE))
	$(PCC) $(PCFLAGS) -Ifirmware -o $(B)/hashrate_test$(EXE) tests/hashrate/hashrate_test.c firmware/hashrate.c
	$(call run,$(B)/hashrate_test$(EXE))
	$(PCC) $(PCFLAGS) -Iethernet -o $(B)/dhcp_test$(EXE) tests/dhcp/dhcp_test.c ethernet/dhcp.c
	$(call run,$(B)/dhcp_test$(EXE))

# sv2crypto.c (SHA-256, HKDF, ChaCha20-Poly1305) and sv2curve.c (secp256k1),
# against values from Go:  go run ./cmd/sv2vectors > tests/sv2/sv2crypto_vectors.h
test-sv2: | $(B)
	$(PCC) $(PCFLAGS) -Ifirmware -Itests/sv2 -o $(B)/test_sv2crypto$(EXE) tests/sv2/test_sv2crypto.c firmware/sv2crypto.c
	$(call run,$(B)/test_sv2crypto$(EXE))
	$(PCC) -std=gnu11 -O2 -Wall -Wextra -Ifirmware -Itests/sv2 -I. -o $(B)/test_sv2curve$(EXE) tests/sv2/test_sv2curve.c firmware/sv2curve.c firmware/sv2crypto.c
	$(call run,$(B)/test_sv2curve$(EXE))

# tests/: the bootloader and the WiFi code compiled unchanged against a fake
# module and fake AVR headers; the web page's script under Node, fed a
# /status made by the real hashrate.c.
test-tests: $(B)/webpage_gz.h | $(B)
	$(PCC) -std=gnu11 -O1 -w -Itests/boot/inc -Iboot -Ifirmware/net -Ifirmware -o $(B)/boottest$(EXE) tests/boot/test.c
	$(call run,$(B)/boottest$(EXE))
	$(PCC) -std=gnu11 -O1 -w -Itests/net/inc -Ifirmware -Ifirmware/net -I$(B) -c tests/net/test.c -o $(B)/nettest.o
	$(PCC) -O1 -w -Itests/winc_layout -Itests/winc_layout/driver/include -Itests/winc_layout/common/include -c tests/net/ref.c -o $(B)/netref.o
	$(PCC) -O1 -w -c tests/net/stubs.c -o $(B)/netstubs.o
	$(PCC) -o $(B)/nettest$(EXE) $(B)/nettest.o $(B)/netref.o $(B)/netstubs.o
	$(call run,$(B)/nettest$(EXE))
	$(PCC) -O2 -w -Ifirmware -o $(B)/statusjson$(EXE) tests/json/j.c firmware/hashrate.c
	$(call run,$(B)/statusjson$(EXE)) full > $(call path,$(B)/status.json)
	node tests/web/test.js $(B)/webpage.html $(B)/status.json
	node tests/web/logtest.js $(B)/webpage.html $(B)/status.json
	node tests/web/statustest.js $(B)/webpage.html $(B)/status.json
	node tests/web/updtest.js $(B)/webpage.html $(B)/status.json
	node tests/web/uitest.js $(B)/webpage.html $(B)/status.json
	node tests/web/wdtest.js $(B)/webpage.html $(B)/status.json

# Everything. Needs avr-gcc, a PC gcc, Python 3, Go and Node.
check: test test-sv2 test-tests sim
	go vet ./...
	go test ./...
	$(MAKE) --no-print-directory TARGET=miner_sv2 NET=w5500 size
	$(MAKE) --no-print-directory TARGET=miner_net size
	$(MAKE) --no-print-directory TARGET=miner_uart size
	$(MAKE) --no-print-directory TARGET=sv2_chip_test size
	$(MAKE) --no-print-directory TARGET=winc_id size
	$(MAKE) --no-print-directory TARGET=miner_sv1 size verify-fast
	$(MAKE) --no-print-directory TARGET=miner_sv2 size verify-fast
	$(MAKE) --no-print-directory check-boot
	@echo check: all passed

# The bootloader on the chip is never meant to change, so boot.c must still
# build into exactly the image that was installed (boot/boot_installed.hex).
# Only meaningful with the avr-gcc that built it; with another it says so.
BOOT_BUILT_WITH := 16.1.0
check-boot: FORCE
	$(MAKE) --no-print-directory -B $(B)/boot.hex
	$(PYTHON) tools/check_boot.py $(B)/boot.hex boot/boot_installed.hex $(BOOT_BUILT_WITH) $(shell $(CC) -dumpversion)

# ---- the fast hashing routine ----
# Generated, and tested as it is generated. `make verify-fast` then decodes
# the routine out of the built program and tests that machine code the same way.
$(B)/blake2b_work_avr.S: tools/gen_blake2b_avr.py | $(B)
	$(PYTHON) tools/gen_blake2b_avr.py $@

verify-fast: $(B)/$(TARGET).elf
	$(PYTHON) tools/gen_blake2b_avr.py --verify-elf $(B)/$(TARGET).elf

# ---- the miners as PC programs ----
# miner.c and the protocol file themselves, with a stand-in for the chip's
# hardware (tests/sim/miner_pc.c):
#   build/miner_sv2_sim HOST PORT WORKER SECONDS [console lines...]
#   build/miner_sv1_sim HOST PORT WORKER SECONDS [console lines...]
# SUBMIT_TEST=1 builds the SV1 one with the submit-message test.
SIM_CC    = $(PCC) -std=gnu11 -O2 -Wall -Wextra -Ifirmware -Ifirmware/net -Iboot -I. -DFIRMWARE_VERSION=\"$(VERSION)\"
SIM_FILES = tests/sim/miner_pc.c firmware/miner.c firmware/hashrate.c firmware/blake2b_work.c firmware/blake2b.c
sim: | $(B)
	$(SIM_CC) -DPROGRAM_NAME=\"miner_sv2\" -o $(B)/miner_sv2_sim$(EXE) $(SIM_FILES) \
	    firmware/miner_sv2.c firmware/sv2.c firmware/sv2curve.c firmware/sv2crypto.c $(WINSOCK)
	$(SIM_CC) -DPROGRAM_NAME=\"miner_sv1\" $(if $(filter 1,$(SUBMIT_TEST)),-DSTRATUM_SUBMIT_TEST) \
	    -o $(B)/miner_sv1_sim$(EXE) $(SIM_FILES) firmware/miner_sv1.c $(WINSOCK)

# ---- firmware ----

# Extra source files a program needs besides its own <name>.c (found through
# vpath), and extra compiler flags.
FAST_HASH := blake2b.c blake2b_work.c $(B)/blake2b_work_avr.S
miner_uart_EXTRA    := blake2b.c uart.c
sv2_chip_test_EXTRA := uart.c entropy.c sv2curve.c sv2crypto.c
sv2_chip_test_FLAGS := -Itests/sv2
winc_id_EXTRA       := uart.c

# Which network chip the miners are built for: net_$(NET).c implements net.h.
# winc = the ATWINC1500 WiFi breakout; w5500 = the Ethernet module
# (make ... NET=w5500).
NET := winc
NET_FILES_winc  := net_winc.c winc.c
NET_FILES_w5500 := net_w5500.c dhcp.c
NET_FILES := $(NET_FILES_$(NET))
NET_DEFINE_winc  := -DNET_WINC
NET_DEFINE_w5500 := -DNET_W5500
miner_net_EXTRA := blake2b.c uart.c settings.c board.c $(NET_FILES)
MINER     := miner.c $(FAST_HASH) uart.c settings.c board.c $(NET_FILES) hashrate.c beststore.c uptime.c entropy.c watchdog.c
miner_sv1_EXTRA := $(MINER)
miner_sv2_EXTRA := $(MINER) sv2.c sv2curve.c sv2crypto.c

# Built-in default pool port (see firmware/net_config.h). These are only
# DEFAULTS: once running, settings are changed on the console and kept in
# EEPROM.  make ... PORT=23334  WORKER=myname  SUBMIT_TEST=1 (regtest only:
# sends one deliberately bad share per job, to exercise the submit path)
miner_sv1_PORT := 23339
miner_sv2_PORT := 34255
PORT := $($(TARGET)_PORT)
ifneq ($(PORT),)
CFLAGS += -DNET_SERVER_PORT=$(PORT)
endif
ifneq ($(WORKER),)
CFLAGS += -DNET_WORKER=\"$(WORKER)\"
endif
ifeq ($(SUBMIT_TEST),1)
CFLAGS += -DSTRATUM_SUBMIT_TEST
endif

# Always rebuilt (it takes a second), so a changed PORT, NET or version is
# never missed.
$(B)/$(TARGET).elf: $(TARGET).c $($(TARGET)_EXTRA) $(if $(filter net_winc.c,$($(TARGET)_EXTRA)),$(B)/webpage_gz.h) FORCE | $(B)
	$(CC) $(CFLAGS) $(INCLUDES) $($(TARGET)_FLAGS) $(VERSION_FLAGS) $(NET_DEFINE_$(NET)) $(LDFLAGS) -o $@ $(filter %.c %.S,$^)

# The web page (net_winc.c) is stored gzipped: written out of webpage.h by a
# small PC program, compressed by Python, served with Content-Encoding: gzip.
# build/webpage.html is a readable copy of what the chip serves.
$(B)/webpage.html: firmware/net/webpage.h tools/webpage_dump.c | $(B)
	$(PCC) -std=c11 -O0 -Wall -Ifirmware/net -o $(B)/webpage_dump$(EXE) tools/webpage_dump.c
	$(call run,$(B)/webpage_dump$(EXE)) $(B)/webpage.html

$(B)/webpage_gz.h: $(B)/webpage.html tools/gzip_header.py
	$(PYTHON) tools/gzip_header.py $(B)/webpage.html $@

FORCE:

$(B)/$(TARGET).hex: $(B)/$(TARGET).elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

size: $(B)/$(TARGET).elf
	$(SIZE) $<

# The USBasp erases the WHOLE chip before writing, boot section included, so
# `make flash` writes the program and the bootloader (boot.c) together.
# NOBOOT=1 writes the program alone (the bootloader is then gone; harmless:
# with the BOOTRST fuse set, a reset runs through the empty boot section and
# wraps round to the program). Afterwards, write the lock byte again.
ifeq ($(NOBOOT),1)
flash: $(B)/$(TARGET).hex
	$(AVRDUDE) -U flash:w:$<:i
else
flash: $(B)/$(TARGET)_boot.hex
	$(AVRDUDE) -U flash:w:$<:i
endif

$(B)/$(TARGET)_boot.hex: $(B)/$(TARGET).hex $(B)/boot.hex
	$(PYTHON) tools/merge_hex.py $@ $(B)/$(TARGET).hex $(B)/boot.hex

# ---- the bootloader (boot/boot.c, boot/bootshare.h) ----
# Linked at the start of the 8 KB boot section; the linker refuses if it
# outgrows it (the flash ends at 0x1FFFF). winc.c without its error texts.
# -fno-jump-tables: code above 64 KB must not read tables out of flash with
# 16-bit LPM (see the note at the top of boot.c). No version: it never changes.
$(B)/boot.elf: boot/boot.c firmware/net/winc.c firmware/net/winc.h boot/bootshare.h | $(B)
	$(CC) $(CFLAGS) $(INCLUDES) -DWINC_QUIET -fno-jump-tables $(LDFLAGS) -Wl,--section-start=.text=0x1E000 -o $@ boot/boot.c firmware/net/winc.c
	$(SIZE) $@

$(B)/boot.hex: $(B)/boot.elf
	$(OBJCOPY) -O ihex -R .eeprom $< $@

# ---- firmware over WiFi ----
# The raw image (from address 0) that the bootloader takes, and sending it:
#   make ota [TARGET=miner_sv2] [HOST=a.b.c.d]
# (console password from $MEGAMINER_PASS, or asked for)
HOST ?= 192.168.1.193
$(B)/$(TARGET).bin: $(B)/$(TARGET).elf
	$(OBJCOPY) -O binary -j .text -j .data $< $@

ota: $(B)/$(TARGET).bin
	go run ./cmd/ota -host $(HOST) $(B)/$(TARGET).bin

# -v is needed: avrdude 8 only prints the signature in verbose mode.
sig:
	$(AVRDUDE) -v

clean:
	$(call RM_DIR,$(B))
