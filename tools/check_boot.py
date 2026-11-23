"""Check that boot.c still builds into the bootloader installed on the chip.

    python tools/check_boot.py boot.hex boot_installed.hex BUILT_WITH YOURS

BUILT_WITH is the avr-gcc version that built the installed image, YOURS the
one that just built boot.hex. Line endings are ignored. A difference with the
same compiler fails; with a different compiler it cannot be judged, so it is
reported and skipped (another gcc lays the same source out differently).
"""
import sys


def records(path):
    with open(path, "rb") as f:
        return f.read().replace(b"\r", b"")


def main():
    built, installed, built_with, yours = sys.argv[1:5]
    if records(built) == records(installed):
        print("check-boot: boot.c builds byte-identical to the installed bootloader")
        return 0
    if built_with != yours:
        print(f"check-boot: SKIPPED: the installed bootloader was built with avr-gcc "
              f"{built_with}, this is {yours}, so the images cannot be compared")
        return 0
    print("check-boot: FAILED: boot.c no longer builds into the installed bootloader.")
    print("The bootloader on the chip is meant never to change (only a USBasp can")
    print("replace it). Undo the change to boot.c, winc.c, winc.h or bootshare.h.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
