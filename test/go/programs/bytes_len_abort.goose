// A variable array landing in a slot whose length prefix cannot hold its
// length aborts (§3.3): 300 elements into a u8[u8].
fn letters(n: i64) -> u8[>..] {
    var out: u8[>..] = [];
    for i in n { out.push(('a' + i % 26) as u8); }
    return out;
}

fn main() {
    let ok: u8[u8] = letters(255);
    print(ok.len);
    let bad: u8[u8] = letters(300);
    print(bad.len);
}
