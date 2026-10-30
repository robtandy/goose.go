//go:build ignore

// The Go that keepalive.goose's extern fn calls, spliced into the file
// `goose --target=go --include helper.go` writes. The build constraint keeps
// the go tool from building it on its own here.
package goose

import (
	"runtime"
	"time"
)

// watched is closed by the finalizer of the memory the call under test was
// handed (keepalive_test.go), and freedInCall says whether collect saw it
// closed.
var (
	watched     <-chan struct{}
	freedInCall bool
)

// collect is extern fn collect: it runs the collector, which frees whatever
// nothing it sees refers to, and gives the finalizer of the memory the
// call was handed time to run, where it was freed.
func collect() {
	runtime.GC()
	runtime.GC()
	select {
	case <-watched:
		freedInCall = true
	case <-time.After(50 * time.Millisecond):
	}
}
