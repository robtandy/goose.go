package goose

import (
	"fmt"
	"strings"
	"testing"
	"unsafe"
)

// The entry points of inlining.goose (issue #22): what each computes, through
// each of its entry points; whether a caller's array on its stack stays there;
// and the calls test/run_go_tests.py compares, as Go compiles them, with the
// calls of the functions they make.

func TestKernels(t *testing.T) {
	xs := []uint32{1, 2, 3, 1 << 31, 1 << 31}
	if got := Sum(xs); got != 6+1<<32 {
		t.Errorf("Sum = %d", got)
	}
	if got := SumWithStack(nil, xs[:3]); got != 6 {
		t.Errorf("SumWithStack = %d", got)
	}
	if got := Sum(nil); got != 0 {
		t.Errorf("Sum(nil) = %d", got)
	}
	// A const u8[:] takes a string or a []byte: either through the generic
	// entry point, or each through one of its own.
	for _, c := range []struct {
		name string
		got  int64
	}{
		{"CountDigits(string)", CountDigits("a1b22c333")},
		{"CountDigits([]byte)", CountDigits([]byte("a1b22c333"))},
		{"CountDigitsWithStack", CountDigitsWithStack(nil, "a1b22c333")},
		{"CountDigitsString", CountDigitsString("a1b22c333")},
		{"CountDigitsBytes", CountDigitsBytes([]byte("a1b22c333"))},
	} {
		if c.got != 6 {
			t.Errorf("%s = %d", c.name, c.got)
		}
	}
	if got := CountDigitsString(""); got != 0 {
		t.Errorf(`CountDigitsString("") = %d`, got)
	}
	if got := Norm2(&Pt_g{x_g: 3, y_g: 4}); got != 25 {
		t.Errorf("Norm2 = %v", got)
	}
	ys := []float64{1, 1, 1, 1}
	Axpy(2, []float64{1, 2, 3}, ys)
	if ys[0] != 3 || ys[1] != 5 || ys[2] != 7 || ys[3] != 1 {
		t.Errorf("Axpy: %v", ys)
	}
	text := []byte("   word ")
	w := TrimLeft(text)
	if string(w) != "word " || &w[0] != &text[3] {
		t.Errorf("TrimLeft = %q, a view of the text: %v", w, len(w) > 0 && &w[0] == &text[3])
	}
	if got := TrimLeftString("  x"); string(got) != "x" {
		t.Errorf("TrimLeftString = %q", got)
	}
	if got := TrimLeft(" y"); string(got) != "y" {
		t.Errorf("TrimLeft(string) = %q", got)
	}
	if got := TrimLeftWithStack(nil, []byte(" z")); string(got) != "z" {
		t.Errorf("TrimLeftWithStack = %q", got)
	}
	if got := TrimLeftBytes([]byte("   ")); len(got) != 0 {
		t.Errorf("TrimLeftBytes = %q", got)
	}
	if got := TagsLen([]string{"ab", "", "cde"}); got != 5 {
		t.Errorf("TagsLen = %d", got)
	}
	if got := TagsLenWithStack(nil, nil); got != 0 {
		t.Errorf("TagsLenWithStack(nil) = %d", got)
	}
	ws := []int64{1, 2, 3, 4}
	gs := AcquireStackBlock(1)
	defer ReleaseStackBlock(gs)
	if got := WeightedSum(ws, 1); got != 1*1+2*2+3*3+4*4 {
		t.Errorf("WeightedSum = %d", got)
	}
	if got := WeightedSum(ws, -2); got != 2*1+3*2 {
		t.Errorf("WeightedSum(-2) = %d", got)
	}
	for _, c := range []struct {
		name      string
		got, want int64
	}{
		{"HeldSum", HeldSum(ws), 11},
		{"KeptSum", KeptSum(ws), 10},
		{"KeptSumWithStack", KeptSumWithStack(gs, ws), 10},
		{"Peeked", Peeked(ws), 2},
		{"Peeked(nil)", Peeked(nil), 1},
		{"FirstOf", FirstOf("xyz"), 'x'},
		{"FirstOfWithStack", FirstOfWithStack(nil, []byte("q")), 'q'},
		{"FirstOfString", FirstOfString(""), -1},
		{"FirstOfBytes", FirstOfBytes([]byte("b")), 'b'},
	} {
		if c.got != c.want {
			t.Errorf("%s = %d, want %d", c.name, c.got, c.want)
		}
	}
}

// A caller's array that nothing else lets escape stays on its stack where
// the entry point it is handed to lets nothing escape: the call allocates
// nothing. Where the export fn may keep it where Go does not see it, the
// entry point moves it to the heap, which is an allocation each call.
func TestCallerArraysStay(t *testing.T) {
	var sink int64
	stay := []struct {
		name string
		f    func()
	}{
		{"Sum", func() {
			var a [16]uint32
			for i := range a {
				a[i] = uint32(i)
			}
			sink += int64(Sum(a[:]))
		}},
		{"SumWithStack", func() {
			var a [16]uint32
			a[3] = 3
			sink += int64(SumWithStack(nil, a[:]))
		}},
		{"CountDigitsBytes", func() {
			var a [8]byte
			copy(a[:], "1a2b3c4d")
			sink += CountDigitsBytes(a[:])
		}},
		{"CountDigits", func() {
			var a [8]byte
			copy(a[:], "1a2b3c4d")
			sink += CountDigits(a[:])
		}},
		{"Norm2", func() {
			p := Pt_g{x_g: 1, y_g: 2}
			sink += int64(Norm2(&p))
		}},
		{"Axpy", func() {
			var a, b [8]float64
			a[1] = 1
			Axpy(2, a[:], b[:])
			sink += int64(b[1])
		}},
		{"TrimLeftBytes", func() {
			var a [8]byte
			copy(a[:], "  ab")
			sink += int64(len(TrimLeftBytes(a[:])))
		}},
		{"WeightedSum", func() {
			var a [8]int64
			a[2] = 5
			sink += WeightedSum(a[:], 1)
		}},
		{"TagsLen", func() {
			var a [4]string
			a[1], a[3] = "two", "four"
			sink += TagsLen(a[:])
		}},
	}
	for _, c := range stay {
		if n := testing.AllocsPerRun(100, c.f); n != 0 {
			t.Errorf("%s: %v allocs/op: the caller's array left its stack", c.name, n)
		}
	}
	gs := AcquireStackBlock(1)
	defer ReleaseStackBlock(gs)
	moved := []struct {
		name string
		f    func()
	}{
		{"HeldSum", func() {
			var a [8]int64
			a[0] = 1
			sink += HeldSum(a[:])
		}},
		{"KeptSum", func() {
			var a [8]int64
			a[0] = 1
			sink += KeptSum(a[:])
		}},
		{"KeptSumWithStack", func() {
			var a [8]int64
			a[0] = 1
			sink += KeptSumWithStack(gs, a[:])
		}},
		{"Peeked", func() {
			var a [8]int64
			a[0] = 1
			sink += Peeked(a[:])
		}},
		{"FirstOfBytes", func() {
			var a [4]byte
			a[0] = 'a'
			sink += FirstOfBytes(a[:])
		}},
		{"FirstOf", func() {
			var a [4]byte
			a[0] = 'a'
			sink += FirstOf(a[:])
		}},
	}
	for _, c := range moved {
		if n := testing.AllocsPerRun(100, c.f); n < 1 {
			t.Errorf("%s: %v allocs/op: the caller's array stayed on its stack", c.name, n)
		}
	}
	_ = sink
}

// The calls run_go_tests.py compares as Go compiles them, for arm64 and
// amd64: each callX calls an entry point, and directX makes the call that
// entry point makes, with the slice built by hand. They must be the same
// instructions: a call of the entry point costs what the call of its
// function does.

//go:noinline
func callSum(xs []uint32) uint64 { return Sum(xs) }

//go:noinline
func directSum(xs []uint32) uint64 {
	return sum_g(GsSlice{Data: unsafe.Pointer(unsafe.SliceData(xs)), Len: int64(len(xs))})
}

//go:noinline
func callSumWithStack(xs []uint32) uint64 { return SumWithStack(nil, xs) }

//go:noinline
func directSumWithStack(xs []uint32) uint64 {
	return sum_g(GsSlice{Data: unsafe.Pointer(unsafe.SliceData(xs)), Len: int64(len(xs))})
}

//go:noinline
func callCountDigitsString(s string) int64 { return CountDigitsString(s) }

//go:noinline
func directCountDigitsString(s string) int64 {
	return count_digits_g(GsSlice{Data: unsafe.Pointer(unsafe.StringData(s)), Len: int64(len(s))})
}

//go:noinline
func callCountDigitsBytes(b []byte) int64 { return CountDigitsBytes(b) }

//go:noinline
func directCountDigitsBytes(b []byte) int64 {
	return count_digits_g(GsSlice{Data: unsafe.Pointer(unsafe.SliceData(b)), Len: int64(len(b))})
}

//go:noinline
func callNorm2(p *Pt_g) float64 { return Norm2(p) }

//go:noinline
func directNorm2(p *Pt_g) float64 { return norm2_g(p) }

//go:noinline
func callAxpy(a float64, xs, ys []float64) { Axpy(a, xs, ys) }

//go:noinline
func directAxpy(a float64, xs, ys []float64) {
	axpy_g(a, GsSlice{Data: unsafe.Pointer(unsafe.SliceData(xs)), Len: int64(len(xs))},
		GsSlice{Data: unsafe.Pointer(unsafe.SliceData(ys)), Len: int64(len(ys))})
}

//go:noinline
func callTrimLeftBytes(b []byte) []byte { return TrimLeftBytes(b) }

//go:noinline
func directTrimLeftBytes(b []byte) []byte {
	r := trim_left_g(GsSlice{Data: unsafe.Pointer(unsafe.SliceData(b)), Len: int64(len(b))})
	return unsafe.Slice((*byte)(r.Data), r.Len)
}

//go:noinline
func callTagsLen(tags []string) int64 { return TagsLen(tags) }

//go:noinline
func directTagsLen(tags []string) int64 {
	return tags_len_g(GsSlice{Data: unsafe.Pointer(unsafe.SliceData(tags)), Len: int64(len(tags))})
}

func TestCalls(t *testing.T) {
	xs := []uint32{1, 2, 3}
	if callSum(xs) != directSum(xs) || callSumWithStack(xs) != directSumWithStack(xs) {
		t.Error("Sum")
	}
	if callCountDigitsString("1a2") != directCountDigitsString("1a2") ||
		callCountDigitsBytes([]byte("1a2")) != directCountDigitsBytes([]byte("1a2")) {
		t.Error("CountDigits")
	}
	p := &Pt_g{x_g: 1, y_g: 1}
	if callNorm2(p) != directNorm2(p) {
		t.Error("Norm2")
	}
	a, b := []float64{1, 1}, []float64{2, 3}
	callAxpy(1, a, b)
	directAxpy(1, a, b)
	if b[0] != 4 || b[1] != 5 {
		t.Errorf("Axpy: %v", b)
	}
	if string(callTrimLeftBytes([]byte(" a"))) != string(directTrimLeftBytes([]byte(" a"))) {
		t.Error("TrimLeft")
	}
	if tags := []string{"a", "bc"}; callTagsLen(tags) != directTagsLen(tags) {
		t.Error("TagsLen")
	}
}

// Each entry point, and the call of its function, written by hand: at every
// length the two take the same time.
var benchSum uint64

func BenchmarkSum(b *testing.B) {
	for _, n := range []int{4, 64, 1024} {
		xs := make([]uint32, n)
		for i := range xs {
			xs[i] = uint32(i)
		}
		b.Run(fmt.Sprintf("entry/%d", n), func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				benchSum += Sum(xs)
			}
		})
		b.Run(fmt.Sprintf("direct/%d", n), func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				benchSum += sum_g(GsSlice{Data: unsafe.Pointer(unsafe.SliceData(xs)), Len: int64(len(xs))})
			}
		})
	}
}

// The generic entry point, the one taking a string, and the call of their
// function.
var benchCount int64

func BenchmarkCountDigits(b *testing.B) {
	for _, n := range []int{4, 64, 1024} {
		s := strings.Repeat("a1", n)[:n]
		b.Run(fmt.Sprintf("generic/%d", n), func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				benchCount += CountDigits(s)
			}
		})
		b.Run(fmt.Sprintf("string/%d", n), func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				benchCount += CountDigitsString(s)
			}
		})
		b.Run(fmt.Sprintf("direct/%d", n), func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				benchCount += count_digits_g(GsSlice{Data: unsafe.Pointer(unsafe.StringData(s)), Len: int64(len(s))})
			}
		})
	}
}
