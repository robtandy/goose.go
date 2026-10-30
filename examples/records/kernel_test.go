//go:build goose

package records

import (
	"math/rand"
	"runtime"
	"testing"
)

// The kernel (countRecords, with -tags goose) against the Go original
// (countRecordsGo): the same count of every segment, and a panic wherever
// the original panics, each its own kind.

// outcome is what a count of a segment did: return n, or panic.
type outcome struct {
	n        int64
	panicked bool
}

// run is count(seg)'s outcome, and what it panicked with.
func run(count func([]byte) int64, seg []byte) (o outcome, r any) {
	defer func() {
		if r = recover(); r != nil {
			o.panicked = true
		}
	}()
	return outcome{n: count(seg)}, nil
}

// same fails t unless the kernel does with seg what the original does.
func same(t *testing.T, seg []byte) {
	t.Helper()
	want, wr := run(countRecordsGo, seg)
	got, gr := run(countRecords, seg)
	if got != want {
		t.Fatalf("% x: the kernel's %+v (%v), the original's %+v (%v)", seg, got, gr, want, wr)
	}
	if want.panicked {
		// A failed bounds check: a runtime.Error in Go, a *gsError in Goose.
		if _, ok := wr.(runtime.Error); !ok {
			t.Fatalf("% x: the original panicked with %v", seg, wr)
		}
		if _, ok := gr.(*gsError); !ok {
			t.Fatalf("% x: the kernel panicked with %v", seg, gr)
		}
	}
}

// payload is n bytes, each its index plus one.
func payload(n int) string {
	b := make([]byte, n)
	for i := range b {
		b[i] = byte(i + 1)
	}
	return string(b)
}

func TestKernelTable(t *testing.T) {
	cases := [][]byte{
		nil,
		segment(""),
		segment("goose", "gander", "gosling"),
		segment(payload(300)),
		{0xff, 0xff, 0xff, 0xff, 0}, // a length past any segment's end
	}
	// Two payloads of 17 bytes between them, either side of a word: whole,
	// cut short anywhere, and with each byte flipped in turn.
	for n := 0; n <= 17; n++ {
		seg := segment(payload(n), payload(17-n))
		cases = append(cases, seg)
		for cut := 0; cut < len(seg); cut++ {
			cases = append(cases, seg[:cut])
		}
		for i := range seg {
			flipped := append([]byte(nil), seg...)
			flipped[i] ^= 0x40
			cases = append(cases, flipped)
		}
	}
	for _, seg := range cases {
		same(t, seg)
	}
}

func TestKernelRandom(t *testing.T) {
	r := rand.New(rand.NewSource(1))
	for i := 0; i < 10000; i++ {
		var seg []byte
		for k := r.Intn(8); k > 0; k-- {
			p := make([]byte, r.Intn(40))
			r.Read(p)
			seg = appendRecord(seg, p)
		}
		switch r.Intn(4) {
		case 0: // a byte flipped: a payload's, a checksum's or a length's
			if len(seg) > 0 {
				seg[r.Intn(len(seg))] ^= byte(1 + r.Intn(255))
			}
		case 1: // cut short
			seg = seg[:r.Intn(len(seg)+1)]
		}
		same(t, seg)
	}
}
