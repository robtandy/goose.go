package goose

import (
	"fmt"
	"math/rand"
	"strconv"
	"testing"
)

// inline_refs.goose's export fns (issue #24), each against the same written
// in Go, whose locals Go keeps in registers.

// scanGo is inline_refs.goose's scan and step.
func scanGo(text []byte) uint64 {
	var tag, uni, sum uint64
	var cur, ctn, count int64
	inObj := false
	for cur < int64(len(text)) {
		c := text[cur]
		cur++
		var done uint64
		switch {
		case tag == 2:
			if c == '"' {
				tag = 0
				count++
				done = uni
			} else {
				uni = uni*31 + uint64(c)
			}
		case c >= '0' && c <= '9':
			if tag != 1 {
				tag = 1
				uni = 0
			}
			uni = uni*10 + uint64(c-'0')
		default:
			if tag == 1 {
				tag = 0
				count++
				done = uni
			}
			switch c {
			case '"':
				tag = 2
				uni = 7
			case '{', '[':
				ctn++
				inObj = c == '{'
			case '}', ']':
				ctn--
				count++
				done += uint64(ctn)*3 + uint64(c)
			}
		}
		sum = sum*31 + done
	}
	r := sum ^ uint64(count)<<32 ^ uint64(ctn)<<16 ^ tag
	if inObj {
		r ^= 1
	}
	return r
}

// placesGo is places, its helpers written out.
func placesGo(n int64) int64 {
	var c, k int64
	px, py := int64(1), int64(2)
	var a [8]uint8
	e := [4]int64{1, 2, 3, 4}
	d := int64(1)
	for i := int64(0); i < n; i++ {
		c++
		c = c*2 + 1
		px += 3
		py++
		j := i & 7
		a[j]++
		a[0] = a[j] + 1
		e[i&3]++
		px++
		d++
		d += 10
		d += d
		d &= 1023
		k &= 3
		at := k
		k++
		e[at] += k
	}
	return c + px*10 + py*100 + int64(a[0]+a[3])*1000 + e[1]*10000 + e[2]*100000 + d*1000000
}

func keptGo(n int64) int64 {
	var a, b int64
	for i := int64(0); i < n; i++ {
		b++
		a++
		b += a
	}
	return a*1000000 + b
}

func largestGo(n int64) int64 {
	a, b, c := int64(0), int64(1), int64(0)
	for i := int64(0); i < n; i++ {
		if a > b {
			a += i
		} else {
			b += i
		}
		c++
	}
	return a*1000000 + b*10 + c
}

// doc is JSON-like text of n bytes at least: values of nested objects and
// arrays, numbers and strings, separated by commas.
func doc(n int, seed int64) []byte {
	r := rand.New(rand.NewSource(seed))
	var b []byte
	var value func(depth int)
	str := func() {
		b = append(b, '"')
		for j := r.Intn(10); j > 0; j-- {
			b = append(b, byte('a'+r.Intn(26)))
		}
		b = append(b, '"')
	}
	value = func(depth int) {
		switch k := r.Intn(6); {
		case k < 2 && depth < 6:
			open, close := byte('['), byte(']')
			if k == 0 {
				open, close = '{', '}'
			}
			b = append(b, open)
			for i, m := 0, r.Intn(6); i < m; i++ {
				if i > 0 {
					b = append(b, ',')
				}
				if open == '{' {
					str()
					b = append(b, ':')
				}
				value(depth + 1)
			}
			b = append(b, close)
		case k < 4:
			b = strconv.AppendInt(b, r.Int63n(1000000), 10)
		default:
			str()
		}
	}
	for len(b) < n {
		if len(b) > 0 {
			b = append(b, ',', ' ')
		}
		value(0)
	}
	return b
}

func TestScan(t *testing.T) {
	texts := [][]byte{nil, []byte("1"), []byte(`"unterminated`), []byte("]]]}"), []byte(`{"a":[1,2,{"b":"c"}],"d":42}`)}
	for i := 0; i < 64; i++ {
		texts = append(texts, doc(i*7, int64(i)))
	}
	r := rand.New(rand.NewSource(7))
	for i := 0; i < 64; i++ {
		b := make([]byte, r.Intn(200))
		r.Read(b)
		texts = append(texts, b)
	}
	for _, s := range texts {
		if got, want := Scan(s), scanGo(s); got != want {
			t.Errorf("Scan(%q) = %#x, want %#x", s, got, want)
		}
		if got, want := Scan(string(s)), scanGo(s); got != want {
			t.Errorf("Scan(string %q) = %#x, want %#x", s, got, want)
		}
	}
}

func TestPlaces(t *testing.T) {
	for _, n := range []int64{-1, 0, 1, 2, 3, 7, 8, 9, 100, 1000, 12345} {
		if got, want := Bumps(n), max(n, 0); got != want {
			t.Errorf("Bumps(%d) = %d, want %d", n, got, want)
		}
		if got, want := Places(n), placesGo(n); got != want {
			t.Errorf("Places(%d) = %d, want %d", n, got, want)
		}
		if got, want := Kept(n), keptGo(n); got != want {
			t.Errorf("Kept(%d) = %d, want %d", n, got, want)
		}
		if got, want := Largest(n), largestGo(n); got != want {
			t.Errorf("Largest(%d) = %d, want %d", n, got, want)
		}
	}
}

// A call allocates nothing, a frame or none.
func TestInlineRefsZeroAllocs(t *testing.T) {
	s := doc(256, 1)
	var sink uint64
	for _, f := range []func(){
		func() { sink += Scan(s) },
		func() { sink += uint64(Bumps(9) + Places(9)) },
		func() { sink += uint64(Kept(9) + Largest(9)) },
	} {
		if n := testing.AllocsPerRun(1000, f); n != 0 {
			t.Errorf("%v allocs/op, want 0", n)
		}
	}
	_ = fmt.Sprint(sink)
}

// go test -bench . compares the scan with the same loop written in Go.
func BenchmarkScan(b *testing.B) {
	s := doc(4096, 3)
	b.SetBytes(int64(len(s)))
	var sum uint64
	for i := 0; i < b.N; i++ {
		sum += Scan(s)
	}
	_ = sum
}

func BenchmarkScanGo(b *testing.B) {
	s := doc(4096, 3)
	b.SetBytes(int64(len(s)))
	var sum uint64
	for i := 0; i < b.N; i++ {
		sum += scanGo(s)
	}
	_ = sum
}

func BenchmarkBumps(b *testing.B) {
	var sum int64
	for i := 0; i < b.N; i++ {
		sum += Bumps(1000)
	}
	_ = sum
}
