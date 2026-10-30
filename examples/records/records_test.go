package records

import (
	"encoding/binary"
	"errors"
	"testing"
)

// Count, in either build: go test runs the Go original, and go test -tags
// goose the kernel, so that a truncated record's panic is recovered both
// ways -- a runtime.Error, and a Goose abort.

// appendRecord appends the record of payload to seg.
func appendRecord(seg, payload []byte) []byte {
	seg = binary.LittleEndian.AppendUint32(seg, uint32(len(payload)))
	seg = append(seg, payload...)
	var x byte
	for _, b := range payload {
		x ^= b
	}
	return append(seg, x)
}

// segment is the segment of the payloads' records.
func segment(payloads ...string) []byte {
	var seg []byte
	for _, p := range payloads {
		seg = appendRecord(seg, []byte(p))
	}
	return seg
}

func TestCount(t *testing.T) {
	bad := segment("goose", "gander")
	bad[len(bad)-1] ^= 1
	for _, c := range []struct {
		name string
		seg  []byte
		n    int
		err  error
	}{
		{"empty", nil, 0, nil},
		{"one", segment("goose"), 1, nil},
		{"three", segment("goose", "", "a gaggle of geese"), 3, nil},
		{"a bad checksum", bad, 0, ErrChecksum},
		{"cut in a length", segment("goose")[:2], 0, ErrTruncated},
		{"cut in a payload", segment("goose")[:7], 0, ErrTruncated},
		{"no checksum", segment("goose", "gander")[:20], 0, ErrTruncated},
	} {
		n, err := Count(c.seg)
		if n != c.n || !errors.Is(err, c.err) {
			t.Errorf("%s: Count = %d, %v; want %d, %v", c.name, n, err, c.n, c.err)
		}
	}
}
