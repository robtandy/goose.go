package unexported

import (
	"strings"
	"testing"
)

// A library compiled with --go-unexported (issue #19): its entry points, and
// its aborts, which are panics with an error.

func TestEntryPoints(t *testing.T) {
	if got := Words("the quick brown fox"); got != 4 {
		t.Errorf("Words = %d", got)
	}
	if got := Kept([]byte("a b c")); got != 3 {
		t.Errorf("Kept = %d", got)
	}
	if got := CallsSoFar(); got < 1 {
		t.Errorf("CallsSoFar = %d", got)
	}
	if got := Checked(700); got != 700 {
		t.Errorf("Checked = %d", got)
	}
}

func TestAbort(t *testing.T) {
	defer func() {
		e, ok := recover().(error)
		if !ok || !strings.Contains(e.Error(), "over the limit: 1001 (test/go/unexported/unexported.goose:") {
			t.Errorf("recovered %v", e)
		}
	}()
	Checked(1001)
	t.Error("Checked(1001) returned")
}
