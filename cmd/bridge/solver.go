package main

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"errors"
	"fmt"
	"io"
	"log"
	"net"
	"strconv"
	"strings"

	"go.bug.st/serial"
)

// job is one unit of work: find a nonce whose digest is <= target.
type job struct {
	work   [workBytes]byte   // the search starts at the nonce stored in here
	target [digestBytes]byte // most significant byte first, like the digest
}

// meets reports whether digest satisfies the job's target.
func (j job) meets(digest [digestBytes]byte) bool {
	return bytes.Compare(digest[:], j.target[:]) <= 0
}

// solver finds a nonce for a job. The chip is one; the PC's own CPU is
// another, used to test the rest of the pipeline without the chip.
type solver interface {
	solve(j job) (nonce uint32, err error)
	close() error
}

// cpuSolver searches on the PC.
type cpuSolver struct{}

func (cpuSolver) solve(j job) (uint32, error) {
	start := binary.LittleEndian.Uint32(j.work[nonceOffset:])
	for n := start; ; n++ {
		if j.meets(powDigest(j.work, n)) {
			return n, nil
		}
		if n+1 == start {
			return 0, errors.New("no nonce satisfies the target")
		}
	}
}

func (cpuSolver) close() error { return nil }

// chip is the megaminer, speaking the line protocol documented at the
// top of miner_uart.c. The link is either a serial port (miner_uart) or a TCP
// connection the chip opened to us (miner_net); the protocol is the same.
type chip struct {
	port  io.ReadWriteCloser
	lines *bufio.Scanner
}

func openChip(portName string) (*chip, error) {
	port, err := serial.Open(portName, &serial.Mode{
		BaudRate: 250000,
		DataBits: 8,
		Parity:   serial.NoParity,
		StopBits: serial.OneStopBit,
	})
	if err != nil {
		return nil, fmt.Errorf("open %s (is PuTTY still holding it?): %w", portName, err)
	}
	return &chip{port: port, lines: bufio.NewScanner(port)}, nil
}

// listenForChip waits for the chip to connect to us over the network.
// addr is a listen address such as ":4242".
func listenForChip(addr string) (*chip, error) {
	l, err := net.Listen("tcp", addr)
	if err != nil {
		return nil, fmt.Errorf("listen on %s: %w", addr, err)
	}
	defer l.Close() // one chip, one connection
	log.Printf("waiting for the chip to connect on %s", l.Addr())
	conn, err := l.Accept()
	if err != nil {
		return nil, err
	}
	log.Printf("chip connected from %s", conn.RemoteAddr())
	return &chip{port: conn, lines: bufio.NewScanner(conn)}, nil
}

func (c *chip) close() error { return c.port.Close() }

// readLine returns the chip's next non-empty line.
func (c *chip) readLine() (string, error) {
	for c.lines.Scan() {
		if line := strings.TrimSpace(c.lines.Text()); line != "" {
			return line, nil
		}
	}
	if err := c.lines.Err(); err != nil {
		return "", err
	}
	return "", errors.New("connection to the chip closed")
}

// start sends a job and waits for the chip to accept it. Lines that arrive
// before the acknowledgement belong to the previous job and are dropped.
func (c *chip) start(j job) error {
	// The leading UUUU is throwaway padding: the first characters of a
	// transmission can arrive damaged, and the chip ignores everything
	// before the J.
	line := "UUUUJ" + hex.EncodeToString(j.work[:]) + hex.EncodeToString(j.target[:]) + "\n"

	const attempts = 3
	for attempt := 1; attempt <= attempts; attempt++ {
		if _, err := c.port.Write([]byte(line)); err != nil {
			return fmt.Errorf("send job: %w", err)
		}
		for {
			reply, err := c.readLine()
			if err != nil {
				return err
			}
			if reply == "job ok" {
				return nil
			}
			if strings.HasPrefix(reply, "bad line") {
				log.Printf("chip rejected the job (attempt %d of %d): %s", attempt, attempts, reply)
				break
			}
			if strings.HasPrefix(reply, "note:") {
				log.Printf("chip: %s", reply)
			}
		}
	}
	return fmt.Errorf("chip rejected the job %d times", attempts)
}

// nextShare waits for the chip's next share for job j and checks it with our
// own hash. The chip is never taken at its word.
func (c *chip) nextShare(j job) (uint32, error) {
	for {
		line, err := c.readLine()
		if err != nil {
			return 0, err
		}
		fields := strings.Fields(line)
		if fields[0] != "share" {
			log.Printf("chip: %s", line)
			continue
		}
		nonce, digest, err := parseShare(fields)
		if err != nil {
			return 0, fmt.Errorf("chip sent %q: %w", line, err)
		}
		ours := powDigest(j.work, nonce)
		if ours != digest {
			return 0, fmt.Errorf("chip's digest for nonce %d is wrong:\n chip %x\n ours %x", nonce, digest, ours)
		}
		if !j.meets(ours) {
			return 0, fmt.Errorf("chip's share for nonce %d is above the target: %x", nonce, ours)
		}
		return nonce, nil
	}
}

func parseShare(fields []string) (nonce uint32, digest [digestBytes]byte, err error) {
	if len(fields) != 3 {
		return 0, digest, fmt.Errorf("want 3 fields, got %d", len(fields))
	}
	n, err := strconv.ParseUint(fields[1], 10, 32)
	if err != nil {
		return 0, digest, fmt.Errorf("parse nonce: %w", err)
	}
	raw, err := hex.DecodeString(fields[2])
	if err != nil || len(raw) != digestBytes {
		return 0, digest, fmt.Errorf("bad digest %q", fields[2])
	}
	copy(digest[:], raw)
	return uint32(n), digest, nil
}

// solve implements solver.
func (c *chip) solve(j job) (uint32, error) {
	if err := c.start(j); err != nil {
		return 0, err
	}
	return c.nextShare(j)
}

// quiet gives the chip a job no digest can satisfy, so it stops reporting
// shares. (It keeps hashing; the protocol has no stop command.)
func (c *chip) quiet(work [workBytes]byte) error {
	return c.start(job{work: work}) // zero target
}
