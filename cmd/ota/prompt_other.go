//go:build !windows

package main

import "errors"

// consoleEchoOff: only Windows has the console call; elsewhere stty does it.
func consoleEchoOff() (func(), error) { return nil, errors.New("not a Windows console") }
