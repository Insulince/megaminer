/*
 * version.h - which firmware this is.
 *
 * The Makefile passes both from the build:
 *   FIRMWARE_VERSION  `git describe --tags --always --dirty`, e.g.
 *                     "v0.1.0"              built exactly at tag v0.1.0
 *                     "v0.1.0-3-g1a2b3c4"   3 commits after it
 *                     "v0.1.0-dirty"        with uncommitted changes
 *   PROGRAM_NAME      the program, e.g. "miner_sv2"
 * Shown at start-up, by `help`, on the web page (/status) and sent to the
 * pool (SV2 SetupConnection's firmware field, SV1's user agent).
 */
#ifndef VERSION_H
#define VERSION_H

#ifndef FIRMWARE_VERSION
#define FIRMWARE_VERSION "dev"          /* built outside git, or by hand */
#endif

#ifndef PROGRAM_NAME
#define PROGRAM_NAME "megaminer"
#endif

#endif
