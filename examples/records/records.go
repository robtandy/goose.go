// Package records reads segments of checksummed records. A record is a
// little-endian uint32 length n, n bytes of payload, and the XOR of those
// bytes.
//
// It is the worked example of docs/go-integration.md: Count runs the Go
// original (count.go) by default, and a Goose kernel
// (_goose/records.goose) when built with -tags goose.
package records

import "errors"

var (
	// ErrChecksum is Count's error for a record whose checksum is wrong.
	ErrChecksum = errors.New("records: bad checksum")
	// ErrTruncated is Count's error for a segment that ends inside a record.
	ErrTruncated = errors.New("records: truncated record")
)

// Count returns the number of records in seg.
func Count(seg []byte) (n int, err error) {
	// countRecords leaves finding a truncated record to its bounds checks,
	// whose panic boundsFailure knows.
	defer func() {
		if r := recover(); r != nil {
			if !boundsFailure(r) {
				panic(r)
			}
			n, err = 0, ErrTruncated
		}
	}()
	c := countRecords(seg)
	if c < 0 {
		return 0, ErrChecksum
	}
	return int(c), nil
}
