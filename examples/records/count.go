package records

import "encoding/binary"

// countRecordsGo is the Go original: the number of records in seg, or -1
// where one's checksum is wrong. A record that seg ends inside panics with a
// runtime.Error, an index or a slice out of range. The default build runs
// it; with -tags goose it stays, the reference the kernel is tested against.
func countRecordsGo(seg []byte) int64 {
	var n int64
	for off := 0; off < len(seg); {
		size := int(binary.LittleEndian.Uint32(seg[off:]))
		var x byte
		for _, b := range seg[off+4 : off+4+size] {
			x ^= b
		}
		if x != seg[off+4+size] {
			return -1
		}
		off += 5 + size
		n++
	}
	return n
}
