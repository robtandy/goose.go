//go:build ignore

// The Go side of 24_call_c.goose under --target=go: call_c.h, function for
// function, in Go. `goose --target=go --include call_c.go` splices it into
// the file it writes, in that file's package, where the program's types and
// the runtime's are at hand: `Stats` is `Stats_g`, with its fields `lo_g`,
// `hi_g` and `mean_g`, a slice is a `GsSlice` and a `u8[>..]&` builder a
// `GsRref`, which a function appending to it is handed after the data stack
// block. The functions keep their plain names because an `extern fn` calls
// the Go function its symbol names. libm's cbrt and hypot are Go's
// math.Cbrt and math.Hypot there; C's abs, from stdlib.h, is here. The
// build constraint keeps the go tool from building this file on its own.
package main

import "hash/crc32"

// abs is C's abs, which the sample binds as c_abs.
func abs(x int32) int32 {
	if x < 0 {
		return -x
	}
	return x
}

// crc32_bytes is CRC-32 (IEEE) over a byte slice, viewed in place.
func crc32_bytes(s GsSlice) uint32 { return crc32.ChecksumIEEE(GsSliceToBytes(s)) }

// stats_of fills a Goose struct through a pointer: min, max and mean of the
// slice.
func stats_of(xs GsSlice, out *Stats_g) {
	v := GsSliceToSlice[int32](xs)
	var sum int64
	out.lo_g = 0
	if len(v) > 0 {
		out.lo_g = v[0]
	}
	out.hi_g = out.lo_g
	for _, x := range v {
		out.lo_g = min(out.lo_g, x)
		out.hi_g = max(out.hi_g, x)
		sum += int64(x)
	}
	out.mean_g = 0
	if len(v) > 0 {
		out.mean_g = float64(sum) / float64(len(v))
	}
}

// c_version appends text to a Goose string builder, the way the Go side
// hands bytes back. It names the library it ports, so that the sample
// prints the same under both targets.
func c_version(gs *GsStackBlock, out GsRref) {
	GsBldAppendString(gs, out, "C library 1.0")
}
