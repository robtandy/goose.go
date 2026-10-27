// A format argument runs before its text is placed (§2, §3.7): what it
// appends to the builder comes first, in format's call and method forms,
// in str, and into a limited array, whose capacity check sees it.
fn add(b: u8[>..]&) -> i64 { b.append("x"); return 42; }
fn appendA(out: u8[>..]&) -> i64 { out.append("A"); return 2; }
fn main() {
    var b: u8[>..] = [];
    format(b, add(b));
    print(b, " ", b.len);
    var c: u8[>..] = [];
    c.append("<");
    format(c, add(c), "-", add(c));
    print(c, " ", c.len);
    let s = str(add(b), add(b));
    print(s, " ", b, " ", b.len);
    var out: u8[>..] = [];
    out.format(appendA(out));
    print(out, " ", out.len);
    var lim: u8[..8] = [];
    fn addl() -> i64 { lim.push('L'); return 5; }
    format(lim, addl(), addl());
    print(lim, " ", lim.len);
}
