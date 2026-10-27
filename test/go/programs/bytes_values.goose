// Variable-size values (bytes class, §3.6): structs whose fields follow
// variable-size ones, arrays of them walked by size, their equality, values
// passed and returned, variable arrays of every length storage, a varint
// prefix that takes a second byte, enums with variable payloads, results
// landing in slots of another layout, and to_bytes / bytes_of / from_bytes
// round trips with the verifier refusing broken images.

struct Rec { name: u8[], age: i64, tags: u8[][], score: varint }
struct Tree { v: i32, l: Tree&<u32>?, r: Tree&<u32>? }
enum Msg { Quit, Say { who: u8[], n: i64 }, Pair { a: u8[], b: u8[] } }

fn mkrec(name: u8[], age: i64) -> Rec {
    return Rec { name: name, age: age, tags: [name, "x"], score: age * 1000 };
}

fn describe(r: Rec&) -> i64 {
    var t = 0;
    for tag in r.tags { t += tag.len; }
    print(r.name, " ", r.age, " ", r.tags.len, " ", t, " ", r.score);
    return r.age + t;
}

fn word(k: i64) -> u8[] {
    if k == 0 { return "zero"; }
    if k == 1 { return "one"; }
    return str(k * 11);
}

fn letters(n: i64) -> u8[>..] {
    var out: u8[>..] = [];
    for i in n { out.push(('a' + i % 26) as u8); }
    return out;
}

fn keep(r: Rec) -> i64 { return r.age * 2 + r.tags.len; }

fn say(m: Msg..&) {
    match m {
        Quit => print("quit"),
        Say &s => print("say ", s.who, " ", s.n),
        Pair &p => print("pair ", p.a, "+", p.b),
    }
}

fn tsum(a: Tree[>..]&) -> i64 {
    var acc = 0;
    for &n in a {
        acc = acc * 7 + n.v;
        let l = n.l;
        if l { acc += l.v; }
        let r = n.r;
        if r { acc += r.v; }
    }
    return acc;
}

fn main() {
    // Fields after variable-size ones, read through the value's size walk.
    let r = mkrec("goose", 7);
    print(describe(r));                                     // goose 7 2 6 7000, 13
    print(r.age, " ", r.score, " ", r.tags);                // 7 7000 ["goose", "x"]
    print(keep(copy(r)));                                   // 16

    // An array of them, walked element by element.
    var recs: Rec[>..] = [];
    recs.push(mkrec("ann", 31));
    recs.push(Rec { name: "bob", age: 42, tags: [], score: -5 });
    recs.push(mkrec(word(5), 9));
    var total = 0;
    for x in recs { total += describe(x); }
    print(total);                                           // 89
    var nth = 0;
    for x in recs {
        if nth == 1 { print(x.name, " ", x.tags, " ", recs.len); }   // bob [] 3
        nth++;
    }

    // Equality compares the values, not where they lie.
    let a = mkrec("ann", 31);
    var eqs: u8[>..] = [];
    for x in recs { format(eqs, x == a, " "); }
    print(eqs);                                             // true false false

    // Variable arrays of each length storage, and a varint prefix of two
    // bytes.
    let v8: u8[u8] = "short";
    let v16: u8[u16] = letters(300);
    let vv: u8[varint] = letters(200);
    let vs: u8[varint][] = [word(0), word(1), letters(130), word(2)];
    print(v8.len, " ", v16.len, " ", vv.len, " ", vv[199]);   // 5 300 200 114
    for w in vs { print(w.len, " ", w[w.len - 1]); }

    // Results built where the caller wants them: appended as runs, and in
    // slots whose prefix differs from the result's.
    var out: u8[>..] = [];
    for k in 4 { out.append(word(k)); out.push(','); }
    print(out);                                             // zero,one,22,33,
    let small: u8[u8][] = [word(1), word(3), letters(3)];
    for w in small { print(w); }
    let filled: u8[][] = [word(4); 3];
    print(filled);

    // Enums with variable-size payloads.
    let ms: Msg..[] = [Msg.Say { who: "ann", n: 2 }, Msg.Quit, Msg.Pair { a: word(1), b: letters(4) }];
    for m in ms { say(m); }

    // Serialization: relative links survive the round trip, and the
    // verifier refuses images that are wrong.
    var pool: Tree[>..] = [];
    let ta: Tree& = pool.push(Tree { v: 11 });
    let tb: Tree& = pool.push(Tree { v: 22 });
    let tc: Tree& = pool.push(Tree { v: 33, l: ta, r: tb });
    pool.push(Tree { v: 44, l: tc });
    var img: u8[>..] = pool.to_bytes();
    let pfx = img.len - pool.bytes_of().len;
    print(pool.len, " ", img.len, " ", pfx);                // 4 49 1
    var back, ok = from_bytes<Tree[>..]>(img);
    print(ok, " ", back.len, " ", tsum(back) == tsum(pool));    // true 4 true
    var wild: u8[>..] = pool.to_bytes();
    wild[pfx + 40] = 200;
    var w, wok = from_bytes<Tree[>..]>(wild);
    print("wild ", wok, " ", w.len);                        // wild false 0
    var cut: u8[>..] = pool.to_bytes();
    cut.pop();
    var c, cok = from_bytes<Tree[>..]>(cut);
    print("cut ", cok, " ", c.len);                         // cut false 0

    // Variable-size elements round trip too.
    var rimg: u8[>..] = recs.to_bytes();
    var rback, rok = from_bytes<Rec[>..]>(rimg);
    print(rok, " ", rback.len, " ", rback == recs);          // true 3 true
    for x in rback { print(x.name, " ", x.age, " ", x.tags, " ", x.score); }
}
