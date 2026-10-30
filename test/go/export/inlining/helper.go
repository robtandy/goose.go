//go:build ignore

// The Go that inlining.goose's extern fn calls, spliced into the file
// `goose --target=go --include helper.go` writes. The build constraint keeps
// the go tool from building it on its own here.
package goose

// peek is extern fn peek: the first element of xs, 0 for none. It could keep
// xs anywhere, as Go code may: an export fn handing it a slice keeps its
// argument off its caller's stack.
func peek(xs GsSlice) int64 {
	if xs.Len == 0 {
		return 0
	}
	return GsSliceToSlice[int64](xs)[0]
}

// first_byte is extern fn first_byte: the first byte of text, -1 for none.
func first_byte(text GsSlice) int64 {
	if text.Len == 0 {
		return -1
	}
	return int64(GsSliceToBytes(text)[0])
}
