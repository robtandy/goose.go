// varint fields built from narrower integers (§3.6): construction stores
// take any integer type but u64, which reach the field as the i64 a varint
// holds -- read from a field through a reference, an element, a local, a
// parameter, a call's result or an expression, signed and unsigned, at
// their extremes -- in structs, ADT payloads and arrays of them, and read
// back.

struct Src { tag: u8[..3], a: i8, b: i16, c: i32, d: u8, e: u16, f: u32 }
struct Packed { name: u8[varint], a: varint, b: varint, c: varint, d: varint, e: varint, f: varint }
enum Tiny {
    Leaf { word: u8[varint], count: varint },
    Pair { x: varint, y: varint },
}

fn narrow(k: i32) -> i16 { (k * 3 - 7) as i16 }

fn pack(s: Src&) -> Packed {
    return Packed { name: s.tag[..], a: s.a, b: s.b, c: s.c, d: s.d, e: s.e, f: s.f };
}

fn leaf(word: u8[:], count: i32) -> Tiny.. { Tiny.Leaf { word: word, count: count } }

fn sum(p: Packed&) -> i64 { p.a + p.b + p.c + p.d + p.e + p.f }

fn main() {
    var srcs: Src[>..] = [];
    srcs.push(Src { tag: "lo", a: -128, b: -32768, c: -2147483648, d: 0, e: 0, f: 0 });
    srcs.push(Src { tag: "hi", a: 127, b: 32767, c: 2147483647, d: 255, e: 65535, f: 4294967295 });
    srcs.push(Src { tag: "mid", a: -1, b: 300, c: -70000, d: 128, e: 40000, f: 3000000000 });
    var packed: Packed[>..] = [];
    for s in srcs { packed.push(pack(s)); }
    for p in packed { print(p.name, " ", p.a, " ", p.b, " ", p.c, " ", p.d, " ", p.e, " ", p.f, " ", sum(p)); }

    // From elements, locals, parameters, calls and expressions.
    let small: i8[4] = [-3, 0, 5, -128];
    let wide: u32[2] = [7, 4000000000];
    var tiny: Tiny..[>..] = [];
    for i in 4 {
        let k = i as i32;
        tiny.push(Tiny.Pair { x: small[i], y: wide[i % 2] });
        tiny.push(Tiny.Pair { x: narrow(k), y: (k * 1000) as i16 });
        tiny.push(leaf("w", k - 2));
        let lo: u16 = (k * 20000) as u16;
        tiny.push(Tiny.Leaf { word: srcs[i % 3].tag[..], count: lo });
    }
    var total = 0;
    for t in tiny {
        match t {
            Leaf &l => { print("leaf ", l.word, " ", l.count); total += l.count; },
            Pair &p => { print("pair ", p.x, " ", p.y); total += p.x - p.y; },
        }
    }
    print("total ", total);
}
