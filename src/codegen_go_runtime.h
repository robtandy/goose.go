// Goose compiler — the runtime the Go backend writes into every file it
// generates (CodeGenGo::Source, codegen_go.h), after the package clause, the
// imports (math, os, strconv, syscall, unsafe, each of which it uses) and the
// configuration constants gsStackReserve and gsStackGap. It is Go source as
// it is written out, in pieces under the 16 KB a string literal may have in
// MSVC; edit it here, and check the result with go vet on a generated file
// (test/run_go_tests.py does, for every program it compiles).
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

// NewStackBlock reserves numStacks data stacks of reserve usable bytes each,
// with gap bytes of PROT_NONE guard behind each one.
func NewStackBlock(numStacks int, reserve, gap int64) *GsStackBlock {
	gs := &GsStackBlock{reserve: reserve, gap: gap}
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
// exit status 1. Any other panic goes on.
func GsExitOnAbort() {
	if r := recover(); r != nil {
		if e, ok := r.(*GsError); ok {
			os.Stderr.WriteString(e.Error() + "\n")
			os.Exit(1)
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
// stack this way and writes it out whole.

const gsFmtMax = 32

func gsFmtBuf(p unsafe.Pointer) []byte { return unsafe.Slice((*byte)(p), gsFmtMax)[:0] }

func gsFmtI64(p unsafe.Pointer, v int64) unsafe.Pointer {
	return unsafe.Add(p, len(strconv.AppendInt(gsFmtBuf(p), v, 10)))
}

func gsFmtU64(p unsafe.Pointer, v uint64) unsafe.Pointer {
	return unsafe.Add(p, len(strconv.AppendUint(gsFmtBuf(p), v, 10)))
}

func gsFmtF64(p unsafe.Pointer, v float64) unsafe.Pointer {
	return unsafe.Add(p, len(gsAppendFloat(gsFmtBuf(p), v, false)))
}

func gsFmtF32(p unsafe.Pointer, v float32) unsafe.Pointer {
	return unsafe.Add(p, len(gsAppendFloat(gsFmtBuf(p), float64(v), true)))
}

func gsFmtBool(p unsafe.Pointer, v bool) unsafe.Pointer {
	if v {
		return gsPutStr(p, "true")
	}
	return gsPutStr(p, "false")
}

// gsPutStr writes the bytes of s at p.
func gsPutStr(p unsafe.Pointer, s string) unsafe.Pointer {
	copy(unsafe.Slice((*byte)(p), len(s)), s)
	return unsafe.Add(p, len(s))
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
};

}  // namespace goose
