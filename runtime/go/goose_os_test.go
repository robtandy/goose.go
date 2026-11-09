package goose

import (
	"bytes"
	"os"
	"testing"
	"unsafe"
)

// withStdin runs f with input as the standard input the runtime reads, its
// buffer empty before and after.
func withStdin(t *testing.T, input []byte, f func()) {
	t.Helper()
	r, w, err := os.Pipe()
	if err != nil {
		t.Fatal(err)
	}
	go func() {
		_, _ = w.Write(input)
		_ = w.Close()
	}()
	saved := os.Stdin
	os.Stdin = r
	gsStdin.pos, gsStdin.end = 0, 0
	defer func() {
		os.Stdin = saved
		gsStdin.pos, gsStdin.end = 0, 0
		_ = r.Close()
	}()
	f()
}

// read_stdin(out, max) (gs_os_read_stdin_max): up to max bytes a call,
// waiting for them across reads of the pipe and of the runtime's 64 KB
// buffer, appended to what the builder holds; false, with nothing appended,
// at the end of the input and for a max of 0 or less; and the buffer
// read_line reads from is the same.
func TestOsReadStdinMax(t *testing.T) {
	gs := NewStackBlock(1, 64<<20, 64<<10)
	defer gs.Close()
	hdr := GsRhdr{Base: gs.Stks[0]}
	out := GsRref{Hdr: &hdr, Stk: 0}
	got := func() []byte { return unsafe.Slice((*byte)(hdr.Base), hdr.Len) }
	input := make([]byte, 150001)
	for i := range input {
		input[i] = byte(i*7 + i/251)
	}
	copy(input, "first line\r\nsecond")
	withStdin(t, input, func() {
		if GsOsReadStdinMax(gs, out, 0) || GsOsReadStdinMax(gs, out, -5) || hdr.Len != 0 {
			t.Fatalf("a max of 0 or less read %d bytes", hdr.Len)
		}
		if !GsOsReadLine(gs, out) || string(got()) != "first line" {
			t.Fatalf("read_line: %q", got())
		}
		if !GsOsReadStdinMax(gs, out, 6) || string(got()) != "first linesecond" {
			t.Fatalf("6 bytes after the line: %q", got())
		}
		at := int64(len("first line\r\nsecond"))
		for _, piece := range []int64{1, 1000, 70000, 65536} {
			n := hdr.Len
			if !GsOsReadStdinMax(gs, out, piece) || hdr.Len != n+piece ||
				!bytes.Equal(got()[n:], input[at:at+piece]) {
				t.Fatalf("a piece of %d: %d bytes", piece, hdr.Len-n)
			}
			at += piece
		}
		n := hdr.Len
		rest := int64(len(input)) - at
		if !GsOsReadStdinMax(gs, out, rest+100) || hdr.Len != n+rest || !bytes.Equal(got()[n:], input[at:]) {
			t.Fatalf("the last %d bytes: %d", rest, hdr.Len-n)
		}
		if GsOsReadStdinMax(gs, out, 10) || hdr.Len != n+rest {
			t.Fatal("a read at the end of the input appended bytes or answered true")
		}
		if gs.Stks[0] != unsafe.Add(hdr.Base, hdr.Len) {
			t.Fatal("the builder's stack top is not past its bytes")
		}
	})
}
