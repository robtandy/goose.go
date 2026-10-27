package goose

import (
	"bufio"
	"errors"
	"os"
	"runtime"
	"runtime/debug"
	"strconv"
	"strings"
	"syscall"
	"testing"
	"time"
	"unsafe"
)

// raceEnabled is set by goose_race_test.go under the race detector, where
// sync.Pool deliberately drops a quarter of the items it is given.
var raceEnabled bool

// byteSink keeps a load the compiler would otherwise drop.
var byteSink byte

// mustAbort runs f, which must panic with a *GsError whose text is want.
func mustAbort(t *testing.T, want string, f func()) {
	t.Helper()
	defer func() {
		t.Helper()
		r := recover()
		e, ok := r.(*GsError)
		if !ok {
			t.Fatalf("panicked with %#v, want a *GsError %q", r, want)
		}
		if got := e.Error(); got != want {
			t.Fatalf("abort %q, want %q", got, want)
		}
	}()
	f()
}

// faultAddr runs f and reports the address of the memory fault it raised,
// if it raised one.
func faultAddr(f func()) (addr uintptr, faulted bool) {
	defer debug.SetPanicOnFault(debug.SetPanicOnFault(true))
	defer func() {
		if r := recover(); r != nil {
			e, ok := r.(interface{ Addr() uintptr })
			if !ok {
				panic(r)
			}
			addr, faulted = e.Addr(), true
		}
	}()
	f()
	return 0, false
}

// pushI64 and popI64 are a push and a pop as the generated code writes
// them for an i64 element of a resizable: the element at the stack's top,
// the top moved past it, and the header's count.
func pushI64(gs *GsStackBlock, hdr *GsRhdr, stk int64, v int64) {
	*(*int64)(gs.Stks[stk]) = v
	gs.Stks[stk] = unsafe.Add(gs.Stks[stk], 8)
	hdr.Len++
}

func popI64(gs *GsStackBlock, hdr *GsRhdr, stk int64) int64 {
	gs.Stks[stk] = unsafe.Add(gs.Stks[stk], -8)
	hdr.Len--
	return *(*int64)(gs.Stks[stk])
}

// pushThrough is a callee growing its caller's resizable through a fat
// reference, as v.push(x) on a `T[]&` parameter does.
//
//go:noinline
func pushThrough(gs *GsStackBlock, r GsRref, v int64) {
	*(*int64)(gs.Stks[r.Stk]) = v
	gs.Stks[r.Stk] = unsafe.Add(gs.Stks[r.Stk], 8)
	r.Hdr.Len++
}

func TestNewStackBlock(t *testing.T) {
	const reserve = 1 << 20
	gs := NewStackBlock(3, reserve, 64<<10)
	defer gs.Close()
	if len(gs.Stks) != 3 || len(gs.Base) != 3 || len(gs.Cap) != 3 {
		t.Fatalf("got %d tops, %d bases, %d capacities, want 3 of each", len(gs.Stks), len(gs.Base), len(gs.Cap))
	}
	for i := range gs.Stks {
		if gs.Stks[i] != gs.Base[i] {
			t.Errorf("stack %d: top %p, want its base %p", i, gs.Stks[i], gs.Base[i])
		}
		if gs.Cap[i] != reserve {
			t.Errorf("stack %d: capacity %d, want %d", i, gs.Cap[i], reserve)
		}
		if uintptr(gs.Base[i])%uintptr(gsPageSize) != 0 {
			t.Errorf("stack %d: base %p not on a page", i, gs.Base[i])
		}
		// Every byte of the region is there to be written, first to last.
		last := unsafe.Add(gs.Base[i], gs.Cap[i]-1)
		*(*byte)(gs.Base[i]), *(*byte)(last) = byte(i), byte(i+1)
		if *(*byte)(gs.Base[i]) != byte(i) || *(*byte)(last) != byte(i+1) {
			t.Errorf("stack %d: bytes written are not read back", i)
		}
		for j := 0; j < i; j++ {
			lo, hi := uintptr(gs.Base[j]), uintptr(gs.Base[j])+uintptr(gs.Cap[j])
			if b := uintptr(gs.Base[i]); b >= lo && b < hi {
				t.Errorf("stacks %d and %d overlap", j, i)
			}
		}
	}
}

func TestNewStackBlockRoundsToPages(t *testing.T) {
	gs := NewStackBlock(1, 3*gsPageSize+100, 1)
	defer gs.Close()
	if gs.Cap[0] != 3*gsPageSize {
		t.Fatalf("capacity %d, want 3 pages (%d)", gs.Cap[0], 3*gsPageSize)
	}
}

func TestNewStackBlockRejects(t *testing.T) {
	mustAbort(t, "goose runtime error: the data stack reservation (GS_STACK_RESERVE) is less than a page",
		func() { NewStackBlock(1, gsPageSize-1, GsStackGap) })
	mustAbort(t, "goose runtime error: GS_STACK_RESERVE exceeds the 2^48 limit (goose_spec.md 10.4)",
		func() { NewStackBlock(1, 1<<48+1, GsStackGap) })
	mustAbort(t, "goose runtime error: negative data stack count",
		func() { NewStackBlock(-1, GsStackReserve, GsStackGap) })
	mustAbort(t, "goose runtime error: negative data stack guard gap",
		func() { NewStackBlock(1, GsStackReserve, -1) })
}

func TestDefaultStackSizes(t *testing.T) {
	if GsStackReserve != 2048<<20 || GsStackGap != 1<<20 {
		t.Fatalf("defaults %d and %d, want GS_STACK_RESERVE's 2 GB and GS_STACK_GAP's 1 MB",
			GsStackReserve, GsStackGap)
	}
	gs := NewStackBlock(2, GsStackReserve, GsStackGap)
	defer gs.Close()
	for i, c := range gs.Cap {
		// A platform short of address space may have halved it.
		if c > GsStackReserve || c < gsRegionMin || c%gsPageSize != 0 {
			t.Errorf("stack %d: capacity %d", i, c)
		}
	}
	end := unsafe.Add(gs.Base[1], gs.Cap[1]-1)
	*(*byte)(end) = 7 // the far end of a 2 GB reservation, committed on touch
	if *(*byte)(end) != 7 {
		t.Fatal("the last byte of the region is not read back")
	}
}

// Growth past a region meets its guard gap, which faults at once.
func TestGuardGapFaults(t *testing.T) {
	const gap = 64 << 10
	gs := NewStackBlock(1, 64<<10, gap)
	defer gs.Close()
	end := unsafe.Add(gs.Base[0], gs.Cap[0])
	if _, faulted := faultAddr(func() { *(*byte)(unsafe.Add(end, -1)) = 1 }); faulted {
		t.Fatal("the last byte before the gap faults")
	}
	for _, off := range []int64{0, 1, gap / 2, gap - 1} {
		want := uintptr(end) + uintptr(off)
		got, faulted := faultAddr(func() { *(*byte)(unsafe.Add(end, off)) = 1 })
		if !faulted || got != want {
			t.Errorf("a write %d bytes into the gap: fault %v at %#x, want one at %#x", off, faulted, got, want)
		}
		got, faulted = faultAddr(func() { byteSink = *(*byte)(unsafe.Add(end, off)) })
		if !faulted || got != want {
			t.Errorf("a read %d bytes into the gap: fault %v at %#x, want one at %#x", off, faulted, got, want)
		}
	}
}

func TestEnsureStacksAndReset(t *testing.T) {
	gs := NewStackBlock(2, 1<<20, 64<<10)
	defer gs.Close()
	hdr := GsRhdr{Base: gs.Stks[1]}
	pushI64(gs, &hdr, 1, 42)
	base0, base1 := gs.Base[0], gs.Base[1]
	gs.EnsureStacks(1) // has more already
	gs.EnsureStacks(5)
	if len(gs.Stks) != 5 || gs.Base[0] != base0 || gs.Base[1] != base1 {
		t.Fatalf("EnsureStacks(5): %d stacks, or the first two moved", len(gs.Stks))
	}
	if gs.Stks[1] != unsafe.Add(base1, 8) || *(*int64)(base1) != 42 {
		t.Fatal("EnsureStacks changed a stack in use")
	}
	for i := 2; i < 5; i++ {
		if gs.Stks[i] != gs.Base[i] || gs.Cap[i] != 1<<20 {
			t.Errorf("new stack %d not empty, or of capacity %d", i, gs.Cap[i])
		}
	}
	for i := range gs.Stks {
		gs.Stks[i] = unsafe.Add(gs.Stks[i], 4096+int64(i))
	}
	gs.Reset()
	for i := range gs.Stks {
		if gs.Stks[i] != gs.Base[i] {
			t.Errorf("stack %d not rewound", i)
		}
	}
	if *(*int64)(base1) != 42 {
		t.Error("Reset changed the memory, not just the tops")
	}
}

// The fixed-frame stack: reserved with the block, written and read through
// its slots, rewound by Reset and released by Close; the zero block gets
// one from EnsureFrame.
func TestFixedFrame(t *testing.T) {
	gs := NewStackBlock(1, 1<<20, 64<<10)
	if gs.Frame == nil || gs.Frame != gs.FrameBase {
		t.Fatal("a new block's frame is not empty")
	}
	if n := int64(uintptr(gs.FrameEnd) - uintptr(gs.FrameBase)); n > GsFrameReserve || n < gsRegionMin {
		t.Fatalf("the frame reserves %d bytes", n)
	}
	fr := gs.Frame
	gs.Frame = unsafe.Add(fr, 32)
	*(*int64)(unsafe.Add(fr, 8)) = 42
	if *(*int64)(unsafe.Add(fr, 8)) != 42 {
		t.Fatal("a frame slot does not hold its value")
	}
	gs.Reset()
	if gs.Frame != gs.FrameBase {
		t.Fatal("Reset did not rewind the frame")
	}
	if err := gs.Close(); err != nil {
		t.Fatal(err)
	}
	if gs.Frame != nil || gs.FrameBase != nil || gs.FrameEnd != nil {
		t.Fatal("a closed block still has a frame")
	}
	if err := gs.Close(); err != nil {
		t.Fatalf("Close of a closed block: %v", err)
	}
	var z GsStackBlock
	z.EnsureFrame()
	defer z.Close()
	if z.Frame == nil || z.Frame != z.FrameBase {
		t.Fatal("the zero block's frame")
	}
}

func TestZeroBlockEnsureStacks(t *testing.T) {
	var gs GsStackBlock
	gs.EnsureStacks(1)
	defer gs.Close()
	if len(gs.Stks) != 1 || gs.Stks[0] == nil || gs.Cap[0] > GsStackReserve || gs.Cap[0] < gsRegionMin {
		t.Fatalf("the zero block's first stack: %d stacks, capacity %v", len(gs.Stks), gs.Cap)
	}
}

func TestClose(t *testing.T) {
	gs := NewStackBlock(3, 1<<20, 64<<10)
	if err := gs.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
	if len(gs.Stks) != 0 || len(gs.Base) != 0 || len(gs.Cap) != 0 {
		t.Fatal("a closed block still has stacks")
	}
	if err := gs.Close(); err != nil {
		t.Fatalf("Close of a closed block: %v", err)
	}
	gs.EnsureStacks(2) // a closed block can be used again
	if len(gs.Stks) != 2 || gs.Cap[1] != 1<<20 {
		t.Fatalf("EnsureStacks after Close: %d stacks", len(gs.Stks))
	}
	*(*byte)(gs.Stks[1]) = 1
	if err := gs.Close(); err != nil {
		t.Fatalf("Close: %v", err)
	}
}

// procMaps is the calling process's mappings on Linux: for each, its
// range, its permissions and its VmFlags.
type procMapping struct {
	lo, hi uintptr
	perms  string
	flags  []string
}

func procMaps(t *testing.T) []procMapping {
	t.Helper()
	if runtime.GOOS != "linux" {
		t.Skip("reads /proc/self/smaps")
	}
	f, err := os.Open("/proc/self/smaps")
	if err != nil {
		t.Skipf("no /proc/self/smaps: %v", err)
	}
	defer f.Close()
	var ms []procMapping
	sc := bufio.NewScanner(f)
	for sc.Scan() {
		fields := strings.Fields(sc.Text())
		if len(fields) == 0 {
			continue
		}
		if fields[0] == "VmFlags:" && len(ms) > 0 {
			ms[len(ms)-1].flags = fields[1:]
			continue
		}
		lohi := strings.SplitN(fields[0], "-", 2)
		if len(lohi) != 2 || len(fields) < 2 {
			continue
		}
		lo, err1 := strconv.ParseUint(lohi[0], 16, 64)
		hi, err2 := strconv.ParseUint(lohi[1], 16, 64)
		if err1 != nil || err2 != nil {
			continue
		}
		ms = append(ms, procMapping{lo: uintptr(lo), hi: uintptr(hi), perms: fields[1]})
	}
	if err := sc.Err(); err != nil {
		t.Fatal(err)
	}
	return ms
}

func mappingAt(ms []procMapping, addr uintptr) (procMapping, bool) {
	for _, m := range ms {
		if addr >= m.lo && addr < m.hi {
			return m, true
		}
	}
	return procMapping{}, false
}

func hasFlag(m procMapping, flag string) bool {
	for _, f := range m.flags {
		if f == flag {
			return true
		}
	}
	return false
}

// regionMapped says whether ms holds the region from base still: its usable
// part read/write from base, and its guard gap PROT_NONE right after, which
// no mapping made since at the same address would have.
func regionMapped(ms []procMapping, base, usable uintptr) bool {
	start, ok := mappingAt(ms, base)
	guard, gok := mappingAt(ms, base+usable)
	return ok && gok && start.lo == base && start.perms == "rw-p" &&
		guard.lo == base+usable && guard.perms == "---p"
}

// A region is a private read/write mapping kept out of transparent huge
// pages, followed by its PROT_NONE gap; Close unmaps both.
func TestRegionMappings(t *testing.T) {
	const reserve, gap = 4 << 20, 1 << 20
	gs := NewStackBlock(1, reserve, gap)
	base := uintptr(gs.Base[0])
	ms := procMaps(t)
	start, ok := mappingAt(ms, base)
	if !ok || start.lo != base || start.perms != "rw-p" {
		t.Fatalf("the region's start is mapped as %+v, want rw-p from %#x", start, base)
	}
	// Only a kernel with transparent huge pages takes MADV_NOHUGEPAGE (any
	// other refuses it, which gsMapRegion lets be) and shows it as "nh".
	if _, err := os.Stat("/sys/kernel/mm/transparent_hugepage"); err == nil && start.flags != nil {
		if !hasFlag(start, "nh") {
			t.Errorf("the region's first 2 MB are not MADV_NOHUGEPAGE: VmFlags %v", start.flags)
		}
	} else {
		t.Log("no transparent huge pages here to keep the region out of")
	}
	if rest, ok := mappingAt(ms, base+reserve-1); !ok || rest.perms != "rw-p" {
		t.Errorf("the region's end is mapped as %+v, want rw-p", rest)
	}
	guard, ok := mappingAt(ms, base+reserve)
	if !ok || guard.lo != base+reserve || guard.hi < base+reserve+gap || guard.perms != "---p" {
		t.Errorf("the guard gap is mapped as %+v, want ---p over [%#x, %#x)", guard, base+reserve, base+reserve+gap)
	}
	if err := gs.Close(); err != nil {
		t.Fatal(err)
	}
	if regionMapped(procMaps(t), base, reserve) {
		t.Error("the region and its guard gap are still mapped after Close")
	}
}

// mappedBlockBase makes a block, checks its region is mapped while the
// block is still reachable, and returns where the region starts. Once it
// returns nothing references the block, which the collector may then
// close at any time.
//
//go:noinline
func mappedBlockBase(t *testing.T) uintptr {
	gs := NewStackBlock(1, 1<<20, 64<<10)
	*(*byte)(gs.Base[0]) = 1
	base := uintptr(gs.Base[0])
	if !regionMapped(procMaps(t), base, 1<<20) {
		t.Fatal("the region is not mapped")
	}
	// Not closed before the check, whenever a collection comes.
	runtime.KeepAlive(gs)
	return base
}

// A block nothing references any more is closed by the collector, as the
// pool drops blocks at collections.
func TestUnreachableBlockIsClosed(t *testing.T) {
	base := mappedBlockBase(t)
	deadline := time.Now().Add(10 * time.Second)
	for {
		runtime.GC()
		if !regionMapped(procMaps(t), base, 1<<20) {
			return
		}
		if time.Now().After(deadline) {
			t.Fatal("an unreachable block's region is still mapped")
		}
		time.Sleep(time.Millisecond)
	}
}

// Where the platform refuses a region, each try asks for half as much, in
// whole pages, down to 1 MB, and every region after starts from what last
// worked.
func TestReserveFallbackHalves(t *testing.T) {
	saved := gsRegionCap.Load()
	defer gsRegionCap.Store(saved)
	gsRegionCap.Store(0)
	var tried []int64
	limit := int64(64 << 20)
	refuse := func(usable, gap int64) ([]byte, error) {
		tried = append(tried, usable)
		if usable > limit {
			return nil, syscall.ENOMEM
		}
		return gsMapRegion(usable, gap)
	}
	mem, usable := gsReserveWith(GsStackReserve, GsStackGap, refuse)
	defer syscall.Munmap(mem)
	want := []int64{2 << 30, 1 << 30, 512 << 20, 256 << 20, 128 << 20, 64 << 20}
	if len(tried) != len(want) {
		t.Fatalf("tried %v, want %v", tried, want)
	}
	for i := range want {
		if tried[i] != want[i] {
			t.Fatalf("tried %v, want %v", tried, want)
		}
	}
	if usable != limit || int64(len(mem)) != limit+GsStackGap || gsRegionCap.Load() != limit {
		t.Fatalf("usable %d of %d mapped, cap %d; want %d", usable, len(mem), gsRegionCap.Load(), limit)
	}
	*(*byte)(unsafe.Add(unsafe.Pointer(&mem[0]), usable-1)) = 1
	if _, faulted := faultAddr(func() { mem[usable] = 1 }); !faulted {
		t.Error("the halved region has no guard gap")
	}

	// The next region starts where the last one settled, and a smaller
	// reservation is not raised.
	tried = nil
	mem2, usable2 := gsReserveWith(GsStackReserve, GsStackGap, refuse)
	syscall.Munmap(mem2)
	if usable2 != limit || len(tried) != 1 {
		t.Fatalf("after a refusal: tried %v, got %d", tried, usable2)
	}
	tried = nil
	mem3, usable3 := gsReserveWith(2<<20, GsStackGap, refuse)
	syscall.Munmap(mem3)
	if usable3 != 2<<20 || len(tried) != 1 {
		t.Fatalf("a smaller reservation: tried %v, got %d", tried, usable3)
	}

	// A refusal at 1 MB is fatal.
	limit = 0
	tried = nil
	mustAbort(t, "goose runtime error: cannot reserve data stack address space", func() {
		gsReserveWith(GsStackReserve, GsStackGap, refuse)
	})
	if last := tried[len(tried)-1]; last != gsRegionMin {
		t.Fatalf("tried %v, want down to %d", tried, gsRegionMin)
	}
	if gsRegionCap.Load() != gsRegionMin {
		t.Fatalf("cap %d after giving up", gsRegionCap.Load())
	}
}

// The largest reservation there is, which no 48-bit address space holds
// whole, is halved by the platform's refusals until it fits.
func TestReserveLargest(t *testing.T) {
	saved := gsRegionCap.Load()
	defer gsRegionCap.Store(saved)
	gs := NewStackBlock(1, 1<<48, GsStackGap)
	defer gs.Close()
	c := gs.Cap[0]
	if c > 1<<48 || c < gsRegionMin || c%gsPageSize != 0 {
		t.Fatalf("capacity %d", c)
	}
	if c < 1<<48 && (gsRegionCap.Load() == 0 || gsRegionCap.Load() > c) {
		t.Fatalf("capacity %d after refusals, but the cap is %d", c, gsRegionCap.Load())
	}
	last := unsafe.Add(gs.Base[0], c-1)
	*(*byte)(last) = 3
	if *(*byte)(last) != 3 {
		t.Fatal("the last byte of the region is not read back")
	}
	t.Logf("a 2^48-byte reservation got %d bytes", c)
}

func TestAcquireRelease(t *testing.T) {
	gs := AcquireStackBlock(3)
	if len(gs.Stks) < 3 {
		t.Fatalf("AcquireStackBlock(3): %d stacks", len(gs.Stks))
	}
	for i := range gs.Stks {
		if gs.Stks[i] != gs.Base[i] {
			t.Fatalf("stack %d of a fresh block is not empty", i)
		}
	}
	hdr := GsRhdr{Base: gs.Stks[2]}
	pushI64(gs, &hdr, 2, 9)
	ReleaseStackBlock(gs)
	if gs.Stks[2] != gs.Base[2] {
		t.Fatal("ReleaseStackBlock did not rewind the block")
	}
	gs = AcquireStackBlock(6) // the same block or another, with six stacks
	if len(gs.Stks) < 6 {
		t.Fatalf("AcquireStackBlock(6): %d stacks", len(gs.Stks))
	}
	for i := range gs.Stks {
		if gs.Stks[i] != gs.Base[i] {
			t.Fatalf("stack %d of a pooled block is not empty", i)
		}
	}
	ReleaseStackBlock(gs)
	ReleaseStackBlock(nil)
}

// Blocks are used by one goroutine at a time, as many at once as there
// are callers.
func TestAcquireConcurrent(t *testing.T) {
	done := make(chan error)
	for g := 0; g < 8; g++ {
		go func(g int) {
			var err error
			for i := 0; i < 200 && err == nil; i++ {
				gs := AcquireStackBlock(2)
				hdr := GsRhdr{Base: gs.Stks[1]}
				for k := int64(0); k < 100; k++ {
					pushI64(gs, &hdr, 1, int64(g)<<32|k)
				}
				for k := int64(99); k >= 0; k-- {
					if v := popI64(gs, &hdr, 1); v != int64(g)<<32|k {
						err = errors.New("a block was shared between goroutines")
						break
					}
				}
				ReleaseStackBlock(gs)
			}
			done <- err
		}(g)
	}
	for g := 0; g < 8; g++ {
		if err := <-done; err != nil {
			t.Error(err)
		}
	}
}

func TestPushThroughReference(t *testing.T) {
	gs := NewStackBlock(2, 1<<20, 64<<10)
	defer gs.Close()
	hdr := GsRhdr{Base: gs.Stks[1]}
	r := GsRref{Hdr: &hdr, Stk: 1}
	for v := int64(0); v < 10; v++ {
		pushThrough(gs, r, v*v)
	}
	xs := GsSliceToSlice[int64](GsSlice{Data: hdr.Base, Len: hdr.Len})
	if len(xs) != 10 || xs[9] != 81 || gs.Stks[1] != unsafe.Add(hdr.Base, 80) {
		t.Fatalf("pushed %v, top %p from %p", xs, gs.Stks[1], hdr.Base)
	}
}

func TestSliceConversions(t *testing.T) {
	b := []byte("hello, goose")
	s := GsSliceFromBytes(b)
	if s.Data != unsafe.Pointer(&b[0]) || s.Len != int64(len(b)) {
		t.Fatalf("GsSliceFromBytes copied or miscounted: %+v", s)
	}
	if back := GsSliceToBytes(s); &back[0] != &b[0] || len(back) != len(b) {
		t.Fatal("GsSliceToBytes copied")
	}
	if GsSliceToString(s) != "hello, goose" {
		t.Fatal("GsSliceToString")
	}
	str := "a string literal"
	ss := GsSliceFromString(str)
	if ss.Data != unsafe.Pointer(unsafe.StringData(str)) || GsSliceToString(ss) != str {
		t.Fatalf("GsSliceFromString: %+v", ss)
	}
	var empty GsSlice
	if GsSliceToBytes(empty) != nil || GsSliceToString(empty) != "" || GsSliceFromBytes(nil) != empty ||
		GsSliceFromString("") != (GsSlice{Data: unsafe.Pointer(unsafe.StringData("")), Len: 0}) {
		t.Fatal("an empty slice does not convert to an empty one")
	}
	xs := []int32{3, 1, 4, 1, 5}
	sx := GsSliceFromSlice(xs)
	if sx.Data != unsafe.Pointer(&xs[0]) || sx.Len != 5 {
		t.Fatalf("GsSliceFromSlice: %+v", sx)
	}
	if ys := GsSliceToSlice[int32](sx); &ys[0] != &xs[0] || len(ys) != 5 {
		t.Fatal("GsSliceToSlice copied")
	}

	// A slice of a data stack's bytes, as Go sees it.
	gs := NewStackBlock(1, 1<<20, 64<<10)
	defer gs.Close()
	GsMemCpy(gs.Stks[0], unsafe.Pointer(unsafe.StringData(str)), int64(len(str)))
	onStack := GsSlice{Data: gs.Stks[0], Len: int64(len(str))}
	gs.Stks[0] = unsafe.Add(gs.Stks[0], len(str))
	if GsSliceToString(onStack) != str {
		t.Fatal("a slice of a data stack does not read back")
	}
}

func TestPtrFromAddr(t *testing.T) {
	gs := NewStackBlock(1, 1<<20, 64<<10)
	defer gs.Close()
	p := unsafe.Add(gs.Base[0], 100)
	// An address kept as an integer on a stack, as values there keep them.
	*(*uintptr)(gs.Base[0]) = uintptr(p)
	q := PtrFromAddr(*(*uintptr)(gs.Base[0]))
	if q != p {
		t.Fatalf("PtrFromAddr(%#x) = %p", uintptr(p), q)
	}
	*(*int64)(q) = 5
	if *(*int64)(p) != 5 {
		t.Fatal("the pointer does not reach the memory")
	}
}

// ---------------------------------------------------------------------------
// Zero allocations: the operations an exported entry point and the code it
// runs perform allocate nothing from the Go heap once warm.

func TestZeroAllocAcquireRelease(t *testing.T) {
	if raceEnabled {
		t.Skip("under the race detector sync.Pool drops a quarter of what it is given")
	}
	ReleaseStackBlock(AcquireStackBlock(4))
	if n := testing.AllocsPerRun(1000, func() {
		gs := AcquireStackBlock(4)
		*(*int64)(gs.Stks[3]) = 1
		ReleaseStackBlock(gs)
	}); n != 0 {
		t.Fatalf("AcquireStackBlock and ReleaseStackBlock: %v allocations a run, want 0", n)
	}
}

func TestZeroAllocPushPop(t *testing.T) {
	gs := NewStackBlock(2, 1<<20, 64<<10)
	defer gs.Close()
	const stk = 1
	if n := testing.AllocsPerRun(100, func() {
		hdr := GsRhdr{Base: gs.Stks[stk]}
		for v := int64(0); v < 1000; v++ {
			pushI64(gs, &hdr, stk, v)
		}
		pushThrough(gs, GsRref{Hdr: &hdr, Stk: stk}, 1000)
		for v := int64(1000); v >= 0; v-- {
			if popI64(gs, &hdr, stk) != v {
				panic("popped another element than pushed")
			}
		}
		gs.Stks[stk] = hdr.Base // the scope's exit restores the watermark
	}); n != 0 {
		t.Fatalf("pushes and pops: %v allocations a run, want 0", n)
	}
	if gs.Stks[stk] != gs.Base[stk] {
		t.Fatal("the stack is not back at its base")
	}
}

func TestZeroAllocSlicePool(t *testing.T) {
	gs := NewStackBlock(2, 64<<20, 64<<10)
	defer gs.Close()
	p := newTestPool(gs, 0, 1, 8)
	var runs [200][2]int64 // (index, length)
	if n := testing.AllocsPerRun(20, func() {
		// Runs enough for free spans in several leaves under a root...
		for k := range runs {
			cnt := int64(1 + k%7)
			runs[k] = [2]int64{p.alloc(cnt), cnt}
		}
		// ...made by freeing every other run, which splits nodes...
		for k := 0; k < len(runs); k += 2 {
			p.free(runs[k][0], runs[k][1])
		}
		// ...placing and moving runs among them...
		for k := 1; k < len(runs); k += 4 {
			runs[k][0] = p.realloc(runs[k][0], runs[k][1], runs[k][1]+3)
			runs[k][1] += 3
		}
		for k := 0; k < len(runs); k += 2 {
			runs[k] = [2]int64{p.alloc(2), 2}
		}
		// ...and freeing it all, which merges the spans and empties nodes.
		for k := range runs {
			p.free(runs[k][0], runs[k][1])
		}
	}); n != 0 {
		t.Fatalf("slice pool operations: %v allocations a run, want 0", n)
	}
	idx, cnt := checkSpans(t, p)
	if len(idx) != 1 || idx[0] != 0 || cnt[0] != p.elems.Len {
		t.Fatalf("after freeing everything the spans are %v %v, want one span of %d", idx, cnt, p.elems.Len)
	}
}

func TestZeroAllocReset(t *testing.T) {
	gs := NewStackBlock(8, 1<<20, 64<<10)
	defer gs.Close()
	if n := testing.AllocsPerRun(1000, func() {
		for i := range gs.Stks {
			*(*int64)(gs.Stks[i]) = int64(i)
			gs.Stks[i] = unsafe.Add(gs.Stks[i], 64*(i+1))
		}
		gs.Reset()
	}); n != 0 {
		t.Fatalf("Reset: %v allocations a run, want 0", n)
	}
	for i := range gs.Stks {
		if gs.Stks[i] != gs.Base[i] {
			t.Fatalf("stack %d not rewound", i)
		}
	}
}

func BenchmarkAcquireRelease(b *testing.B) {
	b.ReportAllocs()
	ReleaseStackBlock(AcquireStackBlock(4))
	for i := 0; i < b.N; i++ {
		ReleaseStackBlock(AcquireStackBlock(4))
	}
}

func BenchmarkPushPop(b *testing.B) {
	gs := NewStackBlock(1, 1<<20, 64<<10)
	defer gs.Close()
	hdr := GsRhdr{Base: gs.Stks[0]}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		pushI64(gs, &hdr, 0, int64(i))
		if popI64(gs, &hdr, 0) != int64(i) {
			b.Fatal("popped another element than pushed")
		}
	}
}

func BenchmarkSlicePoolAllocFree(b *testing.B) {
	gs := NewStackBlock(2, 64<<20, 64<<10)
	defer gs.Close()
	p := newTestPool(gs, 0, 1, 8)
	// A freelist of a few hundred spans, which runs come and go among.
	var keep []int64
	for k := 0; k < 600; k++ {
		i := p.alloc(int64(1 + k%5))
		if k%2 == 0 {
			p.free(i, int64(1+k%5))
		} else {
			keep = append(keep, i)
		}
	}
	b.ReportAllocs()
	b.ResetTimer()
	for i := 0; i < b.N; i++ {
		cnt := int64(1 + i%4)
		at := p.alloc(cnt)
		p.free(at, cnt)
	}
	_ = keep
}

func BenchmarkReset(b *testing.B) {
	gs := NewStackBlock(8, 1<<20, 64<<10)
	defer gs.Close()
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		gs.Stks[i&7] = unsafe.Add(gs.Base[i&7], 64)
		gs.Reset()
	}
}
