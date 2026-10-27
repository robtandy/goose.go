// Package goose is the pure-Go runtime of the Goose compiler's Go backend
// (goose --target=go): what src/runtime/ is to the C backend, without cgo.
//
// Goose places every dynamic value on data stacks: large regions of virtual
// memory reserved up front, which a value grows by bumping the stack's top
// and a scope frees by rewinding it (docs/implementation.md §6.2, §7). Here
// each region is an anonymous syscall.Mmap mapping outside the Go heap,
// with a PROT_NONE guard gap behind it, so the garbage collector never
// scans or frees what lives on a stack, and growth past a region faults at
// once. A thread program's stacks make up a GsStackBlock, which the
// generated code takes as a hidden parameter beside gs_sp, and a sync.Pool
// of blocks gives exported entry points their stacks without allocating.
//
// The other files hold what the generated code calls: goose_spans.go the
// freelists of reusable[] slice pools, and goose_bytes.go integer and float
// semantics, varints, memory moves, byte search and the runtime's aborts.
//
// Memory on a data stack is outside the Go heap, which brings one rule for
// the generated code and for any code that writes there: nothing on a stack
// may be the only reference to a Go heap object, since the collector never
// sees it there (AGENTS.md). Pointers into the stacks themselves, and into
// buffers their owners keep alive, are fine anywhere.
//
// The runtime reserves its regions with syscall.Mmap and syscall.Mprotect,
// which the syscall package has on Linux and macOS; like the C backend it
// assumes a 64-bit host.
package goose

import (
	"runtime"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// Data stack sizes. The C runtime's macros of the same purpose are named
// in each comment.
const (
	// GsStackReserve is the address space each data stack reserves by
	// default (GS_STACK_RESERVE): 2 GB, of which the OS supplies pages only
	// as the stack's top reaches them.
	GsStackReserve int64 = 2 << 30
	// GsStackGap is the PROT_NONE guard gap behind each reservation by
	// default (GS_STACK_GAP), so that a stack growing past its region
	// faults at once instead of writing over whatever lies beyond it.
	GsStackGap int64 = 1 << 20

	// The smallest reservation a refusal halves down to (GS_REGION_MIN).
	gsRegionMin int64 = 1 << 20
	// The largest reservation there is (goose_spec.md §10.4): what lets the
	// compiler take every size, count and index to fit in 48 bits.
	gsReserveMax int64 = 1 << 48
	// MADV_NOHUGEPAGE on every Linux port Go has; syscall defines the name
	// only on Linux, and this file builds for macOS too.
	gsMadvNoHugePage = 15
)

// Goose, and so its runtime, assumes a 64-bit host, where an int holds
// every size a data stack can reach: elsewhere this is a negative length.
var _ [unsafe.Sizeof(uintptr(0)) - 8]byte

// GsSlice is a Goose slice, T[:] (docs/implementation.md §6.1): Len
// elements at Data, which may point into a data stack, into Go memory its
// owner keeps alive, or nowhere at all for an empty slice (nil).
type GsSlice struct {
	Data unsafe.Pointer
	Len  int64
}

// GsRhdr is the header of a resizable value, T[] or T[>..] (§6.1): Len
// elements from Base, which sit on top of the header's data stack, so
// that pushing an element writes it at the stack's top, moves the top past
// it and counts it. The base never moves.
type GsRhdr struct {
	Base unsafe.Pointer
	Len  int64
}

// GsRref is a fat reference to a resizable value (§6.1, C's gs_rref): its
// header, and the index in the GsStackBlock of the data stack its elements
// top, so that a callee can grow the value it was handed.
type GsRref struct {
	Hdr *GsRhdr
	Stk int64
}

// GsPref is a reference to a reusable[] pool (§6.1, C's gs_pref): the
// element array as a GsRref, and the freelist's header and data stack,
// which goose_spans.go keeps.
type GsPref struct {
	Hdr     *GsRhdr
	Stk     int64
	FreeHdr *GsRhdr
	FreeStk int64
}

// GsSliceFromBytes is b as a GsSlice of its bytes, without a copy. The
// slice keeps b's array alive only while the slice itself is where the
// collector sees it (not on a data stack).
func GsSliceFromBytes(b []byte) GsSlice {
	return GsSlice{Data: unsafe.Pointer(unsafe.SliceData(b)), Len: int64(len(b))}
}

// GsSliceFromString is s as a GsSlice of its bytes, without a copy. The
// bytes are s's own, which nothing may write.
func GsSliceFromString(s string) GsSlice {
	return GsSlice{Data: unsafe.Pointer(unsafe.StringData(s)), Len: int64(len(s))}
}

// GsSliceToBytes is the bytes of s as a Go slice, without a copy: valid as
// long as the memory s points at is (a data stack's only until its top is
// rewound below them).
func GsSliceToBytes(s GsSlice) []byte {
	return unsafe.Slice((*byte)(s.Data), s.Len)
}

// GsSliceToString is the bytes of s as a Go string, without a copy: valid,
// and unchanging, only as long as the bytes s points at are left alone.
func GsSliceToString(s GsSlice) string {
	return unsafe.String((*byte)(s.Data), s.Len)
}

// GsSliceFromSlice is the elements of xs as a GsSlice, without a copy. T
// must be laid out as the Goose element type is.
func GsSliceFromSlice[T any](xs []T) GsSlice {
	return GsSlice{Data: unsafe.Pointer(unsafe.SliceData(xs)), Len: int64(len(xs))}
}

// GsSliceToSlice is the elements of s as a Go slice of T, without a copy.
// T must be laid out as the Goose element type is.
func GsSliceToSlice[T any](s GsSlice) []T {
	return unsafe.Slice((*T)(s.Data), s.Len)
}

// PtrFromAddr is the pointer an address held as an integer stands for: a
// data stack address that a value on a stack keeps as a uintptr, so that
// no Go pointer hides in memory the collector does not scan. It is for
// addresses inside data stack regions (or other memory outside the Go
// heap) only: the collector cannot know a Go object an integer points at.
func PtrFromAddr(addr uintptr) unsafe.Pointer {
	return unsafe.Add(nil, addr)
}

// GsStackBlock holds the data stacks of one thread program: a Goose
// program, a worker, or one call into exported Goose code (DESIGN.md
// §2.2). The generated code takes it, with gs_sp, as its leading
// parameters, and addresses its own stacks as gs_sp + k.
//
// A block belongs to one goroutine at a time. Pointers into its stacks are
// valid until it is reset or closed, and only while the block itself stays
// reachable: a block that is dropped without Close is closed by the
// collector, which cannot see the pointers into it.
type GsStackBlock struct {
	// Stks holds the top of each data stack, indexed by gs_sp + k.
	Stks []unsafe.Pointer
	// Base holds where each stack's region begins: its top when empty.
	Base []unsafe.Pointer
	// Cap holds how many bytes each stack holds before its guard gap.
	Cap []int64
	// Frame is the top of the block's fixed-frame stack (DESIGN.md §3.6):
	// the address-taken fixed-size locals of the functions running, each
	// function's frame above its caller's, taken as it starts and given
	// back as it returns. FrameBase and FrameEnd bound its region. Outside
	// the Go heap, the locals never move, so a value's bytes may hold their
	// addresses where the collector does not look.
	Frame, FrameBase, FrameEnd unsafe.Pointer

	reserve int64    // usable bytes asked of each new region
	gap     int64    // guard bytes behind each region
	regions [][]byte // each region's mapping, guard gap included
	frame   []byte   // the fixed-frame stack's mapping
}

// GsFrameReserve is the address space a block's fixed-frame stack
// reserves, whatever its data stacks reserve: C keeps those locals on its
// native stack.
const GsFrameReserve = 1 << 30

// NewStackBlock reserves a block of numStacks data stacks, each a region
// of reserve bytes (rounded down to whole pages, and halved where the
// platform refuses that much address space) followed by a guard gap of
// gap bytes that faults on any access.
//
// The block's stacks are released by Close, or by the collector once the
// block is unreachable.
func NewStackBlock(numStacks int, reserve, gap int64) *GsStackBlock {
	if numStacks < 0 {
		GsPanic("negative data stack count")
	}
	if gap < 0 {
		GsPanic("negative data stack guard gap")
	}
	if reserve > gsReserveMax {
		GsPanic("GS_STACK_RESERVE exceeds the 2^48 limit (goose_spec.md 10.4)")
	}
	// The gap starts on a page, or the guard could not be protected.
	reserve -= reserve % gsPageSize
	if reserve <= 0 {
		GsPanic("the data stack reservation (GS_STACK_RESERVE) is less than a page")
	}
	gs := &GsStackBlock{
		Stks:    make([]unsafe.Pointer, 0, numStacks),
		Base:    make([]unsafe.Pointer, 0, numStacks),
		Cap:     make([]int64, 0, numStacks),
		reserve: reserve,
		gap:     gap,
		regions: make([][]byte, 0, numStacks),
	}
	runtime.SetFinalizer(gs, (*GsStackBlock).Close)
	gs.EnsureStacks(numStacks)
	gs.EnsureFrame()
	return gs
}

// EnsureFrame reserves the block's fixed-frame stack where it has none
// (the zero block, or a closed one), halving the reservation where the
// platform refuses that much address space. The frame starts empty.
func (gs *GsStackBlock) EnsureFrame() {
	if gs.frame != nil {
		return
	}
	n := int64(GsFrameReserve)
	for {
		mem, err := syscall.Mmap(-1, 0, int(n), syscall.PROT_READ|syscall.PROT_WRITE,
			syscall.MAP_ANON|syscall.MAP_PRIVATE|syscall.MAP_NORESERVE)
		if err == nil {
			gs.frame = mem
			gs.FrameBase = unsafe.Pointer(unsafe.SliceData(mem))
			gs.Frame = gs.FrameBase
			gs.FrameEnd = unsafe.Add(gs.FrameBase, n)
			return
		}
		if n <= gsRegionMin {
			GsPanic("cannot reserve the fixed-frame stack's address space")
		}
		n /= 2
	}
}

// GsFrameOverflow reports a function's frame that does not fit what is
// left of the fixed-frame stack, as C reports its native stack's overflow.
func GsFrameOverflow() { GsPanic("native call stack overflow") }

// EnsureStacks gives the block at least n data stacks, reserving regions
// for the ones it lacks: each new stack is empty. A block's stack count
// only ever grows, until Close.
//
// The zero GsStackBlock has no stacks, and gets regions of the default
// sizes here; nothing closes it but Close.
func (gs *GsStackBlock) EnsureStacks(n int) {
	if gs.reserve == 0 {
		gs.reserve = GsStackReserve - GsStackReserve%gsPageSize
		gs.gap = GsStackGap
	}
	for len(gs.Stks) < n {
		mem, usable := gsReserveRegion(gs.reserve, gs.gap)
		base := unsafe.Pointer(unsafe.SliceData(mem))
		gs.regions = append(gs.regions, mem)
		gs.Base = append(gs.Base, base)
		gs.Stks = append(gs.Stks, base)
		gs.Cap = append(gs.Cap, usable)
	}
}

// Reset empties every stack of the block at once, rewinding each top to
// its base. The memory stays reserved, and the pages the stacks reached
// stay with the process, ready for the next use.
func (gs *GsStackBlock) Reset() {
	for i := range gs.Stks {
		gs.Stks[i] = gs.Base[i]
	}
	gs.Frame = gs.FrameBase
}

// Close releases every region of the block, which is then empty again:
// nothing may use a pointer into its stacks any more. Closing a closed
// block does nothing.
func (gs *GsStackBlock) Close() error {
	var err error
	for i := len(gs.regions) - 1; i >= 0; i-- {
		if e := syscall.Munmap(gs.regions[i]); e != nil && err == nil {
			err = e
		}
		// No stale pointer is left to address space the Go heap may reuse.
		gs.regions[i] = nil
		gs.Stks[i] = nil
		gs.Base[i] = nil
	}
	gs.regions = gs.regions[:0]
	gs.Stks = gs.Stks[:0]
	gs.Base = gs.Base[:0]
	gs.Cap = gs.Cap[:0]
	if gs.frame != nil {
		if e := syscall.Munmap(gs.frame); e != nil && err == nil {
			err = e
		}
		gs.frame = nil
		gs.Frame, gs.FrameBase, gs.FrameEnd = nil, nil, nil
	}
	return err
}

// The blocks exported entry points share: per-P caches that the collector
// may empty, which then closes the blocks it drops.
var gsStackBlockPool sync.Pool

// AcquireStackBlock takes a block of at least numStacks empty data stacks
// from a process-wide pool, which allocates and reserves nothing once the
// pool holds a block with that many stacks. It is how an exported entry
// point runs its Goose code, giving the block back with ReleaseStackBlock
// as it returns.
func AcquireStackBlock(numStacks int) *GsStackBlock {
	if gs, _ := gsStackBlockPool.Get().(*GsStackBlock); gs != nil {
		gs.EnsureStacks(numStacks)
		return gs
	}
	return NewStackBlock(numStacks, GsStackReserve, GsStackGap)
}

// ReleaseStackBlock rewinds a block's stacks and gives it to the pool
// AcquireStackBlock takes from. Nothing may use a pointer into its stacks
// afterwards.
func ReleaseStackBlock(gs *GsStackBlock) {
	if gs == nil {
		return
	}
	gs.Reset()
	gsStackBlockPool.Put(gs)
}

// The unit memory is mapped and protected in.
var gsPageSize = int64(syscall.Getpagesize())

// Once the platform has refused a region, the usable size every region
// starts from: halved at each refusal, it only ever settles downward, as
// gs_reserve_region's gs_region_usable does. Zero before any refusal.
var gsRegionCap atomic.Int64

// gsReserveRegion maps a fresh region of usable bytes followed by a guard
// gap of gap bytes, returning the mapping and its usable size, which is
// smaller where the platform refused the size asked for.
func gsReserveRegion(usable, gap int64) ([]byte, int64) {
	return gsReserveWith(usable, gap, gsMapRegion)
}

// gsReserveWith is gsReserveRegion, mapping through mapRegion. Where the
// platform refuses a region (its address space, an address-space limit or
// strict overcommit accounting exhausted), every region from then on is
// half as large, in whole pages, down to gsRegionMin. A smaller region is
// always safe: the checks the compiler leaves out assume no more than
// GsStackReserve bytes in a stack, and the gap behind a smaller region
// stops growth that much sooner.
func gsReserveWith(usable, gap int64, mapRegion func(usable, gap int64) ([]byte, error)) ([]byte, int64) {
	if c := gsRegionCap.Load(); c > 0 && c < usable {
		usable = c
	}
	for {
		mem, err := mapRegion(usable, gap)
		if err == nil {
			return mem, usable
		}
		if usable <= gsRegionMin {
			GsPanic("cannot reserve data stack address space")
		}
		usable /= 2
		usable -= usable % gsPageSize
		for c := gsRegionCap.Load(); c == 0 || c > usable; c = gsRegionCap.Load() {
			if gsRegionCap.CompareAndSwap(c, usable) {
				break
			}
		}
	}
}

// gsMapRegion reserves usable+gap bytes of anonymous memory, with the
// trailing gap bytes PROT_NONE; pages are supplied by the OS on first
// touch. MAP_NORESERVE keeps the reservation out of the commit charge
// where the platform's overcommit accounting allows it.
func gsMapRegion(usable, gap int64) ([]byte, error) {
	mem, err := syscall.Mmap(-1, 0, int(usable+gap), syscall.PROT_READ|syscall.PROT_WRITE,
		syscall.MAP_ANON|syscall.MAP_PRIVATE|syscall.MAP_NORESERVE)
	if err != nil {
		return nil, err
	}
	if gap > 0 {
		if err := syscall.Mprotect(mem[usable:], syscall.PROT_NONE); err != nil {
			syscall.Munmap(mem)
			GsPanic("cannot protect data stack guard gap")
		}
	}
	// Most stacks hold a few bytes, for which a transparent huge page at
	// the start would commit 2 MB.
	if runtime.GOOS == "linux" {
		n := usable
		if n > 2<<20 {
			n = 2 << 20
		}
		syscall.Syscall(syscall.SYS_MADVISE, uintptr(unsafe.Pointer(unsafe.SliceData(mem))),
			uintptr(n), gsMadvNoHugePage)
	}
	return mem, nil
}
