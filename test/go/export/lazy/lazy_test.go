package goose

import (
	"fmt"
	"os"
	"runtime/debug"
	"strings"
	"testing"
)

// A library's start (issue #18): nothing at the package's import, all of it
// at the first call of an export fn's entry point, once.

// TestMain runs on the main goroutine as soon as the package is imported,
// before any call: the import has reserved nothing, left the goroutine's
// fault handling alone and run no initializer.
func TestMain(m *testing.M) {
	if why := importDid(); why != "" {
		fmt.Fprintf(os.Stderr, "importing the package %s\n", why)
		os.Exit(1)
	}
	os.Exit(m.Run())
}

func importDid() string {
	switch {
	case debug.SetPanicOnFault(false):
		return "turned on the main goroutine's panic on fault"
	case mapCount() != 0:
		return fmt.Sprintf("mapped memory %d times", mapCount())
	case blockCount() != 0:
		return fmt.Sprintf("made %d blocks of data stacks", blockCount())
	case starts_g != 0 || ANSWER_g != 0:
		return "ran the globals' initializers"
	}
	return ""
}

// The first call starts the library, which runs the initializers once,
// however many calls follow, from however many goroutines.
func TestStartsOnce(t *testing.T) {
	done := make(chan int64)
	for g := 0; g < 8; g++ {
		go func() {
			n := Answer() + Started()
			for i := 0; i < 10; i++ {
				n += LogLen() + Kept("a b")
			}
			done <- n
		}()
	}
	for g := 0; g < 8; g++ {
		if n := <-done; n != 42+1+10*(int64(len("start:"))+2) {
			t.Errorf("a goroutine's calls came to %d", n)
		}
	}
	if n := Started(); n != 1 {
		t.Errorf("the initializers ran %d times", n)
	}
	if mapCount() == 0 || blockCount() == 0 {
		t.Errorf("the start reserved nothing: %d mappings, %d blocks", mapCount(), blockCount())
	}
}

// catch is what f panics with, nil where it returns.
func catch(f func()) (r any) {
	defer func() { r = recover() }()
	f()
	return nil
}

// Every call leaves its caller's goroutine's fault setting as it found it,
// whether it returns or panics, on a block of the caller's or of the pool.
func TestFaultSettingKept(t *testing.T) {
	gs := NewStackBlock(0, gsStackReserve, gsStackGap)
	defer gs.Close()
	calls := []struct {
		name string
		f    func()
	}{
		{"Answer", func() { Answer() }},
		{"AnswerWithStack", func() { AnswerWithStack(gs) }},
		{"LogLen", func() { LogLen() }},
		{"LogLenWithStack", func() { LogLenWithStack(gs) }},
		{"Kept", func() { Kept("x y z") }},
		{"KeptWithStack", func() { KeptWithStack(gs, []byte("x y z")) }},
		{"Depth", func() { Depth(20) }},
		{"DepthWithStack", func() { DepthWithStack(gs, 20) }},
		{"CheckedDiv", func() { CheckedDiv(1, 0) }},
		{"CheckedDivWithStack", func() { CheckedDivWithStack(gs, 1, 0) }},
		{"Quit", func() { Quit(2) }},
		{"Depth past the frame", func() { Depth(1 << 20) }},
	}
	for _, on := range []bool{false, true} {
		for _, c := range calls {
			debug.SetPanicOnFault(on)
			catch(c.f)
			if got := debug.SetPanicOnFault(false); got != on {
				t.Errorf("%s left the fault setting at %v, found at %v", c.name, got, on)
			}
		}
	}
}

// An abort, and exit(), are panics with a *GsError on the caller's
// goroutine, which it may recover: nothing ends the process.
func TestNothingExits(t *testing.T) {
	gs := NewStackBlock(0, gsStackReserve, gsStackGap)
	defer gs.Close()
	cases := []struct {
		name string
		f    func()
		want string
	}{
		{"CheckedDiv", func() { CheckedDiv(7, 0) }, "checked_div: 7 by zero (test/go/export/lazy/lazy.goose:"},
		{"CheckedDivWithStack", func() { CheckedDivWithStack(gs, 8, 0) }, "checked_div: 8 by zero (test/"},
		{"Quit", func() { Quit(3) }, "exit(3)"},
		{"QuitWithStack", func() { QuitWithStack(gs, 0) }, "exit(0)"},
	}
	for _, c := range cases {
		e, ok := catch(c.f).(*GsError)
		if !ok || !strings.HasPrefix(e.Text(), c.want) {
			t.Errorf("%s panicked with %v, want a *GsError of %q", c.name, e, c.want)
		}
	}
	if got := CheckedDiv(9, 3); got != 3 {
		t.Errorf("CheckedDiv(9, 3) = %d after the aborts", got)
	}
}

// A library's fixed-frame stack is smaller than a program's
// (--frame-reserve): a block's reserves that much, and a recursion past it
// panics with a *GsError, as C reports its native stack's overflow.
func TestFrameReserve(t *testing.T) {
	if gsFrameReserve != 64<<20 {
		t.Errorf("gsFrameReserve = %d, want a library's 64 MB", gsFrameReserve)
	}
	gs := NewStackBlock(0, gsStackReserve, gsStackGap)
	defer gs.Close()
	if n := uintptr(gs.FrameEnd) - uintptr(gs.FrameBase); n != gsFrameReserve {
		t.Errorf("a block's fixed-frame stack is %d bytes", n)
	}
	if a, b := DepthWithStack(gs, 300), Depth(300); a != b || a == 0 {
		t.Errorf("Depth(300) = %d on the caller's block, %d on the pool's", a, b)
	}
	e, ok := catch(func() { Depth(1 << 20) }).(*GsError)
	if !ok || e.Msg != "native call stack overflow" {
		t.Errorf("Depth past the frame panicked with %v", e)
	}
}

// What a call on a block pays for keeping its caller's fault setting
// (DESIGN.md §3.5): the same empty call, with the scope and without it.

//go:noinline
func scopedCall() { defer debug.SetPanicOnFault(debug.SetPanicOnFault(true)) }

//go:noinline
func bareCall() {}

func BenchmarkFaultScope(b *testing.B) {
	for i := 0; i < b.N; i++ {
		scopedCall()
	}
}

func BenchmarkNoScope(b *testing.B) {
	for i := 0; i < b.N; i++ {
		bareCall()
	}
}

// Whole calls, once the library has started: on no block, on the caller's
// and on the pool's.
func BenchmarkAnswer(b *testing.B) {
	for i := 0; i < b.N; i++ {
		Answer()
	}
}

func BenchmarkKeptWithStack(b *testing.B) {
	gs := AcquireStackBlock(1)
	defer ReleaseStackBlock(gs)
	for i := 0; i < b.N; i++ {
		KeptWithStack(gs, "a b")
	}
}

func BenchmarkKept(b *testing.B) {
	for i := 0; i < b.N; i++ {
		Kept("a b")
	}
}
