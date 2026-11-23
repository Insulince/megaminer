package main

import (
	"crypto/sha256"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
)

// candidate is a block that is complete except for a winning nonce.
type candidate struct {
	header   header
	coinbase []byte // the coinbase transaction, in its with-witness wire form
}

// newCandidate turns a block template into a candidate block containing only
// a coinbase transaction that pays payoutScript.
func newCandidate(t *blockTemplate, payoutScript []byte, extranonce [16]byte) (*candidate, error) {
	if len(t.Transactions) != 0 {
		return nil, fmt.Errorf("template has %d transactions; only empty blocks are supported", len(t.Transactions))
	}
	prevDisplay, err := hex.DecodeString(t.PreviousBlockHash)
	if err != nil || len(prevDisplay) != 32 {
		return nil, fmt.Errorf("bad previousblockhash %q", t.PreviousBlockHash)
	}
	bitsRaw, err := hex.DecodeString(t.Bits)
	if err != nil || len(bitsRaw) != 4 {
		return nil, fmt.Errorf("bad bits %q", t.Bits)
	}
	var commitment []byte
	if t.DefaultWitnessCommitment != "" {
		if commitment, err = hex.DecodeString(t.DefaultWitnessCommitment); err != nil {
			return nil, fmt.Errorf("bad default_witness_commitment: %w", err)
		}
	}

	coinbase, txid, err := buildCoinbase(t.Height, t.CoinbaseValue, payoutScript, commitment)
	if err != nil {
		return nil, err
	}

	c := &candidate{coinbase: coinbase}
	c.header = header{
		Version:    t.Version | headerV2Bit,
		PrevHash:   reversed([32]byte(prevDisplay)),
		MerkleRoot: txid, // with a single transaction, the merkle root is its txid
		Time:       t.CurTime,
		Bits:       binary.BigEndian.Uint32(bitsRaw),
		Extranonce: extranonce,
		TxCount:    1,
		Height:     t.Height,
	}
	return c, nil
}

// serialize returns the full block: header, transaction count, coinbase.
func (c *candidate) serialize() []byte {
	h := c.header.serialize()
	block := append([]byte{}, h[:]...)
	block = append(block, 1) // transaction count
	return append(block, c.coinbase...)
}

func doubleSHA256(b []byte) [32]byte {
	first := sha256.Sum256(b)
	return sha256.Sum256(first[:])
}

// appendVarBytes appends a length-prefixed byte string (lengths under 253,
// which is all a coinbase needs).
func appendVarBytes(dst, b []byte) []byte {
	dst = append(dst, byte(len(b)))
	return append(dst, b...)
}

// heightPush is the BIP34 block-height push that must open the coinbase
// input script: the height as a minimal little-endian script number.
func heightPush(height uint32) ([]byte, error) {
	if height <= 16 {
		return nil, fmt.Errorf("height %d needs the small-number encoding, not supported", height)
	}
	var num []byte
	for v := height; v > 0; v >>= 8 {
		num = append(num, byte(v))
	}
	if num[len(num)-1]&0x80 != 0 {
		num = append(num, 0) // keep the number positive
	}
	return appendVarBytes(nil, num), nil
}

// buildCoinbase makes the block's only transaction. It returns the wire form
// (with witness data) and the txid in wire order.
func buildCoinbase(height uint32, value int64, payoutScript, witnessCommitment []byte) (tx []byte, txid [32]byte, err error) {
	if len(payoutScript) == 0 || len(payoutScript) >= 253 {
		return nil, txid, errors.New("payout script must be 1..252 bytes")
	}
	script, err := heightPush(height)
	if err != nil {
		return nil, txid, err
	}
	script = appendVarBytes(script, []byte("megaminer ATmega1284P"))

	le := binary.LittleEndian

	// Input: spends nothing (all-zero outpoint, index 0xffffffff).
	var in []byte
	in = append(in, 1) // input count
	in = append(in, make([]byte, 32)...)
	in = le.AppendUint32(in, 0xffffffff)
	in = appendVarBytes(in, script)
	in = le.AppendUint32(in, 0xffffffff) // sequence

	// Outputs: the payout, then the witness commitment if the node gave one.
	var out []byte
	nOut := byte(1)
	if witnessCommitment != nil {
		nOut = 2
	}
	out = append(out, nOut)
	out = le.AppendUint64(out, uint64(value))
	out = appendVarBytes(out, payoutScript)
	if witnessCommitment != nil {
		out = le.AppendUint64(out, 0)
		out = appendVarBytes(out, witnessCommitment)
	}

	version := le.AppendUint32(nil, 1)
	locktime := le.AppendUint32(nil, 0)

	// The txid covers the transaction without witness data.
	var stripped []byte
	stripped = append(stripped, version...)
	stripped = append(stripped, in...)
	stripped = append(stripped, out...)
	stripped = append(stripped, locktime...)
	txid = doubleSHA256(stripped)

	if witnessCommitment == nil {
		return stripped, txid, nil
	}

	// With a commitment, the coinbase must carry one 32-byte witness item
	// (the "witness reserved value", all zeros).
	tx = append(tx, version...)
	tx = append(tx, 0x00, 0x01) // segwit marker and flag
	tx = append(tx, in...)
	tx = append(tx, out...)
	tx = append(tx, 1, 32) // one witness item, 32 bytes long
	tx = append(tx, make([]byte, 32)...)
	tx = append(tx, locktime...)
	return tx, txid, nil
}
