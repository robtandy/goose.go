//go:build !goose_nowithstack

package unexported

import "testing"

// A NameWithStack entry point runs on a *StackBlock, the one type of the
// runtime's the package exports, which a caller keeps across calls
// (test/run_go_tests.py tests a --go-no-withstack build of the library
// with -tags goose_nowithstack).
func TestWithStack(t *testing.T) {
	gs := AcquireStackBlock(0)
	defer ReleaseStackBlock(gs)
	var b *StackBlock = gs
	for i := 0; i < 3; i++ {
		if got := KeptWithStack(b, "x y"); got != 2 {
			t.Fatalf("KeptWithStack = %d", got)
		}
		if got := WordsWithStack(b, "a b c d"); got != 4 {
			t.Fatalf("WordsWithStack = %d", got)
		}
	}
	if n := testing.AllocsPerRun(100, func() { KeptWithStack(b, "p q") }); n != 0 {
		t.Errorf("KeptWithStack: %v allocs/op", n)
	}
}
