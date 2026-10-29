// A library whose start is lazy (issue #18), compiled with --target=go
// --library: importing its package maps no memory, sets no goroutine's
// fault handling and runs none of its globals' initializers. The first call
// of an entry point starts it, once: the runtime, the package's block and
// the globals. Every call leaves its caller's fault setting as it found it,
// and nothing ends the process: an abort, and exit(), are panics with a
// *GsError on the caller's goroutine. lazy_test.go checks all of that, from
// the package's TestMain on.
import std;

// How many times the globals' initializers ran: once, at the first call.
var starts: i64 = 0;
fn start() -> i64 {
    starts += 1;
    42
}
let ANSWER = start();
// A global on a data stack of its own, which the package's block holds.
var log: u8[>..] = "start:";

export fn answer() -> i64 { ANSWER }
export fn started() -> i64 { starts }
export fn log_len() -> i64 { log.len }

// The bytes of text that are not spaces, kept on a data stack.
export fn kept(text: const u8[:]) -> i64 {
    var k: u8[>..] = [];
    for c in text {
        if c != ' ' { k.push(c); }
    }
    k.len
}

export fn checked_div(a: i64, b: i64) -> i64 {
    if b == 0 { abort(str("checked_div: ", a, " by zero")); }
    a / b
}

export fn quit(code: i64) { exit(code); }

// A recursion whose every activation holds a value of 6008 bytes in its
// frame on the fixed-frame stack (§7.8), which a library reserves less of
// than a program does (--frame-reserve).
struct Big { tag: i64, data: u8[6000] }

recursive fn deep(n: i64) -> i64 {
    var b = Big { tag: n, data: [0; 6000] };
    for i in 6000 { b.data[i] = ((i + n) % 251) as u8; }
    if n == 0 { return b.data[5999]; }
    let below = deep(n - 1);
    var s = 0;
    for i in 6000 { s += b.data[i]; }
    return below + s % 1000 + b.tag;
}

export fn depth(n: i64) -> i64 { deep(n) }
