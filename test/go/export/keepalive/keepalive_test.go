package goose

import (
	"runtime"
	"testing"
	"unsafe"
)

// Each case hands an entry point an argument as its last use: once the call
// has begun, only the call keeps alive the memory the argument views, and
// the Goose code it runs keeps that only where the collector does not look
// before it calls collect (keepalive.goose, helper.go). Go keeps a value
// alive only up to its last use, so the entry point must keep the argument
// alive until the call returns: otherwise the collector frees the memory
// while the call still reads it, and the call reads what the memory holds
// next (or, built with -d=checkptr, Go reports a bad pointer in its heap).

const n = 1 << 12

type ints struct{ xs [n]int64 }

type bytesBuf struct{ b [n]byte }

type pts struct{ ps [n / 16]Pt_g }

// freeable is a new T, whose finalizer closes the channel collect watches
// during the next call.
func freeable[T any]() *T {
	p := new(T)
	freed := make(chan struct{})
	runtime.SetFinalizer(p, func(*T) { close(freed) })
	watched = freed
	return p
}

// n int64 ones.
func ones() []int64 {
	p := freeable[ints]()
	for i := range p.xs {
		p.xs[i] = 1
	}
	return p.xs[:]
}

// n bytes of 1.
func byteOnes() []byte {
	p := freeable[bytesBuf]()
	for i := range p.b {
		p.b[i] = 1
	}
	return p.b[:]
}

// n bytes of 1, as a string viewing them, which nothing writes.
func stringOnes() string {
	b := byteOnes()
	return unsafe.String(unsafe.SliceData(b), len(b))
}

// n/16 points (2, 3).
func points() []Pt_g {
	p := freeable[pts]()
	for i := range p.ps {
		p.ps[i] = Pt_g{x_g: 2, y_g: 3}
	}
	return p.ps[:]
}

// A point (5, 2).
func point() *Pt_g {
	p := freeable[Pt_g]()
	p.x_g, p.y_g = 5, 2
	return p
}

func TestArgumentsLiveThroughTheCall(t *testing.T) {
	gs := NewStackBlock(0, gsStackReserve, gsStackGap)
	defer gs.Close()
	cases := []struct {
		name string
		want int64
		call func() int64
	}{
		{"KeptSum", n, func() int64 { return KeptSum(ones()) }},
		{"KeptSumWithStack", n, func() int64 { return KeptSumWithStack(gs, ones()) }},
		{"KeptBytes string", n, func() int64 { return KeptBytes(stringOnes()) }},
		{"KeptBytes []byte", n, func() int64 { return KeptBytes(byteOnes()) }},
		{"KeptBytesWithStack", n, func() int64 { return KeptBytesWithStack(gs, byteOnes()) }},
		{"KeptCount", n, func() int64 { return KeptCount(byteOnes(), 1) }},
		{"KeptCountWithStack", n, func() int64 { return KeptCountWithStack(gs, byteOnes(), 1) }},
		{"KeptPts", 6 * n / 16, func() int64 { return KeptPts(points()) }},
		{"KeptPtsWithStack", 6 * n / 16, func() int64 { return KeptPtsWithStack(gs, points()) }},
		{"KeptPt", 3, func() int64 { return KeptPt(point()) }},
		{"KeptPtWithStack", 3, func() int64 { return KeptPtWithStack(gs, point()) }},
		{"HeldSum", n + 1, func() int64 { return HeldSum(ones()) }},
		{"HeldSumWithStack", n + 1, func() int64 { return HeldSumWithStack(gs, ones()) }},
	}
	for _, c := range cases {
		freedInCall = false
		got := c.call()
		if freedInCall {
			t.Errorf("%s: the collector freed the argument during the call, which read %d (want %d)",
				c.name, got, c.want)
		} else if got != c.want {
			t.Errorf("%s = %d, want %d", c.name, got, c.want)
		}
		watched = nil
	}
}
