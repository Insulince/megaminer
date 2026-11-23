// Command ota sends a new firmware to the megaminer over WiFi.
//
//	go run ./cmd/ota -host <miner address> build/miner_sv2.bin
//	make ota TARGET=miner_sv2            (builds the .bin first)
//
// It asks the running miner to restart into its bootloader (POST /cmd
// cmd=update), waits until the bootloader answers on the same address
// (GET /boot), uploads the image (POST /flash with its CRC-32), and then
// waits for the miner to come back (GET /status). If the chip is already
// sitting in its bootloader (button menu, or an unfinished update), the first
// step is skipped. If nothing answers at first (joining the WiFi,
// restarting), it waits up to 60 s before deciding which it is. See boot.c
// and bootshare.h.
//
// The console password, if the chip has one, is asked for with the typing
// hidden, and asked again if wrong (3 tries).
// -pass or MEGAMINER_PASS still work for scripts. It is checked before the
// image is sent: the miner's /status says whether one is set, and the
// bootloader answers an empty POST /flash with 403 (wrong password) or 400
// (password fine, nothing to write) without touching the flash.
package main

import (
	"bytes"
	"errors"
	"flag"
	"fmt"
	"hash/crc32"
	"io"
	"net/http"
	"net/url"
	"os"
	"strings"
	"time"
)

const appMax = 0x1E000 // bytes below the boot section (bootshare.h APP_MAX)

func main() {
	host := flag.String("host", "192.168.1.193", "the miner's address")
	pass := flag.String("pass", os.Getenv("MEGAMINER_PASS"), "console password (default $MEGAMINER_PASS; asked for, hidden, when empty)")
	flag.Parse()
	if flag.NArg() != 1 {
		fmt.Fprintln(os.Stderr, "usage: ota [-host ADDR] [-pass PASSWORD] FIRMWARE.bin")
		os.Exit(2)
	}
	if err := run(*host, &password{value: *pass, given: *pass != ""}, flag.Arg(0)); err != nil {
		fmt.Fprintln(os.Stderr, "ota:", err)
		os.Exit(1)
	}
}

// password: the console password, typed (hidden) when needed.
type password struct {
	value string
	given bool // from -pass / MEGAMINER_PASS: wrong means stop, not ask
	tries int
}

// ask prompts (again). why is said first, e.g. "wrong password".
func (p *password) ask(why string) error {
	if p.given {
		return fmt.Errorf("%s (the one from -pass / MEGAMINER_PASS)", why)
	}
	if p.tries == 3 {
		return fmt.Errorf("%s, 3 tries; nothing was written", why)
	}
	p.tries++
	fmt.Println(why)
	v, err := readHidden("console password: ")
	if err != nil {
		return err
	}
	p.value = v
	return nil
}

func run(host string, pass *password, file string) error {
	img, err := os.ReadFile(file)
	if err != nil {
		return err
	}
	if len(img) < 256 || len(img) > appMax {
		return fmt.Errorf("%s is %d bytes; a program must be 256-%d", file, len(img), appMax)
	}
	sum := crc32.ChecksumIEEE(img)
	base := "http://" + host
	client := &http.Client{Timeout: 5 * time.Second}
	fmt.Printf("%s: %d bytes, crc32 %08x\n", file, len(img), sum)

	// Silence is not "the miner": the chip may be joining the WiFi (telnet
	// light blinking) or restarting. Wait for it to answer either way.
	// (First real run, 2026-11-19: the old code took no answer for the miner,
	// sent "update" into the void and gave up, with the chip in its bootloader.)
	st := probe(client, base)
	if st == unreachable {
		fmt.Print("waiting for the chip to answer ")
		deadline := time.Now().Add(60 * time.Second)
		for st == unreachable {
			if time.Now().After(deadline) {
				fmt.Println()
				return fmt.Errorf("nothing answers at %s for 60 s (is it on the WiFi? telnet light steady?)", host)
			}
			fmt.Print(".")
			time.Sleep(time.Second)
			st = probe(client, base)
		}
		fmt.Println()
	}

	if st == bootloader {
		fmt.Println("the chip is already in its bootloader")
	} else {
		if pass.value == "" && minerHasPassword(client, base) {
			if err := pass.ask("the miner has a console password"); err != nil {
				return err
			}
		}
		fmt.Println("asking the miner to restart into its bootloader ...")
		for {
			form := url.Values{"pass": {pass.value}, "cmd": {"update"}}
			req, err := http.NewRequest(http.MethodPost, base+"/cmd", strings.NewReader(form.Encode()))
			if err != nil {
				return err
			}
			req.Header.Set("Content-Type", "application/x-www-form-urlencoded")
			req.Header.Set("X-Megaminer", "1") // the miner refuses commands without it (forged-form protection)
			resp, err := client.Do(req)
			if err != nil {
				return fmt.Errorf("the miner did not answer: %w", err)
			}
			body, _ := io.ReadAll(resp.Body)
			resp.Body.Close()
			if resp.StatusCode == http.StatusForbidden {
				if err := pass.ask("wrong password"); err != nil {
					return err
				}
				continue
			}
			if resp.StatusCode != http.StatusOK {
				return fmt.Errorf("the miner refused: %s", strings.TrimSpace(string(body)))
			}
			fmt.Println("miner:", strings.TrimSpace(string(body)))
			break
		}
		fmt.Print("waiting for the bootloader (it joins the WiFi: 10-30 s) ")
		deadline := time.Now().Add(90 * time.Second)
		time.Sleep(3 * time.Second)
		for probe(client, base) != bootloader {
			if time.Now().After(deadline) {
				fmt.Println()
				return errors.New("the bootloader did not answer within 90 s")
			}
			fmt.Print(".")
			time.Sleep(time.Second)
		}
		fmt.Println(" there")
	}

	// The password, checked before a single page is erased.
	for {
		code, answer, err := checkBootPass(client, base, pass.value)
		if err != nil {
			return err
		}
		if code == http.StatusBadRequest {
			break // password fine (or none set); the empty upload was refused
		}
		if code != http.StatusForbidden {
			return fmt.Errorf("unexpected answer to the password check (%d): %s", code, answer)
		}
		why := "wrong password"
		if pass.value == "" {
			why = "the chip has a console password"
		}
		if err := pass.ask(why); err != nil {
			return err
		}
	}

	fmt.Println("sending ...")
	req, err := http.NewRequest(http.MethodPost, base+"/flash", bytes.NewReader(img))
	if err != nil {
		return err
	}
	req.Header.Set("X-Crc32", fmt.Sprintf("%08x", sum))
	if pass.value != "" {
		req.Header.Set("X-Pass", pass.value)
	}
	start := time.Now()
	resp, err := (&http.Client{Timeout: 3 * time.Minute}).Do(req)
	if err != nil {
		return fmt.Errorf("sending failed: %w", err)
	}
	body, _ := io.ReadAll(resp.Body)
	resp.Body.Close()
	answer := strings.TrimSpace(string(body))
	if resp.StatusCode != http.StatusOK {
		return fmt.Errorf("the bootloader refused (%d): %s", resp.StatusCode, answer)
	}
	fmt.Printf("bootloader: %s (%.1f s)\n", answer, time.Since(start).Seconds())

	fmt.Print("waiting for the miner to come back ")
	deadline := time.Now().Add(90 * time.Second)
	time.Sleep(3 * time.Second)
	for {
		resp, err := client.Get(base + "/status")
		if err == nil {
			resp.Body.Close()
			if resp.StatusCode == http.StatusOK {
				fmt.Println(" it is running")
				return nil
			}
		}
		if time.Now().After(deadline) {
			fmt.Println()
			return errors.New("written and verified, but the miner's web page has not come back within 90 s")
		}
		fmt.Print(".")
		time.Sleep(time.Second)
	}
}

type chipState int

const (
	unreachable chipState = iota // no HTTP answer at all
	miner                        // an answer, but not the bootloader's
	bootloader
)

// probe: GET /boot answers "bootloader" only from the bootloader (the miner
// itself says 404); no answer at all means neither can be told yet.
func probe(c *http.Client, base string) chipState {
	resp, err := c.Get(base + "/boot")
	if err != nil {
		return unreachable
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 64))
	if resp.StatusCode == http.StatusOK && strings.HasPrefix(string(body), "bootloader") {
		return bootloader
	}
	return miner
}

// checkBootPass: an empty POST /flash. The bootloader (boot.c header_line)
// checks the password before the length, so 403 = wrong password and 400 =
// password fine but nothing to write; the flash is not touched either way.
// (It lights the bootloader's "last attempt failed" red until the upload
// starts.)
func checkBootPass(c *http.Client, base, pass string) (int, string, error) {
	req, err := http.NewRequest(http.MethodPost, base+"/flash", http.NoBody)
	if err != nil {
		return 0, "", err
	}
	req.Header.Set("X-Crc32", "00000000")
	if pass != "" {
		req.Header.Set("X-Pass", pass)
	}
	resp, err := c.Do(req)
	if err != nil {
		return 0, "", fmt.Errorf("the bootloader did not answer the password check: %w", err)
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 256))
	return resp.StatusCode, strings.TrimSpace(string(body)), nil
}

// minerHasPassword: the miner's /status carries "pw":1 when a console
// password is set. Unknown (no answer) counts as no, so /cmd decides.
func minerHasPassword(c *http.Client, base string) bool {
	resp, err := c.Get(base + "/status")
	if err != nil {
		return false
	}
	defer resp.Body.Close()
	body, _ := io.ReadAll(io.LimitReader(resp.Body, 4096))
	return strings.Contains(string(body), `"pw":1`)
}
