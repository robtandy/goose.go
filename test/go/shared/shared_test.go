package shared

import (
	"fmt"
	"strings"
	"sync"
	"testing"
)

// Two programs in one package (issue #19): geom.goose's and text.goose's
// entry points, which run on the runtime they share, each program with its
// own globals.

func TestBothPrograms(t *testing.T) {
	if got := PathLen2([]int64{0, 0, 3, 4, 3, 5}); got != 26 {
		t.Errorf("PathLen2 = %d", got)
	}
	if got := PtTextLen(12, -3); got != int64(len("Pt { 12, -3 }")) {
		t.Errorf("PtTextLen = %d", got)
	}
	if got := Distinct("the cat and the hat and the bat"); got != 5 {
		t.Errorf("Distinct = %d", got)
	}
	if got := LastPos("ab\ncde\nf"); got != 3001 {
		t.Errorf("LastPos = %d", got)
	}
	if got := CheckedDiv(9, 3); got != 3 {
		t.Errorf("CheckedDiv = %d", got)
	}
}

// Each program's global calls is its own.
func TestGlobalsApart(t *testing.T) {
	g0, t0 := GeomCalls(), TextCalls()
	PathLen2([]int64{1, 1})
	PathLen2([]int64{2, 2})
	Distinct("x")
	if g, x := GeomCalls()-g0, TextCalls()-t0; g != 2 || x != 1 {
		t.Errorf("calls: geom %d, text %d; want 2 and 1", g, x)
	}
}

// One block, from the runtime the programs share, for either's entry points.
func TestOneBlockForBoth(t *testing.T) {
	gs := AcquireStackBlock(0)
	defer ReleaseStackBlock(gs)
	for i := 0; i < 3; i++ {
		if got := PathLen2WithStack(gs, []int64{0, 0, 1, 1}); got != 2 {
			t.Fatalf("PathLen2WithStack = %d", got)
		}
		if got := DistinctWithStack(gs, "a b a"); got != 2 {
			t.Fatalf("DistinctWithStack = %d", got)
		}
	}
}

// Calls from many goroutines at once, of both programs: of the entry points
// that write no global, which every call shares (README.md), each on a
// block of data stacks of its own where it takes one (PtTextLen).
func TestConcurrent(t *testing.T) {
	var wg sync.WaitGroup
	for g := 0; g < 8; g++ {
		wg.Add(1)
		go func() {
			defer wg.Done()
			for i := 0; i < 50; i++ {
				if PtTextLen(int64(i), 7) != int64(len(fmt.Sprintf("Pt { %d, 7 }", i))) || LastPos("xy") != 1002 ||
					CheckedDiv(int64(i), 1) != int64(i) {
					t.Error("a concurrent call went wrong")
					return
				}
			}
		}()
	}
	wg.Wait()
}

// An abort is a panic whose value is an error naming the check's place.
func TestAbort(t *testing.T) {
	defer func() {
		e, ok := recover().(error)
		if !ok || !strings.Contains(e.Error(), "checked_div: 5 by zero (test/go/shared/geom.goose:") {
			t.Errorf("recovered %v", e)
		}
	}()
	CheckedDiv(5, 0)
	t.Error("CheckedDiv(5, 0) returned")
}

// A call in a loop allocates nothing, on either program's side.
func TestZeroAllocs(t *testing.T) {
	xs := []int64{0, 0, 3, 4}
	text := []byte("one two one")
	var sink int64
	for name, f := range map[string]func(){
		"PathLen2": func() { sink += PathLen2(xs) },
		"Distinct": func() { sink += Distinct(text) },
		"LastPos":  func() { sink += LastPos(text) },
	} {
		if n := testing.AllocsPerRun(200, f); n != 0 {
			t.Errorf("%s: %v allocs/op", name, n)
		}
	}
	_ = sink
}
