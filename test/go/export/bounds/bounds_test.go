package goose

import (
	"fmt"
	"math"
	"math/rand"
	"slices"
	"testing"
)

// bounds.goose's kernels (issue #40), each against the same written in Go,
// whose own bounds checks Go knows do not return, and their checks failing:
// a panic with the *GsError of the check.

// siftGo and heapSortGo are bounds.goose's sift and heap_sort.
func siftGo(v []int64, start, n int64) {
	i := start
	x := v[i]
	for {
		l := 2*i + 1
		if l >= n {
			break
		}
		m := l
		if l+1 < n && v[l] < v[l+1] {
			m = l + 1
		}
		if v[m] <= x {
			break
		}
		v[i] = v[m]
		i = m
	}
	v[i] = x
}

func heapSortGo(v []int64) {
	n := int64(len(v))
	for i := n / 2; i > 0; {
		i--
		siftGo(v, i, n)
	}
	for end := n; end > 1; {
		end--
		v[0], v[end] = v[end], v[0]
		siftGo(v, 0, end)
	}
}

func rowSumsGo(rowptr, cols, x []int64) int64 {
	var h int64
	for r := 0; r+1 < len(rowptr); r++ {
		var s int64
		for k := rowptr[r]; k < rowptr[r+1]; k++ {
			s += x[cols[k]]
		}
		h = h*31 + s
	}
	return h
}

func fieldsGo(text []byte, starts []int64) int64 {
	var h int64
	for j := 0; j+1 < len(starts); j++ {
		f := text[starts[j]:starts[j+1]]
		h = h*31 + int64(len(f))
		if len(f) > 0 {
			h += int64(f[0])
		}
	}
	return h
}

func tallyGo(xs []byte) int64 {
	var counts [16]int64
	for _, x := range xs {
		counts[x]++
	}
	var h int64
	for _, c := range counts {
		h = h*31 + c
	}
	return h
}

// randoms is n random values: from a few values, so that there are equal
// ones, or from all of them, the ends of the range included.
func randoms(r *rand.Rand, n int) []int64 {
	v := make([]int64, n)
	few := r.Intn(2) == 0
	for i := range v {
		switch {
		case few:
			v[i] = r.Int63n(5) - 2
		case r.Intn(16) == 0:
			v[i] = []int64{math.MinInt64, math.MaxInt64, 0, -1}[r.Intn(4)]
		default:
			v[i] = int64(r.Uint64())
		}
	}
	return v
}

// graph is a random graph of n nodes in CSR form, of up to deg neighbours a
// node, and a value for each node.
func graph(r *rand.Rand, n, deg int) (rowptr, cols, x []int64) {
	rowptr = make([]int64, n+1)
	cols = []int64{}
	for i := 0; i < n; i++ {
		rowptr[i] = int64(len(cols))
		for d := r.Intn(deg + 1); d > 0; d-- {
			cols = append(cols, r.Int63n(int64(n)))
		}
	}
	rowptr[n] = int64(len(cols))
	x = randoms(r, n)
	return rowptr, cols, x
}

// offsets is the sorted offsets of n fields of text: 0, len(text) and
// random ones between, equal ones included.
func offsets(r *rand.Rand, text []byte, n int) []int64 {
	starts := []int64{0, int64(len(text))}
	for len(starts) < n+1 {
		starts = append(starts, r.Int63n(int64(len(text))+1))
	}
	slices.Sort(starts)
	return starts
}

func TestHeapSort(t *testing.T) {
	r := rand.New(rand.NewSource(1))
	for n := 0; n < 300; n++ {
		v := randoms(r, n)
		want := slices.Clone(v)
		slices.Sort(want)
		for _, k := range []struct {
			name string
			sort func([]int64)
		}{{"HeapSort", HeapSort}, {"HeapSortAsserted", HeapSortAsserted}, {"heapSortGo", heapSortGo}} {
			got := slices.Clone(v)
			k.sort(got)
			if !slices.Equal(got, want) {
				t.Fatalf("%s(%v) = %v, want %v", k.name, v, got, want)
			}
		}
	}
}

func TestSift(t *testing.T) {
	r := rand.New(rand.NewSource(2))
	for i := 0; i < 2000; i++ {
		v := randoms(r, 1+r.Intn(40))
		n := 1 + r.Int63n(int64(len(v)))
		start := r.Int63n(n)
		want := slices.Clone(v)
		siftGo(want, start, n)
		for _, k := range []struct {
			name string
			sift func([]int64, int64, int64)
		}{{"Sift", Sift}, {"SiftAsserted", SiftAsserted}} {
			got := slices.Clone(v)
			k.sift(got, start, n)
			if !slices.Equal(got, want) {
				t.Fatalf("%s(%v, %d, %d) = %v, want %v", k.name, v, start, n, got, want)
			}
		}
	}
}

func TestRowSums(t *testing.T) {
	r := rand.New(rand.NewSource(3))
	if got := RowSums(nil, nil, nil); got != 0 {
		t.Errorf("RowSums of no nodes = %d", got)
	}
	for n := 1; n < 100; n++ {
		rowptr, cols, x := graph(r, n, 1+n%9)
		want := rowSumsGo(rowptr, cols, x)
		if got := RowSums(rowptr, cols, x); got != want {
			t.Fatalf("RowSums(%v, %v, %v) = %d, want %d", rowptr, cols, x, got, want)
		}
		if got := RowSumsAsserted(rowptr, cols, x); got != want {
			t.Fatalf("RowSumsAsserted(%v, %v, %v) = %d, want %d", rowptr, cols, x, got, want)
		}
	}
}

func TestFields(t *testing.T) {
	r := rand.New(rand.NewSource(4))
	for n := 0; n < 100; n++ {
		text := make([]byte, n)
		r.Read(text)
		for _, starts := range [][]int64{nil, {0}, offsets(r, text, 1), offsets(r, text, 1+r.Intn(8))} {
			if got, want := Fields(text, starts), fieldsGo(text, starts); got != want {
				t.Fatalf("Fields(%q, %v) = %d, want %d", text, starts, got, want)
			}
		}
	}
}

func TestTally(t *testing.T) {
	r := rand.New(rand.NewSource(5))
	for n := 0; n < 100; n++ {
		xs := make([]byte, n)
		for i := range xs {
			xs[i] = byte(r.Intn(16))
		}
		if got, want := Tally(xs), tallyGo(xs); got != want {
			t.Fatalf("Tally(%v) = %d, want %d", xs, got, want)
		}
		if got, want := Tally(string(xs)), tallyGo(xs); got != want {
			t.Fatalf("Tally(string %v) = %d, want %d", xs, got, want)
		}
	}
}

// abortOf is the *GsError f panicked with, or nil, and what else it did.
func abortOf(f func()) (e *GsError, other string) {
	defer func() {
		if r := recover(); r != nil {
			var ok bool
			if e, ok = r.(*GsError); !ok {
				other = fmt.Sprintf("a panic of %v", r)
			}
		}
	}()
	f()
	return nil, "no panic"
}

// A failed check panics with its *GsError in the caller's goroutine, as an
// abort does in a library, naming what failed -- for an index, the index
// and the length, as C's gs_idxfail does -- and the library goes on
// working. The lines are the checks', which test/go/programs/ checks
// against C's (index_abort.goose).
func TestCheckAborts(t *testing.T) {
	cases := []struct {
		name string
		f    func()
		want string
	}{
		{"Sift from 5", func() { Sift([]int64{1, 2, 3}, 5, 3) }, "index 5 out of bounds (length 3)"},
		{"Sift from -1", func() { Sift([]int64{1, 2, 3}, -1, 3) }, "index -1 out of bounds (length 3)"},
		{"Sift past the end", func() { Sift([]int64{1, 2, 3}, 0, 10) }, "index 5 out of bounds (length 3)"},
		{"SiftAsserted past the end", func() { SiftAsserted([]int64{1, 2, 3}, 0, 10) }, "assert failed"},
		{"RowSums", func() { RowSums([]int64{0, 2}, []int64{0, 7}, []int64{1, 2, 3}) },
			"index 7 out of bounds (length 3)"},
		{"RowSums past cols", func() { RowSums([]int64{0, 3}, []int64{0, 1}, []int64{1, 2, 3}) },
			"index 2 out of bounds (length 2)"},
		{"RowSumsAsserted", func() { RowSumsAsserted([]int64{0, 2}, []int64{0, 7}, []int64{1, 2, 3}) },
			"assert failed"},
		{"Fields", func() { Fields("abc", []int64{0, 2, 1}) }, "slice bounds out of range"},
		{"Tally", func() { Tally([]byte{1, 16}) }, "index 16 out of bounds (length 16)"},
	}
	for _, c := range cases {
		e, other := abortOf(c.f)
		if e == nil || e.Msg != c.want || e.File != "test/go/export/bounds/bounds.goose" || e.Line <= 0 {
			t.Errorf("%s: %v%s, want a *GsError of %q at a line of bounds.goose", c.name, e, other, c.want)
		}
	}
	rowptr, cols, x := []int64{0, 2, 3}, []int64{2, 0, 1}, []int64{1, 2, 3}
	if got, want := RowSums(rowptr, cols, x), rowSumsGo(rowptr, cols, x); got != want {
		t.Errorf("RowSums after the aborts = %d, want %d", got, want)
	}
}

// A call allocates nothing.
func TestBoundsZeroAllocs(t *testing.T) {
	r := rand.New(rand.NewSource(6))
	v := randoms(r, 64)
	rowptr, cols, x := graph(r, 64, 8)
	text := []byte("a,bb,ccc,dddd")
	starts := []int64{0, 2, 5, 9, 13}
	var sink int64
	for _, f := range []func(){
		func() { HeapSort(v) },
		func() { HeapSortAsserted(v) },
		func() { sink += RowSums(rowptr, cols, x) + RowSumsAsserted(rowptr, cols, x) },
		func() { sink += Fields(text, starts) + Tally("\x01\x02\x03") },
	} {
		if n := testing.AllocsPerRun(1000, f); n != 0 {
			t.Errorf("%v allocs/op, want 0", n)
		}
	}
	_ = fmt.Sprint(sink)
}

// go test -bench . compares each kernel with its twin whose indices are
// asserted in range, and with the same written in Go.

func benchSort(b *testing.B, sort func([]int64)) {
	r := rand.New(rand.NewSource(7))
	src := make([]int64, 4096)
	for i := range src {
		src[i] = r.Int63()
	}
	v := make([]int64, len(src))
	for i := 0; i < b.N; i++ {
		copy(v, src)
		sort(v)
	}
}

func BenchmarkHeapSort(b *testing.B)         { benchSort(b, HeapSort) }
func BenchmarkHeapSortAsserted(b *testing.B) { benchSort(b, HeapSortAsserted) }
func BenchmarkHeapSortGo(b *testing.B)       { benchSort(b, heapSortGo) }

func benchRowSums(b *testing.B, sums func(rowptr, cols, x []int64) int64) {
	rowptr, cols, x := graph(rand.New(rand.NewSource(8)), 1024, 32)
	var sum int64
	for i := 0; i < b.N; i++ {
		sum += sums(rowptr, cols, x)
	}
	_ = sum
}

func BenchmarkRowSums(b *testing.B)         { benchRowSums(b, RowSums) }
func BenchmarkRowSumsAsserted(b *testing.B) { benchRowSums(b, RowSumsAsserted) }
func BenchmarkRowSumsGo(b *testing.B)       { benchRowSums(b, rowSumsGo) }

func benchTally(b *testing.B, tally func([]byte) int64) {
	xs := make([]byte, 4096)
	for i := range xs {
		xs[i] = byte(i * 7 % 16)
	}
	b.SetBytes(int64(len(xs)))
	var sum int64
	for i := 0; i < b.N; i++ {
		sum += tally(xs)
	}
	_ = sum
}

func BenchmarkTally(b *testing.B)   { benchTally(b, func(xs []byte) int64 { return Tally(xs) }) }
func BenchmarkTallyGo(b *testing.B) { benchTally(b, tallyGo) }
