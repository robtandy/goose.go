//go:build !goose

package records

import "runtime"

// The default build: Count runs the Go original.

func countRecords(seg []byte) int64 { return countRecordsGo(seg) }

// boundsFailure says whether r, recovered from countRecords, is a failed
// bounds check: in Go, a runtime.Error.
func boundsFailure(r any) bool {
	_, ok := r.(runtime.Error)
	return ok
}
