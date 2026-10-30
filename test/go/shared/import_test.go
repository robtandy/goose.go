//go:build goose_import

package shared

import gsrt "github.com/robtandy/goose.go/runtime/go"

// With --go-runtime=import the runtime is the package
// github.com/robtandy/goose.go/runtime/go, whose blocks a host takes from
// it: test/run_go_tests.py tests this package so with -tags goose_import.

func AcquireStackBlock(numStacks int) *gsrt.GsStackBlock { return gsrt.AcquireStackBlock(numStacks) }

func ReleaseStackBlock(gs *gsrt.GsStackBlock) { gsrt.ReleaseStackBlock(gs) }
