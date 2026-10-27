// Goose compiler — the runtime the Go backend writes into every file it
// generates (CodeGenGo::Source, codegen_go.h), after the package clause, the
// imports (math, os, runtime/debug, strconv, syscall, unsafe, each of which
// it uses) and the configuration constants gsStackReserve and gsStackGap. It
// is Go source as it is written out, in pieces under the 16 KB a string
// literal may have in MSVC; edit it here, and check the result with go vet
// on a generated file (test/run_go_tests.py does, for every program it
// compiles).
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
	Stks    []unsafe.Pointer // Current stack tops, indexed by gs_sp + k.
	Base    []unsafe.Pointer // Where each stack's region starts.
	Cap     []int64          // Each region's usable bytes, its guard gap not included.
	regions [][]byte
	reserve int64
	gap     int64
}

// The smallest a region gets before a refusal is fatal, and the usable size
// new regions currently get: it halves, in whole pages, each time the
// platform refuses one (gs_reserve_region).
const gsRegionMin = 1 << 20

var gsRegionUsable int64

// The blocks NewStackBlock has made, in which a write that would run past a
// region's end finds the region (gsOverflow).
var gsBlocks []*GsStackBlock

// NewStackBlock reserves numStacks data stacks of reserve usable bytes each,
// with gap bytes of PROT_NONE guard behind each one. A region is whole pages
// (gs_rt_start).
func NewStackBlock(numStacks int, reserve, gap int64) *GsStackBlock {
	if reserve < int64(syscall.Getpagesize()) {
		GsPanic("the data stack reservation (GS_STACK_RESERVE) is less than a page")
	}
	gs := &GsStackBlock{reserve: reserve, gap: gap}
	gsBlocks = append(gsBlocks, gs)
	gs.EnsureStacks(numStacks)
	return gs
}

// EnsureStacks reserves stacks until the block has at least n.
func (gs *GsStackBlock) EnsureStacks(n int) {
	for len(gs.Stks) < n {
		r, usable := gsReserveRegion(gs.reserve, gs.gap)
		p := unsafe.Pointer(unsafe.SliceData(r))
		gs.regions = append(gs.regions, r)
		gs.Stks = append(gs.Stks, p)
		gs.Base = append(gs.Base, p)
		gs.Cap = append(gs.Cap, usable)
	}
}

// Reset rewinds every stack of the block to empty.
func (gs *GsStackBlock) Reset() { copy(gs.Stks, gs.Base) }

// Close releases the block's regions.
func (gs *GsStackBlock) Close() error {
	var first error
	for _, r := range gs.regions {
		if err := syscall.Munmap(r); err != nil && first == nil {
			first = err
		}
	}
	gs.regions, gs.Stks, gs.Base, gs.Cap = nil, nil, nil, nil
	return first
}

// End is where stack k's region ends, which a line print renders at the
// stack's top does not write past (gsPutStr).
func (gs *GsStackBlock) End(k int64) unsafe.Pointer { return unsafe.Add(gs.Base[k], gs.Cap[k]) }

// gsOverflow is the C runtime's report of a data stack that grows past its
// region (gs_overflow_text), raised before a write would cross end, where
// the region ends. The stacks are main's: there are no threads yet.
func gsOverflow(end unsafe.Pointer) {
	var usable, reserve int64
	for _, b := range gsBlocks {
		for k := range b.Cap {
			if b.End(int64(k)) == end {
				usable, reserve = b.Cap[k], b.reserve
			}
		}
	}
	panic(&GsError{Msg: gsOverflowText(usable, reserve)})
}

func gsOverflowText(usable, reserve int64) string {
	reserve -= reserve % int64(syscall.Getpagesize())
	msg := "data stack overflow: a value on main's data stacks grew past the " + gsSize(usable) +
		" reserved for it"
	if usable < reserve {
		msg += " (halved from " + gsSize(reserve) + " when the platform ran out of address space)"
	}
	return msg + "; --stack-reserve or -DGS_STACK_RESERVE raises the reservation, up to 256 TB"
}

// gsFault is the report of a fault at addr, where it lies in a region's
// guard gap: a data stack grown past its region with no check on the way,
// as a push grows it (gs_fault_handler). "" for any other address.
func gsFault(addr uintptr) string {
	for _, b := range gsBlocks {
		for k, r := range b.regions {
			base := uintptr(unsafe.Pointer(unsafe.SliceData(r)))
			if addr-base < uintptr(len(r)) {
				return "goose runtime error: " + gsOverflowText(b.Cap[k], b.reserve)
			}
		}
	}
	return ""
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

// gsReserveRegion maps one region: address space only, committed by the OS
// page by page as the stack grows into it, with the guard gap at its end.
func gsReserveRegion(reserve, gap int64) ([]byte, int64) {
	page := int64(syscall.Getpagesize())
	usable := reserve - reserve%page
	if gsRegionUsable > 0 && gsRegionUsable < usable {
		usable = gsRegionUsable
	}
	for {
		r, err := syscall.Mmap(-1, 0, int(usable+gap), syscall.PROT_READ|syscall.PROT_WRITE,
			syscall.MAP_ANON|syscall.MAP_PRIVATE|syscall.MAP_NORESERVE)
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
// words it, and exit status 1.

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

func (e *GsError) Error() string {
	if e.File == "" {
		return "goose runtime error: " + e.Msg
	}
	return "goose runtime error: " + e.Msg + " (" + e.File + ":" + strconv.Itoa(e.Line) + ")"
}

// GsExitOnAbort, deferred by the program's main, ends the process as the C
// runtime does when a Goose runtime error panics: the message on stderr and
// exit status 1. So does a fault in a data stack's guard gap, which the
// program's main has the runtime panic with (debug.SetPanicOnFault): a stack
// that grew past its region, as C's fault handler reports it. Any other
// panic goes on.
func GsExitOnAbort() {
	if r := recover(); r != nil {
		if e, ok := r.(*GsError); ok {
			os.Stderr.WriteString(e.Error() + "\n")
			os.Exit(1)
		}
		if f, ok := r.(interface{ Addr() uintptr }); ok {
			if msg := gsFault(f.Addr()); msg != "" {
				os.Stderr.WriteString(msg + "\n")
				os.Exit(1)
			}
		}
		panic(r)
	}
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

// GsExit is exit(code): the process ends at once, with that status.
func GsExit(code int64) { os.Exit(int(code)) }

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

// GsSub is the slice of the elements lo up to hi of the n at p, each es
// bytes. One that is empty at the end of the run starts at p, which keeps
// its address inside the storage it slices.
func GsSub(p unsafe.Pointer, lo, hi, n, es int64) GsSlice {
	if lo < n {
		return GsSlice{unsafe.Add(p, lo*es), hi - lo}
	}
	return GsSlice{p, 0}
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
};

}  // namespace goose
