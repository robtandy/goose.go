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
	// The forms of an entry point taking a const u8[:] as a string and as
	// a []byte, not generic.
	if got := WordsString("one two"); got != 2 {
		t.Errorf("WordsString = %d", got)
	}
	if got := WordsBytes([]byte("one two three")); got != 3 {
		t.Errorf("WordsBytes = %d", got)
	}
	if got := KeptString("a b"); got != 2 {
		t.Errorf("KeptString = %d", got)
	}
	if got := KeptBytes([]byte("abc")); got != 3 {
		t.Errorf("KeptBytes = %d", got)
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
