package goose

import (
	"fmt"
	"math"
	"os"
	"testing"
)

// A library's constant tables (issue #62): static data, there before any
// call, where a global the start computes is the start's, which the first
// call of an entry point makes, a kernel's included.

var (
	wantKey  = [8]uint64{14695981039346656037, 1099511628211, 0x9e3779b97f4a7c15, 0xbf58476d1ce4e5b9, 0x94d049bb133111eb, 1, 0xffffffffffffffff, 0}
	wantSbox = [16]uint8{12, 5, 6, 11, 9, 0, 10, 13, 3, 14, 15, 8, 4, 7, 1, 2}
	wantSeed = wantKey[2] ^ wantKey[3] ^ uint64(wantSbox[15])
)

// TestMain runs before any call: the tables hold their values, and the
// library has not started, so that the global it computes is still zero and
// no memory is mapped. The first call is a kernel's, whose function reads
// that global: it starts the library.
func TestMain(m *testing.M) {
	if why := beforeStart(); why != "" {
		fmt.Fprintf(os.Stderr, "before the first call, %s\n", why)
		os.Exit(1)
	}
	if got, want := Mixed(3), (3^wantSeed)*wantKey[4]; got != want {
		fmt.Fprintf(os.Stderr, "the first call, Mixed(3) = %#x, want %#x: it did not start the library\n", got, want)
		os.Exit(1)
	}
	if SEED_g != wantSeed || !math.Signbit(ZEROS_g[1]) || mapCount() == 0 {
		fmt.Fprintf(os.Stderr, "after the first call, SEED is %#x, ZEROS %v, %d mappings\n", SEED_g, ZEROS_g, mapCount())
		os.Exit(1)
	}
	os.Exit(m.Run())
}

func beforeStart() string {
	span := func(s Span_g) [2]int32 { return [2]int32{s.lo_g, s.hi_g} }
	switch {
	case KEY_g != wantKey || KEY2_g != wantKey:
		return fmt.Sprintf("KEY is %v and KEY2 %v", KEY_g, KEY2_g)
	case SBOX_g != wantSbox || INV_g != [4]uint8{5, 14, 15, 8}:
		return fmt.Sprintf("SBOX is %v and INV %v", SBOX_g, INV_g)
	case ENDS_g != [4]int32{math.MinInt32, -1, 0, math.MaxInt32} || WIDE_g != [2]int64{math.MinInt64, math.MaxInt64}:
		return fmt.Sprintf("ENDS is %v and WIDE %v", ENDS_g, WIDE_g)
	case string(HELLO_g[:]) != "hello" || ONES_g != [8]uint16{1, 1, 1, 1, 1, 1, 1, 1}:
		return fmt.Sprintf("HELLO is %q and ONES %v", HELLO_g[:], ONES_g)
	case HALVES_g != [3]float64{0.5, -2.25, 1e300} || THIRDS_g != [2]float32{0.33333334, -1.5}:
		return fmt.Sprintf("HALVES is %v and THIRDS %v", HALVES_g, THIRDS_g)
	case FLAGS_g != [3]bool{true, false, true} || GRID_g != [2][3]int16{{1, -2, 3}, {-4, 5, -6}}:
		return fmt.Sprintf("FLAGS is %v and GRID %v", FLAGS_g, GRID_g)
	case span(UNIT_g) != [2]int32{0, 1} || span(SPANS_g[0]) != [2]int32{0, 9} || span(SPANS_g[2]) != [2]int32{100, 999}:
		return fmt.Sprintf("UNIT is %v and SPANS %v", UNIT_g, SPANS_g)
	case SEED_g != 0 || math.Signbit(ZEROS_g[1]):
		return "the globals' initializers ran"
	case mapCount() != 0:
		return fmt.Sprintf("the library mapped memory %d times", mapCount())
	}
	return ""
}

// What each computes, from the tables.
func TestTables(t *testing.T) {
	for _, x := range []uint64{0, 1, 0xdeadbeef, math.MaxUint64} {
		h := wantKey[0]
		for i := 0; i < 8; i++ {
			h = (h ^ (x>>(8*i))&255) * wantKey[1]
		}
		if got := Fnv(x); got != h {
			t.Errorf("Fnv(%#x) = %#x, want %#x", x, got, h)
		}
		if got, want := Mixed(x), (x^wantSeed)*wantKey[4]; got != want {
			t.Errorf("Mixed(%#x) = %#x, want %#x", x, got, want)
		}
	}
	for x := 0; x < 256; x++ {
		if got, want := Substitute(uint8(x)), wantSbox[x>>4]<<4|wantSbox[x&15]; got != want {
			t.Errorf("Substitute(%d) = %d, want %d", x, got, want)
		}
	}
	for _, c := range []struct {
		x    int32
		want int64
	}{{0, 1}, {9, 1}, {10, 2}, {99, 2}, {100, 3}, {999, 3}, {1000, 0}, {-1, 0}} {
		if got := Digits(c.x); got != c.want {
			t.Errorf("Digits(%d) = %d, want %d", c.x, got, c.want)
		}
	}
	want := int64(math.MinInt32-1+0+math.MaxInt32) + (math.MinInt64 + math.MaxInt64) +
		104 + 101 + 108 + 108 + 111 + 8 + (1 - 2 + 3 - 4 + 5 - 6) + (5 + 14 + 15 + 8) + 2000 + 1 + int64(wantKey[1])
	if got := TablesSum(); got != want {
		t.Errorf("TablesSum = %d, want %d", got, want)
	}
	if got, want := FloatsSum(), 0.5-2.25+float64(float32(0.33333334))+float64(float32(-1.5)); got != want {
		t.Errorf("FloatsSum = %v, want %v", got, want)
	}
	if !NegativeZero() {
		t.Error("NegativeZero: ZEROS[1] is no negative zero")
	}
}
