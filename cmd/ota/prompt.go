package main

import (
	"bufio"
	"errors"
	"fmt"
	"os"
	"os/exec"
	"os/signal"
	"strings"
)

// readHidden asks for a line without showing what is typed.
//
// Two ways, because Git Bash's usual window (mintty) is not a Windows
// console: programs there get their input through a pipe, which the console
// call cannot touch, while the MSYS `stty` (on PATH in Git Bash) can. A real
// console (PowerShell, cmd, Windows Terminal) takes the console call. If
// neither works, it refuses rather than show the password.
func readHidden(prompt string) (string, error) {
	restore, err := echoOff()
	if err != nil {
		return "", fmt.Errorf("cannot hide typing in this window (%v); give the password with MEGAMINER_PASS or -pass instead", err)
	}
	// Ctrl-C while echo is off must not leave the window without echo.
	stop := make(chan os.Signal, 1)
	signal.Notify(stop, os.Interrupt)
	done := make(chan struct{})
	go func() {
		select {
		case <-stop:
			restore()
			fmt.Println()
			os.Exit(130)
		case <-done:
		}
	}()

	fmt.Print(prompt)
	line, err := bufio.NewReader(os.Stdin).ReadString('\n')
	close(done)
	signal.Stop(stop)
	restore()
	fmt.Println() // the Enter was not echoed either
	if err != nil && line == "" {
		return "", errors.New("no password typed")
	}
	return strings.TrimRight(line, "\r\n"), nil
}

func echoOff() (restore func(), err error) {
	if r, err := consoleEchoOff(); err == nil {
		return r, nil
	}
	if err := stty("-echo"); err != nil {
		return nil, err
	}
	return func() { _ = stty("echo") }, nil
}

func stty(arg string) error {
	cmd := exec.Command("stty", arg)
	cmd.Stdin = os.Stdin // stty works on the terminal it is given as input
	return cmd.Run()
}
