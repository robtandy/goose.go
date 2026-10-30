//go:build goose_import

package goose

import gsrt "github.com/robtandy/goose.go/runtime/go"

// What the tests of test/go/export name of the runtime, for a library
// compiled with --go-runtime=import, whose runtime is the package
// github.com/robtandy/goose.go/runtime/go: test/run_go_tests.py adds this
// file to the module of each library it compiles so, and tests it with
// -tags goose_import. The blocks are those of a program of the library's
// sizes (its configuration, which every file the compiler writes has), as
// the file holding the runtime would make them.

type (
	GsError      = gsrt.GsError
	GsSlice      = gsrt.GsSlice
	GsStackBlock = gsrt.GsStackBlock
)

var shimProg = gsrt.GsProgram{StackReserve: gsStackReserve, StackGap: gsStackGap, FrameReserve: gsFrameReserve}

func NewStackBlock(numStacks int, reserve, gap int64) *GsStackBlock {
	return shimProg.NewStackBlock(numStacks, reserve, gap)
}

func AcquireStackBlock(numStacks int) *GsStackBlock { return shimProg.AcquireStackBlock(numStacks) }

func ReleaseStackBlock(gs *GsStackBlock) { shimProg.ReleaseStackBlock(gs) }

func mapCount() int64 { return gsrt.GsMapCount() }

func blockCount() int { return gsrt.GsBlockCount() }
