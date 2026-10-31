package goose

import (
	"sync"
	"testing"
)

// The package's log, as the library's start leaves it.
const startLog = "start;"

func TestPooledCallsStartFromThePackage(t *testing.T) {
	for i := 0; i < 3; i++ {
		if got := Add("ab"); got != 2 {
			t.Fatalf("Add = %d, want 2: a pooled call starts from the package's log", got)
		}
		if got := LastMark(); got != -1 {
			t.Fatalf("LastMark = %d, want -1", got)
		}
		if got := Take(7); got != 0 {
			t.Fatalf("Take = %d, want 0: a pooled call starts from the package's pool", got)
		}
		if got := Slot(0); got != -1 {
			t.Fatalf("Slot(0) = %d, want -1", got)
		}
	}
	if got := string(Lines()); got != startLog {
		t.Fatalf("Lines = %q, want %q", got, startLog)
	}
}

func TestAKeptBlockKeepsItsOwners(t *testing.T) {
	gs := NewStackBlock(0, gsStackReserve, gsStackGap)
	defer gs.Close()
	for i, text := range []string{"a", "bc", "def"} {
		if got := AddWithStack(gs, text); got != int64(i+2) {
			t.Fatalf("note %d: AddWithStack = %d, want %d", i, got, i+2)
		}
	}
	if got := string(LinesWithStack(gs)); got != startLog+"abcdef" {
		t.Fatalf("LinesWithStack = %q", got)
	}
	if got := LastMarkWithStack(gs); got != int64(len(startLog+"abcdef")) {
		t.Fatalf("LastMarkWithStack = %d", got)
	}
	a, b := TakeWithStack(gs, 10), TakeWithStack(gs, 20)
	GiveWithStack(gs, a)
	if c := TakeWithStack(gs, 30); c != a || b != 1 {
		t.Fatalf("slots %d, %d, then %d: the freed slot is reused", a, b, c)
	}
	if got := SlotWithStack(gs, a); got != 30 {
		t.Fatalf("SlotWithStack = %d, want 30", got)
	}
	// The package's owners are as the start left them.
	if got := string(Lines()); got != startLog {
		t.Fatalf("Lines = %q, want the package's %q", got, startLog)
	}
}

func TestGoroutinesEachOnABlockOfTheirOwn(t *testing.T) {
	var wg sync.WaitGroup
	errs := make(chan string, 64)
	for g := 0; g < 16; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			gs := NewStackBlock(0, gsStackReserve, gsStackGap)
			defer gs.Close()
			for i := 0; i < 50; i++ {
				if got := AddWithStack(gs, "x"); got != int64(i+2) {
					errs <- "AddWithStack counted another goroutine's notes"
					return
				}
				if got := TakeWithStack(gs, int64(g)); got != int64(i) {
					errs <- "TakeWithStack placed into another goroutine's pool"
					return
				}
			}
			if got := Add("y"); got != 2 {
				errs <- "a pooled Add did not start from the package's log"
			}
		}(g)
	}
	wg.Wait()
	close(errs)
	for e := range errs {
		t.Error(e)
	}
}
