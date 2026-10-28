package goose

import (
	"bytes"
	"encoding/binary"
	"math"
	"math/bits"
	"os"
	"strconv"
	"sync"
	"unsafe"
)

// ---------------------------------------------------------------------------
// Aborts (goose_spec.md §9.3), as src/runtime/runtime_impl.h words them.
// A failed check panics with a *GsError: a Go caller of exported Goose code
// can recover it, and a Goose program's main, deferring GsExitOnAbort, ends
// as the C runtime ends one, with the message on stderr and exit status 1.
// The failure helpers whose message needs formatting are out of line, so
// that the checks calling them stay small enough for Go to inline.

// Error ids for GsAbort, numbered as the C runtime's GS_E_* are.
const (
	GsECapacity   = iota // limited array capacity exceeded
	GsESlice             // slice bounds out of range
	GsEReloff            // relative reference offset overflow
	GsEAssert            // assert failed
	GsECapRange          // invalid capacity
	GsEResizeFill        // resize growth requires a fill value
	GsEResizeNeg         // resize to a negative length
	GsEPop               // pop on empty array
	GsEThreadID          // thread_wait on an unknown thread id
	GsEThreadSelf        // a worker cannot wait for itself
	GsETag               // corrupt ADT tag (debug builds only)
	GsEEndian            // serialization on a big-endian host
	GsESliceLen          // slice pool length negative or beyond any data stack
	GsEPoolSlice         // a slice handed to a slice pool is not one of its runs
	GsERelNull           // a non-null optional self-relative target has offset zero
	GsEPopN              // pop_n of a negative count or more than the length
	GsEQBound            // a queue limit of zero or less
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

// GsError is a Goose runtime error: what every failed check and abort
// panics with.
type GsError struct {
	Msg  string // what failed, as the C runtime words it
	File string // the .goose file of the check, "" for the runtime's own failures
	Line int    // the line of the check
}

// Error is the error's text as the C runtime prints it, less the newline:
// "goose runtime error: " and the message, then the check's location.
func (e *GsError) Error() string { return "goose runtime error: " + e.Text() }

// Text is what Error prints after "goose runtime error: ", the location
// included: the message a contained worker's abort leaves for thread_wait
// (goose_spec.md §11.2).
func (e *GsError) Text() string {
	if e.File == "" {
		return e.Msg
	}
	return e.Msg + " (" + e.File + ":" + strconv.Itoa(e.Line) + ")"
}

// GsExitOnAbort, deferred at the top of a goroutine running Goose code,
// ends the process as the C runtime does when a Goose runtime error panics
// out of that goroutine: the error on stderr, then exit status 1. Any other
// panic goes on.
func GsExitOnAbort() {
	if r := recover(); r != nil {
		gsEndOnPanic(r)
	}
}

// gsEndOnPanic ends the process for what a thread program's goroutine
// recovered, as GsExitOnAbort has it; a panic of no runtime error goes on.
func gsEndOnPanic(r any) {
	if e, ok := r.(*GsError); ok {
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

// gsEnd ends the process with code, msg on stderr first.
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

// GsPanic is a failure of the runtime itself (gs_panic), which has no
// location in the program.
func GsPanic(msg string) {
	panic(&GsError{Msg: msg})
}

// GsAbort is a failed check with one of the GsE* error ids (gs_abort).
func GsAbort(err int, file string, line int) {
	panic(&GsError{Msg: gsErrMsgs[err], File: file, Line: line})
}

// GsAbortMsg is the program's own abort(msg) (gs_abort_msg): n bytes of
// message at msg, which may be anywhere a Goose value is, and so are copied
// before the stacks unwind.
//
//go:noinline
func GsAbortMsg(msg unsafe.Pointer, n int64, file string, line int) {
	panic(&GsError{Msg: string(unsafe.Slice((*byte)(msg), n)), File: file, Line: line})
}

// GsIdxFail is a failed bounds check of index i against length n.
//
//go:noinline
func GsIdxFail(i, n int64, file string, line int) {
	panic(&GsError{Msg: "index " + strconv.FormatInt(i, 10) + " out of bounds (length " +
		strconv.FormatInt(n, 10) + ")", File: file, Line: line})
}

// GsIdx is i, checked to index an array of length n with one unsigned
// compare (GS_IDX).
func GsIdx(i, n int64, file string, line int) int64 {
	if uint64(i) >= uint64(n) {
		GsIdxFail(i, n, file, line)
	}
	return i
}

// GsDivFail is a division or modulo by zero, which every build checks.
func GsDivFail(file string, line int) {
	panic(&GsError{Msg: "division by zero", File: file, Line: line})
}

// GsDivOvf is i64.min / -1, which every build checks.
func GsDivOvf(file string, line int) {
	panic(&GsError{Msg: "integer overflow in division", File: file, Line: line})
}

// GsOvf is a debug build's failed overflow check of a op b at type typ.
//
//go:noinline
func GsOvf(a int64, op string, b int64, typ, file string, line int) {
	panic(&GsError{Msg: "integer overflow (debug): " + strconv.FormatInt(a, 10) + " " + op + " " +
		strconv.FormatInt(b, 10) + " at " + typ, File: file, Line: line})
}

// GsOvfNeg is a debug build's failed overflow check of -a at type typ.
//
//go:noinline
func GsOvfNeg(a int64, typ, file string, line int) {
	panic(&GsError{Msg: "integer overflow (debug): -(" + strconv.FormatInt(a, 10) + ") at " + typ,
		File: file, Line: line})
}

func gsAsFail(why, num, typ, file string, line int) {
	panic(&GsError{Msg: "as conversion " + why + " (debug): " + num + " as " + typ, File: file, Line: line})
}

// GsAsFailI is a debug build's failed `as` check of the integer v into typ.
//
//go:noinline
func GsAsFailI(why string, v int64, typ, file string, line int) {
	gsAsFail(why, strconv.FormatInt(v, 10), typ, file, line)
}

// GsAsFailU is a debug build's failed `as` check of the unsigned v into typ.
//
//go:noinline
func GsAsFailU(why string, v uint64, typ, file string, line int) {
	gsAsFail(why, strconv.FormatUint(v, 10), typ, file, line)
}

// GsAsFailF is a debug build's failed `as` check of the float d into typ,
// d named in its text form (§3.7): an f32's own where f32 is set.
//
//go:noinline
func GsAsFailF(why string, d float64, f32 bool, typ, file string, line int) {
	gsAsFail(why, string(gsAppendFloat(nil, d, f32)), typ, file, line)
}

// gsAppendFloat appends a float's text form (§3.7, gs_fmt_f64 and
// gs_fmt_f32): the fewest significant digits that read back as the value
// (of its own type, an f32 where f32 is set), and of those the nearest,
// laid out as C's %g lays them out at a precision of max(15, digits) but
// always with a fraction or an exponent: 1.0, 0.1, 1e+20, 1.5e-07.
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
	// The shortest digits, as d.ddde±xx: dig, and the exponent x of the
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
	switch {
	case x < -4 || x >= max(n, 15):
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
	case x < 0:
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

// ---------------------------------------------------------------------------
// Integer semantics (§6.2). Go's own arithmetic already wraps as Goose's
// release builds do, unsigned and signed, so a release build needs these
// only for division and modulo, which check for zero in every build and
// whose % is Euclidean. A debug build calls the rest, which abort where
// the result does not fit the operands' type, naming the operands, the
// type and the operation's file and line. Shift counts are masked to the
// width, which the generated code writes out itself.

// GsAddI8 is a + b in a debug build: aborts on overflow.
func GsAddI8(a, b int8, file string, line int) int8 {
	r := a + b
	if (a^r)&(b^r) < 0 {
		GsOvf(int64(a), "+", int64(b), "i8", file, line)
	}
	return r
}

// GsAddI16 is a + b in a debug build: aborts on overflow.
func GsAddI16(a, b int16, file string, line int) int16 {
	r := a + b
	if (a^r)&(b^r) < 0 {
		GsOvf(int64(a), "+", int64(b), "i16", file, line)
	}
	return r
}

// GsAddI32 is a + b in a debug build: aborts on overflow.
func GsAddI32(a, b int32, file string, line int) int32 {
	r := a + b
	if (a^r)&(b^r) < 0 {
		GsOvf(int64(a), "+", int64(b), "i32", file, line)
	}
	return r
}

// GsAddI64 is a + b in a debug build: aborts on overflow.
func GsAddI64(a, b int64, file string, line int) int64 {
	r := a + b
	if (a^r)&(b^r) < 0 {
		GsOvf(a, "+", b, "i64", file, line)
	}
	return r
}

// GsSubI8 is a - b in a debug build: aborts on overflow.
func GsSubI8(a, b int8, file string, line int) int8 {
	r := a - b
	if (a^b)&(a^r) < 0 {
		GsOvf(int64(a), "-", int64(b), "i8", file, line)
	}
	return r
}

// GsSubI16 is a - b in a debug build: aborts on overflow.
func GsSubI16(a, b int16, file string, line int) int16 {
	r := a - b
	if (a^b)&(a^r) < 0 {
		GsOvf(int64(a), "-", int64(b), "i16", file, line)
	}
	return r
}

// GsSubI32 is a - b in a debug build: aborts on overflow.
func GsSubI32(a, b int32, file string, line int) int32 {
	r := a - b
	if (a^b)&(a^r) < 0 {
		GsOvf(int64(a), "-", int64(b), "i32", file, line)
	}
	return r
}

// GsSubI64 is a - b in a debug build: aborts on overflow.
func GsSubI64(a, b int64, file string, line int) int64 {
	r := a - b
	if (a^b)&(a^r) < 0 {
		GsOvf(a, "-", b, "i64", file, line)
	}
	return r
}

// GsMulI8 is a * b in a debug build: aborts on overflow.
func GsMulI8(a, b int8, file string, line int) int8 {
	r := int64(a) * int64(b)
	if r != int64(int8(r)) {
		GsOvf(int64(a), "*", int64(b), "i8", file, line)
	}
	return int8(r)
}

// GsMulI16 is a * b in a debug build: aborts on overflow.
func GsMulI16(a, b int16, file string, line int) int16 {
	r := int64(a) * int64(b)
	if r != int64(int16(r)) {
		GsOvf(int64(a), "*", int64(b), "i16", file, line)
	}
	return int16(r)
}

// GsMulI32 is a * b in a debug build: aborts on overflow.
func GsMulI32(a, b int32, file string, line int) int32 {
	r := int64(a) * int64(b)
	if r != int64(int32(r)) {
		GsOvf(int64(a), "*", int64(b), "i32", file, line)
	}
	return int32(r)
}

// GsMulI64 is a * b in a debug build: aborts on overflow.
func GsMulI64(a, b int64, file string, line int) int64 {
	r := a * b
	if a != 0 && b != 0 &&
		((a == -1 && b == math.MinInt64) || (b == -1 && a == math.MinInt64) || r/b != a) {
		GsOvf(a, "*", b, "i64", file, line)
	}
	return r
}

// GsNegI8 is -a in a debug build: aborts on overflow.
func GsNegI8(a int8, file string, line int) int8 {
	if a == math.MinInt8 {
		GsOvfNeg(int64(a), "i8", file, line)
	}
	return -a
}

// GsNegI16 is -a in a debug build: aborts on overflow.
func GsNegI16(a int16, file string, line int) int16 {
	if a == math.MinInt16 {
		GsOvfNeg(int64(a), "i16", file, line)
	}
	return -a
}

// GsNegI32 is -a in a debug build: aborts on overflow.
func GsNegI32(a int32, file string, line int) int32 {
	if a == math.MinInt32 {
		GsOvfNeg(int64(a), "i32", file, line)
	}
	return -a
}

// GsNegI64 is -a in a debug build: aborts on overflow.
func GsNegI64(a int64, file string, line int) int64 {
	if a == math.MinInt64 {
		GsOvfNeg(a, "i64", file, line)
	}
	return -a
}

// Division truncates toward zero; modulo is Euclidean, in [0, |b|) for any
// a. Both abort on a zero divisor in every build. i64.min / -1 aborts in
// every build too, while at the narrower signed types the same case wraps
// in a release build (GsDivI8 and co.) and aborts in a debug one
// (GsDivI8Debug and co.). x % -1 is 0 for every x.

// GsDivU8 is a / b: aborts on a zero divisor.
func GsDivU8(a, b uint8, file string, line int) uint8 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

// GsDivU16 is a / b: aborts on a zero divisor.
func GsDivU16(a, b uint16, file string, line int) uint16 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

// GsDivU32 is a / b: aborts on a zero divisor.
func GsDivU32(a, b uint32, file string, line int) uint32 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

// GsDivU64 is a / b: aborts on a zero divisor.
func GsDivU64(a, b uint64, file string, line int) uint64 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

// GsModU8 is a % b: aborts on a zero divisor.
func GsModU8(a, b uint8, file string, line int) uint8 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a % b
}

// GsModU16 is a % b: aborts on a zero divisor.
func GsModU16(a, b uint16, file string, line int) uint16 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a % b
}

// GsModU32 is a % b: aborts on a zero divisor.
func GsModU32(a, b uint32, file string, line int) uint32 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a % b
}

// GsModU64 is a % b: aborts on a zero divisor.
func GsModU64(a, b uint64, file string, line int) uint64 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a % b
}

// GsDivI8 is a / b in a release build: aborts on a zero divisor, and
// i8.min / -1 wraps to i8.min.
func GsDivI8(a, b int8, file string, line int) int8 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

// GsDivI16 is a / b in a release build: aborts on a zero divisor, and
// i16.min / -1 wraps to i16.min.
func GsDivI16(a, b int16, file string, line int) int16 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

// GsDivI32 is a / b in a release build: aborts on a zero divisor, and
// i32.min / -1 wraps to i32.min.
func GsDivI32(a, b int32, file string, line int) int32 {
	if b == 0 {
		GsDivFail(file, line)
	}
	return a / b
}

// GsDivI8Debug is a / b in a debug build: aborts on a zero divisor and on
// i8.min / -1.
func GsDivI8Debug(a, b int8, file string, line int) int8 {
	if b == 0 {
		GsDivFail(file, line)
	}
	if b == -1 && a == math.MinInt8 {
		GsOvf(int64(a), "/", int64(b), "i8", file, line)
	}
	return a / b
}

// GsDivI16Debug is a / b in a debug build: aborts on a zero divisor and
// on i16.min / -1.
func GsDivI16Debug(a, b int16, file string, line int) int16 {
	if b == 0 {
		GsDivFail(file, line)
	}
	if b == -1 && a == math.MinInt16 {
		GsOvf(int64(a), "/", int64(b), "i16", file, line)
	}
	return a / b
}

// GsDivI32Debug is a / b in a debug build: aborts on a zero divisor and
// on i32.min / -1.
func GsDivI32Debug(a, b int32, file string, line int) int32 {
	if b == 0 {
		GsDivFail(file, line)
	}
	if b == -1 && a == math.MinInt32 {
		GsOvf(int64(a), "/", int64(b), "i32", file, line)
	}
	return a / b
}

// GsDivI64 is a / b in every build: aborts on a zero divisor and on
// i64.min / -1.
func GsDivI64(a, b int64, file string, line int) int64 {
	if b == 0 {
		GsDivFail(file, line)
	}
	if b == -1 && a == math.MinInt64 {
		GsDivOvf(file, line)
	}
	return a / b
}

// GsModI8 is the Euclidean a % b, in [0, |b|): aborts on a zero divisor.
func GsModI8(a, b int8, file string, line int) int8 {
	return int8(gsModNarrow(int64(a), int64(b), file, line))
}

// GsModI16 is the Euclidean a % b, in [0, |b|): aborts on a zero divisor.
func GsModI16(a, b int16, file string, line int) int16 {
	return int16(gsModNarrow(int64(a), int64(b), file, line))
}

// GsModI32 is the Euclidean a % b, in [0, |b|): aborts on a zero divisor.
func GsModI32(a, b int32, file string, line int) int32 {
	return int32(gsModNarrow(int64(a), int64(b), file, line))
}

// gsModNarrow is the Euclidean a % b of two values of a type narrower than
// i64, where |b| always fits.
func gsModNarrow(a, b int64, file string, line int) int64 {
	if b == 0 {
		GsDivFail(file, line)
	}
	t := a % b
	if t < 0 {
		if b < 0 {
			return t - b
		}
		return t + b
	}
	return t
}

// GsModI64 is the Euclidean a % b, in [0, |b|): aborts on a zero divisor.
// |b| is taken unsigned, so a divisor of i64.min works.
func GsModI64(a, b int64, file string, line int) int64 {
	if b == 0 {
		GsDivFail(file, line)
	}
	t := a % b
	if t < 0 {
		if b < 0 {
			return int64(uint64(t) - uint64(b))
		}
		return t + b
	}
	return t
}

// ---------------------------------------------------------------------------
// Conversions (§6.3). A debug build checks every `as` that could change
// its value, aborting with the value, the target type and the cast's file
// and line; a release build converts as Go does, except from a float,
// whose conversion to an integer is defined in every build (GsF2IWrap).
// A conversion to a float is a plain Go conversion in every build.

// GsRangeI is v, checked to lie in [lo, hi], the range of the signed or
// unsigned type typ, in a debug build (GS_RANGE).
func GsRangeI(v, lo, hi int64, typ, file string, line int) int64 {
	if v < lo || v > hi {
		GsAsFailI("out of range", v, typ, file, line)
	}
	return v
}

// GsRangeU is v, checked to be at most hi, the largest value of typ, in a
// debug build (GS_RANGE_U).
func GsRangeU(v, hi uint64, typ, file string, line int) uint64 {
	if v > hi {
		GsAsFailU("out of range", v, typ, file, line)
	}
	return v
}

// GsF2I is d as an integer of typ, a signed type or an unsigned one
// narrower than u64 whose range is [lo, hi], checked in a debug build to
// keep its value (GS_F2I). f32 says d holds an f32, for the message.
func GsF2I(d float64, f32 bool, lo, hi int64, typ, file string, line int) int64 {
	if d >= -0x1p63 && d < 0x1p63 {
		if v := int64(d); v >= lo && v <= hi && float64(v) == d {
			return v
		}
	}
	gsF2IFail(d, f32, lo, hi, typ, file, line)
	return 0
}

//go:noinline
func gsF2IFail(d float64, f32 bool, lo, hi int64, typ, file string, line int) {
	if !(d >= -0x1p63 && d < 0x1p63) {
		GsAsFailF("out of range", d, f32, typ, file, line)
	}
	if v := int64(d); v < lo || v > hi {
		GsAsFailF("out of range", d, f32, typ, file, line)
	}
	GsAsFailF("changes the value", d, f32, typ, file, line)
}

// GsF2U is d as a u64, checked in a debug build to keep its value
// (GS_F2U). f32 says d holds an f32, for the message.
func GsF2U(d float64, f32 bool, file string, line int) uint64 {
	if d >= 0 && d < 0x1p64 {
		if v := uint64(d); float64(v) == d {
			return v
		}
	}
	gsF2UFail(d, f32, file, line)
	return 0
}

//go:noinline
func gsF2UFail(d float64, f32 bool, file string, line int) {
	if !(d >= 0 && d < 0x1p64) {
		GsAsFailF("out of range", d, f32, "u64", file, line)
	}
	GsAsFailF("changes the value", d, f32, "u64", file, line)
}

// GsF2IWrap is `as!` from a float to an integer, and a release build's
// `as` (gs_f2iwrap): truncated toward zero, then wrapped modulo 2^64; the
// caller truncates the result to a narrower type. NaN, and an infinity,
// whose wrap is no number, give 0. A value lies in the i64 range exactly
// when its truncation does, and there Go's conversion truncates by itself,
// so only NaN and the values beyond the range take the wrap, out of line.
func GsF2IWrap(d float64) int64 {
	if d >= -0x1p63 && d < 0x1p63 {
		return int64(d)
	}
	return gsF2IWrapSlow(d)
}

//go:noinline
func gsF2IWrapSlow(d float64) int64 {
	if d != d || math.IsInf(d, 0) {
		return 0
	}
	// Exact: d and its remainder are whole multiples of 2^11.
	d = math.Mod(math.Trunc(d), 0x1p64)
	if d < 0 {
		d += 0x1p64
	}
	return int64(uint64(d))
}

// GsF2UWrap is GsF2IWrap as an unsigned value: `as!` (and a release
// build's `as`) from a float to u64.
func GsF2UWrap(d float64) uint64 {
	return uint64(GsF2IWrap(d))
}

// ---------------------------------------------------------------------------
// varint (§3.6): ULEB128, the signed positions zigzag-encoded. The readers
// trust their input, as the generated code's walkers of a value it built
// may; the checks bound every read by the image's end, for verifying bytes
// that came from elsewhere (docs/design/serialization.md).

// GsUlebRead is the ULEB128 at p. A one-byte varint, the common case of an
// inline array's length prefix, inlines; longer ones continue out of line
// (GS_ULEB_READ).
func GsUlebRead(p unsafe.Pointer) int64 {
	if b := *(*byte)(p); b < 0x80 {
		return int64(b)
	}
	return gsUlebReadSlow(p)
}

//go:noinline
func gsUlebReadSlow(p unsafe.Pointer) int64 {
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
	if *(*byte)(p) < 0x80 {
		return 1
	}
	return gsUlebSizeSlow(p)
}

//go:noinline
func gsUlebSizeSlow(p unsafe.Pointer) int64 {
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

// GsZigEncode is v zigzag-encoded: 0, -1, 1, -2, ... as 0, 1, 2, 3, ...
func GsZigEncode(v int64) uint64 {
	return uint64(v)<<1 ^ uint64(v>>63)
}

// GsZigDecode is the value u zigzag-encodes.
func GsZigDecode(u uint64) int64 {
	return int64(u>>1) ^ -int64(u&1)
}

// GsZigRead is the zigzag-encoded varint at p.
func GsZigRead(p unsafe.Pointer) int64 {
	if b := *(*byte)(p); b < 0x80 {
		return int64(b>>1) ^ -int64(b&1)
	}
	return gsZigReadSlow(p)
}

//go:noinline
func gsZigReadSlow(p unsafe.Pointer) int64 {
	return GsZigDecode(uint64(gsUlebReadSlow(p)))
}

// GsZigWrite writes v as a zigzag-encoded varint at p, returning its size
// in bytes.
func GsZigWrite(p unsafe.Pointer, v int64) int64 {
	return GsUlebWrite(p, GsZigEncode(v))
}

// GsUlebCheck reads the ULEB128 at p of an image that ends at end, for the
// verifier (gs_uleb_check): the value and its size in bytes, which is 0
// where the varint runs past end or past ten bytes, carries payload bits
// above the 64th, or is not in its shortest form.
func GsUlebCheck(p, end unsafe.Pointer) (v uint64, n int64) {
	room := int64(uintptr(end) - uintptr(p))
	for shift := uint(0); ; shift += 7 {
		if n >= room {
			return 0, 0
		}
		b := *(*byte)(unsafe.Add(p, n))
		n++
		if shift > 63 || (shift == 63 && b&0x7e != 0) {
			return 0, 0
		}
		v |= uint64(b&0x7f) << shift
		if b < 0x80 {
			if shift != 0 && b == 0 { // A redundant high zero group.
				return 0, 0
			}
			return v, n
		}
	}
}

// GsZigCheck is GsUlebCheck for a zigzag-encoded varint (gs_zig_check).
func GsZigCheck(p, end unsafe.Pointer) (v int64, n int64) {
	u, n := GsUlebCheck(p, end)
	return GsZigDecode(u), n
}

// ---------------------------------------------------------------------------
// Memory (docs/implementation.md §6.1). An empty slice's Data is nil where
// the slice was zero-filled; these take nil for no bytes. Each inlines: a
// run of 4 to 16 bytes, where keys, names and elements mostly are, is two
// overlapping loads (and stores) per side, as in gs_memeq and gs_memcpy,
// which fold to one path where the length is a constant; any other run is
// one call of Go's own memequal or memmove, which take long runs a block at
// a time. Equality does not care about byte order, so the loads are the
// host's own.

// GsMemEq says whether the n bytes at a and b are equal.
func GsMemEq(a, b unsafe.Pointer, n int64) bool {
	switch {
	case n >= 8 && n <= 16:
		return (*(*uint64)(a)^*(*uint64)(b))|
			(*(*uint64)(unsafe.Add(a, n-8))^*(*uint64)(unsafe.Add(b, n-8))) == 0
	case n >= 4 && n < 8:
		return (*(*uint32)(a)^*(*uint32)(b))|
			(*(*uint32)(unsafe.Add(a, n-4))^*(*uint32)(unsafe.Add(b, n-4))) == 0
	}
	return n <= 0 || unsafe.String((*byte)(a), n) == unsafe.String((*byte)(b), n)
}

// GsMemCpy copies n bytes from src to dst (gs_memcpy). A short run is read
// whole before it is written, so the runs may overlap, though the
// generated code never asks for that here.
func GsMemCpy(dst, src unsafe.Pointer, n int64) {
	switch {
	case n >= 8 && n <= 16:
		x, y := *(*uint64)(src), *(*uint64)(unsafe.Add(src, n-8))
		*(*uint64)(dst), *(*uint64)(unsafe.Add(dst, n-8)) = x, y
	case n >= 4 && n < 8:
		x, y := *(*uint32)(src), *(*uint32)(unsafe.Add(src, n-4))
		*(*uint32)(dst), *(*uint32)(unsafe.Add(dst, n-4)) = x, y
	default:
		copy(unsafe.Slice((*byte)(dst), n), unsafe.Slice((*byte)(src), n))
	}
}

// GsMemMove copies n bytes from src to dst, which may overlap
// (gs_memmove).
func GsMemMove(dst, src unsafe.Pointer, n int64) {
	if n > 0 {
		copy(unsafe.Slice((*byte)(dst), n), unsafe.Slice((*byte)(src), n))
	}
}

// ---------------------------------------------------------------------------
// Byte search: std's find_any and find_pair (docs/stdlib.md). A set
// arrives as std's ByteSet, made by its byte_set, which GsByteSet lays out
// as the Goose struct is: Members is the set, one byte per value, and Kind
// says how a block of bytes is tested for it: by comparing with 1 to 3 of
// Bytes (kinds 1-3), or as the range Bytes[0] .. Bytes[0] + Bytes[1] (kind
// 4), both complemented where Invert is set; kind 0 is the empty set, or
// with Invert every byte. Lo and Hi are the nibble tables a set of kind 5
// has nothing else than, which the C runtime's SSSE3 search reads.
//
// The search tests 8 bytes at a time as a word, for kinds 1 to 4, and one
// byte at a time by Members for any other set, as the C runtime's search
// falls back to; a set of one byte is Go's own IndexByte. No load reaches
// past the range, which may end where a data stack's guard gap begins: its
// last part is tested as the range's last whole word, with the positions
// before it, already tested, shifted out of the result.

// GsByteSet is std's ByteSet (gs_byteset).
type GsByteSet struct {
	Members [256]uint8
	Kind    uint8
	Invert  uint8
	Bytes   [3]uint8
	Lo      [16]uint8
	Hi      [16]uint8
}

const (
	gsLow7  = 0x7f7f7f7f7f7f7f7f
	gsHigh1 = 0x8080808080808080
	gsOnes  = 0x0101010101010101
)

// gsWordSet is a set of kind 1 to 4 as a test of a word of 8 bytes. Each
// field is a byte repeated across a word: for kinds 1 to 3, a, b and c are
// the bytes compared with (the first again where there are fewer); for a
// range, a is its first byte's low 7 bits, b that byte's high bit flipped,
// and c is 255 less the range's width. flip is the high bit of every byte
// where the set is complemented.
type gsWordSet struct {
	a, b, c uint64
	flip    uint64
	rng     bool
}

func gsWordSetOf(s *GsByteSet) gsWordSet {
	var w gsWordSet
	b0 := uint64(s.Bytes[0]) * gsOnes
	switch s.Kind {
	case 1:
		w.a, w.b, w.c = b0, b0, b0
	case 2:
		w.a, w.b, w.c = b0, uint64(s.Bytes[1])*gsOnes, b0
	case 3:
		w.a, w.b, w.c = b0, uint64(s.Bytes[1])*gsOnes, uint64(s.Bytes[2])*gsOnes
	default:
		w.rng = true
		w.a, w.b = b0&gsLow7, ^b0&gsHigh1
		w.c = uint64(255-s.Bytes[1]) * gsOnes
	}
	if s.Invert != 0 {
		w.flip = gsHigh1
	}
	return w
}

// gsEqBytes is the high bit of each byte of x that equals the same byte of
// a, b or c, a set of kind 1 to 3. A byte y of x ^ a is zero exactly where
// (y & 0x7f) + 0x7f leaves the high bit clear and so does y: no carry
// crosses from one byte to the next.
func gsEqBytes(x, a, b, c uint64) uint64 {
	ya, yb, yc := x^a, x^b, x^c
	return ^((((ya & gsLow7) + gsLow7) | ya) & (((yb & gsLow7) + gsLow7) | yb) &
		(((yc & gsLow7) + gsLow7) | yc)) & gsHigh1
}

// gsInRange is the high bit of each byte of x in the range a, b and c
// describe (gsWordSet): those whose distance t above the first byte,
// wrapping, has t + 255 - width carry out of no byte. t comes from a
// subtraction that cannot borrow across bytes, each high bit fixed after,
// and the carries from the sums of the low 7 bits of each byte.
func gsInRange(x, a, b, c uint64) uint64 {
	t := ((x | gsHigh1) - a) ^ ((x & gsHigh1) ^ b)
	return ^((t & c) | ((t ^ c) & ((t & gsLow7) + (c & gsLow7)))) & gsHigh1
}

// gsLoad64 is the 8 bytes at p as a little-endian word, so that byte k of
// the word is p[k] on every host.
func gsLoad64(p unsafe.Pointer) uint64 {
	return binary.LittleEndian.Uint64((*[8]byte)(p)[:])
}

// GsScanAny is the first i < n with p[i] in the set, or -1 where there is
// none (gs_scan_any).
func GsScanAny(p unsafe.Pointer, n int64, set *GsByteSet) int64 {
	if n <= 0 {
		return -1
	}
	switch set.Kind {
	case 0:
		if set.Invert != 0 {
			return 0
		}
		return -1
	case 1, 2, 3, 4:
		if set.Kind == 1 && set.Invert == 0 {
			return int64(bytes.IndexByte(unsafe.Slice((*byte)(p), n), set.Bytes[0]))
		}
		if n >= 8 {
			w := gsWordSetOf(set)
			return gsScanAnyWords(p, n, &w)
		}
	}
	return gsScanAnyBytes(p, n, set)
}

// gsScanAnyWords is GsScanAny over n >= 8 bytes, two words at a time while
// they fit, then a word at a time, the last one ending at n with its bytes
// before i, already tested, shifted out.
func gsScanAnyWords(p unsafe.Pointer, n int64, w *gsWordSet) int64 {
	a, b, c, flip, rng := w.a, w.b, w.c, w.flip, w.rng
	i := int64(0)
	for ; i+16 <= n; i += 16 {
		x0, x1 := gsLoad64(unsafe.Add(p, i)), gsLoad64(unsafe.Add(p, i+8))
		var m0, m1 uint64
		if rng {
			m0, m1 = gsInRange(x0, a, b, c), gsInRange(x1, a, b, c)
		} else {
			m0, m1 = gsEqBytes(x0, a, b, c), gsEqBytes(x1, a, b, c)
		}
		if m0, m1 = m0^flip, m1^flip; m0|m1 != 0 {
			if m0 != 0 {
				return i + int64(bits.TrailingZeros64(m0)>>3)
			}
			return i + 8 + int64(bits.TrailingZeros64(m1)>>3)
		}
	}
	for ; i < n; i += 8 {
		j, shift := i, uint(0)
		if j > n-8 {
			j, shift = n-8, uint(i-n+8)*8
		}
		x := gsLoad64(unsafe.Add(p, j))
		var m uint64
		if rng {
			m = gsInRange(x, a, b, c)
		} else {
			m = gsEqBytes(x, a, b, c)
		}
		if m = (m ^ flip) >> shift; m != 0 {
			return i + int64(bits.TrailingZeros64(m)>>3)
		}
	}
	return -1
}

func gsScanAnyBytes(p unsafe.Pointer, n int64, set *GsByteSet) int64 {
	m := &set.Members
	i := int64(0)
	for ; i+8 <= n; i += 8 {
		q := (*[8]byte)(unsafe.Add(p, i))
		if m[q[0]]|m[q[1]]|m[q[2]]|m[q[3]]|m[q[4]]|m[q[5]]|m[q[6]]|m[q[7]] != 0 {
			break
		}
	}
	for ; i < n; i++ {
		if m[*(*byte)(unsafe.Add(p, i))] != 0 {
			return i
		}
	}
	return -1
}

// GsScanPair is the first i with p[i] in a and p[i + d] in b, of those
// with i + d < n, or -1 where there is none (gs_scan_pair).
func GsScanPair(p unsafe.Pointer, n int64, a *GsByteSet, d int64, b *GsByteSet) int64 {
	if d < 0 || d >= n {
		return -1
	}
	np := n - d // the positions both bytes fit at
	// A set of every byte leaves a search for the other.
	if a.Kind == 0 {
		if a.Invert != 0 {
			return GsScanAny(unsafe.Add(p, d), np, b)
		}
		return -1
	}
	if b.Kind == 0 {
		if b.Invert != 0 {
			return GsScanAny(p, np, a)
		}
		return -1
	}
	if np >= 8 && a.Kind <= 4 && b.Kind <= 4 {
		wa, wb := gsWordSetOf(a), gsWordSetOf(b)
		return gsScanPairWords(p, np, &wa, d, &wb)
	}
	return gsScanPairBytes(p, np, a, d, b)
}

// gsScanPairWords is GsScanPair over np >= 8 positions, a word of each
// set's bytes at a time, the last word ending at np as in gsScanAnyWords.
func gsScanPairWords(p unsafe.Pointer, np int64, wa *gsWordSet, d int64, wb *gsWordSet) int64 {
	aa, ab, ac, aflip, arng := wa.a, wa.b, wa.c, wa.flip, wa.rng
	ba, bb, bc, bflip, brng := wb.a, wb.b, wb.c, wb.flip, wb.rng
	for i := int64(0); i < np; i += 8 {
		j, shift := i, uint(0)
		if j > np-8 {
			j, shift = np-8, uint(i-np+8)*8
		}
		q := unsafe.Add(p, j)
		xa, xb := gsLoad64(q), gsLoad64(unsafe.Add(q, d))
		var ma, mb uint64
		if arng {
			ma = gsInRange(xa, aa, ab, ac)
		} else {
			ma = gsEqBytes(xa, aa, ab, ac)
		}
		if brng {
			mb = gsInRange(xb, ba, bb, bc)
		} else {
			mb = gsEqBytes(xb, ba, bb, bc)
		}
		if m := ((ma ^ aflip) & (mb ^ bflip)) >> shift; m != 0 {
			return i + int64(bits.TrailingZeros64(m)>>3)
		}
	}
	return -1
}

// gsScanPairBytes is the first of the np positions i with p[i] in a and
// p[i + d] in b.
func gsScanPairBytes(p unsafe.Pointer, np int64, a *GsByteSet, d int64, b *GsByteSet) int64 {
	for i := int64(0); i < np; i++ {
		if a.Members[*(*byte)(unsafe.Add(p, i))]&b.Members[*(*byte)(unsafe.Add(p, i+d))] != 0 {
			return i
		}
	}
	return -1
}
