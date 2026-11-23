// Command bridge is the PC side of the megaminer: the controller to
// the chip's hashing engine.
//
// Mine real blocks on a regtest node (the default):
//
//	go run ./cmd/bridge -blocks 1
//
// For each block it asks the node for a block template, builds a block with
// a single coinbase transaction, reduces the header to the 80-byte work
// block, hands that to the chip, and submits the block once the chip finds a
// nonce. Every result from the chip is re-hashed on the PC before use.
//
// Other modes:
//
//	go run ./cmd/bridge -solver cpu -blocks 1    same, but the PC finds the nonce (no chip needed)
//	go run ./cmd/bridge -test-job -zero-bytes 1  fixed test job; print and verify the chip's shares
//
// By default the chip is reached over the serial port (miner_uart firmware).
// The serial port can only be open in one program at a time, so close PuTTY
// first. With -listen the chip is reached over the network instead
// (miner_net firmware): the bridge waits for the chip to connect to it.
//
//	go run ./cmd/bridge -listen :4242 -blocks 1
package main

import (
	"encoding/binary"
	"encoding/hex"
	"flag"
	"fmt"
	"log"
)

// knotsWorkHex is the 80-byte "asic_input" of profile_0_time_offset in
// Bitcoin Knots' src/test/data/block_header_v2.json.
const knotsWorkHex = "000000000000943aff74219e1f45899abfdf536373c0f2fc92e6fe58335cd0ad" +
	"0df0ad0b4433221158020000efcdab89" +
	"7e6326906eaa52fe59e03a14f1dfb8dd5d6e78497e56a8a6e4f4fb4d385e43db"

// mineBlocks mines `blocks` blocks on the node, one after another.
func mineBlocks(n *node, s solver, blocks int, payoutScript []byte) error {
	for i := 1; i <= blocks; i++ {
		tmpl, err := n.template()
		if err != nil {
			return err
		}

		var extranonce [16]byte // first 4 bytes left zero, as the gateway does
		copy(extranonce[4:], "megaminer")
		binary.BigEndian.PutUint16(extranonce[14:], uint16(i))

		cand, err := newCandidate(tmpl, payoutScript, extranonce)
		if err != nil {
			return err
		}
		work, err := cand.header.workBlock()
		if err != nil {
			return err
		}
		target, err := targetFromBits(cand.header.Bits)
		if err != nil {
			return err
		}
		log.Printf("block %d of %d: height %d, target %x", i, blocks, tmpl.Height, target)

		nonce, err := s.solve(job{work: work, target: target})
		if err != nil {
			return err
		}
		cand.header.Nonce = nonce
		hash := powDigest(work, nonce)
		hashHex := hex.EncodeToString(hash[:])
		log.Printf("  solved: nonce %d, block hash %s", nonce, hashHex)

		reason, err := n.submitBlock(cand.serialize())
		if err != nil {
			return err
		}
		if reason != "" {
			return fmt.Errorf("node rejected the block: %s", reason)
		}
		best, err := n.bestBlockHash()
		if err != nil {
			return err
		}
		if best != hashHex {
			return fmt.Errorf("node accepted the block but its tip is %s, not ours", best)
		}
		log.Printf("  ACCEPTED: the node's chain tip is now our block, height %d", tmpl.Height)
	}
	return nil
}

// runTestJob sends the fixed Knots test job and verifies shares forever.
func runTestJob(c *chip, zeroBytes int, startNonce uint32) error {
	if zeroBytes < 0 || zeroBytes > digestBytes {
		return fmt.Errorf("zero-bytes must be 0..%d, got %d", digestBytes, zeroBytes)
	}
	var j job
	raw, err := hex.DecodeString(knotsWorkHex)
	if err != nil {
		return err
	}
	copy(j.work[:], raw)
	binary.LittleEndian.PutUint32(j.work[nonceOffset:], startNonce)
	for i := zeroBytes; i < digestBytes; i++ {
		j.target[i] = 0xff
	}

	if err := c.start(j); err != nil {
		return err
	}
	log.Printf("test job running: %d leading zero byte(s), start nonce %d", zeroBytes, startNonce)
	for verified := 1; ; verified++ {
		nonce, err := c.nextShare(j)
		if err != nil {
			return err
		}
		log.Printf("share: nonce %d, digest %x -> verified (%d so far)", nonce, powDigest(j.work, nonce), verified)
	}
}

func run() error {
	var (
		portName  = flag.String("port", "COM4", "serial port the USB-serial adapter is on")
		listen    = flag.String("listen", "", "wait for the chip to connect over TCP on this address (e.g. :4242) instead of using the serial port")
		sv1       = flag.String("sv1", "", "mine work from a DATUM gateway's SV1 port (e.g. 127.0.0.1:23334) instead of building blocks from the node")
		user      = flag.String("user", "megaminer", "SV1 user (worker) name")
		solverArg = flag.String("solver", "chip", "who searches for the nonce: chip or cpu")
		blocks    = flag.Int("blocks", 1, "how many blocks to mine")
		rpcURL    = flag.String("rpc", "http://127.0.0.1:18443", "node RPC address")
		rpcUser   = flag.String("rpcuser", "regtest", "node RPC user")
		rpcPass   = flag.String("rpcpass", "regtest", "node RPC password")
		payout    = flag.String("payout-script", "51", "hex output script the block reward is paid to (51 = anyone can spend)")

		testJob    = flag.Bool("test-job", false, "run the fixed test job instead of mining on the node")
		zeroBytes  = flag.Int("zero-bytes", 1, "test job: leading zero bytes a share must have")
		startNonce = flag.Uint("start-nonce", 0, "test job: nonce the chip starts counting from")
	)
	flag.Parse()

	// connect reaches the chip by whichever link was asked for.
	connect := func() (*chip, error) {
		if *listen != "" {
			return listenForChip(*listen)
		}
		return openChip(*portName)
	}

	if *sv1 != "" {
		if *solverArg == "cpu" {
			return runSV1(*sv1, *user, nil)
		}
		c, err := connect()
		if err != nil {
			return err
		}
		defer c.close()
		return runSV1(*sv1, *user, c)
	}

	if *testJob {
		c, err := connect()
		if err != nil {
			return err
		}
		defer c.close()
		return runTestJob(c, *zeroBytes, uint32(*startNonce))
	}

	payoutScript, err := hex.DecodeString(*payout)
	if err != nil {
		return fmt.Errorf("payout-script: %w", err)
	}

	var s solver
	switch *solverArg {
	case "cpu":
		s = cpuSolver{}
	case "chip":
		c, err := connect()
		if err != nil {
			return err
		}
		// When done, stop the chip reporting shares for the last job, then
		// release the port.
		defer func() {
			if err := c.quiet([workBytes]byte{}); err != nil {
				log.Printf("could not quiet the chip: %v", err)
			}
			c.close()
		}()
		s = c
	default:
		return fmt.Errorf("unknown solver %q (want chip or cpu)", *solverArg)
	}

	return mineBlocks(newNode(*rpcURL, *rpcUser, *rpcPass), s, *blocks, payoutScript)
}

func main() {
	if err := run(); err != nil {
		log.Fatal(err)
	}
}
