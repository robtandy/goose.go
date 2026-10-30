//go:build unix

package main

import (
	"os"
	"syscall"
)

// execCompiler runs the compiler in this process's place, on args, with
// this process's environment and streams, so that the exit status and the
// signals of the process go generate waits for are the compiler's. It
// returns only where it cannot.
func execCompiler(exe string, args []string) error {
	return syscall.Exec(exe, append([]string{exe}, args...), os.Environ())
}
