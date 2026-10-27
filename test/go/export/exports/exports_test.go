package goose

import (
	"bytes"
	"strings"
	"testing"
	"unsafe"
)

// The export fns' Go entry points (src/codegen_go_export.h): Name, on a
// block from the pool, and NameWithStack, on the caller's.

func TestScalarsAndStructs(t *testing.T) {
	if got := Twice(21); got != 42 {
		t.Errorf("Twice(21) = %d", got)
	}
	r := Rect_g{x_g: 1, y_g: 2, w_g: 3, h_g: 4}
	if got := Area(&r); got != 12 {
		t.Errorf("Area = %v", got)
	}
	s := Scaled(r, 2)
	if s.w_g != 6 || s.h_g != 8 || s.x_g != 1 {
		t.Errorf("Scaled = %+v", s)
	}
}

func TestSlices(t *testing.T) {
	// A const u8[:] takes a string or a []byte, with no copy.
	want := int64(1*'a' + 2*'b' + 3*'c')
	if got := Checksum("a b c"); got != want {
		t.Errorf("Checksum(string) = %d, want %d", got, want)
	}
	if got := Checksum([]byte("a b c")); got != want {
		t.Errorf("Checksum([]byte) = %d, want %d", got, want)
	}
	if got := Checksum(""); got != 0 {
		t.Errorf("Checksum(\"\") = %d", got)
	}
	buf := []byte("Mixed Up")
	if n := Upcase(buf); n != 5 || string(buf) != "MIXED UP" {
		t.Errorf("Upcase = %d, %q", n, buf)
	}
	if got := Total([]int64{1, 2, 3, 4}); got != 10 {
		t.Errorf("Total = %d", got)
	}
	if got := Total(nil); got != 0 {
		t.Errorf("Total(nil) = %d", got)
	}
	// The result views the argument: no copy.
	text := []byte("hello world")
	w := FirstWordOf(text)
	if string(w) != "hello" || &w[0] != &text[0] {
		t.Errorf("FirstWordOf = %q, a view of the text: %v", w, &w[0] == &text[0])
	}
	if got := FirstWordOf("single"); string(got) != "single" {
		t.Errorf("FirstWordOf(single) = %q", got)
	}
}

func TestStdlib(t *testing.T) {
	if got := DistinctWords("the cat and the dog and the bird"); got != 5 {
		t.Errorf("DistinctWords = %d", got)
	}
	doc := `{"a": 1, "b": [2, 3.5, {"c": -0.5}], "d": "x", "e": null}`
	if got := JsonSum(doc); got != 6 {
		t.Errorf("JsonSum = %v", got)
	}
	if got := JsonSum("{oops"); got != -1 {
		t.Errorf("JsonSum(bad) = %v", got)
	}
}

func TestWithStack(t *testing.T) {
	// The caller's own block, kept across calls, each of which gives its
	// stacks back as it found them.
	gs := NewStackBlock(0, gsStackReserve, gsStackGap)
	defer gs.Close()
	DistinctWordsWithStack(gs, "warm up")
	ChecksumWithStack(gs, "warm up")
	tops := append([]unsafe.Pointer(nil), gs.Stks...)
	frame := gs.Frame
	for i := 0; i < 3; i++ {
		if got := DistinctWordsWithStack(gs, "a b a c"); got != 3 {
			t.Fatalf("DistinctWordsWithStack = %d", got)
		}
		if got := ChecksumWithStack(gs, []byte("ab")); got != 'a'+2*'b' {
			t.Fatalf("ChecksumWithStack = %d", got)
		}
		for k, top := range tops {
			if gs.Stks[k] != top {
				t.Fatalf("stack %d's top moved", k)
			}
		}
		if gs.Frame != frame {
			t.Fatalf("the frame moved")
		}
	}
	if got := TwiceWithStack(gs, 5); got != 10 {
		t.Errorf("TwiceWithStack = %d", got)
	}
	// A block from the pool works as well.
	b := AcquireStackBlock(4)
	if got := JsonSumWithStack(b, "[1, 2, 3]"); got != 6 {
		t.Errorf("JsonSumWithStack = %v", got)
	}
	ReleaseStackBlock(b)
}

func TestAbortPanics(t *testing.T) {
	defer func() {
		r := recover()
		e, ok := r.(*GsError)
		if !ok || !strings.Contains(e.Error(), "checked_div: 7 by zero") {
			t.Errorf("recovered %v", r)
		}
	}()
	if got := CheckedDiv(7, 2); got != 3 {
		t.Errorf("CheckedDiv(7, 2) = %d", got)
	}
	CheckedDiv(7, 0)
	t.Errorf("CheckedDiv(7, 0) returned")
}

func TestGlobals(t *testing.T) {
	first := CountCall()
	if second := CountCall(); second != first+1 {
		t.Errorf("CountCall: %d then %d", first, second)
	}
}

// A call in a tight loop allocates nothing: the pool's block, and the
// arguments viewed in place, once the pool holds a block.
func TestZeroAllocs(t *testing.T) {
	text := []byte("the quick brown fox jumps over the lazy dog the end")
	doc := []byte(`{"xs": [1, 2, 3, 4.5], "n": {"m": 10}}`)
	buf := []byte("lower case")
	xs := []int64{3, 1, 4, 1, 5, 9, 2, 6}
	r := Rect_g{w_g: 2, h_g: 3}
	gs := AcquireStackBlock(8)
	defer ReleaseStackBlock(gs)
	var sink float64
	cases := []struct {
		name string
		f    func()
	}{
		{"Twice", func() { sink += float64(Twice(int64(len(text)))) }},
		{"Area", func() { sink += Area(&r) }},
		{"Scaled", func() { sink += Scaled(r, 1.5).w_g }},
		{"Checksum []byte", func() { sink += float64(Checksum(text)) }},
		{"Checksum string", func() { sink += float64(Checksum("a constant string")) }},
		{"Upcase", func() { sink += float64(Upcase(buf)) }},
		{"FirstWordOf", func() { sink += float64(len(FirstWordOf(text))) }},
		{"Total", func() { sink += float64(Total(xs)) }},
		{"DistinctWords", func() { sink += float64(DistinctWords(text)) }},
		{"JsonSum", func() { sink += JsonSum(doc) }},
		{"CountCall", func() { sink += float64(CountCall()) }},
		{"DistinctWordsWithStack", func() { sink += float64(DistinctWordsWithStack(gs, text)) }},
		{"JsonSumWithStack", func() { sink += JsonSumWithStack(gs, doc) }},
	}
	for _, c := range cases {
		if n := testing.AllocsPerRun(1000, c.f); n != 0 {
			t.Errorf("%s: %v allocs/op, want 0", c.name, n)
		}
	}
	if sink == 0 || !bytes.Equal(buf, []byte("LOWER CASE")) {
		t.Errorf("sink %v, buf %q", sink, buf)
	}
}

func BenchmarkJsonSum(b *testing.B) {
	doc := []byte(`{"xs": [1, 2, 3, 4.5], "n": {"m": 10}, "s": "text"}`)
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		JsonSum(doc)
	}
}

func BenchmarkDistinctWords(b *testing.B) {
	text := []byte("the quick brown fox jumps over the lazy dog the end")
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		DistinctWords(text)
	}
}
