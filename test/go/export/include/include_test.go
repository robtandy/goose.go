package goose

import (
	"math/bits"
	"testing"
)

// include.goose's export fns, over the extern fns helper.go defines.

func TestExterns(t *testing.T) {
	h := uint64(1469598103934665603)
	for _, b := range []byte("goose") {
		h = bits.RotateLeft64((h^uint64(b))*1099511628211, 7)
	}
	if got := HashBytes("goose"); got != h {
		t.Errorf("HashBytes = %x, want %x", got, h)
	}
	if got := GreetingLen("go"); got != int64(len("<hello, go>")) {
		t.Errorf("GreetingLen = %d", got)
	}
	if got := GreetingLen(""); got != -1 {
		t.Errorf("GreetingLen(\"\") = %d", got)
	}
	buf := make([]byte, 32)
	n := Greeting([]byte("gopher"), buf)
	if string(buf[:n]) != "hello, gopher" {
		t.Errorf("Greeting = %q", buf[:n])
	}
	if Clamped(2.5) != 1 || Clamped(-1) != 0 || Clamped(0.25) != 0.25 {
		t.Errorf("Clamped: %v %v %v", Clamped(2.5), Clamped(-1), Clamped(0.25))
	}
	if got := Dist(-3, 4); got != 7 {
		t.Errorf("Dist = %d", got)
	}
}

func TestExternsZeroAllocs(t *testing.T) {
	data := []byte("the data a Go caller hands a Goose library")
	buf := make([]byte, 64)
	var sink int64
	cases := []struct {
		name string
		f    func()
	}{
		{"HashBytes", func() { sink += int64(HashBytes(data)) }},
		{"GreetingLen", func() { sink += GreetingLen("world") }},
		{"Greeting", func() { sink += Greeting(data[:8], buf) }},
		{"Clamped", func() { sink += int64(Clamped(0.5) * 4) }},
		{"Dist", func() { sink += Dist(3, -4) }},
	}
	for _, c := range cases {
		if n := testing.AllocsPerRun(1000, c.f); n != 0 {
			t.Errorf("%s: %v allocs/op, want 0", c.name, n)
		}
	}
	if sink == 0 {
		t.Errorf("sink %d", sink)
	}
}

func BenchmarkHashBytes(b *testing.B) {
	data := make([]byte, 4096)
	b.SetBytes(int64(len(data)))
	b.ReportAllocs()
	for i := 0; i < b.N; i++ {
		HashBytes(data)
	}
}
