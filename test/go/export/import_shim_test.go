//go:build goose_import

package goose

import gsrt "github.com/robtandy/goose.go/runtime/go"

// What the tests of test/go/export name of the runtime, for a library
// compiled with --go-runtime=import, whose runtime is the package
// github.com/robtandy/goose.go/runtime/go: test/run_go_tests.py adds this
// file to the module of each library it compiles so, and tests it with
// -tags goose_import. The blocks are the library's program's (gsProg), of
// the sizes it was compiled with, as the file holding its runtime makes
// them.

type (
	GsError      = gsrt.GsError
	GsStackBlock = gsrt.GsStackBlock
)

func NewStackBlock(numStacks int, reserve, gap int64) *GsStackBlock {
	return gsProg.NewStackBlock(numStacks, reserve, gap)
}

func AcquireStackBlock(numStacks int) *GsStackBlock { return gsProg.AcquireStackBlock(numStacks) }

func ReleaseStackBlock(gs *GsStackBlock) { gsProg.ReleaseStackBlock(gs) }

func mapCount() int64 { return gsrt.GsMapCount() }

func blockCount() int { return gsrt.GsBlockCount() }
