//go:build ignore

// The Go that slice_ends.goose's extern fn calls, spliced into the file
// `goose --target=go --include helper.go` writes. The build constraint keeps
// the go tool from building it on its own here.
package goose

// notedSlice is where a slice note was handed starts, and its length: an
// address, which keeps nothing alive past the call.
type notedSlice struct {
	data uintptr
	n    int64
}

// noted is what note was handed, in order.
var noted []notedSlice

// note is extern fn note.
func note(s GsSlice) { noted = append(noted, notedSlice{uintptr(s.Data), s.Len}) }
