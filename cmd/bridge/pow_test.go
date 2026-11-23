package main

import (
	"encoding/binary"
	"encoding/hex"
	"testing"
)

func mustHex(t *testing.T, s string) []byte {
	t.Helper()
	b, err := hex.DecodeString(s)
	if err != nil {
		t.Fatalf("bad hex in test: %v", err)
	}
	return b
}

// Block 4570 of a private regtest chain: `getblockheader <hash> false`.
const (
	regtestHeaderHex = "000000a09b3b986e49997b71ccc6a1f98d14f9e365b0f35e94fcfea80453bfc0" +
		"000000007ac8dca488bc860093cf8f1a848f758994bc4b050367e360a77d07aa" +
		"0db8883158d7bd6affff7f20fba08c450000000058d7bd6a00000000b10cf00d" +
		"0500000000000000000000000100000000000000000000000000000000000000" +
		"da11000000000000000000000000000000000000000000000000000000000000" +
		"00000000"
	regtestBlockHash = "00000000358740228439e7ae1a859932e4e396c1d932b94863dca06a646b066f"
)

// A real header must hash to its real block hash.
func TestRealRegtestHeaderHashesToItsBlockHash(t *testing.T) {
	raw := mustHex(t, regtestHeaderHex)
	h, err := parseHeader(raw)
	if err != nil {
		t.Fatal(err)
	}
	if again := h.serialize(); hex.EncodeToString(again[:]) != regtestHeaderHex {
		t.Fatalf("serialize(parse(header)) changed the header:\n got  %x\n want %s", again, regtestHeaderHex)
	}
	work, err := h.workBlock()
	if err != nil {
		t.Fatal(err)
	}
	digest := powDigest(work, h.Nonce)
	if got := hex.EncodeToString(digest[:]); got != regtestBlockHash {
		t.Fatalf("block hash\n got  %s\n want %s", got, regtestBlockHash)
	}
}

// The canonical work block published with Knots (profile_0_time_offset in
// src/test/data/block_header_v2.json) and its block hash.
func TestKnotsWorkBlockVector(t *testing.T) {
	raw := mustHex(t, knotsWorkHex)
	var work [workBytes]byte
	copy(work[:], raw)
	nonce := binary.LittleEndian.Uint32(work[nonceOffset:])

	digest := powDigest(work, nonce)
	reversedDigest := reversed(digest)
	const want = "04d78755b174467ec8537c230912ddd9bc4f28229b795b78490ad705cf5d494b"
	if got := hex.EncodeToString(reversedDigest[:]); got != want {
		t.Fatalf("reversed digest\n got  %s\n want %s", got, want)
	}
}

func TestTargetFromBits(t *testing.T) {
	cases := []struct {
		bits uint32
		want string
	}{
		{0x207fffff, "7fffff0000000000000000000000000000000000000000000000000000000000"}, // regtest
		{0x1d00ffff, "00000000ffff0000000000000000000000000000000000000000000000000000"},
		{0x1c7fffff, "000000007fffff00000000000000000000000000000000000000000000000000"},
		{0x03123456, "0000000000000000000000000000000000000000000000000000000000123456"},
	}
	for _, c := range cases {
		got, err := targetFromBits(c.bits)
		if err != nil {
			t.Errorf("bits %#08x: %v", c.bits, err)
			continue
		}
		if hex.EncodeToString(got[:]) != c.want {
			t.Errorf("bits %#08x:\n got  %x\n want %s", c.bits, got, c.want)
		}
	}
	if _, err := targetFromBits(0x21000001); err == nil {
		t.Error("bits 0x21000001: expected overflow error")
	}
}
