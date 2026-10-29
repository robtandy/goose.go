// Package goose is the runtime of the Goose compiler's Go backend (goose
// --target=go): what src/runtime/ is to the C backend, in pure Go and
// without cgo.
//
// Goose places every dynamic value on data stacks: large regions of virtual
// memory reserved up front, which a value grows by bumping the stack's top
// and a scope frees by rewinding it (docs/implementation.md §6.2, §7). Here
// each region is an anonymous syscall.Mmap mapping outside the Go heap,
// with a PROT_NONE guard gap behind it, so the garbage collector never
// scans or frees what lives on a stack, and growth past a region faults at
// once. A thread program's stacks make up a GsStackBlock, which the
// generated code takes as a hidden parameter beside gs_sp, and a sync.Pool
// of blocks gives the entry points of export fns their stacks without
// allocating.
//
// This package is the one copy of the runtime there is. The compiler embeds
// the source of its files at its own build (cmake/go_runtime.cmake), all of
// them but goose_package.go and goose_extra.go, and a file it writes holds
// the part of it that the program uses (--go-runtime=embed, the default),
// or all of it (--go-runtime=full); with --go-runtime=import the file
// imports this package instead. So every name the generated code calls is
// exported here, and those that a file holding the runtime keeps to itself
// are marked //goose:internal, which the compiler unexports as it embeds
// them: GsEscape is gsEscape there.
//
// Each file holds a part: this one the blocks of data stacks and what a
// program is to the runtime (GsProgram), goose_bytes.go the aborts and the
// operations on values, goose_format.go the text forms print uses,
// goose_spans.go the freelists of reusable[] slice pools, goose_os.go the OS
// layer behind stdlib/os.goose, and goose_threads.go workers and queues.
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
	"math"
	"os"
	"runtime"
	"strconv"
	"sync"
	"sync/atomic"
	"syscall"
	"unsafe"
)

// ---------------------------------------------------------------------------
// Blocks of data stacks (docs/implementation.md §6.2, DESIGN.md §2.2).

// GsStackBlock holds the data stacks of one thread program: each stack is a
// region of reserved address space, Stks[gs_sp+k] its current top. A
// function that uses data stacks, or calls one that does, takes the block
// and its first stack index as its leading parameters (gs, gs_sp).
type GsStackBlock struct {
	Stks []unsafe.Pointer // Current stack tops, indexed by gs_sp + k.
	Base []unsafe.Pointer // Where each stack's region starts.
	Cap  []int64          // Each region's usable bytes, its guard gap not included.
	// The fixed-frame stack: the address-taken fixed-size locals of the
	// functions running, each function's frame above its caller's, taken as
	// it starts and given back at its every return, as a native stack's
	// frame is. Outside the Go heap, they never move, so the bytes of a
	// value may hold their addresses where the collector does not look.
	Frame     unsafe.Pointer
	FrameBase unsafe.Pointer
	FrameEnd  unsafe.Pointer
	// The program instance the block runs (§11.2), in a program with
	// workers: its globals, which the generated code lays out (gsGlobals).
	// Main's is a static one; a worker's, and an export's caller's, is in
	// memory of the block's own (inst), outside the Go heap, as the data
	// stacks are. nil where the block has none yet.
	Gl      unsafe.Pointer
	inst    []byte
	worker  int64 // The worker running on the block, its id plus one; 0 for none.
	frame   []byte
	regions [][]byte
	reserve int64 // The usable bytes asked of each new region, in whole pages.
	gap     int64 // The guard gap behind each region.
}

// GsProgram is a program's place in the runtime: the configuration it was
// compiled with (--stack-reserve, GS_STACK_RESERVE, GS_STACK_GAP,
// --frame-reserve), which the blocks it makes have; the pool of blocks the
// entry points of its export fns run on; its address space budget and the
// cap that puts on its workers (gs_rt_start); and whether it is a library,
// which never exits the process: its host's. A file the compiler writes
// declares its program's, gsProg, so that the programs of a process each
// have their own, however many packages share this one.
//
//goose:internal
type GsProgram struct {
	StackReserve int64 // The usable bytes of each data stack's region.
	StackGap     int64 // The guard gap behind each region.
	FrameReserve int64 // The address space of each block's fixed-frame stack.
	Library      bool  // A library: nothing it runs exits the process.

	pool          sync.Pool // The blocks AcquireStackBlock hands out.
	budget        uint64    // GS_STACK_BUDGET, the address space for regions.
	mainRegions   int64     // The most regions the main program holds.
	workerRegions int64     // The most regions any one worker holds.
	threadCap     int64     // The workers the budget holds beside main; 0 for no cap.
}

// NewStackBlock reserves numStacks data stacks of reserve usable bytes each
// (in whole pages, and halved where the platform refuses that much address
// space), with gap bytes of PROT_NONE guard behind each one, and a
// fixed-frame stack of the sizes the program was compiled with. The block
// lasts until Close releases it.
func NewStackBlock(numStacks int, reserve, gap int64) *GsStackBlock {
	return gsProg.NewStackBlock(numStacks, reserve, gap)
}

// AcquireStackBlock is a block of at least numStacks empty data stacks, of
// the sizes the program was compiled with, from a pool of them: once the
// pool holds one, it allocates and reserves nothing. Hand it back with
// ReleaseStackBlock; until then it is the caller's alone.
func AcquireStackBlock(numStacks int) *GsStackBlock {
	return gsProg.AcquireStackBlock(numStacks)
}

// ReleaseStackBlock rewinds gs, a block AcquireStackBlock gave, and gives it
// back to the pool: nothing may use what its stacks hold afterwards.
func ReleaseStackBlock(gs *GsStackBlock) { gsProg.ReleaseStackBlock(gs) }

// NewStackBlock is a block of the program's (the package NewStackBlock's).
// It is registered, so that a data stack of it that overflows is reported
// as the thread program's whose it is (gsOverflow, gsFault).
func (p *GsProgram) NewStackBlock(numStacks int, reserve, gap int64) *GsStackBlock {
	gsBlocksMu.Lock()
	defer gsBlocksMu.Unlock()
	gs := gsNewBlock(numStacks, reserve, gap, p.FrameReserve)
	gsBlocks = append(gsBlocks, gs)
	return gs
}

// AcquireStackBlock is a block from the program's pool (the package
// AcquireStackBlock's), which the collector may drop from the pool: one it
// drops is closed.
func (p *GsProgram) AcquireStackBlock(numStacks int) *GsStackBlock {
	if gs, _ := p.pool.Get().(*GsStackBlock); gs != nil {
		gs.EnsureStacks(numStacks)
		return gs
	}
	gsBlocksMu.Lock()
	defer gsBlocksMu.Unlock()
	gs := gsNewBlock(numStacks, p.StackReserve, p.StackGap, p.FrameReserve)
	runtime.SetFinalizer(gs, (*GsStackBlock).Close)
	return gs
}

// ReleaseStackBlock rewinds gs and gives it back to the program's pool.
func (p *GsProgram) ReleaseStackBlock(gs *GsStackBlock) {
	if gs == nil {
		return
	}
	gs.Reset()
	p.pool.Put(gs)
}

// gsNewBlock makes a block, with gsBlocksMu held: numStacks regions, and a
// fixed-frame stack of frame bytes.
func gsNewBlock(numStacks int, reserve, gap, frame int64) *GsStackBlock {
	page := int64(syscall.Getpagesize())
	switch {
	case numStacks < 0:
		GsPanic("negative data stack count")
	case gap < 0:
		GsPanic("negative data stack guard gap")
	case reserve > gsReserveMax:
		GsPanic("GS_STACK_RESERVE exceeds the 2^48 limit (goose_spec.md 10.4)")
	case reserve < page:
		GsPanic("the data stack reservation (GS_STACK_RESERVE) is less than a page")
	}
	gs := &GsStackBlock{reserve: reserve - reserve%page, gap: gap}
	gs.ensureStacks(numStacks)
	gs.reserveFrame(frame)
	return gs
}

// reserveFrame maps the fixed-frame stack, n bytes of address space,
// halving the reservation where the platform refuses one.
func (gs *GsStackBlock) reserveFrame(n int64) {
	for {
		r, err := gsMap(n)
		if err == nil {
			gs.frame = r
			gs.FrameBase = unsafe.Pointer(unsafe.SliceData(r))
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

// GsFrameOverflow is a function's frame that does not fit what is left of
// the fixed-frame stack, which C reports as its native stack's overflow.
func GsFrameOverflow() { GsPanic("native call stack overflow") }

// EnsureStacks reserves stacks until the block has at least n.
func (gs *GsStackBlock) EnsureStacks(n int) {
	if len(gs.Stks) >= n {
		return
	}
	gsBlocksMu.Lock()
	defer gsBlocksMu.Unlock()
	gs.ensureStacks(n)
}

func (gs *GsStackBlock) ensureStacks(n int) {
	for len(gs.Stks) < n {
		r, usable := gsReserveRegion(gs.reserve, gs.gap)
		p := unsafe.Pointer(unsafe.SliceData(r))
		gs.regions = append(gs.regions, r)
		gs.Stks = append(gs.Stks, p)
		gs.Base = append(gs.Base, p)
		gs.Cap = append(gs.Cap, usable)
		gsRegions[uintptr(p)+uintptr(usable)] = gsRegion{uintptr(p), int64(len(r)), usable, gs.reserve}
	}
}

// Reset rewinds every stack of the block to empty, its fixed-frame stack's
// too. A program instance of the block's own (Gl) goes with them: the next
// entry point to run on the block copies the program's globals in again.
func (gs *GsStackBlock) Reset() {
	copy(gs.Stks, gs.Base)
	gs.Frame = gs.FrameBase
	if gs.inst != nil {
		gs.Gl = nil
	}
}

// Close releases the block's regions: nothing may use what its stacks held
// afterwards. Closing a closed block does nothing, and a closed block can
// be given stacks again (EnsureStacks).
func (gs *GsStackBlock) Close() error {
	gsBlocksMu.Lock()
	for i, b := range gsBlocks {
		if b == gs {
			gsBlocks = append(gsBlocks[:i], gsBlocks[i+1:]...)
			break
		}
	}
	for k := range gs.regions {
		delete(gsRegions, uintptr(gs.Base[k])+uintptr(gs.Cap[k]))
	}
	gsBlocksMu.Unlock()
	var first error
	for _, r := range gs.regions {
		if err := syscall.Munmap(r); err != nil && first == nil {
			first = err
		}
	}
	if gs.frame != nil {
		if err := syscall.Munmap(gs.frame); err != nil && first == nil {
			first = err
		}
	}
	if gs.inst != nil {
		if err := syscall.Munmap(gs.inst); err != nil && first == nil {
			first = err
		}
		gs.Gl = nil
	}
	gs.regions, gs.Stks, gs.Base, gs.Cap = nil, nil, nil, nil
	gs.frame, gs.Frame, gs.FrameBase, gs.FrameEnd = nil, nil, nil, nil
	gs.inst = nil
	return first
}

// gsMapPrivate maps n bytes of zeroed memory outside the Go heap, at least
// a page of them, for a program instance's globals and a worker's spawn
// image: what points into them never moves, and the collector frees none
// of it.
func gsMapPrivate(n int64) []byte {
	page := int64(syscall.Getpagesize())
	n = (n + page - 1) / page * page
	if n == 0 {
		n = page
	}
	m, err := gsMap(n)
	if err != nil {
		GsPanic("out of memory copying globals")
	}
	return m
}

// NewInstance gives the block a program instance of its own, size zeroed
// bytes at Gl, where it has none (an export's entry point fills it); the
// memory is the block's until Close.
func (gs *GsStackBlock) NewInstance(size uintptr) {
	if gs.Gl != nil {
		return
	}
	if gs.inst == nil || uintptr(len(gs.inst)) < size {
		if gs.inst != nil {
			_ = syscall.Munmap(gs.inst)
		}
		gs.inst = gsMapPrivate(int64(size))
	} else {
		clear(gs.inst[:size])
	}
	gs.Gl = unsafe.Pointer(unsafe.SliceData(gs.inst))
}

// gsWho names the thread program a block runs, as the runtime's reports
// do: main's, or a worker's by its id.
func (gs *GsStackBlock) gsWho() string {
	if gs == nil || gs.worker == 0 {
		return "main"
	}
	return "worker " + strconv.FormatInt(gs.worker-1, 10)
}

// End is where stack k's region ends, which a line print renders at the
// stack's top does not write past (GsPutStr).
func (gs *GsStackBlock) End(k int64) unsafe.Pointer { return unsafe.Add(gs.Base[k], gs.Cap[k]) }

// Owns says whether p points into memory of the block's own: a data stack's
// region, its fixed-frame stack, its program instance.
func (gs *GsStackBlock) Owns(p unsafe.Pointer) bool {
	in := func(r []byte) bool {
		return r != nil && uintptr(p)-uintptr(unsafe.Pointer(unsafe.SliceData(r))) < uintptr(len(r))
	}
	for _, r := range gs.regions {
		if in(r) {
			return true
		}
	}
	return in(gs.frame) || in(gs.inst)
}

// ---------------------------------------------------------------------------
// Regions: address space mapped outside the Go heap.

// How many times the runtime has mapped memory (gsMap, gsMapRegion): a
// library's package maps none until the first call of an entry point
// starts it (issue #18).
var gsMaps atomic.Int64

// gsMap maps n bytes of zeroed memory outside the Go heap, for a fixed-frame
// stack or a program instance: address space only, which the OS commits
// page by page as it is touched.
func gsMap(n int64) ([]byte, error) {
	gsMaps.Add(1)
	return syscall.Mmap(-1, 0, int(n), syscall.PROT_READ|syscall.PROT_WRITE,
		syscall.MAP_ANON|syscall.MAP_PRIVATE|syscall.MAP_NORESERVE)
}

const (
	// The smallest a region gets before a refusal is fatal (GS_REGION_MIN).
	gsRegionMin = 1 << 20
	// The largest reservation there is (goose_spec.md §10.4): what lets the
	// compiler take every size, count and index to fit in 48 bits.
	gsReserveMax = 1 << 48
	// MADV_NOHUGEPAGE on every Linux port Go has; syscall defines the name
	// only on Linux, and this file builds for macOS too.
	gsMadvNoHugePage = 15
)

// Once the platform has refused a region, the usable size every region
// starts from: halved at each refusal, it only ever settles downward, as
// gs_reserve_region's gs_region_usable does. Zero before any refusal.
var gsRegionCap atomic.Int64

// gsUsable is the usable size a new region of reserve bytes gets: reserve
// in whole pages, or less where the platform has refused regions.
func gsUsable(reserve int64) int64 {
	usable := reserve - reserve%int64(syscall.Getpagesize())
	if c := gsRegionCap.Load(); c > 0 && c < usable {
		usable = c
	}
	return usable
}

// gsReserveRegion maps one region: address space only, committed by the OS
// page by page as the stack grows into it, with the guard gap at its end.
func gsReserveRegion(reserve, gap int64) ([]byte, int64) {
	return gsReserveWith(reserve, gap, gsMapRegion)
}

// gsReserveWith is gsReserveRegion, mapping through mapRegion. Where the
// platform refuses a region (its address space, an address-space limit or
// strict overcommit accounting exhausted), every region from then on is
// half as large, in whole pages, down to gsRegionMin. A smaller region is
// always safe: the checks the compiler leaves out assume no more than
// GS_STACK_RESERVE bytes in a stack, and the gap behind a smaller region
// stops growth that much sooner.
func gsReserveWith(reserve, gap int64, mapRegion func(usable, gap int64) ([]byte, error)) ([]byte, int64) {
	page := int64(syscall.Getpagesize())
	usable := gsUsable(reserve)
	for {
		r, err := mapRegion(usable, gap)
		if err == nil {
			return r, usable
		}
		if usable <= gsRegionMin {
			GsPanic("cannot reserve data stack address space")
		}
		usable /= 2
		usable -= usable % page
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
	r, err := gsMap(usable + gap)
	if err != nil {
		return nil, err
	}
	if gap > 0 {
		if err := syscall.Mprotect(r[usable:], syscall.PROT_NONE); err != nil {
			_ = syscall.Munmap(r)
			GsPanic("cannot protect data stack guard gap")
		}
	}
	// Most stacks hold a few bytes, for which a transparent huge page at
	// the start would commit 2 MB, as the C runtime's regions keep them out.
	if runtime.GOOS == "linux" {
		_ = syscall.Madvise(r[:min(usable, 2<<20)], gsMadvNoHugePage)
	}
	return r, nil
}

// ---------------------------------------------------------------------------
// Overflow: a data stack that grows past its region.

// The blocks NewStackBlock has made, which a report of an overflow names
// the thread program of, and every region there is, by where it ends, its
// pooled blocks' included, which the collector may close: its address, the
// bytes mapped, its usable ones and the reservation asked for. Exported
// entry points make blocks on whatever goroutines call them, so what
// reserves a region, or reads these, holds gsBlocksMu.
var (
	gsBlocksMu sync.Mutex
	gsBlocks   []*GsStackBlock
	gsRegions  = map[uintptr]gsRegion{}
)

type gsRegion struct {
	base            uintptr
	size            int64
	usable, reserve int64
}

// gsRegionWho names the thread program whose region ends at end, as a
// report of its overflow does: that of the block NewStackBlock made it for,
// or main's.
func gsRegionWho(end uintptr) string {
	for _, b := range gsBlocks {
		for k := range b.Cap {
			if uintptr(b.End(int64(k))) == end {
				return b.gsWho()
			}
		}
	}
	return "main"
}

// gsOverflow is the C runtime's report of a data stack that grows past its
// region (gs_overflow_text), raised before a write would cross end, where
// the region ends, naming the thread program whose stacks they are.
func gsOverflow(end unsafe.Pointer) {
	gsBlocksMu.Lock()
	r := gsRegions[uintptr(end)]
	who := gsRegionWho(uintptr(end))
	gsBlocksMu.Unlock()
	panic(&GsError{Msg: gsOverflowText(r.usable, r.reserve, who)})
}

func gsOverflowText(usable, reserve int64, who string) string {
	reserve -= reserve % int64(syscall.Getpagesize())
	msg := "data stack overflow: a value on " + who + "'s data stacks grew past the " + gsSize(usable) +
		" reserved for it"
	if usable < reserve {
		msg += " (halved from " + gsSize(reserve) + " when the platform ran out of address space)"
	}
	return msg + "; --stack-reserve or -DGS_STACK_RESERVE raises the reservation, up to 256 TB"
}

// gsFault is the error of a fault at addr, where it lies in the guard gap
// of a region: a data stack grown past its region with no check on the
// way, as a push grows it (gs_fault_handler). nil for any other address.
func gsFault(addr uintptr) *GsError {
	gsBlocksMu.Lock()
	defer gsBlocksMu.Unlock()
	for end, r := range gsRegions {
		if addr-r.base < uintptr(r.size) {
			return &GsError{Msg: gsOverflowText(r.usable, r.reserve, gsRegionWho(end))}
		}
	}
	return nil
}

// GsAbortOf is what a goroutine running Goose code recovered, r, as the
// runtime reports it: a fault in a guard gap, which the goroutine's panic
// on fault (debug.SetPanicOnFault) made a panic, as the *GsError of the
// overflow (gsFault), and anything else as it is.
//
//goose:internal
func GsAbortOf(r any) any {
	if f, ok := r.(interface{ Addr() uintptr }); ok {
		if e := gsFault(f.Addr()); e != nil {
			return e
		}
	}
	return r
}

// gsSize is a size as the C runtime writes one (gs_size): a whole number of
// the largest of bytes, KB, MB, GB and TB that has one.
func gsSize(n int64) string {
	units := [...]string{" bytes", " KB", " MB", " GB", " TB"}
	u := 0
	for u < 4 && n != 0 && n%1024 == 0 {
		n /= 1024
		u++
	}
	return strconv.FormatInt(n, 10) + units[u]
}

// ---------------------------------------------------------------------------
// A program's start and end.

// Start is the start of the runtime for the program, as main's thread
// program begins (gs_rt_start): its address space budget for regions
// (GS_STACK_BUDGET), the most regions its main program and any one worker
// hold, as the compiler counted them, and the cap that puts on its
// workers, each at the most regions one can take, at least one: what
// hardware_threads() reports at most. Workers that hold no regions leave
// the cap off.
func (p *GsProgram) Start(budget uint64, mainregions, workerregions int64) {
	p.budget, p.mainRegions, p.workerRegions = budget, mainregions, workerregions
	p.threadCap = 0
	if workerregions <= 0 {
		return
	}
	page := int64(syscall.Getpagesize())
	per := uint64(p.StackReserve-p.StackReserve%page) + uint64(p.StackGap)
	total := uint64(0)
	if per != 0 {
		total = budget / per
	}
	spare := int64(math.MaxInt64)
	if total <= math.MaxInt64 {
		spare = int64(total)
	}
	spare -= mainregions
	if spare > workerregions {
		p.threadCap = spare / workerregions
	} else {
		p.threadCap = 1
	}
}

// StackStats is the report a program compiled with GS_STACK_STATS gives on
// stderr as each thread program ends (gs_stack_stats): its indexed data
// stacks, the regions of its block, what each reserves, and the budget.
func (p *GsProgram) StackStats(gs *GsStackBlock, stacks int64) {
	usable := gsUsable(p.StackReserve)
	per := uint64(usable + gs.gap)
	n := uint64(0)
	if per > 0 {
		n = p.budget / per
	}
	workers := "no workers"
	if p.workerRegions > 0 {
		workers = "a worker " + strconv.FormatInt(p.workerRegions, 10) + ", thread cap " +
			strconv.FormatInt(p.threadCap, 10)
	}
	os.Stderr.WriteString("goose stack stats: " + gs.gsWho() + ": " + strconv.FormatInt(stacks, 10) +
		" data stacks, " + strconv.Itoa(len(gs.regions)) + " regions (reserving " + gsSize(usable) +
		" each); budget " + gsSize(int64(min(p.budget, math.MaxInt64))) + " for " +
		strconv.FormatUint(n, 10) + ", main program " + strconv.FormatInt(p.mainRegions, 10) + ", " +
		workers + "\n")
}

// Exit is exit(code): the process ends at once, with that status, from
// whichever thread program calls it (gs_exit). A library's leaves the
// process to its host: it panics with a *GsError naming the status.
func (p *GsProgram) Exit(code int64) {
	if p.Library {
		panic(&GsError{Msg: "exit(" + strconv.FormatInt(code, 10) + ")"})
	}
	gsEnd(int(code), "")
}
