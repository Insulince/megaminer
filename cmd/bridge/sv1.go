package main

// SV1 (Stratum V1) client for a DATUM gateway serving XBT (BLAKE2b) work.
//
// The gateway does almost all of the header work itself. What the hasher is
// sent, and what it must do with it (datum_gateway src/datum_stratum.c and
// src/datum_pow.c, checked against live gateways):
//
//	mining.subscribe reply   extranonce1 (4 bytes as hex), extranonce2 size 8
//	mining.notify params     [0] job id
//	                         [1] prevhash: 32 bytes, already the "hidden" form,
//	                             used as bytes 0-31 of the work block as is
//	                         [2] coinb1: 39 bytes (a commitment to the header)
//	                         [3] coinb2: empty   [4] merkle branches: none
//	                         [5] version         [6] nbits: compact target
//	                         [7] ntime: 8 bytes, used as bytes 40-47 as is
//	                         [8] clean jobs
//
//	root = BLAKE2b-256( 0x00 || coinb1 || extranonce1 || extranonce2 )   52 bytes in
//	work = prevhash(32) || nonce(8) || ntime(8) || root(32)              80 bytes
//	digest = BLAKE2b-256(work)
//
// A share needs the digest's first 4 bytes to be zero (the gateway rejects
// anything else outright) and the digest to be at or below the nbits target.
//
//	mining.submit params     [user, job id, extranonce2 hex, ntime hex, nonce hex]
//
// The nonce is sent as 16 hex digits: the 8 nonce bytes exactly as they sit
// in the work block.

import (
	"bufio"
	"bytes"
	"encoding/binary"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"log"
	"net"
	"runtime"
	"strconv"
	"strings"
	"sync/atomic"
	"time"

	"golang.org/x/crypto/blake2b"
)

// sv1Job is one mining.notify, turned into a work block.
type sv1Job struct {
	id     string
	ntime  string // as sent; echoed back in a submit
	work   [workBytes]byte
	target [digestBytes]byte
}

// sv1Root computes the work block's last 32 bytes.
func sv1Root(coinb1, extranonce1, extranonce2 []byte) [32]byte {
	leaf := make([]byte, 0, 1+len(coinb1)+len(extranonce1)+len(extranonce2))
	leaf = append(leaf, 0x00)
	leaf = append(leaf, coinb1...)
	leaf = append(leaf, extranonce1...)
	leaf = append(leaf, extranonce2...)
	return blake2b.Sum256(leaf)
}

// newSV1Job builds the work block for a mining.notify.
func newSV1Job(params []json.RawMessage, extranonce1, extranonce2 []byte) (*sv1Job, error) {
	if len(params) < 8 {
		return nil, fmt.Errorf("notify has %d params, want at least 8", len(params))
	}
	str := func(i int) (string, error) {
		var s string
		if err := json.Unmarshal(params[i], &s); err != nil {
			return "", fmt.Errorf("notify param %d: %w", i, err)
		}
		return s, nil
	}
	hexBytes := func(i, want int) ([]byte, error) {
		s, err := str(i)
		if err != nil {
			return nil, err
		}
		b, err := hex.DecodeString(s)
		if err != nil || len(b) != want {
			return nil, fmt.Errorf("notify param %d: want %d bytes of hex, got %q", i, want, s)
		}
		return b, nil
	}

	id, err := str(0)
	if err != nil {
		return nil, err
	}
	prevhash, err := hexBytes(1, 32)
	if err != nil {
		return nil, err
	}
	coinb1, err := hexBytes(2, 39)
	if err != nil {
		return nil, err
	}
	nbits, err := hexBytes(6, 4)
	if err != nil {
		return nil, err
	}
	ntime, err := hexBytes(7, 8)
	if err != nil {
		return nil, err
	}

	j := &sv1Job{id: id, ntime: hex.EncodeToString(ntime)}
	root := sv1Root(coinb1, extranonce1, extranonce2)
	copy(j.work[0:32], prevhash)
	// bytes 32-39: the nonce, left zero for the solver
	copy(j.work[40:48], ntime)
	copy(j.work[48:80], root[:])

	if j.target, err = targetFromBits(binary.BigEndian.Uint32(nbits)); err != nil {
		return nil, err
	}
	// The gateway never accepts a digest whose first 4 bytes are not zero,
	// whatever nbits says.
	floor := [digestBytes]byte{}
	for i := 4; i < digestBytes; i++ {
		floor[i] = 0xff
	}
	if bytes.Compare(j.target[:], floor[:]) > 0 {
		j.target = floor
	}
	return j, nil
}

// sv1Conn is one connection to a gateway.
type sv1Conn struct {
	conn        net.Conn
	lines       *bufio.Scanner
	user        string
	extranonce1 []byte
	extranonce2 []byte
	nextID      int
}

type sv1Msg struct {
	ID     json.RawMessage   `json:"id"`
	Method string            `json:"method"`
	Params []json.RawMessage `json:"params"`
	Result json.RawMessage   `json:"result"`
	Error  json.RawMessage   `json:"error"`
}

func (s *sv1Conn) send(method string, params ...any) error {
	s.nextID++
	msg, err := json.Marshal(map[string]any{"id": s.nextID, "method": method, "params": params})
	if err != nil {
		return err
	}
	_, err = s.conn.Write(append(msg, '\n'))
	return err
}

func (s *sv1Conn) read() (*sv1Msg, error) {
	if !s.lines.Scan() {
		if err := s.lines.Err(); err != nil {
			return nil, err
		}
		return nil, errors.New("gateway closed the connection")
	}
	var m sv1Msg
	if err := json.Unmarshal(s.lines.Bytes(), &m); err != nil {
		return nil, fmt.Errorf("gateway sent %q: %w", s.lines.Text(), err)
	}
	return &m, nil
}

// dialSV1 connects, subscribes and logs in.
func dialSV1(addr, user string) (*sv1Conn, error) {
	conn, err := net.DialTimeout("tcp", addr, 10*time.Second)
	if err != nil {
		return nil, err
	}
	s := &sv1Conn{conn: conn, lines: bufio.NewScanner(conn), user: user,
		extranonce2: []byte("megamine")} // any 8 bytes; ours to choose

	if err := s.send("mining.subscribe", "megaminer-atmega1284p/0.1"); err != nil {
		return nil, err
	}
	m, err := s.read()
	if err != nil {
		return nil, err
	}
	// result: [[subscriptions...], "extranonce1 hex", extranonce2 size]
	var result []json.RawMessage
	var en1 string
	var en2Size int
	if err := json.Unmarshal(m.Result, &result); err != nil || len(result) < 3 ||
		json.Unmarshal(result[1], &en1) != nil || json.Unmarshal(result[2], &en2Size) != nil {
		return nil, fmt.Errorf("unexpected subscribe reply: %s", m.Result)
	}
	if s.extranonce1, err = hex.DecodeString(en1); err != nil || len(s.extranonce1) != 4 {
		return nil, fmt.Errorf("unexpected extranonce1 %q", en1)
	}
	if en2Size != len(s.extranonce2) {
		return nil, fmt.Errorf("gateway wants a %d-byte extranonce2, this client uses %d", en2Size, len(s.extranonce2))
	}
	log.Printf("sv1: subscribed to %s, extranonce1 %s", addr, en1)

	return s, s.send("mining.authorize", user, "x")
}

func (s *sv1Conn) submit(j *sv1Job, nonce [8]byte) error {
	return s.send("mining.submit", s.user, j.id,
		hex.EncodeToString(s.extranonce2), j.ntime, hex.EncodeToString(nonce[:]))
}

// runSV1 mines whatever the gateway sends, forever. With c == nil the
// PC's own CPU does the hashing (used to prove the construction, since the
// chip will essentially never find a real share); otherwise the chip does.
func runSV1(addr, user string, c *chip) error {
	s, err := dialSV1(addr, user)
	if err != nil {
		return err
	}
	defer s.conn.Close()

	msgs := make(chan *sv1Msg)
	errs := make(chan error, 2)
	go func() {
		for {
			m, err := s.read()
			if err != nil {
				errs <- err
				return
			}
			msgs <- m
		}
	}()

	// Results from whichever solver is running: the 8 nonce bytes of a share.
	type found struct {
		job   *sv1Job
		nonce [8]byte
	}
	shares := make(chan found, 16)
	var current *sv1Job
	var cpuGeneration atomic.Int64 // bumping this stops the old CPU workers
	var hashes atomic.Uint64

	chipLines := make(chan string, 16)
	if c != nil {
		go func() {
			for {
				line, err := c.readLine()
				if err != nil {
					errs <- fmt.Errorf("chip: %w", err)
					return
				}
				chipLines <- line
			}
		}()
	}

	startJob := func(j *sv1Job) error {
		current = j
		if c != nil {
			line := "J" + hex.EncodeToString(j.work[:]) + hex.EncodeToString(j.target[:]) + "\n"
			_, err := c.port.Write([]byte(line))
			return err
		}
		gen := cpuGeneration.Add(1)
		workers := runtime.NumCPU()
		for w := 0; w < workers; w++ {
			go func(w int) {
				work := j.work
				binary.LittleEndian.PutUint32(work[36:], uint32(w)) // each worker its own range
				for n := uint32(0); cpuGeneration.Load() == gen; n++ {
					binary.LittleEndian.PutUint32(work[32:], n)
					d := blake2b.Sum256(work[:])
					if d[0] == 0 && d[1] == 0 && bytes.Compare(d[:], j.target[:]) <= 0 {
						var nonce [8]byte
						copy(nonce[:], work[32:40])
						shares <- found{j, nonce}
					}
					if n&0xfffff == 0xfffff {
						hashes.Add(0x100000)
					}
				}
			}(w)
		}
		return nil
	}

	tick := time.NewTicker(10 * time.Second)
	defer tick.Stop()
	pending := map[string]string{} // submit id -> description, to label replies

	for {
		select {
		case err := <-errs:
			return err

		case m := <-msgs:
			switch m.Method {
			case "mining.notify":
				j, err := newSV1Job(m.Params, s.extranonce1, s.extranonce2)
				if err != nil {
					return err
				}
				log.Printf("sv1: job %s  prevhash %x...  target %x...", j.id, j.work[:10], j.target[:8])
				if err := startJob(j); err != nil {
					return err
				}
			case "mining.set_difficulty":
				log.Printf("sv1: difficulty %s", m.Params)
			case "":
				id := strings.Trim(string(m.ID), `"`)
				if what, ok := pending[id]; ok {
					delete(pending, id)
					log.Printf("sv1: %s -> result %s error %s", what, m.Result, m.Error)
				} else if string(m.Error) != "null" && len(m.Error) > 0 {
					log.Printf("sv1: reply %s: error %s", id, m.Error)
				}
			default:
				log.Printf("sv1: ignoring %s", m.Method)
			}

		case f := <-shares:
			if f.job != current {
				continue // found on a job that has since been replaced
			}
			work := f.job.work
			copy(work[32:40], f.nonce[:])
			log.Printf("SHARE on job %s: nonce %x digest %x", f.job.id, f.nonce, blake2b.Sum256(work[:]))
			if err := s.submit(f.job, f.nonce); err != nil {
				return err
			}
			pending[strconv.Itoa(s.nextID)] = "share submit"

		case line := <-chipLines:
			fields := strings.Fields(line)
			if len(fields) == 3 && fields[0] == "share" && current != nil {
				n, err := strconv.ParseUint(fields[1], 10, 32)
				if err != nil {
					log.Printf("chip: %s", line)
					continue
				}
				var nonce [8]byte
				binary.LittleEndian.PutUint32(nonce[:], uint32(n))
				work := current.work
				copy(work[32:40], nonce[:])
				d := blake2b.Sum256(work[:])
				if hex.EncodeToString(d[:]) != fields[2] {
					continue // a share for a job that has since been replaced
				}
				shares <- found{current, nonce}
			} else {
				log.Printf("chip: %s", line)
			}

		case <-tick.C:
			if c == nil {
				log.Printf("cpu: %.1f million hashes per second", float64(hashes.Swap(0))/10e6)
			}
		}
	}
}
