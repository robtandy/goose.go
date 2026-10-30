package goose

import (
	"bytes"
	"sync"
	"testing"
	"unsafe"
)

// The export fns' Go entry points in a program without workers whose
// export fns grow globals on data stacks of their own (issue #25): every
// call runs on a program instance of its block's own, a copy of the
// package's globals whose values on data stacks are on the block's.

// raceEnabled is set by race_test.go under the race detector, where
// sync.Pool deliberately drops a quarter of the items it is given.
var raceEnabled bool

// The package's arena and log, as the library's start leaves them: the
// arena's length and its sum, and the log.
const (
	startLen   = 3
	startTotal = 1 + 2 + 3
	startLog   = "init:"
)

func TestPooledCallsStartFromThePackage(t *testing.T) {
	for i := 0; i < 3; i++ {
		if got := Push(10); got != startLen+1 {
			t.Fatalf("Push(10) = %d, want %d: a pooled call starts from the package's arena", got, startLen+1)
		}
		if got := FillArena(5); got != startLen+5 {
			t.Fatalf("FillArena(5) = %d, want %d", got, startLen+5)
		}
		if got := Total(); got != startTotal {
			t.Fatalf("Total() = %d, want %d", got, startTotal)
		}
		if got := Element(2); got != 3 {
			t.Fatalf("Element(2) = %d, want 3", got)
		}
		if got := Element(startLen); got != -1 {
			t.Fatalf("Element(%d) = %d, want -1", startLen, got)
		}
		if got := PushCount(); got != 0 {
			t.Fatalf("PushCount() = %d, want 0", got)
		}
		if got := Note("abc"); got != int64(len(startLog+"abc")) {
			t.Fatalf("Note = %d", got)
		}
		if got := NoteBytes([]byte("abcd")); got != int64(len(startLog+"abcd")) {
			t.Fatalf("NoteBytes = %d", got)
		}
		if got := NewestNote(); len(got) != 0 {
			t.Fatalf("NewestNote() = %q, want the package's, empty", got)
		}
		ClearArena()
	}
	got := LastLog()
	if string(got) != startLog {
		t.Fatalf("LastLog = %q", got)
	}
	// The block it ran on is back in the pool: the result is a copy.
	got[0] = 'X'
	if again := LastLog(); string(again) != startLog {
		t.Fatalf("LastLog after a write to the last result = %q", again)
	}
}

func TestAKeptBlockKeepsItsGlobals(t *testing.T) {
	gs := NewStackBlock(0, gsStackReserve, gsStackGap)
	defer gs.Close()
	want := int64(startTotal)
	for i := int64(1); i <= 4; i++ {
		if got := PushWithStack(gs, 10*i); got != startLen+i {
			t.Fatalf("call %d: PushWithStack = %d, want %d", i, got, startLen+i)
		}
		want += 10 * i
	}
	if got := TotalWithStack(gs); got != want {
		t.Fatalf("TotalWithStack = %d, want %d", got, want)
	}
	if got := PushCountWithStack(gs); got != 4 {
		t.Fatalf("PushCountWithStack = %d, want 4", got)
	}
	// Through a reference to the arena, which the call hands on: 0, 1, 2.
	if got := FillArenaWithStack(gs, 3); got != startLen+4+3 {
		t.Fatalf("FillArenaWithStack = %d", got)
	}
	for i, w := range []int64{1, 2, 3, 10, 20, 30, 40, 0, 1, 2} {
		if got := ElementWithStack(gs, int64(i)); got != w {
			t.Fatalf("ElementWithStack(%d) = %d, want %d", i, got, w)
		}
	}
	NoteWithStack(gs, "a")
	if got := NoteWithStack(gs, []byte("b")); got != int64(len(startLog+"ab")) {
		t.Fatalf("NoteWithStack = %d", got)
	}
	got := LastLogWithStack(gs)
	if string(got) != startLog+"ab" {
		t.Fatalf("LastLogWithStack = %q", got)
	}
	// A kept block's result views its instance's log, in place, as the
	// instance's view of its last note does.
	if !gs.Owns(unsafe.Pointer(&got[0])) {
		t.Errorf("LastLogWithStack made a copy")
	}
	if got := NewestNoteWithStack(gs); string(got) != "b" || !gs.Owns(unsafe.Pointer(&got[0])) {
		t.Errorf("NewestNoteWithStack = %q, a view of the block's log: %v", got, gs.Owns(unsafe.Pointer(&got[0])))
	}
	// The package's own are as the start left them.
	if got := Total(); got != startTotal {
		t.Errorf("Total() = %d after the kept block's calls", got)
	}
	if got := LastLog(); string(got) != startLog {
		t.Errorf("LastLog() = %q after the kept block's calls", got)
	}
	ClearArenaWithStack(gs)
	if got := TotalWithStack(gs); got != 0 {
		t.Errorf("TotalWithStack after ClearArenaWithStack = %d", got)
	}
	if got := PushWithStack(gs, 7); got != 1 {
		t.Errorf("PushWithStack after ClearArenaWithStack = %d", got)
	}
	// Reset gives the instance up: the next call starts from the package's.
	gs.Reset()
	if got := TotalWithStack(gs); got != startTotal {
		t.Errorf("TotalWithStack after Reset = %d", got)
	}
	if got := PushCountWithStack(gs); got != 0 {
		t.Errorf("PushCountWithStack after Reset = %d", got)
	}
}

// A block from the pool that a caller ran an instance on and gave back:
// the calls that take it from the pool next start from the package's.
func TestAReleasedBlockKeepsNothing(t *testing.T) {
	gs := AcquireStackBlock(0)
	for i := 0; i < 100; i++ {
		PushWithStack(gs, 1000)
	}
	NoteWithStack(gs, "kept")
	ReleaseStackBlock(gs)
	for i := 0; i < 10; i++ {
		if got := Push(1); got != startLen+1 {
			t.Fatalf("Push(1) = %d after a released block's calls", got)
		}
		if got := LastLog(); string(got) != startLog {
			t.Fatalf("LastLog() = %q after a released block's calls", got)
		}
	}
}

func TestALargeArena(t *testing.T) {
	gs := NewStackBlock(0, gsStackReserve, gsStackGap)
	defer gs.Close()
	const n = 1 << 16
	if got := FillArenaWithStack(gs, n); got != startLen+n {
		t.Fatalf("FillArenaWithStack(%d) = %d", n, got)
	}
	if got := TotalWithStack(gs); got != startTotal+n*(n-1)/2 {
		t.Fatalf("TotalWithStack = %d", got)
	}
	if got := ElementWithStack(gs, startLen+n-1); got != n-1 {
		t.Fatalf("the last element is %d", got)
	}
}

// Goroutines calling the exports at once, each on a block of its own --
// half of them from NewStackBlock, half from the pool -- or on the pool's,
// share no global: each sees its own arena, log and count throughout.
func TestConcurrentCallsShareNoGlobals(t *testing.T) {
	var wg sync.WaitGroup
	errs := make(chan string, 64)
	for g := 1; g <= 16; g++ {
		wg.Add(1)
		go func(g int64) {
			defer wg.Done()
			var gs *GsStackBlock
			if g%2 == 0 {
				gs = NewStackBlock(0, gsStackReserve, gsStackGap)
				defer gs.Close()
			} else {
				gs = AcquireStackBlock(0)
				defer ReleaseStackBlock(gs)
			}
			total := int64(startTotal)
			for i := int64(1); i <= 50; i++ {
				if got := PushWithStack(gs, g); got != startLen+i {
					errs <- "PushWithStack"
					return
				}
				total += g
				if got := TotalWithStack(gs); got != total {
					errs <- "TotalWithStack"
					return
				}
				if got := Push(g); got != startLen+1 {
					errs <- "Push"
					return
				}
				if got := Total(); got != startTotal {
					errs <- "Total"
					return
				}
				if got := NoteWithStack(gs, "xy"); got != int64(len(startLog))+2*i {
					errs <- "NoteWithStack"
					return
				}
				if got := LastLog(); !bytes.Equal(got, []byte(startLog)) {
					errs <- "LastLog"
					return
				}
				if got := LastLogWithStack(gs); len(got) != len(startLog)+2*int(i) {
					errs <- "LastLogWithStack"
					return
				}
				if got := NewestNoteWithStack(gs); string(got) != "xy" || !gs.Owns(unsafe.Pointer(&got[0])) {
					errs <- "NewestNoteWithStack"
					return
				}
				if got := NewestNote(); len(got) != 0 {
					errs <- "NewestNote"
					return
				}
			}
			if got := PushCountWithStack(gs); got != 50 {
				errs <- "PushCountWithStack"
			}
		}(int64(g))
	}
	wg.Wait()
	close(errs)
	for e := range errs {
		t.Errorf("%s: a goroutine saw another's global", e)
	}
}

func TestNoAllocs(t *testing.T) {
	if raceEnabled {
		t.Skip("under the race detector sync.Pool drops a quarter of what it is given")
	}
	gs := AcquireStackBlock(0)
	defer ReleaseStackBlock(gs)
	var sink int64
	cases := []struct {
		name string
		f    func()
	}{
		{"Push", func() { sink += Push(1) }},
		{"Total", func() { sink += Total() }},
		{"FillArena", func() { sink += FillArena(4) }},
		{"NoteString", func() { sink += NoteString("abc") }},
		{"PushWithStack", func() { sink += PushWithStack(gs, 1) }},
		{"TotalWithStack", func() { sink += TotalWithStack(gs) }},
		{"NoteWithStack", func() { sink += NoteWithStack(gs, "abc") }},
	}
	for _, c := range cases {
		if n := testing.AllocsPerRun(1000, c.f); n != 0 {
			t.Errorf("%s: %v allocs/op, want 0", c.name, n)
		}
	}
	if sink == 0 {
		t.Errorf("sink %v", sink)
	}
}

func BenchmarkPushWithStack(b *testing.B) {
	gs := AcquireStackBlock(0)
	defer ReleaseStackBlock(gs)
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		if i%(1<<20) == 0 {
			ClearArenaWithStack(gs)
		}
		PushWithStack(gs, int64(i))
	}
}

func BenchmarkPush(b *testing.B) {
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		Push(int64(i))
	}
}
