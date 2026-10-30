//go:build goose

package records

//go:generate goose --target=go --go-build goose --go-package records --go-unexported --go-no-withstack -o records_goose.go _goose/records.goose

// The build with -tags goose: Count runs the Goose kernel of
// _goose/records.goose, which go generate -tags goose compiles to
// records_goose.go.

func countRecords(seg []byte) int64 { return CountRecordsBytes(seg) }

// boundsFailure says whether r, recovered from countRecords, is a failed
// bounds check: in Goose, a *gsError, which is no runtime.Error.
func boundsFailure(r any) bool {
	_, ok := r.(*gsError)
	return ok
}
