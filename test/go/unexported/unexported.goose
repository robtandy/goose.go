// A library compiled with --go-unexported (issue #19), whose package exports
// its export fns' entry points and nothing else of its own: none of the
// runtime's names (gsSlice, gsStackBlock, ...), none of the program's (this
// Tally and LIMIT would be Go's exported Tally_g and LIMIT_g), but for the
// block type a NameWithStack entry point takes and the two calls of the
// runtime's pool that give and take one (StackBlock, AcquireStackBlock and
// ReleaseStackBlock); with --go-no-withstack too, the Name entry points
// alone. test/run_go_tests.py checks the package's API with go doc, and
// unexported_test.go calls it.
import std;

// What a text holds, of a Go type Go would export: no entry point takes
// one or gives one.
struct Tally { words: i64, bytes: i64 }

fn tally(text: const u8[:]) -> Tally {
    var t = Tally { words: 0, bytes: text.len };
    var inword = false;
    for c in text {
        let space = c == ' ';
        if !space && !inword { t.words += 1; }
        inword = !space;
    }
    t
}

// The most a checked value may be, a global Go would export too.
var LIMIT = 1000;
var calls = 0;

// The number of words in text.
export fn words(text: const u8[:]) -> i64 {
    calls += 1;
    tally(text).words
}

// The bytes of text that are not spaces, kept on a data stack.
export fn kept(text: const u8[:]) -> i64 {
    var k: u8[>..] = [];
    for c in text {
        if c != ' ' { k.push(c); }
    }
    k.len
}

// How many times words ran.
export fn calls_so_far() -> i64 { calls }

// n, where it is at most LIMIT: past it, an abort.
export fn checked(n: i64) -> i64 {
    if n > LIMIT { abort(str("over the limit: ", n)); }
    n
}
