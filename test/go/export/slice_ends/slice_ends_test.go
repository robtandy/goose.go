package goose

import (
	"strings"
	"testing"
	"unsafe"
)

// Slices that end where their allocations on Go's heap do, sliced at their
// end by the export fns of slice_ends.goose (issue #39): such a slice is
// empty, and starts at no address of the slice it was cut from, nor one
// past its end, which is no valid Go pointer: -d=checkptr (and -race) aborts
// on one. Every other slice starts where its elements do, as Go's own do.

// b[1:4:4] of a b of its own: 1, 2, 3, ending where b's allocation does.
func capTight() []uint32 {
	b := make([]uint32, 4)
	b[1], b[2], b[3] = 1, 2, 3
	return b[1:4:4]
}

// text's bytes, in a []byte of their own: 16 of them end where their
// allocation does.
func ownBytes(text string) []byte {
	b := make([]byte, len(text))
	copy(b, text)
	return b
}

// inOrAtEnd says whether p is an address of the n bytes at start, or one
// past them.
func inOrAtEnd(p, start uintptr, n int) bool { return p >= start && p-start <= uintptr(n) }

func TestNoteTails(t *testing.T) {
	s := capTight()
	noted = nil
	NoteTails(s)
	names := []string{"s[s.len..]", "s[s.len..s.len]", "s[^0..]"}
	if len(noted) != len(names) {
		t.Fatalf("noted %d slices; want %d", len(noted), len(names))
	}
	start, size := uintptr(unsafe.Pointer(&s[0])), len(s)*4
	for i, name := range names {
		if n := noted[i]; n.n != 0 || inOrAtEnd(n.data, start, size) || n.data != noted[0].data {
			t.Errorf("%s = %#x, %d; want 0 elements, starting where %s does, at none of s's %#x..%#x",
				name, n.data, n.n, names[0], start, start+uintptr(size))
		}
	}
}

func TestTails(t *testing.T) {
	b := ownBytes("goose,goose,egg,")
	start := uintptr(unsafe.Pointer(&b[0]))
	tail := uintptr(unsafe.Pointer(unsafe.SliceData(Tail(b))))
	for _, c := range []struct {
		name string
		f    func([]byte) []byte
	}{{"text[text.len..]", Tail[[]byte]}, {"text[text.len..text.len]", TailToLen[[]byte]},
		{"text[^0..]", TailFromEnd[[]byte]}} {
		e := c.f(b)
		if p := uintptr(unsafe.Pointer(unsafe.SliceData(e))); len(e) != 0 || inOrAtEnd(p, start, len(b)) || p != tail {
			t.Errorf("%s = %#x, %d; want 0 bytes, starting where Tail's do, at none of b's %#x..%#x",
				c.name, p, len(e), start, start+uintptr(len(b)))
		}
	}
	// A string's bytes, on Go's heap.
	str := strings.Repeat("goose,", 8)
	e := Tail(str)
	if p := uintptr(unsafe.Pointer(unsafe.SliceData(e))); len(e) != 0 ||
		inOrAtEnd(p, uintptr(unsafe.Pointer(unsafe.StringData(str))), len(str)) {
		t.Errorf("Tail(str) = %#x, %d; want 0 bytes at none of str's", p, len(e))
	}
}

func TestRest(t *testing.T) {
	const text = "goose,goose,egg,"
	b := ownBytes(text)
	tail := unsafe.SliceData(Tail(b))
	for k := 0; k <= len(b); k++ {
		r := Rest(b, int64(k))
		if string(r) != text[k:] {
			t.Errorf("Rest(%q, %d) = %q", text, k, r)
		}
		// Where its elements start, as Go's b[k:] does, but at the end,
		// where Tail does.
		want := unsafe.SliceData(b[k:])
		if k == len(b) {
			want = tail
		}
		if unsafe.SliceData(r) != want {
			t.Errorf("Rest(%q, %d) starts at %p; want %p", text, k, unsafe.SliceData(r), want)
		}
	}
}

func TestSumFrom(t *testing.T) {
	s := capTight()
	for k, want := range []int64{6, 5, 3, 0} {
		if got := SumFrom(s, int64(k)); got != want {
			t.Errorf("SumFrom(s, %d) = %d; want %d", k, got, want)
		}
	}
	// Past the end, the bounds check aborts before any slice is cut.
	defer func() {
		if e, ok := recover().(error); !ok || !strings.Contains(e.Error(), "slice bounds out of range") {
			t.Errorf("SumFrom(s, 4) recovered %v", e)
		}
	}()
	SumFrom(s, 4)
	t.Error("SumFrom(s, 4) returned")
}

func TestPieces(t *testing.T) {
	for _, c := range []struct {
		text string
		want int64
	}{{"goose,goose,egg,", 4}, {"goose,goose,egg", 3}, {",", 2}, {"", 1}} {
		if got := Pieces(ownBytes(c.text)); got != c.want {
			t.Errorf("Pieces([]byte(%q)) = %d; want %d", c.text, got, c.want)
		}
		if got := Pieces(strings.Clone(c.text)); got != c.want {
			t.Errorf("Pieces(%q) = %d; want %d", c.text, got, c.want)
		}
	}
	if got := Pieces(strings.Repeat("goose,", 8)); got != 9 {
		t.Errorf("Pieces of 8 commas = %d; want 9", got)
	}
}
