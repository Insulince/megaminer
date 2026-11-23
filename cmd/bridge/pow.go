package main

import (
	"crypto/sha256"
	"encoding/binary"
	"errors"
	"fmt"

	"golang.org/x/crypto/blake2b"
)

// XBT proof-of-work, as implemented by Bitcoin Knots v29.4.3.knots20260508
// (CBlockHeader::GetHash) and cross-checked against datum_gateway's
// datum_pow.c. The 164-byte header is boiled down to an 80-byte work block,
// and the block hash is the 32-byte BLAKE2b digest of that work block.

const (
	headerBytes = 164
	workBytes   = 80
	digestBytes = 32
	nonceOffset = 32 // nNonce inside the work block, low byte first

	headerV2Bit = 0x80000000 // set in the version of every BLAKE2b header
)

// header is a version-2 block header. Hashes are held in wire order, which
// is the reverse of how block explorers and RPC print them.
type header struct {
	Version      uint32
	PrevHash     [32]byte
	MerkleRoot   [32]byte
	Time         uint32 // as serialized; equals nTime while Flags is 0
	Bits         uint32
	Nonce        uint32
	Nonce2       uint32
	Nonce3       uint32
	Extranonce   [16]byte
	TimeOffset   uint32
	TxCount      uint16
	Flags        uint8
	XorClearBits uint8
	XorKey       [16]byte
	Height       uint32
	MMRhs        [32]byte
}

// serialize returns the 164-byte wire form.
func (h *header) serialize() [headerBytes]byte {
	var b [headerBytes]byte
	le := binary.LittleEndian
	le.PutUint32(b[0:], h.Version)
	copy(b[4:], h.PrevHash[:])
	copy(b[36:], h.MerkleRoot[:])
	le.PutUint32(b[68:], h.Time)
	le.PutUint32(b[72:], h.Bits)
	le.PutUint32(b[76:], h.Nonce)
	le.PutUint32(b[80:], h.Nonce2)
	le.PutUint32(b[84:], h.Nonce3)
	copy(b[88:], h.Extranonce[:])
	le.PutUint32(b[104:], h.TimeOffset)
	le.PutUint16(b[108:], h.TxCount)
	b[110] = h.Flags
	b[111] = h.XorClearBits
	copy(b[112:], h.XorKey[:])
	le.PutUint32(b[128:], h.Height)
	copy(b[132:], h.MMRhs[:])
	return b
}

// parseHeader reads the 164-byte wire form.
func parseHeader(b []byte) (header, error) {
	var h header
	if len(b) != headerBytes {
		return h, fmt.Errorf("header is %d bytes, want %d", len(b), headerBytes)
	}
	le := binary.LittleEndian
	h.Version = le.Uint32(b[0:])
	copy(h.PrevHash[:], b[4:])
	copy(h.MerkleRoot[:], b[36:])
	h.Time = le.Uint32(b[68:])
	h.Bits = le.Uint32(b[72:])
	h.Nonce = le.Uint32(b[76:])
	h.Nonce2 = le.Uint32(b[80:])
	h.Nonce3 = le.Uint32(b[84:])
	copy(h.Extranonce[:], b[88:])
	h.TimeOffset = le.Uint32(b[104:])
	h.TxCount = le.Uint16(b[108:])
	h.Flags = b[110]
	h.XorClearBits = b[111]
	copy(h.XorKey[:], b[112:])
	h.Height = le.Uint32(b[128:])
	copy(h.MMRhs[:], b[132:])
	return h, nil
}

// taggedHash is SHA256(SHA256(tag) || SHA256(tag) || data), as in BIP340.
func taggedHash(tag string, data []byte) [32]byte {
	t := sha256.Sum256([]byte(tag))
	h := sha256.New()
	h.Write(t[:])
	h.Write(t[:])
	h.Write(data)
	var out [32]byte
	h.Sum(out[:0])
	return out
}

func reversed(b [32]byte) [32]byte {
	var r [32]byte
	for i := range b {
		r[i] = b[31-i]
	}
	return r
}

// workBlock derives the 80 bytes the miner hashes. Everything except the
// nonce fields is fixed for a given header, so this is done once per job.
//
// Only the plain case is handled: Flags == 0 (work-block layout 0, no time
// offset) and an all-zero XOR key (so the block hash is the raw digest).
func (h *header) workBlock() ([workBytes]byte, error) {
	var work [workBytes]byte
	if h.Flags != 0 {
		return work, fmt.Errorf("header flags %#02x not supported (only 0)", h.Flags)
	}
	if h.XorKey != [16]byte{} {
		return work, errors.New("non-zero XOR key not supported")
	}
	le := binary.LittleEndian
	prevDisplay := reversed(h.PrevHash)

	xorKeyHash := taggedHash("Bitcoin block hash PoW XOR key", h.XorKey[:])

	// H1 commits to every header field that is not a nonce: 119 bytes.
	h1 := make([]byte, 0, 119)
	h1 = le.AppendUint32(h1, h.Version|headerV2Bit)
	h1 = append(h1, prevDisplay[:]...)
	h1 = le.AppendUint32(h1, h.Height)
	h1 = append(h1, h.MerkleRoot[:]...)
	h1 = le.AppendUint32(h1, h.Time)
	h1 = append(h1, 0)
	h1 = le.AppendUint32(h1, h.Bits)
	h1 = le.AppendUint32(h1, uint32(h.TxCount))
	h1 = append(h1, h.Flags, h.XorClearBits)
	h1 = append(h1, xorKeyHash[:]...)
	h1Hash := taggedHash("Bitcoin block header 1", h1)

	// H2 folds in the merge-mining slot.
	h2 := make([]byte, 0, 96)
	h2 = append(h2, h1Hash[:]...)
	h2 = append(h2, make([]byte, 32)...)
	h2 = append(h2, h.MMRhs[:]...)
	h2Hash := taggedHash("Merge-mining hook", h2)

	// root = BLAKE2b-256(u32 0 || H2 || extranonce): 52 bytes.
	leaf := make([]byte, 0, 52)
	leaf = append(leaf, 0, 0, 0, 0)
	leaf = append(leaf, h2Hash[:]...)
	leaf = append(leaf, h.Extranonce[:]...)
	root := blake2b.Sum256(leaf)

	// The previous block hash, hashed, with its first 6 bytes cleared.
	hidden := taggedHash("Bitcoin prevblock header, hashed", prevDisplay[:])
	for i := 0; i < 6; i++ {
		hidden[i] = 0
	}

	copy(work[0:], hidden[:])
	le.PutUint32(work[32:], h.Nonce)
	le.PutUint32(work[36:], h.Nonce2)
	le.PutUint32(work[40:], h.TimeOffset)
	le.PutUint32(work[44:], h.Nonce3)
	copy(work[48:], root[:])
	return work, nil
}

// powDigest is the proof-of-work hash of a work block with the given nonce.
// Printed as hex it is the block hash as RPC and explorers show it (valid
// while the XOR key is zero).
func powDigest(work [workBytes]byte, nonce uint32) [digestBytes]byte {
	binary.LittleEndian.PutUint32(work[nonceOffset:], nonce)
	return blake2b.Sum256(work[:])
}

// targetFromBits expands the compact nBits field into a 32-byte target, most
// significant byte first. A digest is valid proof of work when, compared
// from its first byte, it is <= the target.
func targetFromBits(bits uint32) ([digestBytes]byte, error) {
	var target [digestBytes]byte
	exponent := int(bits >> 24)
	mantissa := bits & 0x007fffff
	if bits&0x00800000 != 0 {
		return target, fmt.Errorf("bits %#08x: negative target", bits)
	}
	if exponent > 32 {
		return target, fmt.Errorf("bits %#08x: target overflows 256 bits", bits)
	}
	// The target is mantissa * 256^(exponent-3): the three mantissa bytes
	// sit `exponent` bytes from the least-significant end.
	m := [3]byte{byte(mantissa >> 16), byte(mantissa >> 8), byte(mantissa)}
	for i, b := range m {
		pos := 32 - exponent + i
		if pos < 0 || pos >= 32 {
			if b != 0 && pos < 0 {
				return target, fmt.Errorf("bits %#08x: target overflows 256 bits", bits)
			}
			continue
		}
		target[pos] = b
	}
	return target, nil
}
