"""Print a job line for miner_uart.c, to paste into the serial terminal.

Usage:  python make_job.py [zero_bytes] [start_nonce]

The job is the Bitcoin Knots test-vector work block with its nonce set to
start_nonce (default 0). The target asks for zero_bytes leading zero bytes
(default 1): that many 00 bytes followed by ff bytes.
"""

import sys

WORK = bytes.fromhex(
    "000000000000943aff74219e1f45899abfdf536373c0f2fc92e6fe58335cd0ad"
    "0df0ad0b4433221158020000efcdab89"
    "7e6326906eaa52fe59e03a14f1dfb8dd5d6e78497e56a8a6e4f4fb4d385e43db"
)

zero_bytes = int(sys.argv[1]) if len(sys.argv) > 1 else 1
start_nonce = int(sys.argv[2]) if len(sys.argv) > 2 else 0

work = bytearray(WORK)
work[32:36] = start_nonce.to_bytes(4, "little")
target = bytes(zero_bytes) + b"\xff" * (32 - zero_bytes)

# The UUUU in front is throwaway: the first characters of a transmission can
# arrive damaged, and the chip ignores everything before the J.
print("UUUUJ" + work.hex() + target.hex())
