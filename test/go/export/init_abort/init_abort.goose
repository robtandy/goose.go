// A library whose globals' initialization aborts (issue #18), compiled with
// --target=go --library. Importing its package runs nothing, and the abort
// ends no process: it is a panic with a *GsError on the goroutine of the
// first call of an entry point, which may recover it, and every later call
// panics with it again. init_abort_test.go checks that.
import std;

// The number a text of digits writes; anything else aborts.
fn parse_limit(text: const u8[:]) -> i64 {
    var n = 0;
    for c in text {
        if c < '0' || c > '9' { abort(str("not a limit: ", text)); }
        let d: i64 = c - '0';
        n = n * 10 + d;
    }
    n
}

let LIMIT = parse_limit("12x");

export fn limit() -> i64 { LIMIT }

export fn twice(x: i64) -> i64 { x * 2 }

// The bytes of text that are not spaces, kept on a data stack.
export fn kept(text: const u8[:]) -> i64 {
    var k: u8[>..] = [];
    for c in text {
        if c != ' ' { k.push(c); }
    }
    k.len
}
