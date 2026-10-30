package kernels

import (
	"math"
	"strings"
	"testing"
)

// The entry points of a library of kernels (issue #19), which run on no
// block of data stacks: Name calls the function directly, and NameWithStack
// leaves the block it is handed alone, nil included.

func TestKernels(t *testing.T) {
	a, b := []float64{1, 2, 3}, []float64{4, 5, 6, 7}
	if got := Dot(a, b); got != 32 {
		t.Errorf("Dot = %v", got)
	}
	if got := DotWithStack(nil, b, a); got != 32 {
		t.Errorf("DotWithStack = %v", got)
	}
	Scale(a, 0.5)
	if a[0] != 0.5 || a[2] != 1.5 {
		t.Errorf("Scale: %v", a)
	}
	xs := []int64{3, -1, 9, 9, 2}
	if got := Sum(xs); got != 22 {
		t.Errorf("Sum = %d", got)
	}
	if got := Sum([]int64{math.MaxInt64, 1}); got != math.MinInt64 {
		t.Errorf("Sum wraps to %d", got)
	}
	if got := Argmax(xs); got != 2 {
		t.Errorf("Argmax = %d", got)
	}
	if got := Argmax(nil); got != -1 {
		t.Errorf("Argmax(nil) = %d", got)
	}
	out := make([]int64, 3)
	if n := PrefixSums(xs, out); n != 3 || out[0] != 3 || out[1] != 2 || out[2] != 11 {
		t.Errorf("PrefixSums = %d, %v", n, out)
	}
	if got := CountDigits("a1b22c333"); got != 6 {
		t.Errorf("CountDigits(string) = %d", got)
	}
	if got := CountDigits([]byte("2026")); got != 4 {
		t.Errorf("CountDigits([]byte) = %d", got)
	}
}

// An abort is a panic whose value is an error naming the check's place.
func TestAbort(t *testing.T) {
	defer func() {
		e, ok := recover().(error)
		if !ok || !strings.Contains(e.Error(), "division by zero (test/go/kernels/kernels.goose:") {
			t.Errorf("recovered %v", e)
		}
	}()
	Quotient(1, 0)
	t.Error("Quotient(1, 0) returned")
}

// No call allocates.
func TestZeroAllocs(t *testing.T) {
	xs := []int64{1, 2, 3, 4}
	fs := []float64{1, 2}
	out := make([]int64, 4)
	var sink int64
	for name, f := range map[string]func(){
		"Sum":         func() { sink += Sum(xs) },
		"Dot":         func() { sink += int64(Dot(fs, fs)) },
		"PrefixSums":  func() { sink += PrefixSums(xs, out) },
		"CountDigits": func() { sink += CountDigits("12ab") },
	} {
		if n := testing.AllocsPerRun(100, f); n != 0 {
			t.Errorf("%s: %v allocs/op", name, n)
		}
	}
	_ = sink
}
