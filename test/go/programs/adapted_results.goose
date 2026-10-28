// Call results the checker adapted to an ADT they do not arrive as (§3.5):
// a fixed-mode ADT to the variable-mode one, a variant to its ADT in either
// mode (a fixed-size variant and one that is not), and the variable-mode
// ADT, by value and through a reference, to the fixed-mode one. They come
// from calls that stay calls (recursive functions, and every call at -O0),
// from inlined ones, from case functions dispatched on a tag and from a
// function value, and land in a let, an argument by value and by
// reference, a field, a pushed element, a returned value, a forwarded one
// and a match. Each call is made once, which the count of them shows.
enum E { A { n: i64 }, B { s: i64 }, C }
enum Msg { Ping, Text { body: u8[] } }
enum K { X { v: i64 }, Y { v: i64 } }
struct HoldV { e: E.., n: i64 }
struct HoldF { e: E, n: i64 }

var calls = 0;

recursive fn fixed(k: i64) -> E {
    if k > 2 { return fixed(k - 3); }
    calls += 1;
    if k == 0 { return E.A { n: 3 }; }
    if k == 1 { return E.B { s: 5 }; }
    return E.C;
}
recursive fn varmode(k: i64) -> E.. {
    if k > 2 { return varmode(k - 3); }
    calls += 1;
    if k == 0 { return E.A { n: 4 }; }
    if k == 1 { return E.B { s: 6 }; }
    return E.C;
}
recursive fn picked(e: E..&, k: i64) -> E..& {
    if k > 0 { return picked(e, k - 1); }
    calls += 1;
    return e;
}
recursive fn variant(k: i64) -> E.B {
    if k > 0 { return variant(k - 1); }
    calls += 1;
    return E.B { s: 9 };
}
recursive fn text(k: i64) -> Msg.Text {
    if k > 0 { return text(k - 1); }
    calls += 1;
    return Msg.Text { body: "hey" };
}

recursive fn pair(k: i64) -> E, i64 {
    if k > 0 { return pair(k - 1); }
    calls += 1;
    return E.B { s: 41 }, 42;
}
recursive fn vpair(k: i64) -> E.B, i64 {
    if k > 0 { return vpair(k - 1); }
    calls += 1;
    return E.B { s: 43 }, 44;
}
fn fwd(k: i64) -> E.., i64 { return pair(k); }
fn fwdb(k: i64) -> E.., i64 { return vpair(k); }

// The same, which -O2 inlines.
fn ifixed(k: i64) -> E {
    calls += 1;
    if k == 0 { return E.A { n: 13 }; }
    if k == 1 { return E.B { s: 15 }; }
    return E.C;
}
fn ivariant(k: i64) -> E.B {
    calls += 1;
    return E.B { s: 19 + k };
}
fn itext(k: i64) -> Msg.Text {
    calls += 1;
    return Msg.Text { body: "inline" };
}

// Case functions dispatched on a tag, and a function value.
fn conv(k: K.X) -> E {
    calls += 1;
    return E.A { n: k.v };
}
fn conv(k: K.Y) -> E {
    calls += 1;
    return E.B { s: k.v };
}
fn apply<F>(k: i64) -> E { return F(k); }

fn show_v(e: E..) -> i64 { return match e { A a => a.n, B b => b.s * 10, C => -1 }; }
fn show_r(e: E..&) -> i64 { return match e { A a => a.n, B b => b.s * 10, C => -1 }; }
fn show_f(e: E) -> i64 { return match e { A a => a.n, B b => b.s * 10, C => -1 }; }
fn show_m(m: Msg..) -> i64 { return match m { Ping => -1, Text t => t.body.len }; }
fn wrap_v(k: i64) -> E.. { return fixed(k); }
fn wrap_f(k: i64) -> E { return variant(k); }

fn main() {
    var vs: E..[>..] = [];
    for k in 3 {
        // The fixed-mode ADT as the variable-mode one.
        let v: E.. = fixed(k);
        let h = HoldV { e: fixed(k + 3), n: k };
        vs.push(fixed(k));
        let m = match v { A a => a.n, B b => b.s, C => 0 };
        print(v, " ", show_v(fixed(k)), " ", show_r(v), " ", h.e, " ", h.n, " ", wrap_v(k + 3), " ", m,
              " ", calls);
        let iv: E.. = ifixed(k);
        print(iv, " ", show_v(ifixed(k)), " ", calls);
        // The variable-mode ADT as the fixed-mode one, by value.
        let f: E = varmode(k);
        let hf = HoldF { e: varmode(k + 3), n: k };
        print(f, " ", show_f(varmode(k)), " ", hf.e, " ", hf.n, " ", calls);
    }
    // ... and through a reference.
    for e in vs {
        let r: E = picked(e, 2);
        print(e, " ", r, " ", show_f(picked(e, 1)), " ", calls);
    }
    // A variant as its ADT, in either mode.
    let fv: E = variant(2);
    let vv: E.. = variant(1);
    let hv = HoldV { e: variant(1), n: 3 };
    print(fv, " ", vv, " ", show_f(variant(1)), " ", show_v(variant(2)), " ", hv.e, " ", hv.n, " ",
          wrap_f(1), " ", calls);
    let mt: Msg.. = text(2);
    print(mt, " ", show_m(text(1)), " ", calls);
    let ivv: E.. = ivariant(1);
    let ifv: E = ivariant(2);
    let imt: Msg.. = itext(0);
    print(ivv, " ", ifv, " ", show_v(ivariant(3)), " ", imt, " ", show_m(itext(1)), " ", calls);
    // Case functions dispatched on a tag, and a function value.
    var ks: K[>..] = [];
    ks.push(K.X { v: 21 });
    ks.push(K.Y { v: 22 });
    for x in ks {
        let d: E.. = conv(x);
        print(d, " ", show_v(conv(x)), " ", show_f(conv(x)), " ", calls);
    }
    // Forwarded by a return.
    let p1, p2 = fwd(1);
    let p3, p4 = fwdb(2);
    print(p1, " ", p2, " ", p3, " ", p4, " ", calls);
    let q: E.. = apply(31) { E.B { s: it } };
    let qf: E = apply(32) { E.B { s: it } };
    print(q, " ", qf, " ", show_v(apply(33) { E.A { n: it } }));
    print("calls ", calls);
}
