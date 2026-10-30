package records

import (
	"encoding/binary"
	"math/rand"
	"testing"
)

// BenchmarkCount runs the build's countRecords -- the Go original, or with
// -tags goose the kernel -- and BenchmarkCountWords the plain-Go control:
// the kernel's algorithm, 8 bytes at a time, written in Go. Compare the two
// builds with
//
//	go test -run '^$' -bench . -count 10 > go.txt
//	go test -tags goose -run '^$' -bench . -count 10 > goose.txt
//	benchstat go.txt goose.txt

// countRecordsWords is countRecordsGo checksumming 8 bytes at a time, as the
// kernel does: what the algorithm gains, without Goose.
func countRecordsWords(seg []byte) int64 {
	var n int64
	for off := 0; off < len(seg); {
		size := int(binary.LittleEndian.Uint32(seg[off:]))
		p := seg[off+4 : off+4+size]
		var x uint64
		i := 0
		for ; i+8 <= len(p); i += 8 {
			x ^= binary.LittleEndian.Uint64(p[i : i+8])
		}
		for ; i < len(p); i++ {
			x ^= uint64(p[i])
		}
		x ^= x >> 32
		x ^= x >> 16
		x ^= x >> 8
		if byte(x) != seg[off+4+size] {
			return -1
		}
		off += 5 + size
		n++
	}
	return n
}

// segments is count distinct segments of about 8 KB, of records whose
// payloads are lo to hi bytes long, and their mean length.
func segments(count, lo, hi int) ([][]byte, int64) {
	r := rand.New(rand.NewSource(int64(hi)))
	segs := make([][]byte, count)
	total := 0
	for i := range segs {
		for len(segs[i]) < 8<<10 {
			p := make([]byte, lo+r.Intn(hi-lo+1))
			r.Read(p)
			segs[i] = appendRecord(segs[i], p)
		}
		total += len(segs[i])
	}
	return segs, int64(total / count)
}

// bench measures count over inputs distinct segments of each shape, a
// different one each time round.
func bench(b *testing.B, count func([]byte) int64, inputs int) {
	for _, s := range []struct {
		name   string
		lo, hi int
	}{
		{"payload=0-7", 0, 7},         // every byte in a tail, at its payload's end
		{"payload=8-64", 8, 64},       // words, and tails
		{"payload=1K-4K", 1024, 4096}, // nearly every byte in a word, inside its payload
	} {
		segs, mean := segments(inputs, s.lo, s.hi)
		b.Run(s.name, func(b *testing.B) {
			b.SetBytes(mean)
			b.ReportAllocs()
			for i := 0; i < b.N; i++ {
				if count(segs[i%len(segs)]) < 0 {
					b.Fatal("a bad checksum")
				}
			}
		})
	}
}

func BenchmarkCount(b *testing.B)      { bench(b, countRecords, 64) }
func BenchmarkCountWords(b *testing.B) { bench(b, countRecordsWords, 64) }

// BenchmarkCountOneInput is BenchmarkCount on one segment, over and over,
// which the branch predictor learns: how not to measure.
func BenchmarkCountOneInput(b *testing.B) { bench(b, countRecords, 1) }
