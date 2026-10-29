package goose

import (
	"bytes"
	"sync"
	"testing"
	"unsafe"
)

// The export fns' Go entry points in a program with workers
// (src/codegen_go_threads.h): every call runs on a program instance of
// its block's own.

func TestPooledCallsStartFromThePackage(t *testing.T) {
	for i := 0; i < 3; i++ {
		if got := Bump(5); got != 5 {
			t.Fatalf("Bump(5) = %d, want 5: a pooled call starts from the package's globals", got)
		}
		if got := Note("abc"); got != int64(len("init:abc")) {
			t.Fatalf("Note = %d", got)
		}
	}
	got := LastLog()
	if string(got) != "init:" {
		t.Fatalf("LastLog = %q", got)
	}
	// The block it ran on is back in the pool: the result is a copy.
	got[0] = 'X'
	if again := LastLog(); string(again) != "init:" {
		t.Fatalf("LastLog after a write to the last result = %q", again)
	}
}

func TestAKeptBlockKeepsItsGlobals(t *testing.T) {
	gs := NewStackBlock(0, gsStackReserve, gsStackGap)
	defer gs.Close()
	for i := int64(1); i <= 4; i++ {
		if got := BumpWithStack(gs, 3); got != 3*i {
			t.Fatalf("call %d: BumpWithStack = %d, want %d", i, got, 3*i)
		}
	}
	NoteWithStack(gs, "a")
	if got := NoteWithStack(gs, "b"); got != int64(len("init:ab")) {
		t.Fatalf("NoteWithStack = %d", got)
	}
	got := LastLogWithStack(gs)
	if string(got) != "init:ab" {
		t.Fatalf("LastLogWithStack = %q", got)
	}
	// A kept block's result views its instance's log, in place.
	if !gs.Owns(unsafe.Pointer(&got[0])) {
		t.Errorf("LastLogWithStack made a copy")
	}
	// The package's own are as init left them.
	if got := Bump(1); got != 1 {
		t.Errorf("Bump(1) = %d after the kept block's calls", got)
	}
	// Reset gives the instance up: the next call starts from the package's.
	gs.Reset()
	if got := BumpWithStack(gs, 2); got != 2 {
		t.Errorf("BumpWithStack after Reset = %d", got)
	}
}

// Goroutines calling the exports at once, each on a block of its own or on
// the pool's, share no global: each sees its own counts throughout.
func TestConcurrentCallsShareNoGlobals(t *testing.T) {
	var wg sync.WaitGroup
	errs := make(chan string, 64)
	for g := 1; g <= 16; g++ {
		wg.Add(1)
		go func(g int64) {
			defer wg.Done()
			gs := NewStackBlock(0, gsStackReserve, gsStackGap)
			defer gs.Close()
			for i := int64(1); i <= 25; i++ {
				if got := BumpWithStack(gs, g); got != g*i {
					errs <- "BumpWithStack"
					return
				}
				if got := Bump(g); got != g {
					errs <- "Bump"
					return
				}
				if got := NoteWithStack(gs, "xy"); got != int64(len("init:"))+2*i {
					errs <- "NoteWithStack"
					return
				}
				if got := LastLog(); !bytes.Equal(got, []byte("init:")) {
					errs <- "LastLog"
					return
				}
			}
		}(int64(g))
	}
	wg.Wait()
	close(errs)
	for e := range errs {
		t.Errorf("%s: a goroutine saw another's global", e)
	}
}

func TestWorkersOfAnExport(t *testing.T) {
	// Each part's sum is of its 10 squares, mod 8, from its own start.
	want := int64(0)
	for w := 0; w < 4; w++ {
		for i := 0; i < 10; i++ {
			want += [8]int64{0, 1, 4, 9, 16, 25, 36, 49}[(w*10+i)%8]
		}
	}
	for i := 0; i < 3; i++ {
		if got := ParallelSum(4); got != want {
			t.Fatalf("ParallelSum(4) = %d, want %d", got, want)
		}
	}
}
