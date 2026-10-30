package goose

import (
	"bytes"
	"encoding/binary"
	"math/bits"
	"strconv"
	"unsafe"
)

// What the runtime has for Go code that calls it itself, which no file the
// compiler writes calls (yet), and which the compiler therefore does not
// embed (cmake/go_runtime.cmake): conversions to and from Go's own values,
// the debug build's checks of `as` by the type of the value, memory moves,
// and the byte search of std's find_any and find_pair (gs_scan_any) by
// words.

// GsSliceFromSlice is the elements of xs as a GsSlice, without a copy. T
// must be laid out as the Goose element type is.
func GsSliceFromSlice[T any](xs []T) GsSlice {
	return GsSlice{Data: unsafe.Pointer(unsafe.SliceData(xs)), Len: int64(len(xs))}
}

// PtrFromAddr is the pointer an address held as an integer stands for: a
// data stack address that a value on a stack keeps as a uintptr, so that
// no Go pointer hides in memory the collector does not scan. It is for
// addresses inside data stack regions (or other memory outside the Go
// heap) only: the collector cannot know a Go object an integer points at.
func PtrFromAddr(addr uintptr) unsafe.Pointer {
	return unsafe.Add(nil, addr)
}

// GsAsFailI is a debug build's failed `as` check of the integer v into typ.
//
//go:noinline
func GsAsFailI(why string, v int64, typ, file string, line int) {
	GsAsFail(why, strconv.FormatInt(v, 10), typ, file, line)
}

// GsAsFailU is a debug build's failed `as` check of the unsigned v into typ.
//
//go:noinline
func GsAsFailU(why string, v uint64, typ, file string, line int) {
	GsAsFail(why, strconv.FormatUint(v, 10), typ, file, line)
}

// GsAsFailF is a debug build's failed `as` check of the float d into typ,
// d named in its text form (§3.7): an f32's own where f32 is set.
//
//go:noinline
func GsAsFailF(why string, d float64, f32 bool, typ, file string, line int) {
	GsAsFail(why, gsFloatText(d, f32), typ, file, line)
}

// GsF2UWrap is GsF2IWrap as an unsigned value: `as!` (and a release
// build's `as`) from a float to u64.
func GsF2UWrap(d float64) uint64 {
	return uint64(GsF2IWrap(d))
}

// GsZigEncode is v zigzag-encoded: 0, -1, 1, -2, ... as 0, 1, 2, 3, ...
func GsZigEncode(v int64) uint64 {
	return uint64(v)<<1 ^ uint64(v>>63)
}

// GsZigDecode is the value u zigzag-encodes.
func GsZigDecode(u uint64) int64 {
	return int64(u>>1) ^ -int64(u&1)
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
