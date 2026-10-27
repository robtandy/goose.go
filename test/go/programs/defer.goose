// defer (§6.6): a body runs when its block ends, however it ends --
// falling off the end, break, continue, return, an inlined call's return --
// latest first, seeing variables as they are at the exit, after a return's
// value is taken.

var depth = 0;
var log_count = 0;

fn note(what: i64) {
    log_count++;
    print("  note ", what, " depth ", depth);
}

fn nested(n: i64) -> i64 {
    depth++;
    defer depth--;
    defer note(n);
    if n > 2 { return n * 10; }
    {
        defer print("  inner block of ", n);
        print("  in block ", n);
    }
    n
}

fn loops() -> i64 {
    var count = 0;
    for i in 0..6 {
        defer count++;
        if i == 1 { continue; }
        if i == 4 { break; }
        defer print("  iteration ", i, " count ", count);
    }
    var j = 0;
    while j < 3 {
        defer j++;
        let k = j * 2;
        defer print("  while ", k);
    }
    var x = 0;
    let found = loop {
        defer x++;
        if x == 3 { break x * 100; }
    };
    print("  found ", found, " x ", x);
    return count;
}

fn returns_value() -> i64 {
    var v = 1;
    defer v = 100;               // Runs after the value is taken.
    return v + 1;
}

fn early(flag: bool) -> i64 {
    defer print("  early done ", flag);
    let r = block {
        defer print("  leaving block");
        if flag { break 1; }
        2
    };
    r * 10
}

fn main() {
    print("nested 1 -> ", nested(1));
    print("nested 5 -> ", nested(5));
    print("depth ", depth, " notes ", log_count);
    print("loops -> ", loops());
    print("returns_value -> ", returns_value());
    print("early true -> ", early(true));
    print("early false -> ", early(false));
    {
        defer print("block done");
        print("in the block");
    }
}
