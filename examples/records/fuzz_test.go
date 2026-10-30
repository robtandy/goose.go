//go:build goose

package records

import "testing"

// FuzzCountRecords holds the kernel to the original on any bytes at all.
// go test -tags goose runs its seeds; go test -tags goose -run '^$' -fuzz
// '^FuzzCountRecords$' looks for more.
func FuzzCountRecords(f *testing.F) {
	f.Add([]byte{})
	f.Add(segment("goose", "", "a gaggle of geese"))
	f.Add(segment(payload(8), payload(9)))
	f.Add(segment("goose")[:7])
	f.Fuzz(func(t *testing.T, seg []byte) { same(t, seg) })
}
