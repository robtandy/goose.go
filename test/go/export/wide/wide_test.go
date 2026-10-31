package goose

import (
	"bytes"
	"encoding/binary"
	"fmt"
	"math/rand"
	"testing"
)

// The wide loads and stores of wide.goose (issue #23) against
// encoding/binary's, at every offset of slices of every length up to 24 and
// one past either end, the last bytes included; an offset whose bytes are
// not all in the slice reads as 1 << 63 and writes nothing.

const outside = 1 << 63

func data(n int, seed int64) []byte {
	b := make([]byte, n)
	rand.New(rand.NewSource(seed)).Read(b)
	return b
}

func TestLoads(t *testing.T) {
	type load struct {
		name  string
		n     int
		got   func(s []byte, i int64) uint64
		gotS  func(s string, i int64) uint64
		order binary.ByteOrder
	}
	loads := []load{
		{"Le16", 2, Le16[[]byte], Le16[string], binary.LittleEndian},
		{"Be16", 2, Be16[[]byte], Be16[string], binary.BigEndian},
		{"Le32", 4, Le32[[]byte], Le32[string], binary.LittleEndian},
		{"Be32", 4, Be32[[]byte], Be32[string], binary.BigEndian},
		{"Le64", 8, Le64[[]byte], Le64[string], binary.LittleEndian},
		{"Be64", 8, Be64[[]byte], Be64[string], binary.BigEndian},
	}
	want := func(order binary.ByteOrder, b []byte, n int) uint64 {
		switch n {
		case 2:
			return uint64(order.Uint16(b))
		case 4:
			return uint64(order.Uint32(b))
		}
		return order.Uint64(b)
	}
	for size := 0; size <= 24; size++ {
		s := data(size, int64(size))
		for _, l := range loads {
			for i := -2; i <= size+1; i++ {
				w := uint64(outside)
				if i >= 0 && i+l.n <= size {
					w = want(l.order, s[i:], l.n)
				}
				if got := l.got(s, int64(i)); got != w {
					t.Errorf("%s(len %d, %d) = %#x, want %#x", l.name, size, i, got, w)
				}
				if got := l.gotS(string(s), int64(i)); got != w {
					t.Errorf("%s(string of len %d, %d) = %#x, want %#x", l.name, size, i, got, w)
				}
			}
		}
		head, tail := uint64(outside), uint64(outside)
		if size >= 8 {
			head = binary.LittleEndian.Uint64(s)
		}
		if size >= 4 {
			tail = uint64(binary.BigEndian.Uint32(s[size-4:]))
		}
		if got := Le64Head(s); got != head {
			t.Errorf("Le64Head(len %d) = %#x, want %#x", size, got, head)
		}
		if got := Be32Tail(s); got != tail {
			t.Errorf("Be32Tail(len %d) = %#x, want %#x", size, got, tail)
		}
		for i := -1; i <= size; i++ {
			for j := -1; j <= 5; j++ {
				w := uint64(outside)
				if i >= 0 && i+8 <= size && j >= 0 && j <= 4 {
					w = uint64(binary.LittleEndian.Uint32(s[i+j:]))
				}
				if got := Le32ViaLocal(s, int64(i), int64(j)); got != w {
					t.Errorf("Le32ViaLocal(len %d, %d, %d) = %#x, want %#x", size, i, j, got, w)
				}
			}
		}
	}
	var a [8]uint8
	copy(a[:], data(8, 99))
	if got, w := Be64Of(&a), binary.BigEndian.Uint64(a[:]); got != w {
		t.Errorf("Be64Of = %#x, want %#x", got, w)
	}
	if got := Swap64(0x0102030405060708); got != 0x0807060504030201 {
		t.Errorf("Swap64 = %#x", got)
	}
}

func TestStores(t *testing.T) {
	type store struct {
		name  string
		n     int
		put   func(o []byte, j int64, v uint64) bool
		order binary.ByteOrder
	}
	stores := []store{
		{"PutLe16", 2, PutLe16, binary.LittleEndian},
		{"PutBe16", 2, PutBe16, binary.BigEndian},
		{"PutLe32", 4, PutLe32, binary.LittleEndian},
		{"PutBe32", 4, PutBe32, binary.BigEndian},
		{"PutLe64", 8, PutLe64, binary.LittleEndian},
		{"PutBe64", 8, PutBe64, binary.BigEndian},
		{"PutLe32Unrolled", 4, PutLe32Unrolled, binary.LittleEndian},
		{"PutBe64Unrolled", 8, PutBe64Unrolled, binary.BigEndian},
	}
	put := func(order binary.ByteOrder, b []byte, n int, v uint64) {
		switch n {
		case 2:
			order.PutUint16(b, uint16(v))
		case 4:
			order.PutUint32(b, uint32(v))
		default:
			order.PutUint64(b, v)
		}
	}
	for size := 0; size <= 24; size++ {
		for _, st := range stores {
			for j := -2; j <= size+1; j++ {
				v := 0x0123456789ABCDEF * uint64(j+3)
				got, want := data(size, int64(size)), data(size, int64(size))
				in := j >= 0 && j+st.n <= size
				if in {
					put(st.order, want[j:], st.n, v)
				}
				if ok := st.put(got, int64(j), v); ok != in || !bytes.Equal(got, want) {
					t.Errorf("%s(len %d, %d, %#x) = %v: % x, want %v: % x", st.name, size, j, v, ok, got, in, want)
				}
			}
		}
	}
}

// A call in a loop allocates nothing; a local array stays on Go's stack.
func TestWideZeroAllocs(t *testing.T) {
	s := data(64, 1)
	o := make([]byte, 64)
	var sink uint64
	for _, f := range []func(){
		func() { sink += Le64(s, 3) + Be32(s, 9) + Le16(s, 62) },
		func() { PutBe64(o, 56, sink); PutLe16(o, 1, sink) },
		func() { sink += Swap64(sink) + Le32ViaLocal(s, 8, 3) },
	} {
		if n := testing.AllocsPerRun(1000, f); n != 0 {
			t.Errorf("%v allocs/op, want 0", n)
		}
	}
	_ = fmt.Sprint(sink)
}

func TestSums(t *testing.T) {
	for size := 0; size <= 40; size++ {
		s := data(size, int64(size)+100)
		var want uint64
		for i := 0; i+8 <= size; i += 8 {
			want += binary.LittleEndian.Uint64(s[i:])
		}
		if got := SumLe64(s); got != want {
			t.Errorf("SumLe64(len %d) = %#x, want %#x", size, got, want)
		}
		if got := SumLe64Bytewise(s); got != want {
			t.Errorf("SumLe64Bytewise(len %d) = %#x, want %#x", size, got, want)
		}
	}
}

// digest2k is digest_2k in Go: 8 rounds of each 64 bytes of s[..2048] that s
// holds, each block's state stored big-endian into o[8*b..] where o holds it.
func digest2k(s, o []byte) uint64 {
	h := uint64(14695981039346656037)
	for blk := 0; blk < 32; blk++ {
		i, k := 64*blk, 8*blk
		if i > len(s)-64 || k > len(o)-8 {
			continue
		}
		for j := i; j < i+64; j += 8 {
			h = ((h ^ binary.LittleEndian.Uint64(s[j:])) * 1099511628211) ^ (h >> 29)
		}
		binary.BigEndian.PutUint64(o[k:], h)
	}
	return h
}

// The function Go takes to be big (issue #62), whose 256 loads and 32 stores
// are each one load or store with no call: what it computes and stores, for
// inputs that hold every block, some of them and none.
func TestDigest2k(t *testing.T) {
	for _, c := range []struct{ size, out int }{{2048, 256}, {4000, 300}, {2047, 256}, {1000, 256}, {2048, 100}, {0, 0}} {
		s := data(c.size, int64(c.size)+7)
		got, want := make([]byte, c.out), make([]byte, c.out)
		if g, w := Digest2kBytes(s, got), digest2k(s, want); g != w || !bytes.Equal(got, want) {
			t.Errorf("Digest2k(len %d, len %d) = %#x, want %#x; stored %x, want %x", c.size, c.out, g, w, got, want)
		}
	}
}

// go test -bench . compares the wide loads with the bytes read one by one,
// and with encoding/binary's own, and each call with encoding/binary's.
func BenchmarkDigest2k(b *testing.B) {
	s, o := data(2048, 5), make([]byte, 256)
	b.SetBytes(int64(len(s)))
	var sum uint64
	for i := 0; i < b.N; i++ {
		sum += Digest2kBytes(s, o)
	}
	_ = sum
}

func BenchmarkSumLe64(b *testing.B) {
	s := data(4096, 3)
	b.SetBytes(int64(len(s)))
	var sum uint64
	for i := 0; i < b.N; i++ {
		sum += SumLe64(s)
	}
	_ = sum
}

func BenchmarkSumLe64Bytewise(b *testing.B) {
	s := data(4096, 3)
	b.SetBytes(int64(len(s)))
	var sum uint64
	for i := 0; i < b.N; i++ {
		sum += SumLe64Bytewise(s)
	}
	_ = sum
}

func BenchmarkSumBinaryLe64(b *testing.B) {
	s := data(4096, 3)
	b.SetBytes(int64(len(s)))
	var sum uint64
	for i := 0; i < b.N; i++ {
		for j := 0; j+8 <= len(s); j += 8 {
			sum += binary.LittleEndian.Uint64(s[j:])
		}
	}
	_ = sum
}

func BenchmarkLe64(b *testing.B) {
	s := data(4096, 2)
	var sum uint64
	for i := 0; i < b.N; i++ {
		for j := int64(0); j+8 <= int64(len(s)); j += 8 {
			sum += Le64(s, j)
		}
	}
	_ = sum
}

func BenchmarkBinaryLe64(b *testing.B) {
	s := data(4096, 2)
	var sum uint64
	for i := 0; i < b.N; i++ {
		for j := 0; j+8 <= len(s); j += 8 {
			sum += binary.LittleEndian.Uint64(s[j:])
		}
	}
	_ = sum
}

func BenchmarkPutBe32(b *testing.B) {
	o := make([]byte, 4096)
	for i := 0; i < b.N; i++ {
		for j := int64(0); j+4 <= int64(len(o)); j += 4 {
			PutBe32(o, j, uint64(j))
		}
	}
}

func BenchmarkBinaryPutBe32(b *testing.B) {
	o := make([]byte, 4096)
	for i := 0; i < b.N; i++ {
		for j := 0; j+4 <= len(o); j += 4 {
			binary.BigEndian.PutUint32(o[j:], uint32(j))
		}
	}
}
