// Goose compiler — the runtime the Go backend writes into every file it
// generates (CodeGenGo::Source, codegen_go.h), after the package clause, the
// imports (math, os, runtime/debug, strconv, syscall, unsafe, each of which
// it uses) and the configuration constants gsStackReserve and gsStackGap. It
// is Go source as it is written out, in pieces under the 16 KB a string
// literal may have in MSVC, laid out as gofmt lays it out (the pass that
// gives the program's Go gofmt's layout, codegen_go_format.h, leaves it
// alone); edit it here, and check the result with go vet and gofmt -l on a
// generated file (test/run_go_tests.py does both, for every file it
// writes).
#pragma once

namespace goose {

inline const char *const go_runtime_prologue[] = {
R"GSGO(// ---- runtime ----
// The Goose runtime for Go (src/codegen_go_runtime.h), written into every
// file the --target=go backend generates so that the file builds with
// nothing but the Go standard library: data stacks in mmap regions, the
// aborts and their messages, the integer and conversion helpers of §6.2 and
// §6.3, and the text forms print uses (§3.7). Its names start with gs or Gs,
// which no name from the program does (those all end in _g).

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
	reserve int64
	gap     int64
}

// The fixed-frame stack's reservation is gsFrameReserve, a configuration
// constant (--frame-reserve): address space only, as the data stacks' is,
// whatever their own (C's frames are on its native stack).

// How many times the runtime has mapped memory (gsMap): a library's package
// maps none until the first call of an entry point starts it (issue #18).
var gsMaps atomic.Int64

// gsMap maps n bytes of zeroed memory outside the Go heap, for a region, a
// fixed-frame stack or a program instance: address space only, which the
// OS commits page by page as it is touched.
func gsMap(n int64) ([]byte, error) {
	gsMaps.Add(1)
	return syscall.Mmap(-1, 0, int(n), syscall.PROT_READ|syscall.PROT_WRITE,
		syscall.MAP_ANON|syscall.MAP_PRIVATE|syscall.MAP_NORESERVE)
}

// The smallest a region gets before a refusal is fatal, and the usable size
// new regions currently get: it halves, in whole pages, each time the
// platform refuses one (gs_reserve_region).
const gsRegionMin = 1 << 20

var gsRegionUsable int64

// The blocks NewStackBlock has made, in which a write that would run past a
// region's end finds the region (gsOverflow). Exported entry points make
// blocks on whatever goroutines call them, so what reserves a region, or
// reads these, holds gsBlocksMu.
var (
	gsBlocksMu sync.Mutex
	gsBlocks   []*GsStackBlock
)

// NewStackBlock reserves numStacks data stacks of reserve usable bytes each,
// with gap bytes of PROT_NONE guard behind each one. A region is whole pages
// (gs_rt_start). The block lasts as long as the program: Close releases it.
func NewStackBlock(numStacks int, reserve, gap int64) *GsStackBlock {
	gsBlocksMu.Lock()
	defer gsBlocksMu.Unlock()
	gs := gsNewBlock(numStacks, reserve, gap)
	gsBlocks = append(gsBlocks, gs)
	return gs
}

func gsNewBlock(numStacks int, reserve, gap int64) *GsStackBlock {
	if reserve < int64(syscall.Getpagesize()) {
		GsPanic("the data stack reservation (GS_STACK_RESERVE) is less than a page")
	}
	gs := &GsStackBlock{reserve: reserve, gap: gap}
	gs.ensureStacks(numStacks)
	gs.reserveFrame()
	return gs
}

// The blocks the exported entry points run on (AcquireStackBlock), which
// the collector may drop from the pool: one it drops is closed.
var gsStackBlockPool sync.Pool

// AcquireStackBlock is a block of at least numStacks empty data stacks, of
// the sizes the program was compiled with, from a pool of them: once the
// pool holds one, it allocates and reserves nothing. Hand it back with
// ReleaseStackBlock; until then it is the caller's alone.
func AcquireStackBlock(numStacks int) *GsStackBlock {
	if gs, _ := gsStackBlockPool.Get().(*GsStackBlock); gs != nil {
		gs.EnsureStacks(numStacks)
		return gs
	}
	gsBlocksMu.Lock()
	defer gsBlocksMu.Unlock()
	gs := gsNewBlock(numStacks, gsStackReserve, gsStackGap)
	runtime.SetFinalizer(gs, (*GsStackBlock).Close)
	return gs
}

// ReleaseStackBlock rewinds gs, a block AcquireStackBlock gave, and gives it
// back to the pool: nothing may use what its stacks hold afterwards.
func ReleaseStackBlock(gs *GsStackBlock) {
	if gs == nil {
		return
	}
	gs.Reset()
	gsStackBlockPool.Put(gs)
}

// reserveFrame maps the fixed-frame stack, halving its reservation where
// the platform refuses one.
func (gs *GsStackBlock) reserveFrame() {
	n := int64(gsFrameReserve)
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
// afterwards.
func (gs *GsStackBlock) Close() error {
	gsBlocksMu.Lock()
	for i, b := range gsBlocks {
		if b == gs {
			gsBlocks = append(gsBlocks[:i], gsBlocks[i+1:]...)
			break
		}
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
// stack's top does not write past (gsPutStr).
func (gs *GsStackBlock) End(k int64) unsafe.Pointer { return unsafe.Add(gs.Base[k], gs.Cap[k]) }

// gsOverflow is the C runtime's report of a data stack that grows past its
// region (gs_overflow_text), raised before a write would cross end, where
// the region ends, naming the thread program whose stacks they are. A
// pooled block's region (AcquireStackBlock) is of the sizes regions have.
func gsOverflow(end unsafe.Pointer) {
	gsBlocksMu.Lock()
	usable, reserve, who := gsRegionUsable, int64(gsStackReserve), "main"
	for _, b := range gsBlocks {
		for k := range b.Cap {
			if b.End(int64(k)) == end {
				usable, reserve, who = b.Cap[k], b.reserve, b.gsWho()
			}
		}
	}
	gsBlocksMu.Unlock()
	panic(&GsError{Msg: gsOverflowText(usable, reserve, who)})
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
// of a block's region (NewStackBlock): a data stack grown past its region
// with no check on the way, as a push grows it (gs_fault_handler). nil for
// any other address.
func gsFault(addr uintptr) *GsError {
	gsBlocksMu.Lock()
	defer gsBlocksMu.Unlock()
	for _, b := range gsBlocks {
		for k, r := range b.regions {
			base := uintptr(unsafe.Pointer(unsafe.SliceData(r)))
			if addr-base < uintptr(len(r)) {
				return &GsError{Msg: gsOverflowText(b.Cap[k], b.reserve, b.gsWho())}
			}
		}
	}
	return nil
}

// gsAbortOf is what a goroutine running Goose code recovered, r, as the
// runtime reports it: a fault in a guard gap, which the goroutine's panic
// on fault (debug.SetPanicOnFault) made a panic, as the *GsError of the
// overflow (gsFault), and anything else as it is.
func gsAbortOf(r any) any {
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

// The program's address space budget for regions (GS_STACK_BUDGET), the
// most regions its main program and any one worker hold, as the compiler
// counted them, and the workers that leaves room for beside the main
// program: what hardware_threads() reports at most (gs_rt_start).
var (
	gsStackBudget   uint64
	gsMainRegions   int64
	gsWorkerRegions int64
	gsThreadCap     int64 = math.MaxInt64
)

// gsRtStart is the start of the runtime, as main's thread program begins
// (gs_rt_start): the budget, and the cap on the workers it holds, each at
// the most regions one can take, at least one. Workers that hold no
// regions leave the cap off.
func gsRtStart(budget uint64, mainregions, workerregions int64) {
	gsStackBudget, gsMainRegions, gsWorkerRegions = budget, mainregions, workerregions
	if workerregions <= 0 {
		return
	}
	page := int64(syscall.Getpagesize())
	per := uint64(int64(gsStackReserve)-int64(gsStackReserve)%page) + uint64(gsStackGap)
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
		gsThreadCap = spare / workerregions
	} else {
		gsThreadCap = 1
	}
}

// gsStackStats is the report a program compiled with GS_STACK_STATS gives on
// stderr as each thread program ends (gs_stack_stats): its indexed data
// stacks, the regions of its block, what each reserves, and the budget.
func gsStackStats(gs *GsStackBlock, stacks int64) {
	per := uint64(gsRegionUsable + gs.gap)
	n := uint64(0)
	if per > 0 {
		n = gsStackBudget / per
	}
	workers := "no workers"
	if gsWorkerRegions > 0 {
		workers = "a worker " + strconv.FormatInt(gsWorkerRegions, 10) + ", thread cap " +
			strconv.FormatInt(gsThreadCap, 10)
	}
	os.Stderr.WriteString("goose stack stats: " + gs.gsWho() + ": " + strconv.FormatInt(stacks, 10) +
		" data stacks, " + strconv.Itoa(len(gs.regions)) + " regions (reserving " + gsSize(gsRegionUsable) +
		" each); budget " + gsSize(int64(min(gsStackBudget, math.MaxInt64))) + " for " +
		strconv.FormatUint(n, 10) + ", main program " + strconv.FormatInt(gsMainRegions, 10) + ", " +
		workers + "\n")
}

// gsReserveRegion maps one region: address space only, committed by the OS
// page by page as the stack grows into it, with the guard gap at its end.
func gsReserveRegion(reserve, gap int64) ([]byte, int64) {
	page := int64(syscall.Getpagesize())
	usable := reserve - reserve%page
	if gsRegionUsable > 0 && gsRegionUsable < usable {
		usable = gsRegionUsable
	}
	for {
		r, err := gsMap(usable + gap)
		if err == nil {
			if gap > 0 {
				if err := syscall.Mprotect(r[usable:], syscall.PROT_NONE); err != nil {
					_ = syscall.Munmap(r)
					GsPanic("cannot protect data stack guard gap")
				}
			}
			gsRegionUsable = usable
			return r, usable
		}
		if usable <= gsRegionMin {
			GsPanic("cannot reserve data stack address space")
		}
		usable /= 2
		usable -= usable % page
	}
}

// ---------------------------------------------------------------------------
// Aborts (§9.3). A failed check panics with a *GsError, which the program's
// main recovers (GsExitOnAbort): the message on stderr, as the C runtime
// words it, and exit status 1. A library never exits the process, which is
// its host's (gsLibrary): there an abort, and exit(), stay the *GsError's
// panic, which the caller of an export fn's entry point may recover.

// The error ids of GsAbort, numbered as the C runtime's GS_E_* are.
const (
	GsECapacity = iota
	GsESlice
	GsEReloff
	GsEAssert
	GsECapRange
	GsEResizeFill
	GsEResizeNeg
	GsEPop
	GsEThreadID
	GsEThreadSelf
	GsETag
	GsEEndian
	GsESliceLen
	GsEPoolSlice
	GsERelNull
	GsEPopN
	GsEQBound
)

var gsErrMsgs = [...]string{
	GsECapacity:   "limited array capacity exceeded",
	GsESlice:      "slice bounds out of range",
	GsEReloff:     "relative reference offset overflow",
	GsEAssert:     "assert failed",
	GsECapRange:   "invalid capacity",
	GsEResizeFill: "resize growth requires a fill value",
	GsEResizeNeg:  "resize to a negative length",
	GsEPop:        "pop on empty array",
	GsEThreadID:   "thread_wait on an unknown thread id",
	GsEThreadSelf: "thread_wait on the current thread",
	GsETag:        "corrupt ADT tag",
	GsEEndian:     "serialization needs a little-endian host (not supported yet)",
	GsESliceLen:   "invalid slice length",
	GsEPoolSlice:  "slice not from this pool",
	GsERelNull:    "non-null relative reference encodes as null",
	GsEPopN:       "pop_n count negative or beyond the array's length",
	GsEQBound:     "queue limit of zero or less",
}

// GsError is a Goose runtime error: what an abort or a failed check panics
// with.
type GsError struct {
	Msg  string // What failed.
	File string // The .goose file of the check; "" for the runtime's own failures.
	Line int
}

func (e *GsError) Error() string { return "goose runtime error: " + e.Text() }

// Text is what the error prints after "goose runtime error: ", its
// location included: the message a contained worker's abort leaves for
// thread_wait (§11.2).
func (e *GsError) Text() string {
	if e.File == "" {
		return e.Msg
	}
	return e.Msg + " (" + e.File + ":" + strconv.Itoa(e.Line) + ")"
}

// GsExitOnAbort, deferred by the program's main, ends the process as the C
// runtime does when a Goose runtime error panics: the message on stderr and
// exit status 1. So does a fault in a data stack's guard gap, which the
// program's main has the runtime panic with (debug.SetPanicOnFault): a stack
// that grew past its region, as C's fault handler reports it. Any other
// panic goes on.
func GsExitOnAbort() {
	if r := recover(); r != nil {
		gsEndOnPanic(r)
	}
}

// gsEndOnPanic ends the process for what a thread program's goroutine
// recovered, as GsExitOnAbort has it; a panic of no runtime error goes on.
// A library's goes on as the runtime error's *GsError (gsAbortOf): on a
// worker's goroutine, which the runtime started, no host recovers it, and
// it ends the process as any goroutine's unrecovered panic does.
func gsEndOnPanic(r any) {
	r = gsAbortOf(r)
	if e, ok := r.(*GsError); ok && !gsLibrary {
		gsEnd(1, e.Error()+"\n")
	}
	panic(r)
}

// Whether the process is ending: the first thread program to end it does
// (gs_end), and any other that tries meanwhile waits for that to happen,
// so that one report comes out, with its status.
var gsEnding struct {
	sync.Mutex
	on bool
}

// gsEnd ends the process with code, msg on stderr first: a program's, never
// a library's (gsLibrary).
func gsEnd(code int, msg string) {
	gsEnding.Lock()
	if gsEnding.on {
		gsEnding.Unlock()
		select {}
	}
	gsEnding.on = true
	gsEnding.Unlock()
	if msg != "" {
		os.Stderr.WriteString(msg)
	}
	os.Exit(code)
}

// GsPanic is a failure of the runtime itself, which has no location in the
// program.
func GsPanic(msg string) { panic(&GsError{Msg: msg}) }

// GsAbort is a failed check with one of the GsE* ids.
func GsAbort(err int, file string, line int) {
	panic(&GsError{Msg: gsErrMsgs[err], File: file, Line: line})
}

// GsAbortMsg is the program's own abort(msg).
func GsAbortMsg(msg string, file string, line int) {
	panic(&GsError{Msg: msg, File: file, Line: line})
}

// GsAbortSlice is abort(msg) of a message the program computed, the bytes
// of a u8 slice (gs_abort_msg).
func GsAbortSlice(msg GsSlice, file string, line int) {
	panic(&GsError{Msg: string(unsafe.Slice((*byte)(msg.Data), msg.Len)), File: file, Line: line})
}

// GsExit is exit(code): the process ends at once, with that status, from
// whichever thread program calls it (gs_exit). A library's leaves the
// process to its host: it panics with a *GsError naming the status.
func GsExit(code int64) {
	if gsLibrary {
		panic(&GsError{Msg: "exit(" + strconv.FormatInt(code, 10) + ")"})
	}
	gsEnd(int(code), "")
}

// GsUnreachable is what a function panics with where control cannot get to
// the end of it, which the checker found: a compiler bug if it does.
func GsUnreachable(file string, line int) *GsError {
	return &GsError{Msg: "unreachable code reached", File: file, Line: line}
}

// GsBool is b, as a call. Go's vet takes `c && c` and `n != 1 || n != 2`
// for mistakes, which a Goose program may write: it leaves an operand that
// is a call out of the comparisons.
func GsBool(b bool) bool { return b }

)GSGO",
R"GSGO(// GsTagError is a debug build's failed check of an ADT's tag, which no
// variant has (GS_E_TAG): a compiler bug if it ever fails.
func GsTagError(file string, line int) *GsError {
	return &GsError{Msg: gsErrMsgs[GsETag], File: file, Line: line}
}

// ---------------------------------------------------------------------------
// Fixed-size values (§3). A slice is the address of its first element and
// their count; Data is a Go pointer, so that the collector sees a slice of
// a Go variable as it sees a reference to one.

// GsSlice is a slice T[:] of any element type (§3.10).
type GsSlice struct {
	Data unsafe.Pointer
	Len  int64
}

// GsRhdr is the header of a resizable value (§5.1, §5.2, C.2): Len elements
// from Base, which sit on top of the header's data stack, so that pushing an
// element writes it at the stack's top, moves the top past it and counts
// it. The base never moves.
type GsRhdr struct {
	Base unsafe.Pointer
	Len  int64
}

// GsRref is a fat reference to a resizable value (C's gs_rref): its header,
// and the index in the GsStackBlock of the data stack its elements top, so
// that a callee can grow the value it was handed.
type GsRref struct {
	Hdr *GsRhdr
	Stk int64
}

// GsPref is a reference to a reusable[] pool (C's gs_pref): the element
// array as a GsRref, and the freelist's header and data stack.
type GsPref struct {
	Hdr     *GsRhdr
	Stk     int64
	FreeHdr *GsRhdr
	FreeStk int64
}

// GsStr is the slice of a string literal's bytes, which are read-only.
func GsStr(s string) GsSlice { return GsSlice{unsafe.Pointer(unsafe.StringData(s)), int64(len(s))} }

// ---------------------------------------------------------------------------
// Go values across the boundary, both ways: the exported entry points of the
// program's export fns take Go's own slices, strings and pointers, and the
// Go functions its extern fns call take its slices, which these view in
// place, with no copy (DESIGN.md §3.5).

// GsBytes is what a const u8[:] parameter of an exported entry point takes:
// a string's bytes, or a []byte's.
type GsBytes interface{ ~string | ~[]byte }

// What an exported entry point is handed may be stored where only the data
// stacks see it, which Go's stacks must not move away from while it runs:
// gsEscape makes the Go compiler keep it off them, on the heap, where
// nothing moves (its condition never holds).
var gsEscapeSink struct {
	b bool
	p unsafe.Pointer
}

func gsEscape(p unsafe.Pointer) unsafe.Pointer {
	if gsEscapeSink.b {
		gsEscapeSink.p = p
	}
	return p
}

// GsBytesOf is the u8 slice of a string's or a []byte's bytes, in place: a
// string's header begins as a slice's does, with its data and its length.
func GsBytesOf[T GsBytes](s T) GsSlice {
	return GsSlice{gsEscape(*(*unsafe.Pointer)(unsafe.Pointer(&s))), int64(len(s))}
}

// GsSliceOf is the slice of xs's elements, in place. A Go T has the
// layout of the Goose type it stands for.
func GsSliceOf[T any](xs []T) GsSlice {
	return GsSlice{gsEscape(unsafe.Pointer(unsafe.SliceData(xs))), int64(len(xs))}
}

// GsSliceFromBytes is the u8 slice of b's bytes, and GsSliceFromString of
// s's, which are read-only, in place.
func GsSliceFromBytes(b []byte) GsSlice {
	return GsSlice{gsEscape(unsafe.Pointer(unsafe.SliceData(b))), int64(len(b))}
}

func GsSliceFromString(s string) GsSlice {
	return GsSlice{gsEscape(unsafe.Pointer(unsafe.StringData(s))), int64(len(s))}
}

// GsSliceToBytes is the []byte of a u8 slice's bytes, and GsSliceToString
// the string, in place: valid as long as what the slice views is, and for
// the string, unchanged meanwhile. GsSliceToSlice is a []T of a slice's
// elements, its T the Go type of their Goose type.
func GsSliceToBytes(s GsSlice) []byte { return unsafe.Slice((*byte)(s.Data), s.Len) }

func GsSliceToString(s GsSlice) string { return unsafe.String((*byte)(s.Data), s.Len) }

func GsSliceToSlice[T any](s GsSlice) []T { return unsafe.Slice((*T)(s.Data), s.Len) }

// GsBldAppend appends data to the u8[>..] builder b refers to, for a Go
// function an extern fn calls (gs_bld_append): a builder's bytes top its
// data stack, so they go at the stack's top, and its count grows. It may
// not shrink the builder, nor keep b past its call. gs is the block the
// function is handed with the builder.
func GsBldAppend(gs *GsStackBlock, b GsRref, data []byte) {
	if len(data) == 0 {
		return
	}
	gsBldPut(gs, b, unsafe.Pointer(unsafe.SliceData(data)), len(data))
}

// GsBldAppendString appends the bytes of s, as GsBldAppend does.
func GsBldAppendString(gs *GsStackBlock, b GsRref, s string) {
	if len(s) == 0 {
		return
	}
	gsBldPut(gs, b, unsafe.Pointer(unsafe.StringData(s)), len(s))
}

func gsBldPut(gs *GsStackBlock, b GsRref, p unsafe.Pointer, n int) {
	top, end := gs.Stks[b.Stk], gs.End(b.Stk)
	if uintptr(n) > uintptr(end)-uintptr(top) {
		gsOverflow(end)
	}
	copy(unsafe.Slice((*byte)(top), n), unsafe.Slice((*byte)(p), n))
	gs.Stks[b.Stk] = unsafe.Add(top, n)
	b.Hdr.Len += int64(n)
}

// GsSub is the slice of the elements lo up to hi of the n at p, each es
// bytes. An empty one starts where it is, at the end of the run too, so
// that two at different places are different slices (§3.10): what a slice
// can point into is never the Go heap, where an address one past an
// object's end would be no valid Go pointer.
func GsSub(p unsafe.Pointer, lo, hi, n, es int64) GsSlice {
	_ = n
	return GsSlice{unsafe.Add(p, lo*es), hi - lo}
}

// GsIdx is i, checked to be an index of an array of n elements (GS_IDX).
func GsIdx(i, n int64, file string, line int) int64 {
	if uint64(i) >= uint64(n) {
		gsIdxFail(i, n, file, line)
	}
	return i
}

func gsIdxFail(i, n int64, file string, line int) {
	panic(&GsError{Msg: "index " + strconv.FormatInt(i, 10) + " out of bounds (length " +
		strconv.FormatInt(n, 10) + ")", File: file, Line: line})
}

// GsMemEq says whether the n bytes at a and b are the same.
func GsMemEq(a, b unsafe.Pointer, n int64) bool {
	return n == 0 || a == b ||
		string(unsafe.Slice((*byte)(a), n)) == string(unsafe.Slice((*byte)(b), n))
}

// GsRelLoad is the address an optional relative reference at slot holding
// off encodes: null where off is 0 (§3.9).
func GsRelLoad(slot unsafe.Pointer, off int64) unsafe.Pointer {
	if off == 0 {
		return nil
	}
	return unsafe.Add(slot, off)
}

// GsPoolLoad is the address an optional `in pool` reference holding off
// encodes, from its pool's base: null where off is 0 (§3.9).
func GsPoolLoad(base unsafe.Pointer, off int64) unsafe.Pointer {
	if off == 0 {
		return nil
	}
	return unsafe.Add(base, off-1)
}

// GsPoolOff is what an optional `in pool` reference to p stores: its
// distance from its pool's base plus one, or 0 for null.
func GsPoolOff(p, base unsafe.Pointer) uint64 {
	if p == nil {
		return 0
	}
	return uint64(uintptr(p)-uintptr(base)) + 1
}

// GsRelOff is the offset from slot the address p stores as in an optional
// relative reference: 0 for null.
func GsRelOff(p, slot unsafe.Pointer) int64 {
	if p == nil {
		return 0
	}
	return int64(uintptr(p) - uintptr(slot))
}

func GsDivFail(file string, line int) {
	panic(&GsError{Msg: "division by zero", File: file, Line: line})
}

func GsDivOvf(file string, line int) {
	panic(&GsError{Msg: "integer overflow in division", File: file, Line: line})
}

// GsOvf and GsOvfNeg are a debug build's failed overflow checks.
func GsOvf(a int64, op string, b int64, typ, file string, line int) {
	panic(&GsError{Msg: "integer overflow (debug): " + strconv.FormatInt(a, 10) + " " + op + " " +
		strconv.FormatInt(b, 10) + " at " + typ, File: file, Line: line})
}

func GsOvfNeg(a int64, typ, file string, line int) {
	panic(&GsError{Msg: "integer overflow (debug): -(" + strconv.FormatInt(a, 10) + ") at " + typ,
		File: file, Line: line})
}

// GsAsFail is a debug build's failed `as` check, num the source value's text.
func GsAsFail(why, num, typ, file string, line int) {
	panic(&GsError{Msg: "as conversion " + why + " (debug): " + num + " as " + typ,
		File: file, Line: line})
}

// ---------------------------------------------------------------------------
// Integer semantics (§6.2). Go's +, - and * wrap as a release build's do, at
// every width, so only division needs a helper there: a zero divisor aborts
// in every build, and so does i64.min / -1; a narrower signed quotient that
// does not fit wraps, as Go's own `/` does. `%` is Euclidean. A debug build
// checks the signed operations as well.

func GsDivI8(a, b int8, file string, line int) int8 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

func GsDivI16(a, b int16, file string, line int) int16 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

func GsDivI32(a, b int32, file string, line int) int32 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

func GsDivI64(a, b int64, file string, line int) int64 {
	if b == 0 {
		GsDivFail(file, line)
	}
	if b == -1 && a == math.MinInt64 {
		GsDivOvf(file, line)
	}
	return a / b
}

func GsDivU8(a, b uint8, file string, line int) uint8 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

func GsDivU16(a, b uint16, file string, line int) uint16 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

func GsDivU32(a, b uint32, file string, line int) uint32 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

func GsDivU64(a, b uint64, file string, line int) uint64 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

// The debug build's narrow signed division, which also aborts where the
// quotient does not fit (-128 / -1 at i8).
func GsDivI8Debug(a, b int8, file string, line int) int8 {
	if b == 0 {
		GsDivFail(file, line)
	}
	if b == -1 && a == math.MinInt8 {
		GsOvf(int64(a), "/", int64(b), "i8", file, line)
	}
	return a / b
}

func GsDivI16Debug(a, b int16, file string, line int) int16 {
	if b == 0 {
		GsDivFail(file, line)
	}
	if b == -1 && a == math.MinInt16 {
		GsOvf(int64(a), "/", int64(b), "i16", file, line)
	}
	return a / b
}

func GsDivI32Debug(a, b int32, file string, line int) int32 {
	if b == 0 {
		GsDivFail(file, line)
	}
	if b == -1 && a == math.MinInt32 {
		GsOvf(int64(a), "/", int64(b), "i32", file, line)
	}
	return a / b
}

func GsModI8(a, b int8, file string, line int) int8 {
	return int8(gsModNarrow(int64(a), int64(b), file, line))
}

func GsModI16(a, b int16, file string, line int) int16 {
	return int16(gsModNarrow(int64(a), int64(b), file, line))
}

func GsModI32(a, b int32, file string, line int) int32 {
	return int32(gsModNarrow(int64(a), int64(b), file, line))
}

// The Euclidean remainder of operands that fit in 32 bits: in [0, |b|).
func gsModNarrow(a, b int64, file string, line int) int64 {
	if b == 0 {
		GsDivFail(file, line)
	}
	r := a % b
	if r < 0 {
		if b < 0 {
			r -= b
		} else {
			r += b
		}
	}
	return r
}

// i64.min % -1 is 0, as every x % -1 is; |b| is taken unsigned, so a divisor
// of i64.min works too.
func GsModI64(a, b int64, file string, line int) int64 {
	if b == 0 {
		GsDivFail(file, line)
	}
	if b == -1 {
		return 0
	}
	r := a % b
	if r < 0 {
		m := uint64(b)
		if b < 0 {
			m = -m
		}
		r = int64(uint64(r) + m)
	}
	return r
}

func GsModU8(a, b uint8, file string, line int) uint8 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a % b
}

func GsModU16(a, b uint16, file string, line int) uint16 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a % b
}

func GsModU32(a, b uint32, file string, line int) uint32 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a % b
}

func GsModU64(a, b uint64, file string, line int) uint64 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a % b
}

// The debug build's checked signed arithmetic: the operands' type is in the
// name, and the result aborts where it does not fit that type.
func GsAddI8(a, b int8, file string, line int) int8 {
	r := int64(a) + int64(b)
	if r != int64(int8(r)) {
		GsOvf(int64(a), "+", int64(b), "i8", file, line)
	}
	return int8(r)
}

func GsSubI8(a, b int8, file string, line int) int8 {
	r := int64(a) - int64(b)
	if r != int64(int8(r)) {
		GsOvf(int64(a), "-", int64(b), "i8", file, line)
	}
	return int8(r)
}

func GsMulI8(a, b int8, file string, line int) int8 {
	r := int64(a) * int64(b)
	if r != int64(int8(r)) {
		GsOvf(int64(a), "*", int64(b), "i8", file, line)
	}
	return int8(r)
}

func GsNegI8(a int8, file string, line int) int8 {
	if a == math.MinInt8 {
		GsOvfNeg(int64(a), "i8", file, line)
	}
	return -a
}

func GsAddI16(a, b int16, file string, line int) int16 {
	r := int64(a) + int64(b)
	if r != int64(int16(r)) {
		GsOvf(int64(a), "+", int64(b), "i16", file, line)
	}
	return int16(r)
}

)GSGO",
R"GSGO(func GsSubI16(a, b int16, file string, line int) int16 {
	r := int64(a) - int64(b)
	if r != int64(int16(r)) {
		GsOvf(int64(a), "-", int64(b), "i16", file, line)
	}
	return int16(r)
}

func GsMulI16(a, b int16, file string, line int) int16 {
	r := int64(a) * int64(b)
	if r != int64(int16(r)) {
		GsOvf(int64(a), "*", int64(b), "i16", file, line)
	}
	return int16(r)
}

func GsNegI16(a int16, file string, line int) int16 {
	if a == math.MinInt16 {
		GsOvfNeg(int64(a), "i16", file, line)
	}
	return -a
}

func GsAddI32(a, b int32, file string, line int) int32 {
	r := int64(a) + int64(b)
	if r != int64(int32(r)) {
		GsOvf(int64(a), "+", int64(b), "i32", file, line)
	}
	return int32(r)
}

func GsSubI32(a, b int32, file string, line int) int32 {
	r := int64(a) - int64(b)
	if r != int64(int32(r)) {
		GsOvf(int64(a), "-", int64(b), "i32", file, line)
	}
	return int32(r)
}

func GsMulI32(a, b int32, file string, line int) int32 {
	r := int64(a) * int64(b)
	if r != int64(int32(r)) {
		GsOvf(int64(a), "*", int64(b), "i32", file, line)
	}
	return int32(r)
}

func GsNegI32(a int32, file string, line int) int32 {
	if a == math.MinInt32 {
		GsOvfNeg(int64(a), "i32", file, line)
	}
	return -a
}

func GsAddI64(a, b int64, file string, line int) int64 {
	r := a + b
	if (a^r)&(b^r) < 0 {
		GsOvf(a, "+", b, "i64", file, line)
	}
	return r
}

func GsSubI64(a, b int64, file string, line int) int64 {
	r := a - b
	if (a^b)&(a^r) < 0 {
		GsOvf(a, "-", b, "i64", file, line)
	}
	return r
}

func GsMulI64(a, b int64, file string, line int) int64 {
	r := a * b
	if a != 0 && b != 0 &&
		((a == -1 && b == math.MinInt64) || (b == -1 && a == math.MinInt64) || r/b != a) {
		GsOvf(a, "*", b, "i64", file, line)
	}
	return r
}

func GsNegI64(a int64, file string, line int) int64 {
	if a == math.MinInt64 {
		GsOvfNeg(a, "i64", file, line)
	}
	return -a
}

// ---------------------------------------------------------------------------
// Conversions (§6.3). `as!`, and `as` in a release build, take a float to an
// integer by truncating it toward zero and wrapping modulo 2^64, the same on
// every platform; a debug build's `as` aborts wherever the value changes.

func GsF2IWrap(d float64) int64 {
	if d >= -9223372036854775808.0 && d < 9223372036854775808.0 {
		return int64(d)
	}
	return gsF2IWrapSlow(d)
}

func gsF2IWrapSlow(d float64) int64 {
	if d != d || math.IsInf(d, 0) {
		return 0
	}
	d = math.Mod(math.Trunc(d), 18446744073709551616.0)
	if d < 0 {
		d += 18446744073709551616.0
	}
	return int64(uint64(d))
}

// The debug build's checked `as` between integer types, to a range lo..hi,
// and from u64, to 0..hi.
func GsRangeI(v, lo, hi int64, typ, file string, line int) int64 {
	if v < lo || v > hi {
		GsAsFail("out of range", strconv.FormatInt(v, 10), typ, file, line)
	}
	return v
}

func GsRangeU(v, hi uint64, typ, file string, line int) uint64 {
	if v > hi {
		GsAsFail("out of range", strconv.FormatUint(v, 10), typ, file, line)
	}
	return v
}

// The debug build's checked `as` from a float: to a type of range lo..hi,
// and to u64. f32: d is an f32's value, and is named in that type's text.
func GsF2I(d float64, f32 bool, lo, hi int64, typ, file string, line int) int64 {
	if !(d >= -9223372036854775808.0 && d < 9223372036854775808.0) {
		GsAsFail("out of range", gsFloatText(d, f32), typ, file, line)
	}
	v := int64(d)
	if v < lo || v > hi {
		GsAsFail("out of range", gsFloatText(d, f32), typ, file, line)
	}
	if float64(v) != d {
		GsAsFail("changes the value", gsFloatText(d, f32), typ, file, line)
	}
	return v
}

func GsF2U(d float64, f32 bool, file string, line int) uint64 {
	if !(d >= 0 && d < 18446744073709551616.0) {
		GsAsFail("out of range", gsFloatText(d, f32), "u64", file, line)
	}
	v := uint64(d)
	if float64(v) != d {
		GsAsFail("changes the value", gsFloatText(d, f32), "u64", file, line)
	}
	return v
}

// ---------------------------------------------------------------------------
// Text forms (§3.7). Each writes a value's text at p, at most gsFmtMax
// bytes, and returns where the text ends; print renders a line on a data
// stack this way and writes it out whole. end is where the stack's region
// ends: a text that would run past it is a data stack overflow (gsOverflow),
// found before a byte of it is written, where the C runtime's guard gap
// finds it as the byte is written. A value is formatted in place while
// gsFmtMax bytes are left, and in a buffer of its own nearer the end.

const gsFmtMax = 32

func gsFmtBuf(p unsafe.Pointer) []byte { return unsafe.Slice((*byte)(p), gsFmtMax)[:0] }

func gsRoom(p, end unsafe.Pointer) bool { return uintptr(end)-uintptr(p) >= gsFmtMax }

func gsFmtI64(p, end unsafe.Pointer, v int64) unsafe.Pointer {
	if gsRoom(p, end) {
		return unsafe.Add(p, len(strconv.AppendInt(gsFmtBuf(p), v, 10)))
	}
	var b [gsFmtMax]byte
	return gsPutBytes(p, end, strconv.AppendInt(b[:0], v, 10))
}

func gsFmtU64(p, end unsafe.Pointer, v uint64) unsafe.Pointer {
	if gsRoom(p, end) {
		return unsafe.Add(p, len(strconv.AppendUint(gsFmtBuf(p), v, 10)))
	}
	var b [gsFmtMax]byte
	return gsPutBytes(p, end, strconv.AppendUint(b[:0], v, 10))
}

func gsFmtF64(p, end unsafe.Pointer, v float64) unsafe.Pointer {
	if gsRoom(p, end) {
		return unsafe.Add(p, len(gsAppendFloat(gsFmtBuf(p), v, false)))
	}
	var b [gsFmtMax]byte
	return gsPutBytes(p, end, gsAppendFloat(b[:0], v, false))
}

func gsFmtF32(p, end unsafe.Pointer, v float32) unsafe.Pointer {
	if gsRoom(p, end) {
		return unsafe.Add(p, len(gsAppendFloat(gsFmtBuf(p), float64(v), true)))
	}
	var b [gsFmtMax]byte
	return gsPutBytes(p, end, gsAppendFloat(b[:0], float64(v), true))
}

func gsFmtBool(p, end unsafe.Pointer, v bool) unsafe.Pointer {
	if v {
		return gsPutStr(p, end, "true")
	}
	return gsPutStr(p, end, "false")
}

// gsPutStr writes the bytes of s at p.
func gsPutStr(p, end unsafe.Pointer, s string) unsafe.Pointer {
	if uintptr(len(s)) > uintptr(end)-uintptr(p) {
		gsOverflow(end)
	}
	copy(unsafe.Slice((*byte)(p), len(s)), s)
	return unsafe.Add(p, len(s))
}

// gsPutBytes writes the bytes of b at p.
func gsPutBytes(p, end unsafe.Pointer, b []byte) unsafe.Pointer {
	if uintptr(len(b)) > uintptr(end)-uintptr(p) {
		gsOverflow(end)
	}
	copy(unsafe.Slice((*byte)(p), len(b)), b)
	return unsafe.Add(p, len(b))
}

// gsPutQuoted writes the n bytes at s quoted, as a u8 array inside an
// aggregate renders (gs_fmt_quoted): `"`, `\` and the line breaks and tabs
// escaped.
func gsPutQuoted(p, end unsafe.Pointer, s unsafe.Pointer, n int64) unsafe.Pointer {
	b := unsafe.Slice((*byte)(s), n)
	size := 2
	for _, c := range b {
		switch c {
		case '"', '\\', '\n', '\r', '\t':
			size += 2
		default:
			size++
		}
	}
	if uintptr(size) > uintptr(end)-uintptr(p) {
		gsOverflow(end)
	}
	d := unsafe.Slice((*byte)(p), size)[:0]
	d = append(d, '"')
	for _, c := range b {
		switch c {
		case '"', '\\':
			d = append(d, '\\', c)
		case '\n':
			d = append(d, '\\', 'n')
		case '\r':
			d = append(d, '\\', 'r')
		case '\t':
			d = append(d, '\\', 't')
		default:
			d = append(d, c)
		}
	}
	d = append(d, '"')
	return unsafe.Add(p, len(d))
}

// gsOut writes the bytes from base up to end to stdout, in one write.
func gsOut(base, end unsafe.Pointer) {
	_, _ = os.Stdout.Write(unsafe.Slice((*byte)(base), uintptr(end)-uintptr(base)))
}

func gsFloatText(d float64, f32 bool) string { return string(gsAppendFloat(nil, d, f32)) }

// gsAppendFloat appends a float's text: the fewest significant digits that
// read back as the value, of its own type (an f32's where f32 is set), and
// of those the nearest, laid out as C's %g lays them out at a precision of
// max(15, digits), with a fraction or an exponent always: 1.0, 0.1, 1e+21,
// 1.5e-07 (gs_fmt_float).
func gsAppendFloat(dst []byte, v float64, f32 bool) []byte {
	switch {
	case v != v:
		return append(dst, "nan"...)
	case math.IsInf(v, 1):
		return append(dst, "inf"...)
	case math.IsInf(v, -1):
		return append(dst, "-inf"...)
	}
	if math.Signbit(v) {
		dst = append(dst, '-')
		v = -v
	}
	if v == 0 {
		return append(dst, "0.0"...)
	}
	size := 64
	if f32 {
		size = 32
	}
	// The shortest digits as d.ddde±xx: dig[:n], and x, the exponent of the
	// first digit.
	var buf [32]byte
	e := strconv.AppendFloat(buf[:0], v, 'e', -1, size)
	var dig [24]byte
	n, i := 0, 0
	for ; e[i] != 'e'; i++ {
		if e[i] != '.' {
			dig[n] = e[i]
			n++
		}
	}
	x := 0
	for _, c := range e[i+2:] {
		x = x*10 + int(c-'0')
	}
	if e[i+1] == '-' {
		x = -x
	}
	if x < -4 || x >= max(n, 15) {
		dst = append(dst, dig[0])
		if n > 1 {
			dst = append(dst, '.')
			dst = append(dst, dig[1:n]...)
		}
		dst = append(dst, 'e')
		if x < 0 {
			dst = append(dst, '-')
			x = -x
		} else {
			dst = append(dst, '+')
		}
		if x >= 100 {
			dst = append(dst, byte('0'+x/100))
		}
		return append(dst, byte('0'+x/10%10), byte('0'+x%10))
	}
	if x < 0 {
		dst = append(dst, "0."...)
		for j := -1; j > x; j-- {
			dst = append(dst, '0')
		}
		return append(dst, dig[:n]...)
	}
	for j := 0; j < n || j <= x; j++ {
		if j == x+1 {
			dst = append(dst, '.')
		}
		if j < n {
			dst = append(dst, dig[j])
		} else {
			dst = append(dst, '0')
		}
	}
	// A whole number still reads as a float: 1.0, not 1.
	if n <= x+1 {
		dst = append(dst, ".0"...)
	}
	return dst
}

)GSGO",
R"GSGO(// ---------------------------------------------------------------------------
// Slice pools (goose_spec.md §5.4, reusable[]), ported from the gs_spans_*
// functions of src/runtime/runtime_impl.h, as runtime/go/goose_spans.go has
// them (keep the two the same). A pool's freelist holds its
// free (index, count) spans, no two of them touching, in a B+ tree ordered
// by index: leaves hold spans, and every inner entry holds the first index
// and the largest count in the subtree under it, which is what lets first
// fit, the span at or before an index, and the last span each be found by
// one descent.
//
// The tree lives on a data stack of its own, in nodes of GsSpanNode bytes
// numbered from the freelist's base: node 0 is the header, and the others
// come off a chain of emptied nodes or past the last one, so the node count
// is the freelist's length, and the tree, in node numbers, can be copied
// whole. A freelist starts empty, without a header, until a span is freed.
// Every span comes from a slice of the pool, so spans stay inside the
// pool's length whatever the program frees: freeing a slice twice leaves
// overlapping spans that hand the same elements out twice, never anything
// outside the pool.
//
// The generated code keeps the element array's length and top and fills
// in elements; these functions keep only the spans. Each takes the
// freelist's base, a pointer to its node count (its header's Len) and a
// pointer to its stack's top, which stays past the last node: the stack's
// slot in GsStackBlock.Stks, or a local the generated code caches it in.
// Indices and counts are in elements.

// GsSpanNode is the size of a freelist node in bytes (GS_SPAN_NODE).
const GsSpanNode = 512

const (
	gsSpanLeaf = (GsSpanNode - 16) / 16 // spans in a leaf
	gsSpanFan  = (GsSpanNode - 16) / 24 // children of an inner node
	// A full node splits into halves and an empty one leaves its parent,
	// so every level a tree gains takes about gsSpanFan / 2 times the
	// insertions of the one before: no freelist gets this deep.
	gsSpanDepth = 24
)

// The nodes as they lie on the stack; link chains emptied nodes.
type gsSpLeaf struct {
	n, link int64
	idx     [gsSpanLeaf]int64
	cnt     [gsSpanLeaf]int64
}

type gsSpInner struct {
	n, link int64
	key     [gsSpanFan]int64
	max     [gsSpanFan]int64
	child   [gsSpanFan]int64
}

type gsSpHead struct {
	root, height, chain int64
}

// gsSpans is a freelist as the functions are handed it.
type gsSpans struct {
	base unsafe.Pointer
	n    *int64
	top  *unsafe.Pointer
}

// gsSpPath is a path down a freelist: the node and the entry taken at
// each level, the leaf's last.
type gsSpPath struct {
	h    int64
	node [gsSpanDepth]int64
	pos  [gsSpanDepth]int64
}

func (f *gsSpans) head() *gsSpHead { return (*gsSpHead)(f.base) }

func (f *gsSpans) leaf(i int64) *gsSpLeaf {
	return (*gsSpLeaf)(unsafe.Add(f.base, i*GsSpanNode))
}

func (f *gsSpans) inner(i int64) *gsSpInner {
	return (*gsSpInner)(unsafe.Add(f.base, i*GsSpanNode))
}

func (f *gsSpans) newNode() int64 {
	hd := f.head()
	if i := hd.chain; i != 0 {
		hd.chain = f.leaf(i).link
		return i
	}
	*f.top = unsafe.Add(*f.top, GsSpanNode)
	i := *f.n
	*f.n = i + 1
	return i
}

func (f *gsSpans) dropNode(i int64) {
	hd := f.head()
	f.leaf(i).link = hd.chain
	hd.chain = i
}

// gsSpOpen moves a leaf's entries from position s on up one, making room
// at s; gsSpClose moves those after s down onto it. Loops over the arrays
// together rather than copies of each: a node holds a few entries, and the
// calls cost more than moving them.
func gsSpOpen(lf *gsSpLeaf, s int64) {
	for j := lf.n; j > s; j-- {
		lf.idx[j] = lf.idx[j-1]
		lf.cnt[j] = lf.cnt[j-1]
	}
	lf.n++
}

func gsSpClose(lf *gsSpLeaf, s int64) {
	lf.n--
	for j := s; j < lf.n; j++ {
		lf.idx[j] = lf.idx[j+1]
		lf.cnt[j] = lf.cnt[j+1]
	}
}

func gsSpOpenIn(in *gsSpInner, s int64) {
	for j := in.n; j > s; j-- {
		in.key[j] = in.key[j-1]
		in.max[j] = in.max[j-1]
		in.child[j] = in.child[j-1]
	}
	in.n++
}

func gsSpCloseIn(in *gsSpInner, s int64) {
	in.n--
	for j := s; j < in.n; j++ {
		in.key[j] = in.key[j+1]
		in.max[j] = in.max[j+1]
		in.child[j] = in.child[j+1]
	}
}

// gsSpMost is the largest of the counts v, or stop as soon as one reaches
// it.
func gsSpMost(v []int64, stop int64) int64 {
	m := int64(0)
	for k := 0; k < len(v) && m < stop; k++ {
		if v[k] > m {
			m = v[k]
		}
	}
	return m
}

// max is the largest count under node i, a leaf or an inner node, or stop
// as soon as one reaches it.
func (f *gsSpans) max(i int64, leaf bool, stop int64) int64 {
	if leaf {
		lf := f.leaf(i)
		return gsSpMost(lf.cnt[:lf.n], stop)
	}
	in := f.inner(i)
	return gsSpMost(in.max[:in.n], stop)
}

func (f *gsSpans) key(i int64, leaf bool) int64 {
	if leaf {
		return f.leaf(i).idx[0]
	}
	return f.inner(i).key[0]
}

// fix brings the entries above level l of the path (the leaf being level
// h) up to date with the node below each, after one count in the node at
// level l went from was to now (0 for an entry added or removed), stopping
// at the first entry that is unchanged. A node's largest count is rescanned
// only where the count that held it shrank, and only until another as large
// turns up; a was of math.MaxInt64 rescans the node at level l whole,
// whatever changed in it.
func (f *gsSpans) fix(p *gsSpPath, l, was, now int64) {
	for ; l > 0; l-- {
		in := f.inner(p.node[l-1])
		k, c := p.pos[l-1], p.node[l]
		m := in.max[k]
		leaf := l == p.h
		key := f.key(c, leaf)
		var nm int64
		switch {
		case now >= m:
			nm = now
		case was < m:
			nm = m
		default:
			nm = f.max(c, leaf, was)
		}
		if in.key[k] == key && nm == m {
			return
		}
		in.key[k] = key
		in.max[k] = nm
		was = m
		now = nm
	}
}

// find descends to the leaf holding the last span that starts at or before
// idx, and returns its position there, or -1 where no span does.
func (f *gsSpans) find(p *gsSpPath, idx int64) int64 {
	hd := f.head()
	i := hd.root
	p.h = hd.height
	for l := int64(0); l < p.h; l++ {
		in := f.inner(i)
		k := int64(1)
		for k < in.n && in.key[k] <= idx {
			k++
		}
		p.node[l] = i
		p.pos[l] = k - 1
		i = in.child[k-1]
	}
	p.node[p.h] = i
	lf := f.leaf(i)
	k := int64(0)
	for k < lf.n && lf.idx[k] <= idx {
		k++
	}
	p.pos[p.h] = k - 1
	return k - 1
}

// newRoot puts a node full at the root, split into a and b, under a new
// root of the two halves.
func (f *gsSpans) newRoot(a, b int64, leaf bool) {
	r := f.newNode()
	rn := f.inner(r)
	rn.n = 2
	rn.key[0] = f.key(a, leaf)
	rn.max[0] = f.max(a, leaf, math.MaxInt64)
	rn.child[0] = a
	rn.key[1] = f.key(b, leaf)
	rn.max[1] = f.max(b, leaf, math.MaxInt64)
	rn.child[1] = b
	hd := f.head()
	hd.root = r
	hd.height++
}

// addChild puts the entry for node child at position k of the inner node
// at level l of the path, splitting full nodes on the way up.
func (f *gsSpans) addChild(p *gsSpPath, l, k, child int64, leaf bool) {
	for {
		in := f.inner(p.node[l])
		o := in
		key, max := f.key(child, leaf), f.max(child, leaf, math.MaxInt64)
		s, nn := k, int64(0)
		if in.n == gsSpanFan {
			const half = gsSpanFan / 2
			nn = f.newNode()
			r := f.inner(nn)
			r.n = in.n - half
			copy(r.key[:r.n], in.key[half:])
			copy(r.max[:r.n], in.max[half:])
			copy(r.child[:r.n], in.child[half:])
			in.n = half
			if k > half {
				o = r
				s = k - half
			}
		}
		gsSpOpenIn(o, s)
		o.key[s] = key
		o.max[s] = max
		o.child[s] = child
		if nn == 0 {
			f.fix(p, l, 0, max)
			return
		}
		if l == 0 {
			f.newRoot(p.node[0], nn, false)
			return
		}
		// The node keeps its place above, and its new right half goes
		// after it.
		f.fix(p, l, math.MaxInt64, 0)
		child = nn
		leaf = false
		k = p.pos[l-1] + 1
		l--
	}
}

// insert puts span (idx, cnt) at position k of the path's leaf.
func (f *gsSpans) insert(p *gsSpPath, k, idx, cnt int64) {
	lf := f.leaf(p.node[p.h])
	o := lf
	s, nn := k, int64(0)
	if lf.n == gsSpanLeaf {
		const half = gsSpanLeaf / 2
		nn = f.newNode()
		r := f.leaf(nn)
		r.n = lf.n - half
		copy(r.idx[:r.n], lf.idx[half:])
		copy(r.cnt[:r.n], lf.cnt[half:])
		lf.n = half
		if k > half {
			o = r
			s = k - half
		}
	}
	gsSpOpen(o, s)
	o.idx[s] = idx
	o.cnt[s] = cnt
	switch {
	case nn == 0:
		f.fix(p, p.h, 0, cnt)
	case p.h == 0:
		f.newRoot(p.node[0], nn, true)
	default:
		f.fix(p, p.h, math.MaxInt64, 0)
		f.addChild(p, p.h-1, p.pos[p.h-1]+1, nn, true)
	}
}

// delete removes the span at position k of the path's leaf. An emptied
// node leaves its parent, and a root left with one child hands the root
// down.
func (f *gsSpans) delete(p *gsSpPath, k int64) {
	lf := f.leaf(p.node[p.h])
	c := lf.cnt[k]
	gsSpClose(lf, k)
	if lf.n > 0 || p.h == 0 {
		f.fix(p, p.h, c, 0)
		return
	}
	f.dropNode(p.node[p.h])
	for l := p.h - 1; ; l-- {
		in := f.inner(p.node[l])
		e := p.pos[l]
		m := in.max[e]
		gsSpCloseIn(in, e)
		if in.n > 0 {
			f.fix(p, l, m, 0)
			break
		}
		// Never the root: a root of one child is handed down below, so
		// every root has two or more.
		f.dropNode(p.node[l])
	}
	hd := f.head()
	for hd.height > 0 && f.inner(hd.root).n == 1 {
		r := hd.root
		hd.root = f.inner(r).child[0]
		hd.height--
		f.dropNode(r)
	}
}

// take takes cnt elements off the front of the span at position k of the
// path's leaf, or the whole span where it holds no more.
func (f *gsSpans) take(p *gsSpPath, k, cnt int64) {
	lf := f.leaf(p.node[p.h])
	c := lf.cnt[k]
	if c <= cnt {
		f.delete(p, k)
		return
	}
	lf.idx[k] += cnt
	lf.cnt[k] = c - cnt
	f.fix(p, p.h, c, c-cnt)
}

)GSGO",
R"GSGO(// GsSpansAlloc is where a run of cnt elements goes in a pool of length
// elements, taken off the freelist (gs_spans_alloc): for alloc_slice, and
// for a slice realloc_slice moves. That is the front of the first free span
// that holds cnt, in index order, else the end of the array, where a free
// span that reaches the end starts the run. The caller grows the array to
// the returned index plus cnt. First fit rather than best: in index order
// it packs runs toward the start and leaves the end free.
func GsSpansAlloc(base unsafe.Pointer, n *int64, top *unsafe.Pointer, length, cnt int64) int64 {
	if cnt == 0 || *n == 0 {
		return length
	}
	f := gsSpans{base, n, top}
	var p gsSpPath
	var before [gsSpanDepth]int64
	var lf *gsSpLeaf
	var l, k, m int64
	hd := f.head()
	p.h = hd.height
	i := hd.root
	// Down the first entry at each level whose largest count holds cnt,
	// noting the largest count of the ones before it, all smaller than
	// cnt, for bringing the entries up to date once the span is taken
	// from. Where the root holds none, down to the last span.
	for l = 0; l < p.h; l++ {
		in := f.inner(i)
		for k, m = 0, 0; k < in.n && in.max[k] < cnt; k++ {
			if in.max[k] > m {
				m = in.max[k]
			}
		}
		if k == in.n {
			break
		}
		p.node[l] = i
		p.pos[l] = k
		before[l] = m
		i = in.child[k]
	}
	if l == p.h {
		lf = f.leaf(i)
		for k, m = 0, 0; k < lf.n && lf.cnt[k] < cnt; k++ {
			if lf.cnt[k] > m {
				m = lf.cnt[k]
			}
		}
		before[l] = m
	}
	if l < p.h || k == lf.n {
		for l, i = 0, hd.root; l < p.h; l++ {
			in := f.inner(i)
			p.node[l] = i
			p.pos[l] = in.n - 1
			i = in.child[in.n-1]
		}
		p.node[p.h] = i
		lf = f.leaf(i)
		k = lf.n - 1
		if k < 0 || lf.idx[k]+lf.cnt[k] != length {
			return length
		}
		idx := lf.idx[k]
		f.take(&p, k, cnt)
		return idx
	}
	p.node[p.h] = i
	p.pos[p.h] = k
	idx, c := lf.idx[k], lf.cnt[k]
	var now, j int64
	switch {
	case c > cnt:
		now = c - cnt
		lf.idx[k] = idx + cnt
		lf.cnt[k] = now
		j = k + 1
	case lf.n > 1 || p.h == 0:
		gsSpClose(lf, k)
		now = 0
		j = k
	default:
		f.delete(&p, k)
		return idx
	}
	// As fix, with what the descent noted: the count on the path at level
	// l went from c to now, and the node's other entries are the ones
	// before, smaller than cnt, and those from j on, which need scanning
	// only where c was the node's largest.
	for l = p.h; l > 0; l-- {
		in := f.inner(p.node[l-1])
		e := p.pos[l-1]
		mx := in.max[e]
		nm := mx
		key := f.key(p.node[l], l == p.h)
		if c == mx {
			var v []int64
			if l == p.h {
				v = lf.cnt[:lf.n]
			} else {
				below := f.inner(p.node[l])
				v = below.max[:below.n]
			}
			nm = before[l]
			if now > nm {
				nm = now
			}
			for ; j < int64(len(v)) && nm < c; j++ {
				if v[j] > nm {
					nm = v[j]
				}
			}
		}
		if in.key[e] == key && nm == mx {
			break
		}
		in.key[e] = key
		in.max[e] = nm
		c = mx
		now = nm
		j = e + 1
	}
	return idx
}

// gsSpNear is the spans around a run [idx, end): the leaf holding the last
// span that starts at or before idx (at position k, or -1), and the span
// after that one, where it follows in the same leaf, or where it starts
// the next leaf and at end (nl and nk, nl nil where there is none). Where
// that span is in the next leaf, the path to it is the other path near
// fills (inQ).
type gsSpNear struct {
	lf, nl *gsSpLeaf
	k, nk  int64
	inQ    bool
}

// near finds the spans around the run [idx, end), with the path to the one
// before it in p. A next leaf's first index is the key of the entry that
// leads to it, so a path to it, in q, is only made where its first span
// touches the run.
func (f *gsSpans) near(p, q *gsSpPath, idx, end int64) gsSpNear {
	var s gsSpNear
	s.k = f.find(p, idx)
	s.lf = f.leaf(p.node[p.h])
	s.nk = -1
	if s.k+1 < s.lf.n {
		s.nl = s.lf
		s.nk = s.k + 1
		return s
	}
	l := p.h - 1
	for l >= 0 && p.pos[l]+1 >= f.inner(p.node[l]).n {
		l--
	}
	if l < 0 || f.inner(p.node[l]).key[p.pos[l]+1] != end {
		return s
	}
	q.h = p.h
	copy(q.node[:l+1], p.node[:l+1])
	copy(q.pos[:l+1], p.pos[:l+1])
	for q.pos[l]++; l < q.h; l++ {
		q.node[l+1] = f.inner(q.node[l]).child[q.pos[l]]
		q.pos[l+1] = 0
	}
	s.inQ = true
	s.nl = f.leaf(q.node[q.h])
	s.nk = 0
	return s
}

// release puts [idx, idx + cnt) back on the freelist beside the spans near
// found around it, merged with the ones it touches.
func (f *gsSpans) release(p, q *gsSpPath, s *gsSpNear, idx, cnt int64) {
	lf, nl := s.lf, s.nl
	k, nk := s.k, s.nk
	np := p
	if s.inQ {
		np = q
	}
	prev := k >= 0 && lf.idx[k]+lf.cnt[k] == idx
	next := nl != nil && idx+cnt == nl.idx[nk]
	switch {
	case prev:
		c := lf.cnt[k]
		m := c + cnt
		if next {
			m += nl.cnt[nk]
		}
		lf.cnt[k] = m
		// The merged count is larger than both it replaces, so one fix
		// covers the span after leaving the leaf as well.
		if next && nl == lf {
			gsSpClose(lf, nk)
		}
		f.fix(p, p.h, c, m)
		if next && nl != lf {
			f.delete(np, nk)
		}
	case next:
		c := nl.cnt[nk]
		nl.idx[nk] = idx
		nl.cnt[nk] = c + cnt
		f.fix(np, np.h, c, c+cnt)
	default:
		f.insert(p, k+1, idx, cnt)
	}
}

// GsSpansFree puts [idx, idx + cnt) back on the freelist, merged with the
// spans it touches (gs_spans_free): free_slice, and what realloc_slice lets
// go of. The first span freed lays out the freelist's header.
func GsSpansFree(base unsafe.Pointer, n *int64, top *unsafe.Pointer, idx, cnt int64) {
	if cnt <= 0 {
		return
	}
	f := gsSpans{base, n, top}
	if *n == 0 {
		hd := f.head()
		*n = 1
		*top = unsafe.Add(base, GsSpanNode)
		hd.chain = 0
		hd.height = 0
		hd.root = f.newNode()
		f.leaf(hd.root).n = 0
	}
	var p, q gsSpPath
	s := f.near(&p, &q, idx, idx+cnt)
	f.release(&p, &q, &s, idx, cnt)
}

// GsSpansRegrow is where the non-empty slice [idx, idx + ol), which does
// not end the array, starts once realloc_slice grows it to cnt elements
// (gs_spans_regrow). That is in place where a free span starts where the
// slice ends and holds the difference or reaches the end of the array;
// otherwise the slice's elements go back on the freelist and the run goes
// where GsSpansAlloc places one of cnt elements, and the caller copies the
// elements there. The span after the slice is the one after the last span
// before it, so one descent finds both what growth takes from and what the
// elements merge with.
func GsSpansRegrow(base unsafe.Pointer, n *int64, top *unsafe.Pointer, length, idx, ol, cnt int64) int64 {
	if *n == 0 {
		GsSpansFree(base, n, top, idx, ol)
		return GsSpansAlloc(base, n, top, length, cnt)
	}
	f := gsSpans{base, n, top}
	end := idx + ol
	var p, q gsSpPath
	s := f.near(&p, &q, idx, end)
	if s.nl != nil && s.nl.idx[s.nk] == end &&
		(s.nl.cnt[s.nk] >= cnt-ol || end+s.nl.cnt[s.nk] == length) {
		np := &p
		if s.inQ {
			np = &q
		}
		f.take(np, s.nk, cnt-ol)
		return idx
	}
	f.release(&p, &q, &s, idx, ol)
	return GsSpansAlloc(base, n, top, length, cnt)
}

)GSGO",
R"GSGO(// ---------------------------------------------------------------------------
// Variable-size values (§3.6, docs/implementation.md §6.1): a length prefix
// or a varint is ULEB128, a signed varint value zigzag-encoded on top, as
// runtime/go/goose_bytes.go has them (keep the two the same).

// GsUlebRead is the ULEB128 at p (GS_ULEB_READ).
func GsUlebRead(p unsafe.Pointer) int64 {
	var v uint64
	for shift := uint(0); ; shift += 7 {
		b := *(*byte)(p)
		v |= uint64(b&0x7f) << shift
		if b < 0x80 {
			return int64(v)
		}
		p = unsafe.Add(p, 1)
	}
}

// GsUlebSize is the size in bytes of the ULEB128 at p (GS_ULEB_SIZE).
func GsUlebSize(p unsafe.Pointer) int64 {
	n := int64(1)
	for *(*byte)(unsafe.Add(p, n-1)) >= 0x80 {
		n++
	}
	return n
}

// GsUlebWrite writes v as a ULEB128 at p, returning its size in bytes,
// at most 10.
func GsUlebWrite(p unsafe.Pointer, v uint64) int64 {
	n := int64(0)
	for v >= 0x80 {
		*(*byte)(unsafe.Add(p, n)) = byte(v) | 0x80
		v >>= 7
		n++
	}
	*(*byte)(unsafe.Add(p, n)) = byte(v)
	return n + 1
}

// GsZigRead is the zigzag-encoded varint at p (gs_zig_read).
func GsZigRead(p unsafe.Pointer) int64 {
	u := uint64(GsUlebRead(p))
	return int64(u>>1) ^ -int64(u&1)
}

// GsZigWrite writes v as a zigzag-encoded varint at p, returning its size
// in bytes (gs_zig_write).
func GsZigWrite(p unsafe.Pointer, v int64) int64 {
	return GsUlebWrite(p, uint64(v)<<1^uint64(v>>63))
}

// GsLenCheck is the check of a count that goes into a length field: it
// must fit the field's storage (EmitLenCheck).
func GsLenCheck(n int64, max uint64) {
	if uint64(n) > max {
		GsPanic("array length exceeds storage range")
	}
}

// gsUlebCheck reads the ULEB128 at byte q of the n bytes at p, for the
// verifier (gs_uleb_check): the value and its size in bytes, which is 0
// where the varint runs past n or past ten bytes, carries payload bits
// above the 64th, or is not in its shortest form. Nothing past the n bytes
// is formed into an address.
func gsUlebCheck(p unsafe.Pointer, q, n int64) (v uint64, k int64) {
	for shift := uint(0); ; shift += 7 {
		if q+k >= n {
			return 0, 0
		}
		b := *(*byte)(unsafe.Add(p, q+k))
		k++
		if shift > 63 || (shift == 63 && b&0x7e != 0) {
			return 0, 0
		}
		v |= uint64(b&0x7f) << shift
		if b < 0x80 {
			if shift != 0 && b == 0 { // A redundant high zero group.
				return 0, 0
			}
			return v, k
		}
	}
}

// gsZigCheck is gsUlebCheck for a zigzag-encoded varint (gs_zig_check).
func gsZigCheck(p unsafe.Pointer, q, n int64) (int64, int64) {
	u, k := gsUlebCheck(p, q, n)
	return int64(u>>1) ^ -int64(u&1), k
}

// The verifier's element starts, one bit per image byte (GS_BM_SET,
// GS_BM_GET), in scratch on a data stack.
func gsBmSet(bm unsafe.Pointer, i int64) {
	*(*byte)(unsafe.Add(bm, i>>3)) |= 1 << (i & 7)
}

func gsBmGet(bm unsafe.Pointer, i int64) bool {
	return *(*byte)(unsafe.Add(bm, i>>3))>>(i&7)&1 != 0
}

// gsIsLE says whether the host is little-endian, which serialization needs
// (gs_is_le).
func gsIsLE() bool {
	x := uint16(1)
	return *(*byte)(unsafe.Pointer(&x)) == 1
}

// ---------------------------------------------------------------------------
// The C functions std and math bind as extern fns (runtime_ext.h), where
// Go's own library has no equivalent. A ByteSet's members, a bool[256],
// lead it.

// GsFindAny is std's find_any: the first i with s[i] in set, -1 if none
// (gs_find_any).
func GsFindAny(s GsSlice, set unsafe.Pointer) int64 {
	m := (*[256]bool)(set)
	for i := int64(0); i < s.Len; i++ {
		if m[*(*byte)(unsafe.Add(s.Data, i))] {
			return i
		}
	}
	return -1
}

// GsFindPair is std's byte_set_pair: the first i with s[i] in a and
// s[i+d] in b, of those with i + d < s.Len; -1 if none (gs_find_pair).
func GsFindPair(s GsSlice, a unsafe.Pointer, d int64, b unsafe.Pointer) int64 {
	ma, mb := (*[256]bool)(a), (*[256]bool)(b)
	for i := int64(0); i+d < s.Len; i++ {
		if ma[*(*byte)(unsafe.Add(s.Data, i))] && mb[*(*byte)(unsafe.Add(s.Data, i+d))] {
			return i
		}
	}
	return -1
}

// GsFrexp is C's frexp: the fraction, its exponent through e.
func GsFrexp(x float64, e *int32) float64 {
	f, n := math.Frexp(x)
	*e = int32(n)
	return f
}

// GsMulAddF32 is math's mul_add at f32 (gs_mul_addf), x * y + z rounded
// once, as math.FMA is at f64 (gs_mul_add): Go has FMA wherever it runs,
// as C has it on arm64. The f32 product is exact in an f64, and so is the
// sum but where its rounding to an f64 lands halfway between two f32s,
// which is then moved toward what the rounding dropped, so that the one
// rounding to f32 is the exact sum's (musl's fmaf).
func GsMulAddF32(x, y, z float32) float32 {
	xy := float64(float64(x) * float64(y))
	r := xy + float64(z)
	u := math.Float64bits(r)
	if u&0x1fffffff != 0x10000000 || u>>52&0x7ff == 0x7ff || (r-xy == float64(z) && r-float64(z) == xy) {
		return float32(r)
	}
	var err float64
	neg := u>>63 != 0
	if neg == (float64(z) > xy) {
		err = xy - r + float64(z)
	} else {
		err = float64(z) - r + xy
	}
	if neg == (err < 0) {
		u++
	} else {
		u--
	}
	return float32(math.Float64frombits(u))
}
)GSGO",
};

// The OS primitives behind stdlib/os.goose (runtime_os.h), written into a
// file whose program calls one of them (CodeGenGo::usesos), with the imports
// they take besides the runtime's own: bytes, io, path/filepath, strings and
// time.
inline const char *const go_runtime_os[] = {
R"GSGO(
// ---------------------------------------------------------------------------
// The OS primitives behind stdlib/os.goose (runtime_os.h), as the extern
// fns there name them, gs_os_read_file being GsOsReadFile. A path, or an
// environment variable's name, is UTF-8 bytes, fewer than gsOsPathMax of
// them and none of them NUL, where an OS call would end it: any other is
// refused, since read another way it could name another file. Text goes
// into the builders the program hands them (GsBldAppend), and nothing on a
// failure.

const gsOsPathMax = 4096

// gsOsPath is the Go string of a path, false where it is refused
// (gs_os_native).
func gsOsPath(s GsSlice) (string, bool) {
	b := GsSliceToBytes(s)
	if len(b) >= gsOsPathMax {
		return "", false
	}
	for _, c := range b {
		if c == 0 {
			return "", false
		}
	}
	return string(b), true
}

// gsOsKind is what a path names: 0 nothing that can be reached, 1 something
// other than a directory, 2 a directory (gs_os_kind).
func gsOsKind(p string) int {
	st, err := os.Stat(p)
	if err != nil {
		return 0
	}
	if st.IsDir() {
		return 2
	}
	return 1
}

// gsBldReadAll appends all of r to out: false on a read error, with out as
// it was.
func gsBldReadAll(gs *GsStackBlock, out GsRref, r io.Reader) bool {
	n0, top0 := out.Hdr.Len, gs.Stks[out.Stk]
	buf := make([]byte, 65536)
	for {
		n, err := r.Read(buf)
		GsBldAppend(gs, out, buf[:n])
		if err == io.EOF {
			return true
		}
		if err != nil {
			out.Hdr.Len, gs.Stks[out.Stk] = n0, top0
			return false
		}
	}
}

// GsOsReadFile appends the whole file to out.
func GsOsReadFile(gs *GsStackBlock, path GsSlice, out GsRref) bool {
	p, ok := gsOsPath(path)
	if !ok {
		return false
	}
	f, err := os.Open(p)
	if err != nil {
		return false
	}
	defer f.Close()
	return gsBldReadAll(gs, out, f)
}

// GsOsWriteFile and GsOsAppendFile write data to a file, which they create
// where there is none.
func GsOsWriteFile(path, data GsSlice) bool { return gsOsPutFile(path, data, os.O_TRUNC) }

func GsOsAppendFile(path, data GsSlice) bool { return gsOsPutFile(path, data, os.O_APPEND) }

func gsOsPutFile(path, data GsSlice, mode int) bool {
	p, ok := gsOsPath(path)
	if !ok {
		return false
	}
	f, err := os.OpenFile(p, os.O_WRONLY|os.O_CREATE|mode, 0666)
	if err != nil {
		return false
	}
	_, err = f.Write(GsSliceToBytes(data))
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	return err == nil
}

// GsOsWriteFileAtomic writes data to a new file beside path, flushes it to
// the disk and renames it over path, so that path holds its old contents or
// all of the new ones whatever happens meanwhile; on a failure the new file
// is removed. Its name is path's with a random tag and .tmp after, created
// only where nothing has that name already, and it takes the permissions
// of the file it replaces.
func GsOsWriteFileAtomic(path, data GsSlice) bool {
	p, ok := gsOsPath(path)
	if !ok || p == "" {
		return false
	}
	for tries := 0; tries < 8; tries++ {
		t := p + "." + gsHex16(GsOsRandomU64()) + ".tmp"
		f, err := os.OpenFile(t, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0666)
		if os.IsExist(err) {
			continue
		}
		if err != nil {
			return false
		}
		if st, err := os.Stat(p); err == nil {
			ok = f.Chmod(st.Mode().Perm()) == nil
		}
		if ok {
			_, err = f.Write(GsSliceToBytes(data))
			ok = err == nil
		}
		ok = ok && f.Sync() == nil
		if f.Close() != nil {
			ok = false
		}
		ok = ok && syscall.Rename(t, p) == nil
		if ok {
			gsOsSyncDir(p)
		} else {
			_ = syscall.Unlink(t)
		}
		return ok
	}
	return false
}

// gsHex16 is v in 16 hex digits.
func gsHex16(v uint64) string {
	var b [16]byte
	for i := range b {
		b[i] = "0123456789abcdef"[v>>(60-4*uint(i))&15]
	}
	return string(b[:])
}

// gsOsSyncDir puts a rename on the disk, flushing the directory that holds
// the new name; best effort, since the rename has happened either way.
func gsOsSyncDir(p string) {
	d, err := os.Open(filepath.Dir(p))
	if err != nil {
		return
	}
	_ = d.Sync()
	_ = d.Close()
}

// GsOsFileExists says whether path names something that is not a directory.
func GsOsFileExists(path GsSlice) bool {
	p, ok := gsOsPath(path)
	return ok && gsOsKind(p) == 1
}

// GsOsRemoveFile removes a file, and nothing else (unlink, not remove).
func GsOsRemoveFile(path GsSlice) bool {
	p, ok := gsOsPath(path)
	return ok && syscall.Unlink(p) == nil
}

// GsOsRenameFile replaces to, if there is one, in one step: to names the old
// file or the moved one, never neither.
func GsOsRenameFile(from, to GsSlice) bool {
	f, ok := gsOsPath(from)
	t, ok2 := gsOsPath(to)
	if !ok || !ok2 || syscall.Rename(f, t) != nil {
		return false
	}
	gsOsSyncDir(t)
	return true
}

// GsOsMakeDir is true where path is a directory afterwards, one that was
// there already included.
func GsOsMakeDir(path GsSlice) bool {
	p, ok := gsOsPath(path)
	if !ok {
		return false
	}
	return os.Mkdir(p, 0777) == nil || gsOsKind(p) == 2
}

func GsOsIsDir(path GsSlice) bool {
	p, ok := gsOsPath(path)
	return ok && gsOsKind(p) == 2
}

// GsOsRemoveDir removes an empty directory.
func GsOsRemoveDir(path GsSlice) bool {
	p, ok := gsOsPath(path)
	return ok && syscall.Rmdir(p) == nil
}

// GsOsListDir appends the name of every entry of a directory but . and ..,
// each followed by a newline, in bytewise order, so that a listing does not
// depend on the file system. A name holding a newline cannot be a line of
// it, and is left out.
func GsOsListDir(gs *GsStackBlock, path GsSlice, out GsRref) bool {
	p, ok := gsOsPath(path)
	if !ok || p == "" {
		return false
	}
	entries, err := os.ReadDir(p)
	if err != nil {
		return false
	}
	for _, e := range entries {
		name := e.Name()
		if strings.IndexByte(name, '\n') >= 0 {
			continue
		}
		GsBldAppendString(gs, out, name)
		GsBldAppendString(gs, out, "\n")
	}
	return true
}

// The standard streams. Go's are unbuffered, as print's writes are, so
// flushing has nothing to do; stdin is read through a buffer of its own,
// which read_line and read_stdin share, as C's share stdio's.
func GsOsWriteStdout(s GsSlice) { _, _ = os.Stdout.Write(GsSliceToBytes(s)) }

func GsOsWriteStderr(s GsSlice) { _, _ = os.Stderr.Write(GsSliceToBytes(s)) }

func GsOsFlushStdout() {}

var gsStdin struct {
	mu       sync.Mutex
	buf      []byte
	pos, end int
}

// gsStdinFill reads more of stdin into its buffer: false at its end.
func gsStdinFill() bool {
	if gsStdin.buf == nil {
		gsStdin.buf = make([]byte, 65536)
	}
	n, _ := os.Stdin.Read(gsStdin.buf)
	gsStdin.pos, gsStdin.end = 0, n
	return n > 0
}

// GsOsReadLine appends one line of stdin, without its newline or a carriage
// return before it: false at the end of the input.
func GsOsReadLine(gs *GsStackBlock, out GsRref) bool {
	gsStdin.mu.Lock()
	defer gsStdin.mu.Unlock()
	if gsStdin.pos == gsStdin.end && !gsStdinFill() {
		return false
	}
	start := out.Hdr.Len
	for {
		chunk := gsStdin.buf[gsStdin.pos:gsStdin.end]
		if i := bytes.IndexByte(chunk, '\n'); i >= 0 {
			GsBldAppend(gs, out, chunk[:i])
			gsStdin.pos += i + 1
			break
		}
		GsBldAppend(gs, out, chunk)
		gsStdin.pos = gsStdin.end
		if !gsStdinFill() {
			break
		}
	}
	if out.Hdr.Len > start && *(*byte)(unsafe.Add(out.Hdr.Base, out.Hdr.Len-1)) == '\r' {
		out.Hdr.Len--
		gs.Stks[out.Stk] = unsafe.Add(gs.Stks[out.Stk], -1)
	}
	return true
}

// GsOsReadStdin appends everything until the end of the input.
func GsOsReadStdin(gs *GsStackBlock, out GsRref) {
	gsStdin.mu.Lock()
	defer gsStdin.mu.Unlock()
	for gsStdin.pos < gsStdin.end || gsStdinFill() {
		GsBldAppend(gs, out, gsStdin.buf[gsStdin.pos:gsStdin.end])
		gsStdin.pos = gsStdin.end
	}
}

// The program's arguments, the program itself first, and its environment.
func GsOsArgCount() int64 { return int64(len(os.Args)) }

func GsOsArg(gs *GsStackBlock, i int64, out GsRref) {
	if i >= 0 && i < int64(len(os.Args)) {
		GsBldAppendString(gs, out, os.Args[i])
	}
}

func GsOsGetenv(gs *GsStackBlock, name GsSlice, out GsRref) bool {
	n, ok := gsOsPath(name)
	if !ok {
		return false
	}
	v, ok := os.LookupEnv(n)
	if ok {
		GsBldAppendString(gs, out, v)
	}
	return ok
}

// GsOsResourceDir appends the application's resource directory, with its
// final separator: the directory of the running executable (Resources/ in a
// macOS .app), or the entry source's for `goose run`, as a JIT run has it
// (gsJitResourceDir).
func GsOsResourceDir(gs *GsStackBlock, out GsRref) bool {
	dir := gsJitResourceDir
	if dir == "" {
		exe, err := os.Executable()
		if err == nil {
			exe, err = filepath.EvalSymlinks(exe)
		}
		if err != nil || len(exe) >= gsOsPathMax {
			return false
		}
		dir = exe[:strings.LastIndexByte(exe, '/')+1]
		if dir == "" {
			return false
		}
		if runtime.GOOS == "darwin" && strings.HasSuffix(dir, ".app/Contents/MacOS/") {
			dir = dir[:len(dir)-len("MacOS/")] + "Resources/"
		}
	}
	GsBldAppendString(gs, out, dir)
	return true
}

// Time: the wall clock in nanoseconds since the Unix epoch, a monotonic
// clock for intervals, and a sleep of whole milliseconds.
var gsClockStart = time.Now()

func GsOsTimeNs() int64 { return time.Now().UnixNano() }

func GsOsClockNs() int64 { return int64(time.Since(gsClockStart)) }

func GsOsSleepMs(ms int64) {
	if ms <= 0 {
		return
	}
	if ms > math.MaxInt64/int64(time.Millisecond) {
		ms = math.MaxInt64 / int64(time.Millisecond)
	}
	time.Sleep(time.Duration(ms) * time.Millisecond)
}

// GsOsRandomU64 is entropy for seeding a PRNG, not for cryptography:
// /dev/urandom's, or else a mix of the clocks and of which call this is.
var gsRandomCalls struct {
	mu sync.Mutex
	n  uint64
}

func gsMix64(x uint64) uint64 {
	x += 0x9e3779b97f4a7c15
	x = (x ^ x>>30) * 0xbf58476d1ce4e5b9
	x = (x ^ x>>27) * 0x94d049bb133111eb
	return x ^ x>>31
}

func GsOsRandomU64() uint64 {
	if f, err := os.Open("/dev/urandom"); err == nil {
		var b [8]byte
		n, _ := io.ReadFull(f, b[:])
		_ = f.Close()
		if n == 8 {
			var v uint64
			for i := range b {
				v |= uint64(b[i]) << (8 * uint(i))
			}
			return v
		}
	}
	gsRandomCalls.mu.Lock()
	gsRandomCalls.n++
	count := gsRandomCalls.n
	gsRandomCalls.mu.Unlock()
	seed := gsMix64(uint64(GsOsTimeNs()) ^ uint64(GsOsClockNs()))
	seed = gsMix64(seed ^ uint64(os.Getpid()))
	return gsMix64(seed ^ count)
}

// GsTrimMemory is os.trim_memory: the complete pages above each stack's
// top in gs, its fixed-frame stack's included, given back to the OS, their
// contents read as zeros if they are used again; false where the OS refused
// (gs_trim_memory).
func GsTrimMemory(gs *GsStackBlock) bool {
	page := uintptr(syscall.Getpagesize())
	ok := true
	trim := func(r []byte, top unsafe.Pointer, usable uintptr) {
		if r == nil {
			return
		}
		from := (uintptr(top) - uintptr(unsafe.Pointer(unsafe.SliceData(r))) + page - 1) &^ (page - 1)
		if from < usable && syscall.Madvise(r[from:usable], syscall.MADV_DONTNEED) != nil {
			ok = false
		}
	}
	for k, r := range gs.regions {
		trim(r, gs.Stks[k], uintptr(gs.Cap[k]))
	}
	trim(gs.frame, gs.Frame, uintptr(len(gs.frame)))
	return ok
}
)GSGO",
};

// Workers and typed queues (§11.2, runtime_threads.h), written into a file
// whose program spawns a worker, waits for one, asks for hardware_threads()
// or uses a queue (CodeGenGo::usesthreads), with the import they take
// besides the runtime's own: time.
inline const char *const go_runtime_threads[] = {
R"GSGO(
// ---------------------------------------------------------------------------
// Workers and typed queues (§11.2), as runtime_threads.h has them. A worker
// is a goroutine running its thread program on a block of its own, with a
// program instance of its own: the globals its program uses, copied from
// the spawning instance at the spawn with its arguments (GsThreadSpawn). A
// queue is a FIFO of the images of the values put, under a mutex (GsQueue).
// This is the one part of the runtime that allocates on Go's heap: what a
// queue holds, and a worker's record.

// GsHardwareThreads is hardware_threads(): the processors Go runs goroutines
// on at once, but no more than the workers the data stack budget holds
// beside the main program (gsRtStart), so that a pool sized by it never runs
// the program out of address space.
func GsHardwareThreads() int64 {
	n := int64(runtime.GOMAXPROCS(0))
	if n > gsThreadCap {
		n = gsThreadCap
	}
	return n
}

// The workers (gs_threads): ids increase from 0 and are never reused, and
// only a running worker has a record, the channel its end closes. Of those
// that ended, the runtime keeps a bit for each contained worker that
// aborted, from which thread_wait answers, and the message of each such
// abort until a wait takes it.
var gsThreads struct {
	mu      sync.Mutex
	next    int64
	running map[int64]chan struct{}
	aborted []uint64
	msgs    map[int64]string
}

// GsThreadSpawn starts a worker running run on a block of its own and
// returns its id (gs_thread_start): `stacks` data stacks, which the worker
// reserves as it starts, a program instance of glsize zeroed bytes at its
// Gl, and a copy of the n-byte spawn image at args, taken now, which run is
// handed and the worker owns, outside the Go heap as the stacks are. In a contained worker an abort ends run early, its message
// kept for thread_wait (§11.2). Whichever way it ends, the worker releases
// all it holds before thread_wait sees it ended.
func GsThreadSpawn(run func(gs *GsStackBlock, p unsafe.Pointer), stacks int, glsize uintptr,
	args unsafe.Pointer, n int64, contained bool) int64 {
	if n < 0 {
		GsPanic("invalid thread argument size")
	}
	// The instance and the image, in one mapping the worker's block owns.
	off := (int64(glsize) + 7) &^ 7
	inst := gsMapPrivate(off + n)
	p := unsafe.Add(unsafe.Pointer(unsafe.SliceData(inst)), off)
	copy(unsafe.Slice((*byte)(p), n), unsafe.Slice((*byte)(args), n))
	done := make(chan struct{})
	gsThreads.mu.Lock()
	if gsThreads.next == math.MaxInt64 {
		gsThreads.mu.Unlock()
		GsPanic("thread id space exhausted")
	}
	id := gsThreads.next
	gsThreads.next++
	if gsThreads.running == nil {
		gsThreads.running = map[int64]chan struct{}{}
	}
	gsThreads.running[id] = done
	gsThreads.mu.Unlock()
	go gsThreadMain(inst, stacks, run, p, contained, id, done)
	return id
}

// gsThreadMain is a worker's goroutine (gs_thread_main): its block, whose
// stacks it reserves as it starts (gs_stack_block), its thread program,
// then the release of everything it holds, and only then its end
// published.
func gsThreadMain(inst []byte, stacks int, run func(*GsStackBlock, unsafe.Pointer), p unsafe.Pointer,
	contained bool, id int64, done chan struct{}) {
	// A fault in a guard gap reaches the worker's recover, as main's does.
	debug.SetPanicOnFault(true)
	var gs *GsStackBlock
	msg, aborted := gsRunWorker(func() {
		gs = NewStackBlock(stacks, gsStackReserve, gsStackGap)
		gs.inst, gs.Gl, gs.worker = inst, unsafe.Pointer(unsafe.SliceData(inst)), id+1
		run(gs, p)
	}, contained)
	_ = gs.Close()
	gsThreads.mu.Lock()
	delete(gsThreads.running, id)
	if aborted {
		for int64(len(gsThreads.aborted)) <= id/64 {
			gsThreads.aborted = append(gsThreads.aborted, 0)
		}
		gsThreads.aborted[id/64] |= 1 << uint(id%64)
		if gsThreads.msgs == nil {
			gsThreads.msgs = map[int64]string{}
		}
		gsThreads.msgs[id] = msg
	}
	close(done)
	gsThreads.mu.Unlock()
}

// gsRunWorker runs a worker's thread program, f. An abort in a contained one
// returns here, with its message (gs_run_contained); anything else ends the
// process, as it does on main's (gsEndOnPanic): an abort in a worker that is
// not contained, the runtime's own failures, which have no location in the
// program, a fault in a guard gap.
func gsRunWorker(f func(), contained bool) (msg string, aborted bool) {
	defer func() {
		if r := recover(); r != nil {
			if e, ok := r.(*GsError); ok && contained && e.File != "" {
				msg, aborted = e.Text(), true
				return
			}
			gsEndOnPanic(r)
		}
	}()
	f()
	return "", false
}

// GsThreadWait waits until the worker id has ended and released all it held
// (gs_thread_wait): false for a contained worker that aborted. With take,
// the first such wait for that worker takes the abort's message, where
// later ones, and any for a worker that returned, get "". gs is the block
// of the thread program waiting.
func GsThreadWait(gs *GsStackBlock, id int64, take bool, file string, line int) (bool, string) {
	gsThreads.mu.Lock()
	if id < 0 || id >= gsThreads.next {
		gsThreads.mu.Unlock()
		GsAbort(GsEThreadID, file, line)
	}
	if id == gs.worker-1 {
		gsThreads.mu.Unlock()
		GsAbort(GsEThreadSelf, file, line)
	}
	done := gsThreads.running[id]
	gsThreads.mu.Unlock()
	if done != nil {
		<-done
	}
	gsThreads.mu.Lock()
	defer gsThreads.mu.Unlock()
	if id/64 >= int64(len(gsThreads.aborted)) || gsThreads.aborted[id/64]>>uint(id%64)&1 == 0 {
		return true, ""
	}
	if !take {
		return false, ""
	}
	m := gsThreads.msgs[id]
	delete(gsThreads.msgs, id)
	return false, m
}

// GsKeep is s, or where it views memory of gs's own -- a global of the
// program instance a call of an export fn ran on -- a copy of it on Go's
// heap: an export's result crosses to its caller once the call's block is
// back in the pool, where the next call takes its memory.
func GsKeep[T any](gs *GsStackBlock, s []T) []T {
	if len(s) == 0 || !gs.owns(unsafe.Pointer(unsafe.SliceData(s))) {
		return s
	}
	return append([]T(nil), s...)
}

// owns says whether p points into memory of the block's own: a data stack's
// region, its fixed-frame stack, its program instance.
func (gs *GsStackBlock) owns(p unsafe.Pointer) bool {
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

// GsQueue is the queue of one element type (§11.2): the image of each value
// put, in order, and how many there are. The zero GsQueue is an empty one
// with no limit, which is how the program declares each of its queues.
type GsQueue struct {
	mu      sync.Mutex
	head    *gsQNode
	tail    *gsQNode
	count   int64
	limit   int64           // 0 until qlimit gives one.
	waiters []chan struct{} // The receivers waiting, the first first: a put wakes one.
}

type gsQNode struct {
	next *gsQNode
	b    []byte
}

// GsQPut queues a copy of the n bytes at p (gs_qput), taken before the lock
// is: false, queueing nothing, where the queue holds its limit of messages.
// It never waits.
func GsQPut(q *GsQueue, p unsafe.Pointer, n int64) bool {
	nd := &gsQNode{b: make([]byte, n)}
	copy(nd.b, unsafe.Slice((*byte)(p), n))
	q.mu.Lock()
	if q.limit > 0 && q.count >= q.limit {
		q.mu.Unlock()
		return false
	}
	q.count++
	if q.tail != nil {
		q.tail.next = nd
	} else {
		q.head = nd
	}
	q.tail = nd
	if len(q.waiters) > 0 {
		close(q.waiters[0])
		q.unwait(q.waiters[0])
	}
	q.mu.Unlock()
	return true
}

// GsQPutV queues the image of v, a fixed-size value, n bytes of it.
func GsQPutV[T any](q *GsQueue, v T, n int64) bool { return GsQPut(q, unsafe.Pointer(&v), n) }

// GsQValue is the fixed-size value of type T whose image b is: all zero
// bytes past what b holds, all of them for a value of no bytes or a miss.
func GsQValue[T any](b []byte) (v T) {
	copy(unsafe.Slice((*byte)(unsafe.Pointer(&v)), unsafe.Sizeof(v)), b)
	return
}

// take unlinks the first message, under the lock.
func (q *GsQueue) take() []byte {
	nd := q.head
	q.head = nd.next
	if q.head == nil {
		q.tail = nil
	}
	q.count--
	return nd.b
}

// unwait drops the receiver w from those waiting, where a put has not
// already, under the lock.
func (q *GsQueue) unwait(w chan struct{}) {
	for i, x := range q.waiters {
		if x == w {
			copy(q.waiters[i:], q.waiters[i+1:])
			q.waiters[len(q.waiters)-1] = nil
			q.waiters = q.waiters[:len(q.waiters)-1]
			return
		}
	}
}

// GsQGet takes the first message, waiting for one (gs_qget).
func GsQGet(q *GsQueue) []byte {
	q.mu.Lock()
	for q.head == nil {
		w := make(chan struct{})
		q.waiters = append(q.waiters, w)
		q.mu.Unlock()
		<-w
		q.mu.Lock()
	}
	b := q.take()
	q.mu.Unlock()
	return b
}

// GsQWait takes the first message, waiting at most ms milliseconds for one,
// on a monotonic clock (gs_qwait): false where none came. Zero or less does
// not wait, which is qpoll (gs_qpoll).
func GsQWait(q *GsQueue, ms int64) ([]byte, bool) {
	// The deadline in nanoseconds cannot overflow.
	if ms > math.MaxInt64/2000000 {
		ms = math.MaxInt64 / 2000000
	}
	var deadline time.Time
	if ms > 0 {
		deadline = time.Now().Add(time.Duration(ms) * time.Millisecond)
	}
	q.mu.Lock()
	for q.head == nil && ms > 0 {
		left := time.Until(deadline)
		if left <= 0 {
			break
		}
		w := make(chan struct{})
		q.waiters = append(q.waiters, w)
		q.mu.Unlock()
		t := time.NewTimer(left)
		select {
		case <-w:
		case <-t.C:
		}
		t.Stop()
		q.mu.Lock()
		q.unwait(w)
	}
	if q.head == nil {
		q.mu.Unlock()
		return nil, false
	}
	b := q.take()
	q.mu.Unlock()
	return b, true
}

// GsQLimit gives the queue a limit of n messages (gs_qlimit): one below what
// it holds keeps them, and puts fail until fewer remain. A limit of zero or
// less aborts.
func GsQLimit(q *GsQueue, n int64, file string, line int) {
	if n <= 0 {
		GsAbort(GsEQBound, file, line)
	}
	q.mu.Lock()
	q.limit = n
	q.mu.Unlock()
}
)GSGO",
};

}  // namespace goose
