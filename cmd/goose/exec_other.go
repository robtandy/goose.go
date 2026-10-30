//go:build !unix

package main

import (
	"errors"
	"os"
	"os/exec"
)

// execCompiler runs the compiler on args, with this process's environment
// and streams, and exits as it does. It returns only where it cannot run.
func execCompiler(exe string, args []string) error {
	cmd := exec.Command(exe, args...)
	cmd.Stdin, cmd.Stdout, cmd.Stderr = os.Stdin, os.Stdout, os.Stderr
	err := cmd.Run()
	var exit *exec.ExitError
	if errors.As(err, &exit) {
		os.Exit(exit.ExitCode())
	}
	if err != nil {
		return err
	}
	os.Exit(0)
	return nil
}
