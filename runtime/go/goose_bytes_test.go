package goose

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math"
	"math/big"
	"os"
	"os/exec"
	"runtime"
	"strconv"
	"strings"
	"testing"
	"unsafe"
)

// lcg is the generator the Goose tests draw from: next(n) is in [0, n).
type lcg uint64

func (s *lcg) next(n int64) int64 {
	*s = *s*6364136223846793005 + 1442695040888963407
	return int64((uint64(*s) >> 33) % uint64(n))
}

// rand64 is 64 random bits, drawn 31 at a time.
func (s *lcg) rand64() uint64 {
	return uint64(s.next(1<<31))<<33 | uint64(s.next(1<<31))<<2 | uint64(s.next(4))
}

// abortText runs f and returns the text of the *GsError it panicked with,
// or "" where it returned.
func abortText(f func()) (text string) {
	defer func() {
		if r := recover(); r != nil {
			e, ok := r.(*GsError)
			if !ok {
				panic(r)
			}
			text = e.Error()
		}
	}()
	f()
	return ""
}

const at = " (t.goose:7)" // where the checks below say they are

func TestAbortMessages(t *testing.T) {
	cases := []struct {
		f    func()
		want string
	}{
		{func() { GsPanic("cannot reserve data stack address space") }, "goose runtime error: cannot reserve data stack address space"},
		{func() { GsAbort(GsEAssert, "t.goose", 7) }, "goose runtime error: assert failed" + at},
		{func() { GsAbort(GsECapacity, "t.goose", 7) }, "goose runtime error: limited array capacity exceeded" + at},
		{func() { GsAbort(GsEPop, "t.goose", 7) }, "goose runtime error: pop on empty array" + at},
		{func() { GsAbort(GsESliceLen, "t.goose", 7) }, "goose runtime error: invalid slice length" + at},
		{func() { GsAbort(GsEPoolSlice, "t.goose", 7) }, "goose runtime error: slice not from this pool" + at},
		{func() { GsAbort(GsEQBound, "t.goose", 7) }, "goose runtime error: queue limit of zero or less" + at},
		{func() { GsAbort(GsEArrayLen, "t.goose", 7) }, "goose runtime error: invalid array length" + at},
		{func() { GsAbort(GsEArraySlice, "t.goose", 7) }, "goose runtime error: slice not within this array" + at},
		{func() { GsAbort(GsESpanTree, "t.goose", 7) }, "goose runtime error: slice pool span tree is corrupt" + at},
		{func() { panic(gsIdxErr(5, 3, "t.goose", 7)) }, "goose runtime error: index 5 out of bounds (length 3)" + at},
		{func() { GsIdx(-1, 3, "t.goose", 7) }, "goose runtime error: index -1 out of bounds (length 3)" + at},
		{func() { GsIdx(3, 3, "t.goose", 7) }, "goose runtime error: index 3 out of bounds (length 3)" + at},
		{func() { GsUpto(4, 3, "t.goose", 7) }, "goose runtime error: index 4 out of bounds (length 3)" + at},
		{func() { GsUpto(-1, 3, "t.goose", 7) }, "goose runtime error: index -1 out of bounds (length 3)" + at},
		{func() { GsDivFail("t.goose", 7) }, "goose runtime error: division by zero" + at},
		{func() { GsDivOvf("t.goose", 7) }, "goose runtime error: integer overflow in division" + at},
		{func() { GsOvf(2000000000, "+", 1000000000, "i32", "t.goose", 7) },
			"goose runtime error: integer overflow (debug): 2000000000 + 1000000000 at i32" + at},
		{func() { GsOvfNeg(-128, "i8", "t.goose", 7) }, "goose runtime error: integer overflow (debug): -(-128) at i8" + at},
		{func() { GsAsFailI("out of range", 256, "u8", "t.goose", 7) },
			"goose runtime error: as conversion out of range (debug): 256 as u8" + at},
		{func() { GsAsFailU("out of range", math.MaxUint64, "i64", "t.goose", 7) },
			"goose runtime error: as conversion out of range (debug): 18446744073709551615 as i64" + at},
		{func() { GsAsFailF("out of range", math.Inf(-1), false, "i32", "t.goose", 7) },
			"goose runtime error: as conversion out of range (debug): -inf as i32" + at},
		{func() { GsAsFailF("changes the value", float64(float32(0.1)), true, "i64", "t.goose", 7) },
			"goose runtime error: as conversion changes the value (debug): 0.1 as i64" + at},
	}
	for _, c := range cases {
		if got := abortText(c.f); got != c.want {
			t.Errorf("got %q, want %q", got, c.want)
		}
	}
	if len(gsErrMsgs) != GsESpanTree+1 {
		t.Errorf("%d error messages for %d ids", len(gsErrMsgs), GsESpanTree+1)
	}
	for id, m := range gsErrMsgs {
		if m == "" {
			t.Errorf("error id %d has no message", id)
		}
	}
	if i := GsIdx(2, 3, "t.goose", 7); i != 2 {
		t.Errorf("GsIdx(2, 3) = %d", i)
	}
	if i := GsUpto(3, 3, "t.goose", 7); i != 3 {
		t.Errorf("GsUpto(3, 3) = %d", i)
	}
}

// abort(msg)'s message is the program's bytes, copied before the stacks
// they may live on unwind (GsAbortSlice), or a literal's (GsAbortMsg).
func TestAbortMsgCopies(t *testing.T) {
	gs := NewStackBlock(1, 1<<20, 64<<10)
	defer gs.Close()
	msg := "custom failure"
	GsMemCpy(gs.Stks[0], unsafe.Pointer(unsafe.StringData(msg)), int64(len(msg)))
	var e *GsError
	func() {
		defer func() { e, _ = recover().(*GsError) }()
		GsAbortSlice(GsSlice{Data: gs.Stks[0], Len: int64(len(msg))}, "abort_msg.goose", 15)
	}()
	*(*byte)(gs.Stks[0]) = 'X'
	if e == nil || e.Msg != msg || e.Error() != "goose runtime error: custom failure (abort_msg.goose:15)" {
		t.Fatalf("abort(msg): %#v", e)
	}
	if got := abortText(func() { GsAbortSlice(GsSlice{}, "t.goose", 7) }); got != "goose runtime error: "+at {
		t.Errorf("an empty message: %q", got)
	}
	if got := abortText(func() { GsAbortMsg("a literal", "t.goose", 7) }); got != "goose runtime error: a literal"+at {
		t.Errorf("a literal message: %q", got)
	}
}

// A Goose program's main defers GsExitOnAbort: an abort ends the process
// with the message on stderr and exit status 1, as the C runtime ends it.
func TestExitOnAbort(t *testing.T) {
	switch os.Getenv("GOOSE_TEST_EXIT") {
	case "abort":
		defer GsExitOnAbort()
		os.Stdout.WriteString("output before\n")
		GsAbort(GsEPop, "exit.goose", 12)
		return
	case "panic":
		defer GsExitOnAbort()
		panic("not a goose error")
	case "return":
		defer GsExitOnAbort()
		return
	}
	run := func(mode string) (stdout, stderr string, code int) {
		cmd := exec.Command(os.Args[0], "-test.run=^TestExitOnAbort$")
		cmd.Env = append(os.Environ(), "GOOSE_TEST_EXIT="+mode)
		var out, errs bytes.Buffer
		cmd.Stdout, cmd.Stderr = &out, &errs
		err := cmd.Run()
		if ee, ok := err.(*exec.ExitError); ok {
			code = ee.ExitCode()
		} else if err != nil {
			t.Fatalf("%s: %v", mode, err)
		}
		return out.String(), errs.String(), code
	}
	stdout, stderr, code := run("abort")
	if code != 1 || stderr != "goose runtime error: pop on empty array (exit.goose:12)\n" ||
		!strings.Contains(stdout, "output before\n") {
		t.Errorf("an abort: exit %d, stderr %q, stdout %q", code, stderr, stdout)
	}
	_, stderr, code = run("panic")
	if code != 2 || !strings.Contains(stderr, "panic: not a goose error") {
		t.Errorf("another panic: exit %d, stderr %q", code, stderr)
	}
	if _, stderr, code = run("return"); code != 0 {
		t.Errorf("no panic: exit %d, stderr %q", code, stderr)
	}
}

// ---------------------------------------------------------------------------
// Integer semantics, against arbitrary-precision arithmetic.

func ovfText(a int64, op string, b int64, typ string) string {
	return fmt.Sprintf("goose runtime error: integer overflow (debug): %d %s %d at %s%s", a, op, b, typ, at)
}

// checkOp compares one checked operation with the exact result r: it must
// return r where r is in [lo, hi] and abort with the overflow message where
// it is not.
func checkOp(t *testing.T, name string, a, b int64, r *big.Int, lo, hi int64, op, typ string, f func() int64) {
	t.Helper()
	var got int64
	text := abortText(func() { got = f() })
	if r.IsInt64() && r.Int64() >= lo && r.Int64() <= hi {
		if text != "" || got != r.Int64() {
			t.Fatalf("%s(%d, %d) = %d, %q; want %v", name, a, b, got, text, r)
		}
	} else if want := ovfText(a, op, b, typ); text != want {
		t.Fatalf("%s(%d, %d) = %d, %q; want %q", name, a, b, got, text, want)
	}
}

func bigOf(v int64) *big.Int { return big.NewInt(v) }

type intType struct {
	name   string
	lo, hi int64
	add    func(a, b int64) int64
	sub    func(a, b int64) int64
	mul    func(a, b int64) int64
	neg    func(a int64) int64
}

var signedTypes = []intType{
	{"i8", math.MinInt8, math.MaxInt8,
		func(a, b int64) int64 { return int64(GsAddI8(int8(a), int8(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsSubI8(int8(a), int8(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsMulI8(int8(a), int8(b), "t.goose", 7)) },
		func(a int64) int64 { return int64(GsNegI8(int8(a), "t.goose", 7)) }},
	{"i16", math.MinInt16, math.MaxInt16,
		func(a, b int64) int64 { return int64(GsAddI16(int16(a), int16(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsSubI16(int16(a), int16(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsMulI16(int16(a), int16(b), "t.goose", 7)) },
		func(a int64) int64 { return int64(GsNegI16(int16(a), "t.goose", 7)) }},
	{"i32", math.MinInt32, math.MaxInt32,
		func(a, b int64) int64 { return int64(GsAddI32(int32(a), int32(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsSubI32(int32(a), int32(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsMulI32(int32(a), int32(b), "t.goose", 7)) },
		func(a int64) int64 { return int64(GsNegI32(int32(a), "t.goose", 7)) }},
	{"i64", math.MinInt64, math.MaxInt64,
		func(a, b int64) int64 { return GsAddI64(a, b, "t.goose", 7) },
		func(a, b int64) int64 { return GsSubI64(a, b, "t.goose", 7) },
		func(a, b int64) int64 { return GsMulI64(a, b, "t.goose", 7) },
		func(a int64) int64 { return GsNegI64(a, "t.goose", 7) }},
}

// operands are a type's edge values and a spread of others; all of them
// for i8.
func operands(ty intType, seed *lcg) []int64 {
	if ty.hi == math.MaxInt8 {
		var all []int64
		for v := ty.lo; v <= ty.hi; v++ {
			all = append(all, v)
		}
		return all
	}
	vs := []int64{ty.lo, ty.lo + 1, ty.lo / 2, -2, -1, 0, 1, 2, ty.hi / 2, ty.hi - 1, ty.hi}
	sq := int64(math.Sqrt(float64(ty.hi)))
	vs = append(vs, sq, sq+1, -sq, -sq-1)
	for len(vs) < 140 {
		v := int64(seed.rand64()) >> uint(seed.next(64))
		if v >= ty.lo && v <= ty.hi {
			vs = append(vs, v)
		}
	}
	return vs
}

func TestCheckedArithmetic(t *testing.T) {
	seed := lcg(1)
	for _, ty := range signedTypes {
		vs := operands(ty, &seed)
		for _, a := range vs {
			for _, b := range vs {
				checkOp(t, "add "+ty.name, a, b, new(big.Int).Add(bigOf(a), bigOf(b)), ty.lo, ty.hi, "+", ty.name,
					func() int64 { return ty.add(a, b) })
				checkOp(t, "sub "+ty.name, a, b, new(big.Int).Sub(bigOf(a), bigOf(b)), ty.lo, ty.hi, "-", ty.name,
					func() int64 { return ty.sub(a, b) })
				checkOp(t, "mul "+ty.name, a, b, new(big.Int).Mul(bigOf(a), bigOf(b)), ty.lo, ty.hi, "*", ty.name,
					func() int64 { return ty.mul(a, b) })
			}
			var got int64
			text := abortText(func() { got = ty.neg(a) })
			want := fmt.Sprintf("goose runtime error: integer overflow (debug): -(%d) at %s%s", a, ty.name, at)
			if a == ty.lo && text != want || a != ty.lo && (text != "" || got != -a) {
				t.Fatalf("neg %s(%d) = %d, %q", ty.name, a, got, text)
			}
		}
	}
}

type divType struct {
	name     string
	lo, hi   int64
	div, dbg func(a, b int64) int64 // release and debug a / b
	mod      func(a, b int64) int64
}

var signedDivs = []divType{
	{"i8", math.MinInt8, math.MaxInt8,
		func(a, b int64) int64 { return int64(GsDivI8(int8(a), int8(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsDivI8Debug(int8(a), int8(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsModI8(int8(a), int8(b), "t.goose", 7)) }},
	{"i16", math.MinInt16, math.MaxInt16,
		func(a, b int64) int64 { return int64(GsDivI16(int16(a), int16(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsDivI16Debug(int16(a), int16(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsModI16(int16(a), int16(b), "t.goose", 7)) }},
	{"i32", math.MinInt32, math.MaxInt32,
		func(a, b int64) int64 { return int64(GsDivI32(int32(a), int32(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsDivI32Debug(int32(a), int32(b), "t.goose", 7)) },
		func(a, b int64) int64 { return int64(GsModI32(int32(a), int32(b), "t.goose", 7)) }},
	{"i64", math.MinInt64, math.MaxInt64,
		func(a, b int64) int64 { return GsDivI64(a, b, "t.goose", 7) },
		func(a, b int64) int64 { return GsDivI64(a, b, "t.goose", 7) },
		func(a, b int64) int64 { return GsModI64(a, b, "t.goose", 7) }},
}

// Division truncates and aborts on zero; % is Euclidean, in [0, |b|), and
// x % -1 is 0. i64.min / -1 aborts in every build; at narrower types the
// release build wraps it and the debug build aborts.
func TestSignedDivMod(t *testing.T) {
	seed := lcg(2)
	const zero = "goose runtime error: division by zero" + at
	for _, ty := range signedDivs {
		intTy := intType{name: ty.name, lo: ty.lo, hi: ty.hi}
		vs := operands(intTy, &seed)
		for _, a := range vs {
			for _, b := range vs {
				var q, qd, m int64
				tq := abortText(func() { q = ty.div(a, b) })
				tqd := abortText(func() { qd = ty.dbg(a, b) })
				tm := abortText(func() { m = ty.mod(a, b) })
				if b == 0 {
					if tq != zero || tqd != zero || tm != zero {
						t.Fatalf("%s %d / 0: %q %q %q", ty.name, a, tq, tqd, tm)
					}
					continue
				}
				wantQ := new(big.Int).Quo(bigOf(a), bigOf(b))
				wantM := new(big.Int).Mod(bigOf(a), bigOf(b))
				if tm != "" || m != wantM.Int64() {
					t.Fatalf("%s %d %% %d = %d, %q; want %v", ty.name, a, b, m, tm, wantM)
				}
				if a == ty.lo && b == -1 {
					if ty.name == "i64" {
						if want := "goose runtime error: integer overflow in division" + at; tq != want || tqd != want {
							t.Fatalf("i64.min / -1: %q %q", tq, tqd)
						}
					} else {
						if tq != "" || q != ty.lo {
							t.Fatalf("%s.min / -1 in release: %d, %q; want it wrapped", ty.name, q, tq)
						}
						if want := ovfText(a, "/", b, ty.name); tqd != want {
							t.Fatalf("%s.min / -1 in debug: %d, %q", ty.name, qd, tqd)
						}
					}
					continue
				}
				if tq != "" || tqd != "" || q != wantQ.Int64() || qd != q {
					t.Fatalf("%s %d / %d = %d, %d (%q %q); want %v", ty.name, a, b, q, qd, tq, tqd, wantQ)
				}
			}
		}
	}
}

func TestUnsignedDivMod(t *testing.T) {
	const zero = "goose runtime error: division by zero" + at
	for a := 0; a < 256; a++ {
		for b := 0; b < 256; b++ {
			var q, m uint8
			tq := abortText(func() { q = GsDivU8(uint8(a), uint8(b), "t.goose", 7) })
			tm := abortText(func() { m = GsModU8(uint8(a), uint8(b), "t.goose", 7) })
			if b == 0 {
				if tq != zero || tm != zero {
					t.Fatalf("u8 %d / 0: %q %q", a, tq, tm)
				}
			} else if tq != "" || tm != "" || int(q) != a/b || int(m) != a%b {
				t.Fatalf("u8 %d / %d: %d %d", a, b, q, m)
			}
		}
	}
	us := []uint64{0, 1, 2, 3, 7, 1000, math.MaxUint16, math.MaxUint32, 1 << 40, math.MaxInt64, math.MaxUint64 - 1, math.MaxUint64}
	for _, a := range us {
		for _, b := range us {
			var got [6]uint64
			text := abortText(func() {
				got = [6]uint64{uint64(GsDivU16(uint16(a), uint16(b), "t.goose", 7)), uint64(GsModU16(uint16(a), uint16(b), "t.goose", 7)),
					uint64(GsDivU32(uint32(a), uint32(b), "t.goose", 7)), uint64(GsModU32(uint32(a), uint32(b), "t.goose", 7)),
					GsDivU64(a, b, "t.goose", 7), GsModU64(a, b, "t.goose", 7)}
			})
			switch {
			case uint16(b) == 0:
				if text != zero {
					t.Fatalf("u16 %d / %d: %q", a, b, text)
				}
			case text != "":
				t.Fatalf("%d / %d: %q", a, b, text)
			case got[0] != uint64(uint16(a)/uint16(b)) || got[1] != uint64(uint16(a)%uint16(b)) ||
				got[2] != uint64(uint32(a)/uint32(b)) || got[3] != uint64(uint32(a)%uint32(b)) ||
				got[4] != a/b || got[5] != a%b:
				t.Fatalf("%d / %d: %v", a, b, got)
			}
		}
	}
	for i, f := range []func(){
		func() { GsDivU16(1, 0, "t.goose", 7) }, func() { GsModU16(1, 0, "t.goose", 7) },
		func() { GsDivU32(1, 0, "t.goose", 7) }, func() { GsModU32(1, 0, "t.goose", 7) },
		func() { GsDivU64(1, 0, "t.goose", 7) }, func() { GsModU64(1, 0, "t.goose", 7) },
	} {
		if text := abortText(f); text != zero {
			t.Errorf("unsigned division %d by zero: %q", i, text)
		}
	}
}

// ---------------------------------------------------------------------------
// Conversions.

func TestRangeChecks(t *testing.T) {
	cases := []struct {
		v      int64
		lo, hi int64
		typ    string
		ok     bool
	}{
		{255, 0, 255, "u8", true}, {256, 0, 255, "u8", false}, {-1, 0, 255, "u8", false},
		{-128, math.MinInt8, math.MaxInt8, "i8", true}, {-129, math.MinInt8, math.MaxInt8, "i8", false},
		{math.MinInt64, math.MinInt64, math.MaxInt64, "i64", true},
		{math.MaxInt32 + 1, math.MinInt32, math.MaxInt32, "i32", false},
	}
	for _, c := range cases {
		var got int64
		text := abortText(func() { got = GsRangeI(c.v, c.lo, c.hi, c.typ, "t.goose", 7) })
		want := ""
		if !c.ok {
			want = fmt.Sprintf("goose runtime error: as conversion out of range (debug): %d as %s%s", c.v, c.typ, at)
		}
		if text != want || (c.ok && got != c.v) {
			t.Errorf("GsRangeI(%d) as %s: %d, %q", c.v, c.typ, got, text)
		}
	}
	if v := GsRangeU(math.MaxInt64, math.MaxInt64, "i64", "t.goose", 7); v != math.MaxInt64 {
		t.Errorf("GsRangeU = %d", v)
	}
	if text := abortText(func() { GsRangeU(1<<63, math.MaxInt64, "i64", "t.goose", 7) }); text !=
		"goose runtime error: as conversion out of range (debug): 9223372036854775808 as i64"+at {
		t.Errorf("GsRangeU(2^63) as i64: %q", text)
	}
}

func TestFloatToIntChecks(t *testing.T) {
	cases := []struct {
		d      float64
		f32    bool
		lo, hi int64
		typ    string
		want   int64
		why    string // "" where the value converts
	}{
		{1, false, math.MinInt8, math.MaxInt8, "i8", 1, ""},
		{-128, false, math.MinInt8, math.MaxInt8, "i8", -128, ""},
		{127.5, false, math.MinInt8, math.MaxInt8, "i8", 0, "changes the value (debug): 127.5"},
		{128, false, math.MinInt8, math.MaxInt8, "i8", 0, "out of range (debug): 128.0"},
		{-0.5, false, 0, math.MaxUint8, "u8", 0, "changes the value (debug): -0.5"},
		{-1, false, 0, math.MaxUint8, "u8", 0, "out of range (debug): -1.0"},
		{-0.0, false, 0, math.MaxUint8, "u8", 0, ""},
		{math.NaN(), false, math.MinInt32, math.MaxInt32, "i32", 0, "out of range (debug): nan"},
		{math.Inf(-1), false, math.MinInt32, math.MaxInt32, "i32", 0, "out of range (debug): -inf"},
		{-0x1p63, false, math.MinInt64, math.MaxInt64, "i64", math.MinInt64, ""},
		{0x1p63, false, math.MinInt64, math.MaxInt64, "i64", 0, "out of range (debug): 9.223372036854776e+18"},
		{float64(float32(0.1)), true, math.MinInt64, math.MaxInt64, "i64", 0, "changes the value (debug): 0.1"},
		{float64(float32(2.0)), true, math.MinInt64, math.MaxInt64, "i64", 2, ""},
		{float64(float32(3e9)), true, math.MinInt32, math.MaxInt32, "i32", 0, "out of range (debug): 3000000000.0"},
	}
	for _, c := range cases {
		var got int64
		text := abortText(func() { got = GsF2I(c.d, c.f32, c.lo, c.hi, c.typ, "t.goose", 7) })
		want := ""
		if c.why != "" {
			want = "goose runtime error: as conversion " + c.why + " as " + c.typ + at
		}
		if text != want || got != c.want {
			t.Errorf("GsF2I(%v) as %s: %d, %q; want %d, %q", c.d, c.typ, got, text, c.want, want)
		}
	}
	ucases := []struct {
		d    float64
		f32  bool
		want uint64
		why  string
	}{
		{0, false, 0, ""}, {-0.0, false, 0, ""},
		{18446744073709549568.0, false, 18446744073709549568, ""},
		{0x1p64, false, 0, "out of range (debug): 1.8446744073709552e+19"},
		{-1, false, 0, "out of range (debug): -1.0"},
		{0.5, false, 0, "changes the value (debug): 0.5"},
		{math.NaN(), false, 0, "out of range (debug): nan"},
		{float64(float32(1e19)), true, 9999999980506447872, ""},
		{float64(float32(1e20)), true, 0, "out of range (debug): 1e+20"},
	}
	for _, c := range ucases {
		var got uint64
		text := abortText(func() { got = GsF2U(c.d, c.f32, "t.goose", 7) })
		want := ""
		if c.why != "" {
			want = "goose runtime error: as conversion " + c.why + " as u64" + at
		}
		if text != want || got != c.want {
			t.Errorf("GsF2U(%v): %d, %q; want %d, %q", c.d, got, text, c.want, want)
		}
	}
}

// test/runtime/float_int_wrap.goose's values, and the C runtime's text and
// conversions of each (test/expected/float_int_wrap.out): d, then d as! i64,
// u64, i32, u32, i16 and u8.
var floatIntWrap = []string{
	"0.0 0 0 0 0 0 0",
	"-0.0 0 0 0 0 0 0",
	"0.5 0 0 0 0 0 0",
	"-0.5 0 0 0 0 0 0",
	"0.9999999999999999 0 0 0 0 0 0",
	"-0.9999999999999999 0 0 0 0 0 0",
	"2.5 2 2 2 2 2 2",
	"-2.5 -2 18446744073709551614 -2 4294967294 -2 254",
	"255.9 255 255 255 255 255 255",
	"256.0 256 256 256 256 256 0",
	"-129.5 -129 18446744073709551487 -129 4294967167 -129 127",
	"65535.75 65535 65535 65535 65535 -1 255",
	"4294967296.5 4294967296 4294967296 0 0 0 0",
	"4503599627370497.0 4503599627370497 4503599627370497 1 1 1 1",
	"9.223372036854775e+18 9223372036854774784 9223372036854774784 -1024 4294966272 -1024 0",
	"-9.223372036854775e+18 -9223372036854774784 9223372036854776832 1024 1024 1024 0",
	"-9.223372036854776e+18 -9223372036854775808 9223372036854775808 0 0 0 0",
	"9.223372036854776e+18 -9223372036854775808 9223372036854775808 0 0 0 0",
	"-9.223372036854778e+18 9223372036854773760 9223372036854773760 -2048 4294965248 -2048 0",
	"1.2345678901234567e+19 -6101065172474984448 12345678901234567168 -350287872 3944679424 2048 0",
	"1.844674407370955e+19 -2048 18446744073709549568 -2048 4294965248 -2048 0",
	"1.8446744073709552e+19 0 0 0 0 0 0",
	"-1.8446744073709552e+19 0 0 0 0 0 0",
	"1.8446744073709556e+19 4096 4096 4096 4096 4096 0",
	"-1.8446744073709556e+19 -4096 18446744073709547520 -4096 4294963200 -4096 0",
	"3e+19 -6893488147419103232 11553255926290448384 -1648885760 2646081536 0 0",
	"-3e+19 6893488147419103232 6893488147419103232 1648885760 1648885760 0 0",
	"1e+300 0 0 0 0 0 0",
	"-1e+300 0 0 0 0 0 0",
}

func TestFloatToIntWrap(t *testing.T) {
	ds := []float64{0.0, math.Copysign(0, -1), 0.5, -0.5, 0.9999999999999999, -0.9999999999999999, 2.5, -2.5,
		255.9, 256.0, -129.5, 65535.75, 4294967296.5, 4503599627370497.0,
		9223372036854774784.0, -9223372036854774784.0, -9223372036854775808.0,
		9223372036854775808.0, -9223372036854777856.0, 12345678901234567890.0,
		18446744073709549568.0, 18446744073709551616.0, -18446744073709551616.0,
		18446744073709555712.0, -18446744073709555712.0, 30000000000000000000.0,
		-30000000000000000000.0, 1e300, -1e300}
	for i, d := range ds {
		w := GsF2IWrap(d)
		got := fmt.Sprintf("%s %d %d %d %d %d %d", gsAppendFloat(nil, d, false), w, GsF2UWrap(d),
			int32(w), uint32(w), int16(w), uint8(w))
		if got != floatIntWrap[i] {
			t.Errorf("got  %s\nwant %s", got, floatIntWrap[i])
		}
	}
	nan := math.NaN()
	if GsF2IWrap(nan) != 0 || GsF2UWrap(nan) != 0 || GsF2IWrap(math.Inf(1)) != 0 || GsF2IWrap(math.Inf(-1)) != 0 {
		t.Error("NaN or an infinity does not wrap to 0")
	}
	f32s := []struct {
		f    float32
		want string
	}{
		{-1.5, "-1.5 -1 18446744073709551615 -1"},
		{3e9, "3000000000.0 3000000000 3000000000 -1294967296"},
		{-3e9, "-3000000000.0 -3000000000 18446744070709551616 1294967296"},
		{1e19, "1e+19 -8446744093203103744 9999999980506447872 0"},
		{1e20, "1e+20 7766281635539976192 7766281635539976192 0"},
	}
	for _, c := range f32s {
		d := float64(c.f)
		got := fmt.Sprintf("%s %d %d %d", gsAppendFloat(nil, d, true), GsF2IWrap(d), GsF2UWrap(d), int32(GsF2IWrap(d)))
		if got != c.want {
			t.Errorf("got  %s\nwant %s", got, c.want)
		}
	}
	// Any value: the exact truncation, modulo 2^64.
	seed := lcg(3)
	two64 := new(big.Int).Lsh(big.NewInt(1), 64)
	for i := 0; i < 20000; i++ {
		d := math.Float64frombits(seed.rand64())
		if math.IsNaN(d) || math.IsInf(d, 0) {
			continue
		}
		want, _ := new(big.Float).SetFloat64(math.Trunc(d)).Int(nil)
		want.Mod(want, two64)
		if got := GsF2UWrap(d); got != want.Uint64() {
			t.Fatalf("GsF2UWrap(%v) = %d, want %v", d, got, want)
		}
	}
}

// The text forms of test/runtime/float_text_digits.goose and
// float_text_f32.goose, as the C runtime prints them
// (test/expected/float_text_*.out).
func TestFloatText(t *testing.T) {
	pow2 := func(k int) float64 { return math.Ldexp(1, k) }
	f64s := []struct {
		v    float64
		want string
	}{
		{0.1, "0.1"}, {1.5, "1.5"}, {1.000000000000001, "1.000000000000001"},
		{0.3333333333333333, "0.3333333333333333"}, {123456789012345.6, "123456789012345.6"}, {4.35, "4.35"},
		{5e-324, "5e-324"}, {1e-310, "1e-310"}, {2.2250738585072014e-308, "2.2250738585072014e-308"},
		{1.7976931348623157e308, "1.7976931348623157e+308"}, {1e20, "1e+20"}, {100.0, "100.0"},
		{0.30000000000000004, "0.30000000000000004"}, {0.5, "0.5"}, {math.Copysign(0, -1), "-0.0"},
		{0.0, "0.0"}, {1.0 / 3.0, "0.3333333333333333"}, {2.0 / 3.0, "0.6666666666666666"},
		{pow2(-1017), "7.120236347223045e-307"}, {pow2(-662), "5.225680706521042e-200"},
		{pow2(-140), "7.174648137343064e-43"}, {pow2(-97), "6.310887241768095e-30"},
		{pow2(-77), "6.617444900424222e-24"}, {pow2(-44), "5.684341886080802e-14"},
		{pow2(-24), "5.960464477539063e-08"}, {pow2(89), "6.189700196426902e+26"},
		{pow2(122), "5.316911983139664e+36"}, {-pow2(976), "-6.386688990511104e+293"},
		{pow2(49) + 0.25, "562949953421312.2"}, {pow2(49) + 0.75, "562949953421312.8"},
		{1e-4, "0.0001"}, {1.5e-5, "1.5e-05"}, {1e15, "1e+15"}, {123456789012345.0, "123456789012345.0"},
		{1234567890123456.0, "1234567890123456.0"}, {12345678901234567.0, "12345678901234568.0"}, {1e16, "1e+16"}, {1.5e16, "1.5e+16"},
		{math.Inf(1), "inf"}, {math.Inf(-1), "-inf"}, {math.NaN(), "nan"}, {-math.NaN(), "nan"},
	}
	for _, c := range f64s {
		if got := string(gsAppendFloat(nil, c.v, false)); got != c.want {
			t.Errorf("f64 %v: %q, want %q", c.v, got, c.want)
		}
	}
	f32s := []struct {
		v    float32
		want string
	}{
		{0.1, "0.1"}, {1.0 / 3.0, "0.33333334"}, {1.0, "1.0"}, {-0.1, "-0.1"}, {1e-7, "1e-07"},
		{3.4e38, "3.4e+38"}, {16777216.0, "16777216.0"}, {1e6, "1000000.0"}, {1.0000001, "1.0000001"},
		{1e-45, "1e-45"}, {123456.7, "123456.7"}, {0.3, "0.3"}, {2.5e-10, "2.5e-10"}, {1e15, "1e+15"},
		{1e16, "1e+16"}, {1e17, "1e+17"}, {1e29, "1e+29"},
		{float32(pow2(-96)), "1.2621775e-29"}, {float32(pow2(87)), "1.5474251e+26"},
		{float32(pow2(90)), "1.2379401e+27"}, {float32(pow2(21) + 0.25), "2097152.2"},
		{float32(pow2(21) + 0.75), "2097152.8"},
	}
	for _, c := range f32s {
		if got := string(gsAppendFloat(nil, float64(c.v), true)); got != c.want {
			t.Errorf("f32 %v: %q, want %q", c.v, got, c.want)
		}
	}
	// Whatever the layout, the text reads back as the value.
	seed := lcg(4)
	for i := 0; i < 20000; i++ {
		d := math.Float64frombits(seed.rand64())
		if math.IsNaN(d) || math.IsInf(d, 0) {
			continue
		}
		text := string(gsAppendFloat(nil, d, false))
		if back, err := strconv.ParseFloat(text, 64); err != nil || back != d {
			t.Fatalf("%v prints as %q", d, text)
		}
		f := math.Float32frombits(uint32(seed.rand64()))
		if math.IsNaN(float64(f)) || math.IsInf(float64(f), 0) {
			continue
		}
		text = string(gsAppendFloat(nil, float64(f), true))
		if back, err := strconv.ParseFloat(text, 32); err != nil || float32(back) != f {
			t.Fatalf("f32 %v prints as %q", f, text)
		}
	}
}

// The rest of C99's <math.h> where Go's math differs or has none, against
// glibc's values: fmin and fmax are IEEE 754's minNum and maxNum, fdim is
// +0.0 of two equal infinities, nextafter of equal operands is y, lgamma is
// +Inf at -Inf, modf's fraction of an infinity is a zero, and remquo's
// quotient bits are glibc's.
func TestC99Math(t *testing.T) {
	negz, inf, qnan := math.Copysign(0, -1), math.Inf(1), math.NaN()
	snan := math.Float64frombits(0x7ff4000000000001)
	same := func(a, b float64) bool { return math.Float64bits(a) == math.Float64bits(b) || a != a && b != b }
	for _, c := range []struct{ x, y, min, max float64 }{
		{2, -1, -1, 2}, {qnan, 3, 3, 3}, {3, qnan, 3, 3}, {negz, 0, negz, 0}, {0, negz, negz, 0},
		{-inf, inf, -inf, inf}, {qnan, qnan, qnan, qnan}, {snan, 3, qnan, qnan}, {3, snan, qnan, qnan},
	} {
		if m := GsFmin(c.x, c.y); !same(m, c.min) {
			t.Errorf("fmin(%v, %v) = %v, want %v", c.x, c.y, m, c.min)
		}
		if m := GsFmax(c.x, c.y); !same(m, c.max) {
			t.Errorf("fmax(%v, %v) = %v, want %v", c.x, c.y, m, c.max)
		}
		// An f32 of the f64's signaling NaN is a quiet one.
		if math.Float64bits(c.x) == math.Float64bits(snan) || math.Float64bits(c.y) == math.Float64bits(snan) {
			continue
		}
		x, y := float32(c.x), float32(c.y)
		if m := GsFminF32(x, y); !same(float64(m), c.min) {
			t.Errorf("fminf(%v, %v) = %v, want %v", x, y, m, c.min)
		}
		if m := GsFmaxF32(x, y); !same(float64(m), c.max) {
			t.Errorf("fmaxf(%v, %v) = %v, want %v", x, y, m, c.max)
		}
	}
	// An f32's signaling NaN, which no f64 of it is.
	if s32 := math.Float32frombits(0x7fa00001); GsFminF32(s32, 3) == GsFminF32(s32, 3) ||
		GsFmaxF32(3, s32) == GsFmaxF32(3, s32) || GsFminF32(math.Float32frombits(0x7fc00001), 3) != 3 {
		t.Error("fminf and fmaxf of an f32's signaling NaN are not a quiet NaN")
	}
	for _, c := range []struct{ x, y, want float64 }{
		{5, 3, 2}, {3, 5, 0}, {inf, inf, 0}, {-inf, -inf, 0}, {inf, -inf, inf}, {qnan, 1, qnan}, {1, qnan, qnan},
	} {
		if d := GsFdim(c.x, c.y); !same(d, c.want) {
			t.Errorf("fdim(%v, %v) = %v, want %v", c.x, c.y, d, c.want)
		}
	}
	if n := GsNextafter(0, negz); !same(n, negz) || !same(GsNextafter(1, 2)-1, math.Ldexp(1, -52)) {
		t.Errorf("nextafter(0, -0.0) = %v", n)
	}
	if n := GsNextafterF32(float32(negz), 0); math.Float32bits(n) != 0 || GsNextafterF32(1, 2)-1 != 1.0/8388608 {
		t.Errorf("nextafterf(-0.0, 0) = %v", n)
	}
	if l := GsLgamma(-inf); l != inf || GsLgamma(1) != 0 || GsLgamma(negz) != inf || GsLgamma(-3) != inf {
		t.Errorf("lgamma(-inf) = %v", l)
	}
	for _, c := range []struct{ x, frac, ip float64 }{
		{-3.25, -0.25, -3}, {2.5, 0.5, 2}, {-0.5, -0.5, negz}, {-4, negz, -4}, {inf, 0, inf}, {-inf, negz, -inf},
		{qnan, qnan, qnan}, {1e300, 0, 1e300},
	} {
		var ip float64
		if f := GsModf(c.x, &ip); !same(f, c.frac) || !same(ip, c.ip) {
			t.Errorf("modf(%v) = %v, %v, want %v, %v", c.x, f, ip, c.frac, c.ip)
		}
		var ipf float32
		if f := GsModfF32(float32(c.x), &ipf); c.x != 1e300 && (!same(float64(f), c.frac) || !same(float64(ipf), c.ip)) {
			t.Errorf("modff(%v) = %v, %v, want %v, %v", c.x, f, ipf, c.frac, c.ip)
		}
	}
	// glibc's remquo and remquof (q 99 where it is left as it was).
	for _, c := range []struct {
		x, y, r float64
		q       int32
		rf      float64
		qf      int32
	}{
		{10, 3, 1, 3, 1, 3}, {-10, 3, -1, -3, -1, -3}, {10, -3, 1, -3, 1, -3}, {7, 2, -1, 4, -1, 4},
		{5, 2, 1, 2, 1, 2}, {1e10, 3, 1, 5, 1, 5}, {-1e10, 3.5, -0.5, -1, -0.5, -1}, {29, 4, 1, 7, 1, 7},
		{3, 3, 0, 1, 0, 1}, {-3, 3, negz, -1, negz, -1}, {0.75, 0.5, -0.25, 2, -0.25, 2},
		{1e300, 1e-300, 0x1.4f722a6f79f9cp-998, 2, qnan, 99}, {5e-324, 1e-323, 5e-324, 0, qnan, 99},
		{1, 0, qnan, 99, qnan, 99}, {inf, 1, qnan, 99, qnan, 99}, {1, qnan, qnan, 99, qnan, 99}, {1, inf, 1, 0, 1, 0},
	} {
		q, qf := int32(99), int32(99)
		if r := GsRemquo(c.x, c.y, &q); !same(r, c.r) || q != c.q {
			t.Errorf("remquo(%v, %v) = %v, %d, want %v, %d", c.x, c.y, r, q, c.r, c.q)
		}
		if r := GsRemquoF32(float32(c.x), float32(c.y), &qf); !same(float64(r), c.rf) || qf != c.qf {
			t.Errorf("remquof(%v, %v) = %v, %d, want %v, %d", c.x, c.y, r, qf, c.rf, c.qf)
		}
	}
}

// float_text::shortest_digits (gs_f64_digits): the digits print writes, by
// magnitude, and the power of ten they are a fraction of, truncated to the
// slice they go into; nothing for zero, NaN and the infinities.
func TestF64Digits(t *testing.T) {
	digits := func(x float64, room int) (string, int64, int64) {
		dig := make([]byte, room)
		point := int64(-12345)
		n := GsF64Digits(x, GsSlice{Data: unsafe.Pointer(unsafe.SliceData(dig)), Len: int64(room)}, &point)
		return string(dig[:n]), n, point
	}
	cases := []struct {
		v     float64
		dig   string
		point int64
	}{
		{0.1, "1", 0}, {1.5, "15", 1}, {-1.5, "15", 1}, {123.45, "12345", 3}, {100.0, "1", 3},
		{0.30000000000000004, "30000000000000004", 0}, {5e-324, "5", -323}, {1e-310, "1", -309},
		{1.7976931348623157e308, "17976931348623157", 309}, {2.2250738585072014e-308, "22250738585072014", -307},
		{1e20, "1", 21}, {math.Ldexp(1, 49) + 0.25, "5629499534213122", 15}, {-0.000125, "125", -3},
	}
	for _, c := range cases {
		if d, n, point := digits(c.v, 17); d != c.dig || n != int64(len(c.dig)) || point != c.point {
			t.Errorf("%v: %q (%d) point %d, want %q point %d", c.v, d, n, point, c.dig, c.point)
		}
	}
	for _, v := range []float64{0, math.Copysign(0, -1), math.NaN(), math.Inf(1), math.Inf(-1)} {
		if d, n, point := digits(v, 17); n != 0 || d != "" || point != -12345 {
			t.Errorf("%v: %q (%d) point %d, want no digits and point untouched", v, d, n, point)
		}
	}
	// A slice shorter than the digits takes their first ones; the point is
	// the value's still.
	if d, n, point := digits(0.30000000000000004, 5); d != "30000" || n != 5 || point != 0 {
		t.Errorf("into 5 bytes: %q (%d) point %d", d, n, point)
	}
	// The digits are print's, and read back as the value.
	seed := lcg(11)
	for i := 0; i < 20000; i++ {
		x := math.Float64frombits(seed.rand64())
		if math.IsNaN(x) || math.IsInf(x, 0) || x == 0 {
			continue
		}
		d, n, point := digits(x, 17)
		back, err := strconv.ParseFloat("0."+d+"e"+strconv.FormatInt(point, 10), 64)
		if err != nil || back != math.Abs(x) || n < 1 || n > 17 || d[0] == '0' || d[n-1] == '0' {
			t.Fatalf("%v: digits %q point %d", x, d, point)
		}
		text := string(gsAppendFloat(nil, math.Abs(x), false))
		if strings.Trim(strings.ReplaceAll(strings.SplitN(text, "e", 2)[0], ".", ""), "0") != d {
			t.Fatalf("%v: digits %q, print's %q", x, d, text)
		}
	}
}

// ---------------------------------------------------------------------------
// Wide loads and stores (issue #62): each the integer encoding/binary reads
// at every offset of a buffer on a data stack, in both orders, and each
// store the bytes it writes, the bytes around them untouched.

func TestWideLoadsStores(t *testing.T) {
	gs := NewStackBlock(1, 1<<20, 64<<10)
	defer gs.Close()
	buf := unsafe.Slice((*byte)(gs.Base[0]), 32)
	seed := lcg(62)
	fill := func() {
		for i := range buf {
			buf[i] = byte(seed.next(256))
		}
	}
	at := func(off int) unsafe.Pointer { return unsafe.Add(gs.Base[0], off) }
	for off := 0; off+8 <= len(buf); off++ {
		fill()
		b := buf[off:]
		for _, c := range []struct {
			name      string
			got, want uint64
		}{
			{"GsLe16", uint64(GsLe16((*[2]byte)(at(off)))), uint64(binary.LittleEndian.Uint16(b))},
			{"GsBe16", uint64(GsBe16((*[2]byte)(at(off)))), uint64(binary.BigEndian.Uint16(b))},
			{"GsLe32", uint64(GsLe32((*[4]byte)(at(off)))), uint64(binary.LittleEndian.Uint32(b))},
			{"GsBe32", uint64(GsBe32((*[4]byte)(at(off)))), uint64(binary.BigEndian.Uint32(b))},
			{"GsLe64", GsLe64((*[8]byte)(at(off))), binary.LittleEndian.Uint64(b)},
			{"GsBe64", GsBe64((*[8]byte)(at(off))), binary.BigEndian.Uint64(b)},
		} {
			if c.got != c.want {
				t.Errorf("%s at %d = %#x, want %#x", c.name, off, c.got, c.want)
			}
		}
		v := uint64(seed.next(math.MaxInt64))<<1 | 1
		le, be := binary.ByteOrder(binary.LittleEndian), binary.ByteOrder(binary.BigEndian)
		for _, c := range []struct {
			name  string
			n     int
			order binary.ByteOrder
			put   func()
		}{
			{"GsPutLe16", 2, le, func() { GsPutLe16((*[2]byte)(at(off)), uint16(v)) }},
			{"GsPutBe16", 2, be, func() { GsPutBe16((*[2]byte)(at(off)), uint16(v)) }},
			{"GsPutLe32", 4, le, func() { GsPutLe32((*[4]byte)(at(off)), uint32(v)) }},
			{"GsPutBe32", 4, be, func() { GsPutBe32((*[4]byte)(at(off)), uint32(v)) }},
			{"GsPutLe64", 8, le, func() { GsPutLe64((*[8]byte)(at(off)), v) }},
			{"GsPutBe64", 8, be, func() { GsPutBe64((*[8]byte)(at(off)), v) }},
		} {
			fill()
			want := append([]byte(nil), buf...)
			switch c.n {
			case 2:
				c.order.PutUint16(want[off:], uint16(v))
			case 4:
				c.order.PutUint32(want[off:], uint32(v))
			default:
				c.order.PutUint64(want[off:], v)
			}
			c.put()
			if !bytes.Equal(buf, want) {
				t.Errorf("%s at %d wrote %x, want %x", c.name, off, buf, want)
			}
		}
	}
}

// ---------------------------------------------------------------------------
// varints.

func TestVarints(t *testing.T) {
	gs := NewStackBlock(1, 1<<20, 64<<10)
	defer gs.Close()
	p := gs.Base[0]
	vals := []uint64{0, 1, 127, 128, 255, 300, 16383, 16384, 1<<21 - 1, 1 << 21, 1<<35 + 5, 1<<56 - 1,
		1 << 63, math.MaxInt64, math.MaxUint64}
	seed := lcg(5)
	for i := 0; i < 2000; i++ {
		vals = append(vals, seed.rand64()>>uint(seed.next(64)))
	}
	for _, v := range vals {
		n := GsUlebWrite(p, v)
		size := int64(1)
		for x := v >> 7; x != 0; x >>= 7 {
			size++
		}
		if n != size || GsUlebSize(p) != n || uint64(GsUlebRead(p)) != v {
			t.Fatalf("%d: wrote %d bytes, size %d, read %d", v, n, GsUlebSize(p), GsUlebRead(p))
		}
		if got, k := GsUlebCheck(p, 0, n); got != v || k != n {
			t.Fatalf("GsUlebCheck of %d: %d, %d bytes", v, got, k)
		}
		if _, k := GsUlebCheck(p, 0, n-1); k != 0 {
			t.Fatalf("GsUlebCheck of %d cut short: %d bytes", v, k)
		}
		// At an offset into the image, bounded by its end.
		*(*byte)(unsafe.Add(p, n)) = 0
		GsUlebWrite(unsafe.Add(p, 1), v)
		if got, k := GsUlebCheck(p, 1, n+1); got != v || k != n {
			t.Fatalf("GsUlebCheck of %d at 1: %d, %d bytes", v, got, k)
		}
		if _, k := GsUlebCheck(p, 1, n); k != 0 {
			t.Fatalf("GsUlebCheck of %d at 1 cut short: %d bytes", v, k)
		}
		// Zigzag: the signed values round the same encodings.
		s := int64(v)
		if GsZigDecode(GsZigEncode(s)) != s {
			t.Fatalf("zigzag of %d", s)
		}
		n = GsZigWrite(p, s)
		if GsZigRead(p) != s || GsUlebSize(p) != n {
			t.Fatalf("zigzag varint of %d", s)
		}
		if got, k := GsZigCheck(p, 0, n); got != s || k != n {
			t.Fatalf("GsZigCheck of %d: %d, %d bytes", s, got, k)
		}
	}
	for v, want := range map[int64]uint64{0: 0, -1: 1, 1: 2, -2: 3, 2: 4, math.MaxInt64: math.MaxUint64 - 1,
		math.MinInt64: math.MaxUint64} {
		if GsZigEncode(v) != want || GsZigDecode(want) != v {
			t.Errorf("zigzag %d is %d, want %d", v, GsZigEncode(v), want)
		}
	}
	bad := [][]byte{
		{},                 // nothing
		{0x80},             // runs past the end
		{0x80, 0x00},       // a redundant high zero group
		{0xff, 0x80, 0x00}, // likewise
		{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x02},       // a bit above the 64th
		{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x81, 0x00}, // past ten bytes
		{0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01}, // likewise
	}
	for _, b := range bad {
		GsMemCpy(p, unsafe.Pointer(unsafe.SliceData(b)), int64(len(b)))
		if v, k := GsUlebCheck(p, 0, int64(len(b))); k != 0 {
			t.Errorf("GsUlebCheck(% x) = %d, %d bytes; want it rejected", b, v, k)
		}
	}
	u64max := []byte{0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x01}
	GsMemCpy(p, unsafe.Pointer(&u64max[0]), 10)
	if v, k := GsUlebCheck(p, 0, 10); v != math.MaxUint64 || k != 10 {
		t.Errorf("GsUlebCheck of u64.max: %d, %d bytes", v, k)
	}
	if _, k := GsUlebCheck(p, 0, -1); k != 0 {
		t.Error("GsUlebCheck reads before an image that ends before it starts")
	}
	if _, k := GsUlebCheck(p, 10, 10); k != 0 {
		t.Error("GsUlebCheck reads at the end of an image")
	}
}

// ---------------------------------------------------------------------------
// Memory.

func TestMemory(t *testing.T) {
	gs := NewStackBlock(1, 1<<20, 64<<10)
	defer gs.Close()
	a, b := gs.Base[0], unsafe.Add(gs.Base[0], 4096)
	for n := int64(0); n <= 300; n++ {
		for i := int64(0); i < n; i++ {
			*(*byte)(unsafe.Add(a, i)) = byte(i*7 + n)
		}
		GsMemCpy(b, a, n)
		if !GsMemEq(a, b, n) {
			t.Fatalf("%d bytes copied are not equal", n)
		}
		for i := int64(0); i < n; i++ {
			q := (*byte)(unsafe.Add(b, i))
			*q ^= 0x10
			if GsMemEq(a, b, n) {
				t.Fatalf("%d bytes differing at %d are equal", n, i)
			}
			*q ^= 0x10
		}
	}
	if !GsMemEq(nil, nil, 0) || !GsMemEq(a, nil, 0) {
		t.Error("no bytes are not equal")
	}
	GsMemCpy(nil, nil, 0)
	GsMemMove(nil, a, 0)
	// Overlapping moves, up and down, of every length a short copy takes.
	for _, mv := range []func(dst, src unsafe.Pointer, n int64){GsMemMove, GsMemCpy} {
		for _, shift := range []int64{1, 3, 4, 7, 8, 100} {
			for n := int64(1); n <= 1000; n += 1 + n/8 {
				for i := int64(0); i < n+shift; i++ {
					*(*byte)(unsafe.Add(a, i)) = byte(i)
				}
				mv(unsafe.Add(a, shift), a, n)
				for i := int64(0); i < n; i++ {
					if *(*byte)(unsafe.Add(a, shift+i)) != byte(i) {
						t.Fatalf("a move of %d bytes %d up", n, shift)
					}
				}
				for i := int64(0); i < n+shift; i++ {
					*(*byte)(unsafe.Add(a, i)) = byte(i)
				}
				mv(a, unsafe.Add(a, shift), n)
				for i := int64(0); i < n; i++ {
					if *(*byte)(unsafe.Add(a, i)) != byte(i+shift) {
						t.Fatalf("a move of %d bytes %d down", n, shift)
					}
				}
			}
		}
	}
}

// ---------------------------------------------------------------------------
// Slices.

// s[lo..hi] (GsSub) of a Go slice an export fn's entry point views in place
// (GsSliceOf, GsBytesOf), which ends where its allocation on Go's heap ends:
// an address one past that is no valid Go pointer, which -d=checkptr (and
// -race, which turns it on) aborts on, and which Go's collector may take
// for a bad pointer into its heap. So s[s.len..] and s[s.len..s.len] start
// at gsSubEnd, where no slice of s does, and every other slice where its
// elements are, empty ones included, as C's are: empty slices at different
// places stay different slices (issue #39).
func TestSubAtTheEnd(t *testing.T) {
	end := unsafe.Pointer(&gsSubEnd)
	b := make([]uint32, 4)
	s := GsSliceOf(b[1:4:4]) // ends where b's allocation does
	if s.Data != unsafe.Pointer(&b[1]) || s.Len != 3 {
		t.Fatalf("GsSliceOf(b[1:4:4]) = %p, %d; want %p, 3", s.Data, s.Len, &b[1])
	}
	// s[s.len..] and s[s.len..s.len] are both this call in the generated
	// code, whose omitted bound is the length.
	e := GsSub(s.Data, s.Len, s.Len, s.Len, 4)
	if e.Data != end || e.Len != 0 {
		t.Errorf("s[3..3] = %p, %d; want %p (gsSubEnd), 0", e.Data, e.Len, end)
	}
	for lo := int64(0); lo <= s.Len; lo++ {
		for hi := lo; hi <= s.Len; hi++ {
			want := end
			if lo < s.Len {
				want = unsafe.Pointer(&b[1+lo])
			}
			if got := GsSub(s.Data, lo, hi, s.Len, 4); got.Data != want || got.Len != hi-lo {
				t.Errorf("s[%d..%d] = %p, %d; want %p, %d", lo, hi, got.Data, got.Len, want, hi-lo)
			}
		}
	}
	// The empty slice at the end slices and converts as any empty one.
	if again := GsSub(e.Data, 0, 0, e.Len, 4); again != e {
		t.Errorf("s[3..][0..] = %p, %d; want %p, 0", again.Data, again.Len, e.Data)
	}
	if xs := GsSliceToSlice[uint32](e); len(xs) != 0 || unsafe.Pointer(unsafe.SliceData(xs)) != end {
		t.Errorf("s[3..] as a []uint32: %p, %d", unsafe.SliceData(xs), len(xs))
	}
	if ss := GsSliceToSlice[GsSlice](e); len(ss) != 0 {
		t.Errorf("s[3..] as a []GsSlice: %d", len(ss))
	}

	// The same of bytes: a []byte's, and a string's on Go's heap.
	bs := make([]byte, 16)
	u := GsSliceOf(bs[5:16:16])
	if e := GsSub(u.Data, u.Len, u.Len, u.Len, 1); e.Data != end || e.Len != 0 {
		t.Errorf("bytes[11..] = %p, %d; want %p, 0", e.Data, e.Len, end)
	}
	if e := GsSub(u.Data, 10, 10, u.Len, 1); e.Data != unsafe.Pointer(&bs[15]) || e.Len != 0 {
		t.Errorf("bytes[10..10] = %p, %d; want %p, 0", e.Data, e.Len, &bs[15])
	}
	str := strings.Repeat("goose", 3)
	v := GsBytesOf(str)
	if e := GsSub(v.Data, v.Len, v.Len, v.Len, 1); e.Data != end || e.Len != 0 || GsSliceToString(e) != "" {
		t.Errorf("str[15..] = %p, %d; want %p, 0", e.Data, e.Len, end)
	}

	// Where the slice would not move, it is where its run is: an empty
	// run's, nil included, and a run of elements of no bytes.
	if e := GsSub(nil, 0, 0, 0, 4); e != (GsSlice{}) {
		t.Errorf("nil[0..0] = %p, %d", e.Data, e.Len)
	}
	if e := GsSub(s.Data, 0, 0, 0, 4); e.Data != s.Data || e.Len != 0 {
		t.Errorf("an empty run's [0..0] = %p, %d; want %p, 0", e.Data, e.Len, s.Data)
	}
	if e := GsSub(s.Data, 3, 3, 3, 0); e.Data != s.Data || e.Len != 0 {
		t.Errorf("zero-size elements' [3..3] = %p, %d; want %p, 0", e.Data, e.Len, s.Data)
	}
	runtime.KeepAlive(b)
	runtime.KeepAlive(bs)
	runtime.KeepAlive(str)
}

// subSinks hold slices where Go's collector looks, as variables of the
// generated code may.
var subSinks []GsSlice

// Go's collector checks each pointer it finds into its heap, -d=checkptr or
// not: one past the last object of a span whose objects stop short of its
// end is a bad pointer to it, which ends the process. The slices at the
// ends of objects filling several spans, the last of each among them, pass
// a collection (issue #39).
func TestSubAtTheEndOfSpans(t *testing.T) {
	// 48-byte objects, which stop short of the end of their 8 KB spans.
	objs := make([][]byte, 1000)
	subSinks = make([]GsSlice, len(objs))
	for i := range objs {
		objs[i] = make([]byte, 48)
		s := GsSliceOf(objs[i])
		subSinks[i] = GsSub(s.Data, s.Len, s.Len, s.Len, 1)
	}
	runtime.GC()
	for i, e := range subSinks {
		if e.Data != unsafe.Pointer(&gsSubEnd) || e.Len != 0 {
			t.Fatalf("object %d's [48..] = %p, %d; want %p, 0", i, e.Data, e.Len, &gsSubEnd)
		}
	}
	subSinks = nil
}

// ---------------------------------------------------------------------------
// Byte search.

// byteSet is std's byte_set (stdlib/std.goose): the set's members, its
// tables, and the kind a block is tested by.
func byteSet(members *[256]bool) GsByteSet {
	s := GsByteSet{Kind: 5}
	n := 0
	for b := 0; b < 256; b++ {
		if members[b] {
			s.Members[b] = 1
			n++
			bit := uint8(1) << ((b >> 4) & 7)
			if b < 128 {
				s.Lo[b&15] |= bit
			} else {
				s.Hi[b&15] |= bit
			}
		}
	}
	invert := n > 128
	k, first, last := 0, 0, 0
	for b := 0; b < 256; b++ {
		if members[b] != invert {
			if k == 0 {
				first = b
			}
			if k < 3 {
				s.Bytes[k] = uint8(b)
			}
			last = b
			k++
		}
	}
	if k <= 3 {
		s.Kind = uint8(k)
		if invert {
			s.Invert = 1
		}
		for j := k; j < 3; j++ {
			s.Bytes[j] = s.Bytes[0]
		}
	} else if last-first+1 == k {
		s.Kind = 4
		if invert {
			s.Invert = 1
		}
		s.Bytes[0] = uint8(first)
		s.Bytes[1] = uint8(last - first)
	}
	return s
}

func byteSetOf(bs ...byte) GsByteSet {
	var m [256]bool
	for _, b := range bs {
		m[b] = true
	}
	return byteSet(&m)
}

func byteRange(lo, hi int, invert bool) GsByteSet {
	var m [256]bool
	for b := 0; b < 256; b++ {
		m[b] = (b >= lo && b <= hi) != invert
	}
	return byteSet(&m)
}

// testSets are sets of every kind, complemented or not.
func testSets(seed *lcg) []GsByteSet {
	sets := []GsByteSet{
		byteSetOf(), byteRange(0, 255, false), // kind 0, and every byte
		byteSetOf('"'), byteSetOf('"', '\\'), byteSetOf('\n', '\r', 0), byteSetOf(0), byteSetOf(255),
		byteRange('0', '9', false), byteRange(0, 31, false), byteRange(128, 255, false), byteRange(0, 127, false),
		byteRange('a', 'z', true), byteRange(1, 254, false), byteRange(200, 203, false),
		byteRange(0, 254, false), byteRange(1, 255, false), byteRange(10, 10, true), byteRange(10, 11, true),
	}
	for len(sets) < 60 {
		var m [256]bool
		density := 1 + seed.next(250)
		for b := range m {
			m[b] = seed.next(256) < density
		}
		sets = append(sets, byteSet(&m))
	}
	for i := 0; i < 20; i++ {
		lo := seed.next(256)
		hi := lo + seed.next(256-lo)
		sets = append(sets, byteRange(int(lo), int(hi), seed.next(2) == 0))
	}
	return sets
}

func scanAnyRef(b []byte, s *GsByteSet) int64 {
	for i, c := range b {
		if s.Members[c] != 0 {
			return int64(i)
		}
	}
	return -1
}

func scanPairRef(b []byte, a *GsByteSet, d int64, s *GsByteSet) int64 {
	for i := int64(0); i+d < int64(len(b)) && d >= 0; i++ {
		if a.Members[b[i]]&s.Members[b[i+d]] != 0 {
			return i
		}
	}
	return -1
}

func TestByteSetKinds(t *testing.T) {
	cases := []struct {
		s            GsByteSet
		kind, invert uint8
	}{
		{byteSetOf(), 0, 0}, {byteRange(0, 255, false), 0, 1}, {byteSetOf('x'), 1, 0},
		{byteSetOf('x', 'y'), 2, 0}, {byteSetOf(1, 5, 9), 3, 0}, {byteSetOf(1, 5, 9, 13), 5, 0},
		{byteRange('0', '9', false), 4, 0}, {byteRange('a', 'a', true), 1, 1},
		{byteRange('a', 'z', true), 4, 1},
	}
	for _, c := range cases {
		if c.s.Kind != c.kind || c.s.Invert != c.invert {
			t.Errorf("kind %d invert %d, want %d %d", c.s.Kind, c.s.Invert, c.kind, c.invert)
		}
	}
	if unsafe.Sizeof(GsByteSet{}) != 293 {
		t.Errorf("GsByteSet is %d bytes, std's ByteSet 293", unsafe.Sizeof(GsByteSet{}))
	}
}

// Every word test agrees with the set's members on every byte.
func TestByteSetWordTests(t *testing.T) {
	words := make([]uint64, 32)
	for i := range words {
		for k := 0; k < 8; k++ {
			words[i] |= uint64(i*8+k) << (8 * k)
		}
	}
	check := func(s *GsByteSet) {
		w := gsWordSetOf(s)
		for _, x := range words {
			var m uint64
			if w.rng {
				m = gsInRange(x, w.a, w.b, w.c)
			} else {
				m = gsEqBytes(x, w.a, w.b, w.c)
			}
			m ^= w.flip
			for k := 0; k < 8; k++ {
				in := m>>(8*k)&0xff == 0x80
				if m>>(8*k)&0x7f != 0 || in != (s.Members[byte(x>>(8*k))] != 0) {
					t.Fatalf("kind %d %v: byte %#x tests %#x", s.Kind, s.Bytes, byte(x>>(8*k)), m>>(8*k)&0xff)
				}
			}
		}
	}
	for lo := 0; lo < 256; lo++ {
		for hi := lo; hi < 256; hi++ {
			for _, inv := range []bool{false, true} {
				if s := byteRange(lo, hi, inv); s.Kind == 4 {
					check(&s)
				}
			}
		}
	}
	seed := lcg(6)
	for i := 0; i < 3000; i++ {
		var bs []byte
		for j := seed.next(3); j >= 0; j-- {
			bs = append(bs, byte(seed.next(256)))
		}
		s := byteSetOf(bs...)
		check(&s)
		var m [256]bool
		for b := range m {
			m[b] = true
		}
		for _, b := range bs {
			m[b] = false
		}
		s = byteSet(&m)
		check(&s)
	}
}

// The searches agree with testing each byte against the set, over runs
// that end where a data stack's guard gap begins, so that a load past the
// end would fault.
func TestScan(t *testing.T) {
	gs := NewStackBlock(1, 64<<10, 64<<10)
	defer gs.Close()
	end := unsafe.Add(gs.Base[0], gs.Cap[0])
	seed := lcg(7)
	sets := testSets(&seed)
	buf := make([]byte, 200)
	for round := 0; round < 3000; round++ {
		n := seed.next(int64(len(buf)))
		if round%4 == 0 {
			n = seed.next(20)
		}
		a := &sets[seed.next(int64(len(sets)))]
		b := &sets[seed.next(int64(len(sets)))]
		// Mostly bytes outside a, now and then one inside it.
		var outside, inside []byte
		for c := 0; c < 256; c++ {
			if a.Members[c] != 0 {
				inside = append(inside, byte(c))
			} else {
				outside = append(outside, byte(c))
			}
		}
		for i := range buf[:n] {
			switch {
			case len(inside) > 0 && (len(outside) == 0 || seed.next(40) == 0):
				buf[i] = inside[seed.next(int64(len(inside)))]
			case len(outside) > 0 && seed.next(8) != 0:
				buf[i] = outside[seed.next(int64(len(outside)))]
			default:
				buf[i] = byte(seed.next(256))
			}
		}
		p := unsafe.Add(end, -n)
		GsMemCpy(p, unsafe.Pointer(&buf[0]), n)
		data := buf[:n]
		if got, want := GsScanAny(p, n, a), scanAnyRef(data, a); got != want {
			t.Fatalf("GsScanAny(% x) kind %d invert %d: %d, want %d", data, a.Kind, a.Invert, got, want)
		}
		for _, d := range []int64{0, 1, 2, 3, 7, 8, 9, seed.next(n + 2), n - 1, n, n + 1, -1} {
			if got, want := GsScanPair(p, n, a, d, b), scanPairRef(data, a, d, b); got != want {
				t.Fatalf("GsScanPair(% x, %d) kinds %d/%d invert %d/%d: %d, want %d",
					data, d, a.Kind, b.Kind, a.Invert, b.Invert, got, want)
			}
		}
	}
	empty := byteSetOf()
	if GsScanAny(nil, 0, &empty) != -1 || GsScanPair(nil, 0, &empty, 0, &empty) != -1 {
		t.Error("a search of nothing finds something")
	}
}

func BenchmarkScanAny(b *testing.B) {
	data := bytes.Repeat([]byte("the quick brown fox jumps over the lazy dog, "), 100)
	data = append(data, '\\', '"', '9', 0xfe)
	p := unsafe.Pointer(&data[0])
	n := int64(len(data))
	for _, c := range []struct {
		name string
		set  GsByteSet
	}{
		{"one byte", byteSetOf('"')},
		{"two bytes", byteSetOf('"', '\\')},
		{"range", byteRange('0', '9', false)},
		{"any other byte", byteRange(' ', 'z', true)},
		{"table", byteSetOf(1, 5, 9, 0xfe)},
	} {
		b.Run(c.name, func(b *testing.B) {
			b.SetBytes(n)
			for i := 0; i < b.N; i++ {
				if GsScanAny(p, n, &c.set) < 0 {
					b.Fatal("not found")
				}
			}
		})
	}
}

func BenchmarkMemEq(b *testing.B) {
	for _, n := range []int64{8, 16, 64, 1024} {
		x, y := bytes.Repeat([]byte{'a'}, int(n)), bytes.Repeat([]byte{'a'}, int(n))
		b.Run(strconv.FormatInt(n, 10), func(b *testing.B) {
			b.SetBytes(n)
			for i := 0; i < b.N; i++ {
				if !GsMemEq(unsafe.Pointer(&x[0]), unsafe.Pointer(&y[0]), n) {
					b.Fatal("unequal")
				}
			}
		})
	}
}
