// References to slices (a value whose Go type holds a pointer) printed and
// read through, as values and as returned references.
var words: (const u8[:])[>..] = [];
fn pick(i: i64) -> (const u8[:])& { return words[i]; }
fn show(r: (const u8[:])&) { print(r, " ", r.len); }
fn main() {
    words.push("alpha");
    words.push("beta");
    let r = pick(1);
    print(r);
    show(words[0]);
    let copied: u8[] = pick(0);
    print(copied, " ", copied.len);
    var held: (const u8[:])& = words[0];
    print(held[1], " ", held);
    held .= words[1];
    let again: u8[] = held;
    print(again);
}
