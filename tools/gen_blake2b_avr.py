#!/usr/bin/env python3
"""
gen_blake2b_avr.py - writes blake2b_work_avr.S, the fast hashing routine for
the mining loop, and proves it correct before writing it.

    python tools/gen_blake2b_avr.py [OUT.S]          write blake2b_work_avr.S (after testing)
    python tools/gen_blake2b_avr.py --verify-elf X   decode bw_asm out of a built X.elf and
                                               test the real machine code the same way

What the routine computes: the first 8 bytes of BLAKE2b-256 over the 80-byte
work block, for one nonce. It is BLAKE2b exactly (same digests), arranged for
this one shape of input:

  * straight-line code, all 12 rounds unrolled, no loops, no pointers: every
    byte of state and message is read and written by fixed address, so all
    32 registers can hold data (4 state words of 8 bytes);
  * rotations by 32, 24 and 16 bits cost nothing: only the note of which
    register holds which byte changes;
  * message words 10..15 are always zero (bytes 80..127 of the block) and
    are not added; the high half of the nonce word is always zero too, so
    one fetched zero serves for its last four bytes;
  * the three mixing steps of round 0 that do not involve the nonce are done
    once per job by bw_setup() in blake2b_work.c; this routine starts from
    their result (bw_init);
  * every instruction whose result does not reach the first 8 digest bytes
    is deleted (most of the last round's tail).

Memory it uses (all defined in blake2b_work.c):
    bw_m[128]     the padded work block; the nonce is at bytes 32..35
    bw_init[128]  the 16 state words to start from
    bw_v[128]     scratch
    bw_h0[8]      constant: IV0 ^ parameter block
    bw_out[8]     result: digest bytes 0..7

How it is tested: the instruction list is run by a small simulator in this
file and compared with Python's hashlib for random work blocks and nonces.
"""
import hashlib
import random
import re
import struct
import subprocess
import sys

MASK = (1 << 64) - 1
IV = [0x6a09e667f3bcc908, 0xbb67ae8584caa73b, 0x3c6ef372fe94f82b, 0xa54ff53a5f1d36f1,
      0x510e527fade682d1, 0x9b05688c2b3e6c1f, 0x1f83d9abfb41bd6b, 0x5be0cd19137e2179]
SIGMA = [
    [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15],
    [14, 10, 4, 8, 9, 15, 13, 6, 1, 12, 0, 2, 11, 7, 5, 3],
    [11, 8, 12, 0, 5, 2, 15, 13, 10, 14, 3, 6, 7, 1, 9, 4],
    [7, 9, 3, 1, 13, 12, 11, 14, 2, 6, 5, 10, 4, 0, 15, 8],
    [9, 0, 5, 7, 2, 4, 10, 15, 14, 1, 11, 12, 6, 8, 3, 13],
    [2, 12, 6, 10, 0, 11, 8, 3, 4, 13, 7, 5, 15, 14, 1, 9],
    [12, 5, 1, 15, 14, 13, 4, 10, 0, 7, 6, 3, 9, 2, 8, 11],
    [13, 11, 7, 14, 12, 1, 3, 9, 5, 0, 15, 4, 8, 6, 2, 10],
    [6, 15, 14, 9, 11, 3, 0, 8, 12, 2, 13, 7, 1, 4, 10, 5],
    [10, 2, 8, 4, 7, 6, 1, 5, 15, 11, 9, 14, 3, 12, 13, 0],
]
H0 = IV[0] ^ 0x01010020                 # 32-byte digest, no key
ZERO_WORDS = range(10, 16)              # message words that are always zero
NONCE_WORD = 4                          # bytes 32..39; only 32..35 are ever non-zero
CALL_SAVED = list(range(2, 18)) + [28, 29]

COLUMNS = [(j, 4 + j, 8 + j, 12 + j) for j in range(4)]
DIAGONALS = [(j, 4 + (j + 1) % 4, 8 + (j + 2) % 4, 12 + (j + 3) % 4) for j in range(4)]
PRECOMPUTED = (0, 1, 3)                 # round-0 columns done by bw_setup()


# ---------------------------------------------------------------- reference

def rotr(x, n):
    return ((x >> n) | (x << (64 - n))) & MASK


def mix(v, a, b, c, d, x, y):
    v[a] = (v[a] + v[b] + x) & MASK
    v[d] = rotr(v[d] ^ v[a], 32)
    v[c] = (v[c] + v[d]) & MASK
    v[b] = rotr(v[b] ^ v[c], 24)
    v[a] = (v[a] + v[b] + y) & MASK
    v[d] = rotr(v[d] ^ v[a], 16)
    v[c] = (v[c] + v[d]) & MASK
    v[b] = rotr(v[b] ^ v[c], 63)


def setup(work):
    """What bw_setup() in blake2b_work.c must produce: (bw_m, bw_init)."""
    block = bytes(work) + bytes(48)
    m = struct.unpack('<16Q', block)
    v = [H0] + IV[1:] + IV[:]
    v[12] ^= 80                         # bytes hashed
    v[14] ^= MASK                       # this is the last block
    for j in PRECOMPUTED:
        a, b, c, d = COLUMNS[j]
        mix(v, a, b, c, d, m[2 * j], m[2 * j + 1])
    return block, struct.pack('<16Q', *v)


# ---------------------------------------------------------------- generator

class Gen:
    def __init__(self):
        self.ops = []
        self.free = list(range(32))
        self.held = {}                  # state word -> its 8 registers, byte 0 first
        self.stored = set()             # state words that have been written to bw_v

    def emit(self, *op):
        self.ops.append(op)

    def where(self, w):
        return 'bw_v' if w in self.stored else 'bw_init'

    def load(self, w):
        if w in self.held:
            return self.held[w]
        regs = [self.free.pop(0) for _ in range(8)]
        for i, r in enumerate(regs):
            self.emit('lds', r, self.where(w), 8 * w + i)
        self.held[w] = regs
        return regs

    def store(self, w):
        for i, r in enumerate(self.held.pop(w)):
            self.emit('sts', 'bw_v', 8 * w + i, r)
            self.free.append(r)
        self.free.sort()
        self.stored.add(w)

    def add(self, a, b):                # a += b
        ra, rb = self.held[a], self.held[b]
        for i in range(8):
            self.emit('add' if i == 0 else 'adc', ra[i], rb[i])

    def add_message(self, a, word):     # a += message word (lds leaves the carry alone)
        if word in ZERO_WORDS:
            return
        t = self.free[0]
        for i, r in enumerate(self.held[a]):
            # Bytes 4..7 of the nonce word are all zero (bw_setup sees to
            # it), so the zero fetched for byte 4 serves for 5, 6 and 7.
            if not (word == NONCE_WORD and i > 4):
                self.emit('lds', t, 'bw_m', 8 * word + i)
            self.emit('add' if i == 0 else 'adc', r, t)

    def xor(self, a, b):                # a ^= b
        for ra, rb in zip(self.held[a], self.held[b]):
            self.emit('eor', ra, rb)

    def rotate_bytes(self, w, n):       # rotate right by 8*n bits: free
        self.held[w] = self.held[w][n:] + self.held[w][:n]

    def rotate_left_1(self, w):         # = rotate right by 63
        r = self.held[w]
        self.emit('bst', r[7], 7)       # remember the top bit
        self.emit('add', r[0], r[0])
        for i in range(1, 8):
            self.emit('adc', r[i], r[i])
        self.emit('bld', r[0], 0)       # it comes back in at the bottom

    def mix(self, words, x, y, keep):
        a, b, c, d = words
        self.load(a)
        self.load(b)
        self.add(a, b)
        self.add_message(a, x)
        self.load(d)
        self.xor(d, a)
        self.rotate_bytes(d, 4)
        self.add_message(a, y)          # moved up: c's registers are still free here
        self.load(c)
        self.add(c, d)
        self.xor(b, c)
        self.rotate_bytes(b, 3)
        self.add(a, b)
        self.xor(d, a)
        self.rotate_bytes(d, 2)
        self.add(c, d)
        self.xor(b, c)
        self.rotate_left_1(b)
        for w in words:
            if w not in keep:
                self.store(w)

    def build(self):
        steps = []
        for rnd in range(12):
            s = SIGMA[rnd % 10]
            for j in range(4):
                if rnd == 0 and j in PRECOMPUTED:
                    continue
                steps.append((COLUMNS[j], s[2 * j], s[2 * j + 1]))
            # This order lets one word stay in registers across each change
            # between columns and diagonals.
            for j in (3, 1, 2, 0):
                steps.append((DIAGONALS[j], s[8 + 2 * j], s[9 + 2 * j]))
        for n, (words, x, y) in enumerate(steps):
            keep = set()
            if n + 1 < len(steps):
                a, b, c, d = steps[n + 1][0]
                keep = set(words) & {a, b, d}       # never as the next c: see mix()
            self.mix(words, x, y, keep)
        assert not self.held
        # digest bytes 0..7 = h0 ^ v0 ^ v8
        t, u = self.free[0], self.free[1]
        for i in range(8):
            self.emit('lds', t, 'bw_v', i)
            self.emit('lds', u, 'bw_v', 64 + i)
            self.emit('eor', t, u)
            self.emit('lds', u, 'bw_h0', i)
            self.emit('eor', t, u)
            self.emit('sts', 'bw_out', i, t)


def uses_defs(op):
    k = op[0]
    if k == 'lds':
        return {('m', op[2], op[3])}, {('r', op[1])}
    if k == 'sts':
        return {('r', op[3])}, {('m', op[1], op[2])}
    if k == 'add':
        return {('r', op[1]), ('r', op[2])}, {('r', op[1]), 'C'}
    if k == 'adc':
        return {('r', op[1]), ('r', op[2]), 'C'}, {('r', op[1]), 'C'}
    if k == 'eor':
        return {('r', op[1]), ('r', op[2])}, {('r', op[1])}
    if k == 'bst':
        return {('r', op[1])}, {'T'}
    if k == 'bld':
        return {('r', op[1]), 'T'}, {('r', op[1])}
    raise ValueError(op)


def drop_unused(ops):
    """Delete every instruction none of whose results is ever used."""
    live = {('m', 'bw_out', i) for i in range(8)}
    kept = []
    for op in reversed(ops):
        uses, defs = uses_defs(op)
        if not (defs & live):
            continue
        # A register an instruction both reads and rewrites stays live through `uses`.
        live -= defs
        live |= uses
        kept.append(op)
    kept.reverse()
    return kept


def wrap(body):
    """Add the entry and exit code the C compiler's calling rules require."""
    used = set()
    for op in body:
        used |= {x[1] for part in uses_defs(op) for x in part if isinstance(x, tuple) and x[0] == 'r'}
    saved = [r for r in CALL_SAVED if r in used]
    ops = [('push', r) for r in saved] + body + [('pop', r) for r in reversed(saved)]
    ops += [('eor', 1, 1), ('ret',)]            # r1 must be zero for C code
    return ops


CYCLES = {'lds': 2, 'sts': 2, 'push': 2, 'pop': 2, 'ret': 4}
WORDS = {'lds': 2, 'sts': 2}


# ---------------------------------------------------------------- simulator

def run(ops, mem):
    """Execute `ops`. `mem` maps (symbol, offset) or an address to a byte."""
    reg = [random.randrange(256) for _ in range(32)]
    before = list(reg)
    carry = t = 0
    stack = []
    for op in ops:
        k = op[0]
        if k == 'lds':
            reg[op[1]] = mem[(op[2], op[3])]
        elif k == 'sts':
            mem[(op[1], op[2])] = reg[op[3]]
        elif k in ('add', 'adc'):
            total = reg[op[1]] + reg[op[2]] + (carry if k == 'adc' else 0)
            reg[op[1]] = total & 0xff
            carry = total >> 8
        elif k == 'eor':
            reg[op[1]] ^= reg[op[2]]
        elif k == 'bst':
            t = (reg[op[1]] >> op[2]) & 1
        elif k == 'bld':
            reg[op[1]] = (reg[op[1]] & ~(1 << op[2]) & 0xff) | (t << op[2])
        elif k == 'push':
            stack.append(reg[op[1]])
        elif k == 'pop':
            reg[op[1]] = stack.pop()
        elif k == 'ret':
            break
        else:
            raise ValueError(op)
    assert not stack, 'stack not balanced'
    assert reg[1] == 0, 'r1 not zero on return'
    for r in CALL_SAVED:
        assert reg[r] == before[r], 'r%d not restored' % r


def check(ops, rounds, label):
    rng = random.Random(20261116)
    for n in range(rounds):
        work = bytearray(rng.randrange(256) for _ in range(80))
        if n % 3 == 0:
            work[32:48] = bytes(16)             # as the live pools send it
        nonce = rng.choice([0, 1, 0xffffffff, rng.randrange(1 << 32)])
        work[36:40] = bytes(4)                  # the high half of the nonce field
        block, init = setup(work[:32] + bytes(4) + work[36:])
        block = bytearray(block)
        block[32:36] = struct.pack('<I', nonce)
        work[32:36] = struct.pack('<I', nonce)
        mem = {}
        for name, data in (('bw_m', block), ('bw_init', init),
                           ('bw_h0', struct.pack('<Q', H0)),
                           ('bw_v', bytes(rng.randrange(256) for _ in range(128))),
                           ('bw_out', bytes(8))):
            for i, byte in enumerate(data):
                mem[(name, i)] = byte
        run(ops, mem)
        got = bytes(mem[('bw_out', i)] for i in range(8))
        want = hashlib.blake2b(bytes(work), digest_size=32).digest()[:8]
        if got != want:
            sys.exit('%s: MISMATCH on test %d: got %s want %s' % (label, n, got.hex(), want.hex()))
    print('%s: %d random work blocks match hashlib' % (label, rounds))


# ---------------------------------------------------------------- output

def text(op):
    k = op[0]
    if k == 'lds':
        return 'lds r%d, %s+%d' % (op[1], op[2], op[3])
    if k == 'sts':
        return 'sts %s+%d, r%d' % (op[1], op[2], op[3])
    if k in ('add', 'adc', 'eor'):
        return '%s r%d, r%d' % (k, op[1], op[2])
    if k in ('bst', 'bld'):
        return '%s r%d, %d' % (k, op[1], op[2])
    if k in ('push', 'pop'):
        return '%s r%d' % (k, op[1])
    return k


def generate():
    g = Gen()
    g.build()
    body = drop_unused(g.ops)
    return g.ops, wrap(body)


def write(path):
    full, ops = generate()
    check(wrap(full), 200, 'before deleting unused instructions')
    check(ops, 2000, 'final routine')
    cycles = sum(CYCLES.get(op[0], 1) for op in ops) + 4      # + the call
    size = 2 * sum(WORDS.get(op[0], 1) for op in ops)
    with open(path, 'w', newline='\n') as f:
        f.write('; blake2b_work_avr.S - GENERATED by gen_blake2b_avr.py; do not edit.\n')
        f.write('; See that file for what this is and how it is tested.\n')
        f.write('; %d instructions, %d bytes, %d clock cycles per call.\n\n' % (len(ops), size, cycles))
        f.write('    .text\n    .global bw_asm\n    .type bw_asm, @function\nbw_asm:\n')
        for op in ops:
            f.write('    ' + text(op) + '\n')
        f.write('    .size bw_asm, .-bw_asm\n')
    print('%s: %d instructions (%d deleted as unused), %d bytes of flash' %
          (path, len(ops), len(wrap(full)) - len(ops), size))
    print('%d cycles per call = %.0f microseconds at 20 MHz = %.0f calls per second (hashing alone)' %
          (cycles, cycles / 20.0, 20e6 / cycles))


# ---------------------------------------------------------------- the built program

def verify_elf(elf):
    """Decode bw_asm from the linked program and test those instructions."""
    symbols = {}
    for line in subprocess.run(['avr-nm', elf], capture_output=True, text=True, check=True).stdout.splitlines():
        fields = line.split()
        if len(fields) == 3:            # undefined (weak) symbols have no address
            symbols[fields[2]] = int(fields[0], 16)
    names = {}
    for name in ('bw_m', 'bw_init', 'bw_v', 'bw_h0', 'bw_out'):
        size = 8 if name in ('bw_h0', 'bw_out') else 128
        for i in range(size):
            names[(symbols[name] & 0xffff) + i] = (name, i)
    dump = subprocess.run(['avr-objdump', '-d', '--no-show-raw-insn', elf],
                          capture_output=True, text=True, check=True).stdout
    body = dump.split('<bw_asm>:')[1].split('\n\n')[0]
    ops = []
    for line in body.splitlines():
        m = re.match(r'\s*[0-9a-f]+:\s+(\w+)\s*([^;]*)', line)
        if not m:
            continue
        k = m.group(1)
        args = [a.strip() for a in m.group(2).split(',') if a.strip()]

        def reg(a):
            assert a[0] == 'r', line
            return int(a[1:])

        if k == 'lds':
            ops.append(('lds', reg(args[0])) + names[int(args[1], 16) & 0xffff])
        elif k == 'sts':
            ops.append(('sts',) + names[int(args[0], 16) & 0xffff] + (reg(args[1]),))
        elif k in ('add', 'adc', 'eor'):
            ops.append((k, reg(args[0]), reg(args[1])))
        elif k in ('lsl', 'rol', 'clr'):            # the disassembler's short names
            ops.append(({'lsl': 'add', 'rol': 'adc', 'clr': 'eor'}[k], reg(args[0]), reg(args[0])))
        elif k in ('bst', 'bld'):
            ops.append((k, reg(args[0]), int(args[1], 0)))
        elif k in ('push', 'pop'):
            ops.append((k, reg(args[0])))
        elif k == 'ret':
            ops.append(('ret',))
        else:
            sys.exit('unexpected instruction in bw_asm: ' + line)
    assert ops and ops[-1] == ('ret',), 'bw_asm does not end in ret'
    expected = generate()[1]
    if ops != expected:
        sys.exit('%s: bw_asm differs from what the generator produces now' % elf)
    check(ops, 2000, '%s, machine code as built (%d instructions)' % (elf, len(ops)))


if __name__ == '__main__':
    if len(sys.argv) == 3 and sys.argv[1] == '--verify-elf':
        verify_elf(sys.argv[2])
    else:
        write(sys.argv[1] if len(sys.argv) == 2 else 'blake2b_work_avr.S')
