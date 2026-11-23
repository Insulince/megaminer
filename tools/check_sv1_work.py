"""Check the work blocks miner_sv1 built, from its console output.

Copy some of the chip's console output (PuTTY) to the clipboard, then:

    powershell Get-Clipboard | python tools/check_sv1_work.py

For every job it finds, it rebuilds the 80-byte work block on the PC from
the gateway's own messages (the "<" lines) and compares it with the "work"
line the chip printed.

    root = BLAKE2b-256( 0x00 || coinb1 || extranonce1 || extranonce2 )
    work = prevhash || 8 zero nonce bytes || ntime || root
"""

import hashlib
import json
import re
import sys

EXTRANONCE2 = b"megamine"  # must match miner_sv1.c

extranonce1 = None
params = None
checked = bad = 0

# PuTTY pads a copied line with spaces wherever it wrapped on screen, and may
# split it across lines. Neither the gateway's messages nor the chip's hex
# contain spaces, so: glue continuation lines back on, then drop all spaces.
# Every line the miner prints starts with a time stamp, [12:34:56Z] or
# [+12s]; a line without one is a piece of the line before it.
STAMP = re.compile(r"\[(\d\d:\d\d:\d\dZ|\+\d+s)\] ")
PREFIXES = ("< ", "> ", "work ", "target ", "nbits ", "rate ", "net:", "sv1:", "miner:", "megaminer", "SHARE", "submit test")
lines = []
for raw in sys.stdin:
    raw = raw.strip()
    if not raw:
        continue
    stamp = STAMP.match(raw)
    if stamp:
        raw = raw[stamp.end():]
    if stamp or raw.startswith(PREFIXES) or not lines:
        lines.append(raw)
    else:
        lines[-1] += raw

for line in lines:
    if line.startswith(("< ", "work ")):
        head, rest = line.split(" ", 1)
        line = head + " " + "".join(rest.split())
    if line.startswith("< "):
        try:
            msg = json.loads(line[2:])
        except ValueError:
            continue  # a line PuTTY wrapped or cut; skip it
        if msg.get("method") == "mining.notify":
            params = msg["params"]
        elif isinstance(msg.get("result"), list) and len(msg["result"]) == 3:
            extranonce1 = bytes.fromhex(msg["result"][1])
            extranonce2 = EXTRANONCE2[: msg["result"][2]]
            print("extranonce1", extranonce1.hex(), "extranonce2 size", len(extranonce2))
    elif line.startswith("work ") and params is not None:
        if extranonce1 is None:
            sys.exit("no subscribe reply seen before this job; copy from the start of the session")
        prevhash, coinb1, ntime = (bytes.fromhex(params[i]) for i in (1, 2, 7))
        root = hashlib.blake2b(b"\x00" + coinb1 + extranonce1 + extranonce2, digest_size=32).digest()
        want = (prevhash + bytes(8) + ntime + root).hex()
        ok = want == line[5:]
        checked += 1
        bad += not ok
        print("job", params[0], "OK" if ok else "MISMATCH\n  chip " + line[5:] + "\n  pc   " + want)
        params = None

print(f"{checked} job(s) checked, {bad} wrong")
sys.exit(1 if bad or not checked else 0)
