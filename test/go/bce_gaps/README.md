# Bounds-check elimination gaps (issue #26)

The kernels of issue #26 kept bounds checks that their indices made
unnecessary. Bounds-check elimination is the front end's
(`vendor/goose/src/bce.h`), which both backends share, so these are gaps in
upstream Goose, to be reported there. Each file here is a minimal reproducer:
its `// bce:keep` lines keep a check BCE could remove, and its `// bce:elide`
lines are the same shape with the gap stepped around, whose check is removed.
`goose -O1 --check --bce-test file.goose` verifies both, as upstream's
`test/run_tests.py` does for its `test/optimizer/bce_*.goose` fixtures, and
`python3 test/run_go_tests.py bce_gaps` runs it at -O1 and -O2, the levels
that inline (Runner.bce_gaps): once a gap is closed, its `bce:keep` line
fails, to become `bce:elide`, and this page is out of date. Each file is a
program too, with a `main` that runs it. The code quoted from `bce.h` is the
code at the commit `vendor/goose` pins.

## 1. A fact an inlined body states does not outlive the body

### 1a. Where the body returns early (`inlined_abort_guard.goose`)

```goose
fn checked(xs: const i64[:], di: i64) -> i64 {
    if di < 0 { abort("negative index"); }
    if di >= xs.len { return 0; }
    xs[di] // bce:elide
}

fn after(xs: const i64[:], di: i64) -> i64 {
    let v = checked(xs, di);
    // di >= 0 here, and di < xs.len is tested: elidable.
    if di < xs.len { return v + xs[di]; } // bce:keep
    v
}
```

The abort ends every path on which `di < 0`, so `di >= 0` holds wherever
`checked` returns, at its early return and at its end alike. BCE uses the
fact inside the body (`xs[di]` there is elided), but not after it in
`after`, where `checked` is inlined: `InlineBlock::BceWalk` walks a body
that has a return to it before its end ("early") and then puts the flow back
to the one at the body's entry, less what the body writes
(`b.flow = std::move(entry); b.StripKills(body);`), which drops every fact the
body stated. `EarlyBlock::BceWalk` does the same for a `block { }` that a
`break` leaves. A body with no early return keeps its facts: the flow after
it is the flow at its end (`checked_once` and `after_once`, elided).

Possible fix: record the flow at each `return` to the inlined body (and at
each `break` out of a `block`), as the walk reaches it, and meet those with
the flow at the body's end (`BCE::Meet`, as `IfExpr::BceWalk` meets its two
branches), rather than starting again from the entry's.

### 1b. Where the body cannot complete (`inlined_abort_call.goose`)

```goose
fn fail(msg: const u8[:]) { abort(msg); }

fn at(xs: const i64[:], di: i64) -> i64 {
    if di < 0 || di >= xs.len { fail("index out of range"); }
    xs[di] // bce:keep
}
```

`fail` only aborts, so past the guard the index is in range, as it is past
the same guard with `abort(...)` in its place (`at_abort`, elided). Once
`fail` is inlined, though, its body ends in an abort, which ends the path
(`Call::BceWalk` returns false for `B_ABORT`), but `InlineBlock::BceWalk`
reports that the inlined body falls through whatever the walk of the body
found (`b.Walk(body); return true;`), so the guard's then-branch joins the
code after it with the facts of a bad index, and the meet keeps nothing.

Possible fix: where the body has no early return, return what its walk
returned (`return b.Walk(body);`); with early returns, the body completes
where any of them is reached or its end is. (Where `fail` is not inlined,
-O0, no call is taken to end a path at all, which an effect summary of
"never returns" per function would give.)

## 2. A loop bounded by its trailing break keeps every check of that bound (`trailing_break.goose`)

```goose
fn sum_until(xs: const i64[:], t: i64) -> i64 {
    if xs.len == 0 { return 0; }
    var i = 0;
    var s = 0;
    while s <= t {
        s += xs[i]; // bce:keep
        i += 1;
        if i >= xs.len { break; }
    }
    s
}
```

`i < xs.len` holds at the top of every iteration: the guard before the loop
makes it so for the first (`i` is 0 and `xs.len` is not), and the break
leaves before any other where it would not hold. But BCE carries no fact
around a loop: `While::BceWalk` strips every fact on what the body writes
at the loop's entry (`b.StripKills(body)`), and at the top of the body only
the condition's facts hold, which say nothing of `i` here. `LoopExpr` (`loop
{ }`) is walked the same way, with no condition at all. So every check whose
bound is the trailing break stays, which is every check of a `while true {
...; if c { break; } }` loop. `copy_some` is a kernel's shape with two
indices: the condition `si < xs.len` bounds the source's, whose check is
elided, and the trailing break `di >= o.len` the destination's, whose is
not; with both bounds in the condition (`copy_both`) both are elided, which
is the way around the gap for now.

Possible fix: a loop invariant, found as a fixpoint: take the meet of the
flow at the loop's entry and the flow at the end of the body (and at each
`continue`), walk the body once more from it, and keep the facts that
survive the walk, so that a fact the body re-establishes on every path back
to the top holds there.
