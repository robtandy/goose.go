// Code that control never reaches, where Go's own flow analysis ends: after
// a return, a break or a continue, after a loop no break leaves, and inside
// a construct whose condition, scrutinee or argument ends the flow before
// the rest of it runs. None of it may be written into the Go (vet reports
// such code), every construct around it must still close, the locals it
// leaves unread must still be used, and the data stacks it would take are
// counted as the C backend counts them (the runner compares --stacks).

var trail: i64 = 0;

fn note(n: i64) -> i64 {
    trail = trail * 10 + n;
    n
}

fn sum3(a: i64, b: i64, c: i64) -> i64 { a * 100 + b * 10 + c }

// A while whose condition ends the loop itself, or the function.
fn cond_break(c: bool) -> i64 {
    var n = 0;
    while { if c { break; } n < 3 } { n += 1; }
    n
}

fn cond_return(c: bool) -> i64 {
    var n = 0;
    while { if c { return -1; } n < 3 } { n += 1; }
    n
}

// A constant the optimizer folds leaves the condition one that always
// breaks, and the body unreached.
fn cond_always() -> i64 {
    let always = true;
    var n = 5;
    while { if always { break; } n < 10 } {
        n += 1;
        print("never");
    }
    n
}

// A print whose argument returns, from an inlined body at -O2 and from the
// function itself at -O0: the line is never written.
fn early_print(c: bool) {
    print("line ", { if c { return; } 1 }, " end");
    print("after the line");
}

// Statements after a return, and after branches that all return.
fn after_return() -> i64 {
    return note(1);
    print("not printed");
    note(2)
}

fn all_return(n: i64) -> i64 {
    if n > 0 { return 1; } else { return -1; }
    print("never");
    0
}

fn match_return(n: i64) -> i64 {
    match n {
        0 => { return 10; },
        _ => { return 20; },
    }
    print("never");
    0
}

// After a loop that only a return leaves.
fn spin(n: i64) -> i64 {
    var i = 0;
    loop {
        i += 1;
        if i == n { return i * 2; }
    }
    print("never");
    -1
}

// Values that end the flow before they are complete.
fn value_exits(c: bool) -> i64 {
    let x = { if c { return note(3); } note(4) };
    x * 10
}

fn and_exits(c: bool) -> bool {
    let r = ({ if c { return false; } true }) && note(5) == 5;
    r
}

fn assert_exits(c: bool) -> i64 {
    assert({ if c { return 9; } true });
    8
}

fn break_value_exits(c: bool) -> i64 {
    let v = loop {
        break { if c { return 1; } 2 };
    };
    v * 3
}

fn bound_exits(c: bool) -> i64 {
    var total = 0;
    for i in 0..{ if c { return -5; } 4 } { total += i; }
    total
}

fn args_exit(c: bool) -> i64 {
    sum3(note(6), { if c { return 0; } 2 }, note(7))
}

fn apply<F>(x: i64) -> i64 { F(x) }

fn hof_exits(c: bool) -> i64 {
    apply({ if c { return 11; } 12 }) { it * 2 }
}

// A defer after a return is never registered; one before it runs.
fn dead_defer() -> i64 {
    defer note(8);
    return trail % 10;
    defer note(9);
}

fn dead_defer_in_if(c: bool) -> i64 {
    {
        defer note(1);
        if c { return 5; defer note(2); }
    }
    7
}

// A break or a continue that is never reached names no loop: one whose only
// break is unreached has no label, and only a return leaves it.
fn dead_jumps(n: i64) -> i64 {
    var i = 0;
    var total = 0;
    while i < n {
        i += 1;
        if i % 2 == 0 {
            total += 100;
            continue;
            break;
        }
        total += i;
    }
    total
}

fn only_dead_break(n: i64) -> i64 {
    var i = 0;
    while i < n {
        i += 1;
        return i * 7;
        break;
    }
    0
}

fn dead_break_in_loop(n: i64) -> i64 {
    var i = 0;
    loop {
        i += 1;
        if i >= n { return i; break; }
    }
}

// A scrutinee, and values nested in values, that end the flow.
fn dead_match(c: bool) -> i64 {
    match { if c { return 1; } 2 } {
        1 => 10,
        _ => 20,
    }
}

fn nested(c: bool, d: bool) -> i64 {
    let x = {
        let y = { if c { return 1; } 2 };
        if { if d { return 3; } true } { y * 10 } else { y }
    };
    x + 1
}

fn main() {
    print(cond_break(true), " ", cond_break(false), " ", cond_return(true), " ",
          cond_return(false), " ", cond_always());
    early_print(true);
    early_print(false);
    print(after_return(), " ", all_return(2), " ", all_return(-2), " ", match_return(0), " ",
          match_return(1), " ", spin(4));
    trail = 0;
    print(value_exits(true), " ", value_exits(false), " ", trail);
    trail = 0;
    print(and_exits(true), " ", and_exits(false), " ", trail);
    print(assert_exits(true), " ", assert_exits(false), " ", break_value_exits(true), " ",
          break_value_exits(false), " ", bound_exits(true), " ", bound_exits(false));
    trail = 0;
    print(args_exit(true), " ", args_exit(false), " ", trail);
    print(hof_exits(true), " ", hof_exits(false));
    trail = 0;
    print(dead_defer(), " ", trail);
    trail = 0;
    print(dead_defer_in_if(true), " ", trail);
    trail = 0;
    print(dead_defer_in_if(false), " ", trail);
    print(dead_jumps(5), " ", only_dead_break(3), " ", only_dead_break(0), " ",
          dead_break_in_loop(3));
    print(dead_match(true), " ", dead_match(false), " ", nested(true, true), " ",
          nested(false, true), " ", nested(false, false));
}
