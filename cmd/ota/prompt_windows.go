package main

import (
	"os"

	"golang.org/x/sys/windows"
)

// consoleEchoOff: a real Windows console stops echoing when told so. Fails
// when standard input is not a console (e.g. Git Bash's mintty: a pipe).
func consoleEchoOff() (func(), error) {
	h := windows.Handle(os.Stdin.Fd())
	var mode uint32
	if err := windows.GetConsoleMode(h, &mode); err != nil {
		return nil, err
	}
	if err := windows.SetConsoleMode(h, mode&^windows.ENABLE_ECHO_INPUT); err != nil {
		return nil, err
	}
	return func() { _ = windows.SetConsoleMode(h, mode) }, nil
}
