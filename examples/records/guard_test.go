//go:build goose && (linux || darwin)

package records

import (
	"runtime/debug"
	"syscall"
	"testing"
)

// The kernel reads 4 and 8 bytes at a time through pointers Go does not
// check. Each segment here ends where a PROT_NONE page starts, so that a
// read past its end faults, whatever it would have read.

// guarded is n bytes that end where a PROT_NONE page starts, their capacity
// included.
func guarded(t *testing.T, n int) []byte {
	page := syscall.Getpagesize()
	size := (n + page - 1) / page * page
	mem, err := syscall.Mmap(-1, 0, size+page, syscall.PROT_READ|syscall.PROT_WRITE,
		syscall.MAP_ANON|syscall.MAP_PRIVATE)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { syscall.Munmap(mem) })
	if err := syscall.Mprotect(mem[size:], syscall.PROT_NONE); err != nil {
		t.Fatal(err)
	}
	return mem[size-n : size : size]
}

func TestKernelGuardPage(t *testing.T) {
	// A fault panics, which same reports, rather than ending the test.
	defer debug.SetPanicOnFault(debug.SetPanicOnFault(true))
	g := guarded(t, 64)
	for n := 0; n <= 17; n++ {
		seg := segment(payload(n), payload(17-n))
		for cut := 0; cut <= len(seg); cut++ {
			buf := g[len(g)-cut:]
			copy(buf, seg)
			same(t, buf)
		}
	}
}
