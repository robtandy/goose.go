package goose

import (
	"syscall"
	"unsafe"
)

// What the runtime has as a package of its own, which the compiler does not
// embed (cmake/go_runtime.cmake): a file it writes with --go-runtime=embed
// declares its own program, gsProg, of the sizes the program was compiled
// with, in this one's stead.

// The runtime's default sizes: the address space each data stack reserves
// (GS_STACK_RESERVE, 2 GB, of which the OS supplies pages only as the
// stack's top reaches them), the PROT_NONE guard gap behind each one
// (GS_STACK_GAP), and each block's fixed-frame stack's reservation.
const (
	GsStackReserve int64 = 2 << 30
	GsStackGap     int64 = 1 << 20
	GsFrameReserve int64 = 1 << 30
)

// gsProg is the program NewStackBlock, AcquireStackBlock and
// ReleaseStackBlock make blocks for, of the default sizes: what a Go
// program that calls the runtime itself, or hands a block to the entry
// point of an export fn of a package compiled with --go-runtime=import,
// has. Every such package declares a program of its own.
var gsProg = GsProgram{StackReserve: GsStackReserve, StackGap: GsStackGap, FrameReserve: GsFrameReserve}

// Goose, and so its runtime, assumes a 64-bit host, where an int holds
// every size a data stack can reach: elsewhere this is a negative length.
var _ [unsafe.Sizeof(uintptr(0)) - 8]byte

// The unit memory is mapped and protected in.
var gsPageSize = int64(syscall.Getpagesize())

// GsMapCount is how many times the runtime has mapped memory: none, until a
// program starts or a block is made (a library's package maps none at its
// import, issue #18).
func GsMapCount() int64 { return gsMaps.Load() }

// GsBlockCount is how many blocks NewStackBlock has made that are not
// closed.
func GsBlockCount() int {
	gsBlocksMu.Lock()
	defer gsBlocksMu.Unlock()
	return len(gsBlocks)
}
