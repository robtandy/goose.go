package goose

import (
	"fmt"
	"os"
	"runtime/debug"
	"testing"
)

// A library whose start aborts (issue #18): its globals' initialization,
// which the first call of an entry point runs, aborts.

// TestMain runs as soon as the package is imported: the import ran no
// initializer, so the process is still here, and reserved nothing.
func TestMain(m *testing.M) {
	if debug.SetPanicOnFault(false) || mapCount() != 0 || blockCount() != 0 {
		fmt.Fprintf(os.Stderr, "importing the package reserved memory (%d mappings, %d blocks) or set the "+
			"fault handling\n", mapCount(), blockCount())
		os.Exit(1)
	}
	os.Exit(m.Run())
}

// catch is what f panics with, nil where it returns.
func catch(f func()) (r any) {
	defer func() { r = recover() }()
	f()
	return nil
}

// Every entry point, whether its function takes a block or not, panics with
// the start's abort, as the first call did, and leaves its caller's fault
// setting as it found it.
func TestStartAbortPanics(t *testing.T) {
	gs := NewStackBlock(0, gsStackReserve, gsStackGap)
	defer gs.Close()
	calls := []struct {
		name string
		f    func()
	}{
		{"Limit", func() { Limit() }},
		{"LimitWithStack", func() { LimitWithStack(gs) }},
		{"Twice", func() { Twice(2) }},
		{"TwiceWithStack", func() { TwiceWithStack(gs, 2) }},
		{"Kept", func() { Kept("a b") }},
		{"KeptWithStack", func() { KeptWithStack(gs, "a b") }},
	}
	var first *GsError
	for round := 0; round < 2; round++ {
		for _, c := range calls {
			for _, on := range []bool{false, true} {
				debug.SetPanicOnFault(on)
				r := catch(c.f)
				if got := debug.SetPanicOnFault(false); got != on {
					t.Errorf("%s left the fault setting at %v, found at %v", c.name, got, on)
				}
				e, ok := r.(*GsError)
				if !ok || e.Msg != "not a limit: 12x" || e.File != "test/go/export/init_abort/init_abort.goose" {
					t.Fatalf("%s panicked with %v, want the start's abort", c.name, r)
				}
				if first == nil {
					first = e
				} else if e != first {
					t.Errorf("%s panicked with another *GsError, %v", c.name, e)
				}
			}
		}
	}
}
