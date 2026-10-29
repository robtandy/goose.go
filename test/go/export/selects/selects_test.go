package goose

import (
	"math"
	"math/rand"
	"sort"
	"testing"
	"unsafe"
)

// The choices of selects.goose (issue #26) against the same choices made in
// Go as std makes them: min(a, b) is `b < a ? b : a`, max(a, b) is `a < b ?
// b : a`, clamp(x, lo, hi) is lo where x < lo, else hi where hi < x, else x,
// at every integer type and at f32 and f64, whatever a NaN or a zero's sign:
// Go's builtin max has max(1, NaN) = NaN and max(-0.0, 0.0) = 0.0, std's max
// 1 and -0.0. Each runs on random values and on the ends of its type's
// range; floats are compared bit for bit.

type number interface {
	~int8 | ~int16 | ~int32 | ~int64 | ~uint8 | ~uint16 | ~uint32 | ~uint64 | ~float32 | ~float64
}

func stdMin[T number](a, b T) T {
	if b < a {
		return b
	}
	return a
}

func stdMax[T number](a, b T) T {
	if a < b {
		return b
	}
	return a
}

func stdClamp[T number](x, lo, hi T) T {
	if x < lo {
		return lo
	}
	if hi < x {
		return hi
	}
	return x
}

func stdAbs[T number](x T) T {
	if x < 0 {
		return -x
	}
	return x
}

func stdSign(x int64) int64 {
	if x < 0 {
		return -1
	}
	if 0 < x {
		return 1
	}
	return 0
}

// Values of every integer type: its ends, the values around zero, and n
// random ones.
func ints[T ~int8 | ~int16 | ~int32 | ~int64 | ~uint8 | ~uint16 | ~uint32 | ~uint64](r *rand.Rand, n int) []T {
	var zero T
	ones := ^zero
	xs := []T{0, 1, 2, ones, ones - 1}
	if ones < 0 {
		// Signed: the least value is the one after the greatest.
		big := T(uint64(1)<<(8*unsafe.Sizeof(zero)-1) - 1)
		xs = append(xs, big, big-1, big+1, big+2)
	} else {
		xs = append(xs, ones>>1, ones>>1+1)
	}
	for i := 0; i < n; i++ {
		xs = append(xs, T(r.Uint64()))
	}
	return xs
}

var specialF64 = []float64{0, math.Copysign(0, -1), 1, -1, 0.5, -2.5, math.MaxFloat64, -math.MaxFloat64,
	math.SmallestNonzeroFloat64, -math.SmallestNonzeroFloat64, math.Inf(1), math.Inf(-1), math.NaN(),
	-math.NaN(), math.Float64frombits(0x7ff4000000000001)}

func floats(r *rand.Rand, n int) []float64 {
	xs := append([]float64(nil), specialF64...)
	for i := 0; i < n; i++ {
		xs = append(xs, r.NormFloat64()*1e3)
	}
	return xs
}

func sameF64(a, b float64) bool { return math.Float64bits(a) == math.Float64bits(b) }
func sameF32(a, b float32) bool { return math.Float32bits(a) == math.Float32bits(b) }

func fold[T number](xs []T, m T, f func(a, b T) T) T {
	for _, x := range xs {
		m = f(m, x)
	}
	return m
}

func TestReductions(t *testing.T) {
	r := rand.New(rand.NewSource(26))
	for n := 0; n <= 40; n++ {
		i64 := ints[int64](r, n)
		for _, xs := range [][]int64{i64[:n%len(i64)], i64, nil} {
			if got, want := MaxI64(xs), fold(xs, math.MinInt64, stdMax[int64]); got != want {
				t.Errorf("MaxI64(%v) = %d, want %d", xs, got, want)
			}
			if got, want := MinI64(xs), fold(xs, math.MaxInt64, stdMin[int64]); got != want {
				t.Errorf("MinI64(%v) = %d, want %d", xs, got, want)
			}
			if got, want := Spread(xs), fold(xs, math.MinInt64, stdMax[int64])-fold(xs, math.MaxInt64, stdMin[int64]); got != want {
				t.Errorf("Spread(%v) = %d, want %d", xs, got, want)
			}
			if got, want := ReluSum(xs), fold(xs, 0, func(s, x int64) int64 { return s + stdMax(x, 0) }); got != want {
				t.Errorf("ReluSum(%v) = %d, want %d", xs, got, want)
			}
			for _, b := range [][2]int64{{-100, 100}, {0, 0}, {5, -5}, {math.MinInt64, math.MaxInt64}, {math.MaxInt64, math.MinInt64}} {
				want := fold(xs, 0, func(s, x int64) int64 { return s + stdClamp(x, b[0], b[1]) })
				if got := ClampSum(xs, b[0], b[1]); got != want {
					t.Errorf("ClampSum(%v, %d, %d) = %d, want %d", xs, b[0], b[1], got, want)
				}
			}
		}
		u64 := ints[uint64](r, n)
		if got, want := MaxU64(u64), fold(u64, 0, stdMax[uint64]); got != want {
			t.Errorf("MaxU64(%v) = %d, want %d", u64, got, want)
		}
		i32 := ints[int32](r, n)
		if got, want := MinI32(i32), fold(i32, math.MaxInt32, stdMin[int32]); got != want {
			t.Errorf("MinI32(%v) = %d, want %d", i32, got, want)
		}
		if got, want := LargerI32(i32), fold(i32, math.MinInt32, stdMax[int32]); got != want {
			t.Errorf("LargerI32(%v) = %d, want %d", i32, got, want)
		}
		u16 := ints[uint16](r, n)
		if got, want := MaxU16(u16), fold(u16, 0, stdMax[uint16]); got != want {
			t.Errorf("MaxU16(%v) = %d, want %d", u16, got, want)
		}
		u8 := ints[uint8](r, n)
		if got, want := MaxU8(u8), fold(u8, 0, stdMax[uint8]); got != want {
			t.Errorf("MaxU8(%v) = %d, want %d", u8, got, want)
		}
		if got, want := MaxU8(string(u8)), fold(u8, 0, stdMax[uint8]); got != want {
			t.Errorf("MaxU8(string %q) = %d, want %d", u8, got, want)
		}
		i8 := ints[int8](r, n)
		if got, want := MinI8(i8), fold(i8, math.MaxInt8, stdMin[int8]); got != want {
			t.Errorf("MinI8(%v) = %d, want %d", i8, got, want)
		}
	}
}

func TestInPlace(t *testing.T) {
	r := rand.New(rand.NewSource(260))
	for n := 0; n <= 40; n++ {
		xs := ints[int64](r, n)
		o := ints[int64](r, len(xs))[:len(xs)]
		want := make([]int64, len(o))
		for i := range o {
			want[i] = stdMax(o[i], xs[i])
		}
		MaxInto(o, xs)
		for i := range o {
			if o[i] != want[i] {
				t.Errorf("MaxInto: [%d] = %d, want %d", i, o[i], want[i])
			}
		}
		// A destination too short is left as it is.
		short := append([]int64(nil), o[:len(o)/2]...)
		MaxInto(short, xs)
		if len(xs) > 0 && len(short) < len(xs) {
			for i := range short {
				if short[i] != o[i] {
					t.Errorf("MaxInto(shorter): [%d] = %d, want %d", i, short[i], o[i])
				}
			}
		}
		a, b := ints[int64](r, n), ints[int64](r, n)
		pairs := make([]int64, len(a)+1)
		pairs[len(a)] = 26
		MaxPairs(pairs, a, b)
		for i := range a {
			if w := stdMax(a[i], b[i]); pairs[i] != w {
				t.Errorf("MaxPairs: [%d] = %d, want %d", i, pairs[i], w)
			}
		}
		if pairs[len(a)] != 26 {
			t.Errorf("MaxPairs wrote past its pairs: %d", pairs[len(a)])
		}
		// Too few of o or of b: nothing is written.
		one := []int64{7}
		MaxPairs(one, a, b)
		if one[0] != 7 {
			t.Errorf("MaxPairs(shorter o) wrote %d", one[0])
		}
		before := append([]int64(nil), pairs...)
		MaxPairs(pairs, a, b[:len(b)-1])
		for i := range pairs {
			if pairs[i] != before[i] {
				t.Errorf("MaxPairs(shorter b) wrote [%d] = %d", i, pairs[i])
			}
		}
		i32, o32 := ints[int32](r, n), ints[int32](r, n)
		want32 := make([]int32, len(o32))
		for i := range o32 {
			want32[i] = stdMin(o32[i], i32[i])
		}
		MinInto(o32, i32)
		for i := range o32 {
			if o32[i] != want32[i] {
				t.Errorf("MinInto: [%d] = %d, want %d", i, o32[i], want32[i])
			}
		}
		for _, b := range [][2]int64{{-100, 100}, {7, 7}, {100, -100}, {math.MinInt64, math.MaxInt64}} {
			o := ints[int64](r, n)
			want := make([]int64, len(o))
			for i := range o {
				want[i] = stdClamp(o[i], b[0], b[1])
			}
			ClampInto(o, b[0], b[1])
			for i := range o {
				if o[i] != want[i] {
					t.Errorf("ClampInto(%d, %d): [%d] = %d, want %d", b[0], b[1], i, o[i], want[i])
				}
			}
		}
		for _, b := range [][2]uint8{{16, 235}, {0, 255}, {200, 100}, {9, 9}} {
			o := ints[uint8](r, n)
			want := make([]uint8, len(o))
			for i := range o {
				want[i] = stdClamp(o[i], b[0], b[1])
			}
			ClampU8(o, b[0], b[1])
			for i := range o {
				if o[i] != want[i] {
					t.Errorf("ClampU8(%d, %d): [%d] = %d, want %d", b[0], b[1], i, o[i], want[i])
				}
			}
		}
		o = ints[int64](r, n)
		in := append([]int64(nil), o...)
		AbsInto(o)
		for i := range o {
			if w := stdAbs(in[i]); o[i] != w {
				t.Errorf("AbsInto: [%d] = %d, want %d", i, o[i], w)
			}
		}
		o = ints[int64](r, n)
		in = append([]int64(nil), o...)
		SignInto(o)
		for i := range o {
			if w := stdSign(in[i]); o[i] != w {
				t.Errorf("SignInto: [%d] = %d, want %d", i, o[i], w)
			}
		}
		for _, th := range []int64{0, -5, math.MinInt64, math.MaxInt64} {
			o = ints[int64](r, n)
			in = append([]int64(nil), o...)
			AtLeastInto(o, th)
			for i := range o {
				if w := stdMax(in[i], th); o[i] != w {
					t.Errorf("AtLeastInto(%d): [%d] = %d, want %d", th, i, o[i], w)
				}
			}
		}
	}
}

func TestOneChoice(t *testing.T) {
	r := rand.New(rand.NewSource(2626))
	i64 := ints[int64](r, 20)
	for _, a := range i64 {
		for _, b := range i64 {
			if got, want := Max2(a, b), stdMax(a, b); got != want {
				t.Errorf("Max2(%d, %d) = %d, want %d", a, b, got, want)
			}
		}
		if got, want := Sign1(a), stdSign(a); got != want {
			t.Errorf("Sign1(%d) = %d, want %d", a, got, want)
		}
	}
	u8 := ints[uint8](r, 10)
	for _, a := range u8 {
		for _, b := range u8 {
			if got, want := Min2U8(a, b), stdMin(a, b); got != want {
				t.Errorf("Min2U8(%d, %d) = %d, want %d", a, b, got, want)
			}
		}
	}
	i32 := ints[int32](r, 6)
	for _, x := range i32 {
		for _, lo := range i32 {
			for _, hi := range i32 {
				if got, want := Clamp3(x, lo, hi), stdClamp(x, lo, hi); got != want {
					t.Errorf("Clamp3(%d, %d, %d) = %d, want %d", x, lo, hi, got, want)
				}
			}
		}
	}
}

func TestFloats(t *testing.T) {
	// std's choices, where Go's builtins choose otherwise.
	nan, negz := math.NaN(), math.Copysign(0, -1)
	if got := MaxF64(1, nan); got != 1 {
		t.Errorf("MaxF64(1, NaN) = %v, want 1", got)
	}
	if got := MaxF64(nan, 1); !math.IsNaN(got) {
		t.Errorf("MaxF64(NaN, 1) = %v, want NaN", got)
	}
	if got := MaxF64(negz, 0); !sameF64(got, negz) {
		t.Errorf("MaxF64(-0, 0) = %v, want -0", got)
	}
	if got := MinF64(0, negz); !sameF64(got, 0) {
		t.Errorf("MinF64(0, -0) = %v, want 0", got)
	}
	if got := MinF64(1, nan); got != 1 {
		t.Errorf("MinF64(1, NaN) = %v, want 1", got)
	}
	r := rand.New(rand.NewSource(26260))
	fs := floats(r, 8)
	for _, a := range fs {
		for _, b := range fs {
			if got, want := MaxF64(a, b), stdMax(a, b); !sameF64(got, want) {
				t.Errorf("MaxF64(%v, %v) = %v, want %v", a, b, got, want)
			}
			if got, want := MinF64(a, b), stdMin(a, b); !sameF64(got, want) {
				t.Errorf("MinF64(%v, %v) = %v, want %v", a, b, got, want)
			}
			a32, b32 := float32(a), float32(b)
			if got, want := MaxF32(a32, b32), stdMax(a32, b32); !sameF32(got, want) {
				t.Errorf("MaxF32(%v, %v) = %v, want %v", a32, b32, got, want)
			}
			if got, want := MinF32(a32, b32), stdMin(a32, b32); !sameF32(got, want) {
				t.Errorf("MinF32(%v, %v) = %v, want %v", a32, b32, got, want)
			}
		}
		if got, want := AbsF64(a), stdAbs(a); !sameF64(got, want) {
			t.Errorf("AbsF64(%v) = %v, want %v", a, got, want)
		}
	}
	for _, x := range specialF64 {
		for _, lo := range specialF64 {
			for _, hi := range specialF64 {
				if got, want := ClampF64(x, lo, hi), stdClamp(x, lo, hi); !sameF64(got, want) {
					t.Errorf("ClampF64(%v, %v, %v) = %v, want %v", x, lo, hi, got, want)
				}
			}
		}
	}
	for n := 0; n <= len(fs); n++ {
		xs := fs[:n]
		want := 0.0
		if n > 0 {
			want = fold(xs[1:], xs[0], stdMax[float64])
		}
		if got := MaxF64s(xs); !sameF64(got, want) {
			t.Errorf("MaxF64s(%v) = %v, want %v", xs, got, want)
		}
		o := append([]float64(nil), fs[len(fs)-n:]...)
		w := make([]float64, n)
		for i := range o {
			w[i] = stdMax(o[i], xs[i])
		}
		MaxF64Into(o, xs)
		for i := range o {
			if !sameF64(o[i], w[i]) {
				t.Errorf("MaxF64Into: [%d] = %v, want %v", i, o[i], w[i])
			}
		}
	}
}

func TestSelectsZeroAllocs(t *testing.T) {
	xs := []int64{3, -1, 4, -1, 5, -9, 2, 6}
	o := make([]int64, len(xs))
	px := []byte("pixels, clamped")
	if n := testing.AllocsPerRun(100, func() {
		_ = MaxI64(xs)
		_ = ClampSum(xs, -2, 2)
		MaxInto(o, xs)
		ClampU8(px, 'b', 'p')
		_ = MaxF64(1, 2)
	}); n != 0 {
		t.Errorf("the calls allocate %v times, want 0", n)
	}
}

// A choice that is a select takes as long whichever way it goes: on random
// data, where a branch would mispredict half the time, as on sorted data,
// where it would not. No benchmark's data changes from one pass to the next.

const benchN = 4096

func benchData(sorted bool) ([]int64, []int64) {
	r := rand.New(rand.NewSource(1))
	a, b := make([]int64, benchN), make([]int64, benchN)
	for i := range a {
		a[i], b[i] = r.Int63n(1000), r.Int63n(1000)
	}
	if sorted {
		sort.Slice(a, func(i, j int) bool { return a[i] < a[j] })
		for i := range b {
			b[i] = 500
		}
	}
	return a, b
}

func benchMaxPairs(b *testing.B, sorted bool) {
	x, y := benchData(sorted)
	o := make([]int64, benchN)
	b.SetBytes(benchN * 8)
	for i := 0; i < b.N; i++ {
		MaxPairs(o, x, y)
	}
}

func BenchmarkMaxPairsRandom(b *testing.B) { benchMaxPairs(b, false) }
func BenchmarkMaxPairsSorted(b *testing.B) { benchMaxPairs(b, true) }

func benchClampSum(b *testing.B, sorted bool) {
	xs, _ := benchData(sorted)
	b.SetBytes(benchN * 8)
	for i := 0; i < b.N; i++ {
		_ = ClampSum(xs, 250, 750)
	}
}

func BenchmarkClampSumRandom(b *testing.B) { benchClampSum(b, false) }
func BenchmarkClampSumSorted(b *testing.B) { benchClampSum(b, true) }

// The pixels are clamped in place, so each pass clamps a fresh copy.
func BenchmarkClampU8Random(b *testing.B) {
	orig, px := make([]byte, benchN), make([]byte, benchN)
	rand.New(rand.NewSource(2)).Read(orig)
	b.SetBytes(benchN)
	for i := 0; i < b.N; i++ {
		copy(px, orig)
		ClampU8(px, 64, 192)
	}
}

func BenchmarkMaxI64(b *testing.B) {
	xs, _ := benchData(false)
	b.SetBytes(benchN * 8)
	for i := 0; i < b.N; i++ {
		_ = MaxI64(xs)
	}
}
