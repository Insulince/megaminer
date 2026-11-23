//go:build ignore

// fake chip on 127.0.0.1:18080: "boot" = bootloader with console password
// "pw" (checked before the length, as boot.c); "open" = bootloader without
// one; "miner" = miner with password "pw" that never becomes a bootloader.
package main

import (
	"fmt"
	"io"
	"net/http"
	"os"
	"sync/atomic"
)

func main() {
	mode := os.Args[1]
	var flashes atomic.Int32
	http.HandleFunc("/boot", func(w http.ResponseWriter, r *http.Request) {
		if mode == "miner" {
			http.NotFound(w, r)
			return
		}
		fmt.Fprint(w, "bootloader\n")
	})
	http.HandleFunc("/status", func(w http.ResponseWriter, r *http.Request) { fmt.Fprint(w, `{"rssi":-40,"pw":1}`) })
	http.HandleFunc("/cmd", func(w http.ResponseWriter, r *http.Request) {
		r.ParseForm()
		if r.Header.Get("X-Megaminer") == "" { // as strict as the real miner
			w.WriteHeader(403)
			fmt.Fprint(w, "refused: commands come from the miner's own page (X-Megaminer header)\n")
			return
		}
		if r.Form.Get("pass") != "pw" {
			w.WriteHeader(403)
			fmt.Fprint(w, "wrong password\n")
			return
		}
		fmt.Fprint(w, "restarting into the bootloader\n")
	})
	http.HandleFunc("/flash", func(w http.ResponseWriter, r *http.Request) {
		n, _ := io.Copy(io.Discard, r.Body)
		if mode != "open" && r.Header.Get("X-Pass") != "pw" {
			w.WriteHeader(403)
			fmt.Fprint(w, "wrong console password\n")
			return
		}
		if n == 0 {
			w.WriteHeader(400)
			fmt.Fprint(w, "needs Content-Length\n")
			return
		}
		fmt.Fprintf(w, "ok: written and verified (%d bytes, upload #%d)\n", n, flashes.Add(1))
	})
	http.ListenAndServe("127.0.0.1:18080", nil)
}
