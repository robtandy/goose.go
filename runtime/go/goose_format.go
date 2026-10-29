package goose

import (
	"math"
	"os"
	"strconv"
	"unsafe"
)

// ---------------------------------------------------------------------------
// Text forms (§3.7). Each writes a value's text at p, at most GsFmtMax
// bytes, and returns where the text ends; print renders a line on a data
// stack this way and writes it out whole. end is where the stack's region
// ends: a text that would run past it is a data stack overflow (gsOverflow),
// found before a byte of it is written, where the C runtime's guard gap
// finds it as the byte is written. A value is formatted in place while
// GsFmtMax bytes are left, and in a buffer of its own nearer the end.

// GsFmtMax is the most bytes a scalar's text takes.
//
//goose:internal
const GsFmtMax = 32

func gsFmtBuf(p unsafe.Pointer) []byte { return unsafe.Slice((*byte)(p), GsFmtMax)[:0] }

func gsRoom(p, end unsafe.Pointer) bool { return uintptr(end)-uintptr(p) >= GsFmtMax }

// GsFmtI64, GsFmtU64, GsFmtF64, GsFmtF32 and GsFmtBool write the text of
// a scalar of their type at p.
//
//goose:internal
func GsFmtI64(p, end unsafe.Pointer, v int64) unsafe.Pointer {
	if gsRoom(p, end) {
		return unsafe.Add(p, len(strconv.AppendInt(gsFmtBuf(p), v, 10)))
	}
	var b [GsFmtMax]byte
	return GsPutBytes(p, end, strconv.AppendInt(b[:0], v, 10))
}

//goose:internal
func GsFmtU64(p, end unsafe.Pointer, v uint64) unsafe.Pointer {
	if gsRoom(p, end) {
		return unsafe.Add(p, len(strconv.AppendUint(gsFmtBuf(p), v, 10)))
	}
	var b [GsFmtMax]byte
	return GsPutBytes(p, end, strconv.AppendUint(b[:0], v, 10))
}

//goose:internal
func GsFmtF64(p, end unsafe.Pointer, v float64) unsafe.Pointer {
	if gsRoom(p, end) {
		return unsafe.Add(p, len(gsAppendFloat(gsFmtBuf(p), v, false)))
	}
	var b [GsFmtMax]byte
	return GsPutBytes(p, end, gsAppendFloat(b[:0], v, false))
}

//goose:internal
func GsFmtF32(p, end unsafe.Pointer, v float32) unsafe.Pointer {
	if gsRoom(p, end) {
		return unsafe.Add(p, len(gsAppendFloat(gsFmtBuf(p), float64(v), true)))
	}
	var b [GsFmtMax]byte
	return GsPutBytes(p, end, gsAppendFloat(b[:0], float64(v), true))
}

//goose:internal
func GsFmtBool(p, end unsafe.Pointer, v bool) unsafe.Pointer {
	if v {
		return GsPutStr(p, end, "true")
	}
	return GsPutStr(p, end, "false")
}

// GsPutStr writes the bytes of s at p.
//
//goose:internal
func GsPutStr(p, end unsafe.Pointer, s string) unsafe.Pointer {
	if uintptr(len(s)) > uintptr(end)-uintptr(p) {
		gsOverflow(end)
	}
	copy(unsafe.Slice((*byte)(p), len(s)), s)
	return unsafe.Add(p, len(s))
}

// GsPutBytes writes the bytes of b at p.
//
//goose:internal
func GsPutBytes(p, end unsafe.Pointer, b []byte) unsafe.Pointer {
	if uintptr(len(b)) > uintptr(end)-uintptr(p) {
		gsOverflow(end)
	}
	copy(unsafe.Slice((*byte)(p), len(b)), b)
	return unsafe.Add(p, len(b))
}

// GsPutQuoted writes the n bytes at s quoted, as a u8 array inside an
// aggregate renders (gs_fmt_quoted): `"`, `\` and the line breaks and tabs
// escaped.
//
//goose:internal
func GsPutQuoted(p, end unsafe.Pointer, s unsafe.Pointer, n int64) unsafe.Pointer {
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

// GsOut writes the bytes from base up to end to stdout, in one write.
//
//goose:internal
func GsOut(base, end unsafe.Pointer) {
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
