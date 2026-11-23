"""merge_hex.py - PC tool: join a program's .hex and the bootloader's .hex
into one file for the USBasp.

    python tools/merge_hex.py OUT.hex miner_sv2.hex boot.hex

Needed because avrdude erases the whole chip, boot section included, before
writing: flashing the program alone would wipe the bootloader. Refuses if
the two overlap (a program too big for the space below the bootloader).
Reads and writes Intel HEX (data records, extended segment/linear address
records, end record); writes extended linear address records.
"""
import sys


def read_hex(path):
    data = {}
    base = 0
    for n, line in enumerate(open(path), 1):
        line = line.strip()
        if not line:
            continue
        if not line.startswith(":"):
            sys.exit("%s:%d: not an Intel HEX line" % (path, n))
        raw = bytes.fromhex(line[1:])
        count, addr, kind = raw[0], raw[1] << 8 | raw[2], raw[3]
        body = raw[4:4 + count]
        if (sum(raw) & 0xFF) != 0 or len(raw) != count + 5:
            sys.exit("%s:%d: bad checksum or length" % (path, n))
        if kind == 0:
            for i, b in enumerate(body):
                a = base + addr + i
                if a in data:
                    sys.exit("%s: address 0x%x given twice" % (path, a))
                data[a] = b
        elif kind == 1:
            break
        elif kind == 2:
            base = (body[0] << 8 | body[1]) << 4
        elif kind == 4:
            base = (body[0] << 8 | body[1]) << 16
        # 3 and 5 (start addresses) mean nothing to avrdude: ignored
    return data


def record(kind, addr, body):
    raw = bytes([len(body), addr >> 8 & 0xFF, addr & 0xFF, kind]) + bytes(body)
    return ":" + (raw + bytes([-sum(raw) & 0xFF])).hex().upper()


def main():
    if len(sys.argv) < 4:
        sys.exit("usage: merge_hex.py OUT.hex IN1.hex IN2.hex ...")
    merged = {}
    for path in sys.argv[2:]:
        part = read_hex(path)
        clash = merged.keys() & part.keys()
        if clash:
            sys.exit("%s overlaps what came before it at 0x%x: the program is too big "
                     "for the space below the bootloader" % (path, min(clash)))
        merged.update(part)
    lines, upper = [], None
    addrs = sorted(merged)
    i = 0
    while i < len(addrs):
        start = addrs[i]
        if start >> 16 != upper:
            upper = start >> 16
            lines.append(record(4, 0, [upper >> 8, upper & 0xFF]))
        body = [merged[start]]
        i += 1
        while (i < len(addrs) and addrs[i] == start + len(body) and len(body) < 16
               and addrs[i] >> 16 == upper):
            body.append(merged[addrs[i]])
            i += 1
        lines.append(record(0, start & 0xFFFF, body))
    lines.append(record(1, 0, []))
    with open(sys.argv[1], "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    print("%s: %d bytes from %s" % (sys.argv[1], len(merged), " + ".join(sys.argv[2:])))


if __name__ == "__main__":
    main()
