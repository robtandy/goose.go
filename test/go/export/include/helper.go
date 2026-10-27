//go:build ignore

// The Go that include.goose's extern fns call, spliced into the file
// `goose --target=go --include helper.go` writes: in its package, with the
// generated types (Pt_g) and the runtime's (GsSlice, GsRref) at hand. The
// build constraint keeps the go tool from building it on its own here.
package goose

import (
	"math"
	"math/bits"
)

// mix is extern fn mix: a step of FNV-1a, rotated.
func mix(h, b uint64) uint64 { return bits.RotateLeft64((h^b)*1099511628211, 7) }

// greet is extern fn greet, which appends to a builder: it is handed the
// data stack block first.
func greet(gs *GsStackBlock, name GsSlice, out GsRref) bool {
	if name.Len == 0 {
		return false
	}
	GsBldAppendString(gs, out, "hello, ")
	GsBldAppend(gs, out, GsSliceToBytes(name))
	return true
}

func goClamp(x, lo, hi float64) float64 { return math.Max(lo, math.Min(hi, x)) }

func manhattan(p *Pt_g) int64 {
	abs := func(v int32) int64 {
		if v < 0 {
			return -int64(v)
		}
		return int64(v)
	}
	return abs(p.x_g) + abs(p.y_g)
}
