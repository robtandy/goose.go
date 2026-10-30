package goose

import (
	"math"
	"os"
	"strconv"
	"sync"
	"unsafe"
)

// ---------------------------------------------------------------------------
// Aborts (§9.3). A failed check panics with a *GsError, which the program's
// main recovers (GsExitOnAbort): the message on stderr, as the C runtime
// words it, and exit status 1. A library never exits the process, which is
// its host's (GsProgram.Library): there an abort, and exit(), stay the
// *GsError's panic, which the caller of an export fn's entry point may
// recover.

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

// Error is the error's text as the C runtime prints it, less the newline:
// "goose runtime error: " and the message, then the check's location.
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
		gsEndOnPanic(r, false)
	}
}

// gsEndOnPanic ends the process for what a thread program's goroutine
// recovered, as GsExitOnAbort has it; a panic of no runtime error goes on.
// A library's goes on as the runtime error's *GsError (GsAbortOf): on a
// worker's goroutine, which the runtime started, no host recovers it, and
// it ends the process as any goroutine's unrecovered panic does.
func gsEndOnPanic(r any, library bool) {
	r = GsAbortOf(r)
	if e, ok := r.(*GsError); ok && !library {
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
// a library's (GsProgram.Library).
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

// GsUnreachable is what a function panics with where control cannot get to
// the end of it, which the checker found: a compiler bug if it does.
func GsUnreachable(file string, line int) *GsError {
	return &GsError{Msg: "unreachable code reached", File: file, Line: line}
}

// GsBool is b, as a call. Go's vet takes `c && c` and `n != 1 || n != 2`
// for mistakes, which a Goose program may write: it leaves an operand that
// is a call out of the comparisons.
func GsBool(b bool) bool { return b }

// GsTagError is a debug build's failed check of an ADT's tag, which no
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
// GsEscape makes the Go compiler keep it off them, on the heap, where
// nothing moves (its condition never holds). The collector does not see it
// there either, so the entry point keeps the argument alive until the call
// returns (runtime.KeepAlive).
var gsEscapeSink struct {
	b bool
	p unsafe.Pointer
}

// GsEscape is p, which the Go compiler takes to escape to the heap.
//
//goose:internal
func GsEscape(p unsafe.Pointer) unsafe.Pointer {
	if gsEscapeSink.b {
		gsEscapeSink.p = p
	}
	return p
}

// GsBytesOf is the u8 slice of a string's or a []byte's bytes, in place: a
// string's header begins as a slice's does, with its data and its length.
func GsBytesOf[T GsBytes](s T) GsSlice {
	return GsSlice{GsEscape(*(*unsafe.Pointer)(unsafe.Pointer(&s))), int64(len(s))}
}

// GsSliceOf is the slice of xs's elements, in place. A Go T has the
// layout of the Goose type it stands for.
func GsSliceOf[T any](xs []T) GsSlice {
	return GsSlice{GsEscape(unsafe.Pointer(unsafe.SliceData(xs))), int64(len(xs))}
}

// GsSliceFromBytes is the u8 slice of b's bytes, and GsSliceFromString of
// s's, which are read-only, in place.
func GsSliceFromBytes(b []byte) GsSlice {
	return GsSlice{GsEscape(unsafe.Pointer(unsafe.SliceData(b))), int64(len(b))}
}

func GsSliceFromString(s string) GsSlice {
	return GsSlice{GsEscape(unsafe.Pointer(unsafe.StringData(s))), int64(len(s))}
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
		GsIdxFail(i, n, file, line)
	}
	return i
}

// GsIdxFail is a failed bounds check of index i against length n.
//
//goose:internal
func GsIdxFail(i, n int64, file string, line int) {
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

func GsSubI16(a, b int16, file string, line int) int16 {
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
// Variable-size values (§3.6, docs/implementation.md §6.1): a length prefix
// or a varint is ULEB128, a signed varint value zigzag-encoded on top.

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

// GsUlebCheck reads the ULEB128 at byte q of the n bytes at p, for the
// verifier (gs_uleb_check): the value and its size in bytes, which is 0
// where the varint runs past n or past ten bytes, carries payload bits
// above the 64th, or is not in its shortest form. Nothing past the n bytes
// is formed into an address.
//
//goose:internal
func GsUlebCheck(p unsafe.Pointer, q, n int64) (v uint64, k int64) {
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

// GsZigCheck is GsUlebCheck for a zigzag-encoded varint (gs_zig_check).
//
//goose:internal
func GsZigCheck(p unsafe.Pointer, q, n int64) (int64, int64) {
	u, k := GsUlebCheck(p, q, n)
	return int64(u>>1) ^ -int64(u&1), k
}

// GsBmSet and GsBmGet are the verifier's element starts, one bit per image
// byte (GS_BM_SET, GS_BM_GET), in scratch on a data stack.
//
//goose:internal
func GsBmSet(bm unsafe.Pointer, i int64) {
	*(*byte)(unsafe.Add(bm, i>>3)) |= 1 << (i & 7)
}

//goose:internal
func GsBmGet(bm unsafe.Pointer, i int64) bool {
	return *(*byte)(unsafe.Add(bm, i>>3))>>(i&7)&1 != 0
}

// GsIsLE says whether the host is little-endian, which serialization needs
// (gs_is_le).
//
//goose:internal
func GsIsLE() bool {
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
