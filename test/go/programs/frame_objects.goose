// Frame objects (C.2): a struct whose last field is resizable and whose
// other fields are fixed-size is the Go struct of those fields with the
// tail's header last, held in the frame like a fixed value, its tail's
// elements alone on a data stack. A reference to one (or to its tail) is a
// GsRref, through which a callee grows the tail.

struct Stack { name: i64, items: i64[>..<] }
struct Doc { id: u32, title: const u8[:], body: u8[>..] }
struct Outer { tag: u8, inner: Stack }

fn push2(s: Stack&, a: i64, b: i64) {
    s.items.push(a);
    s.items.push(b);
    s.name += 1;
}

fn tail(items: i64[>..<]&) -> i64 {
    items.pop();
    return items.len;
}

fn render(d: Doc&) -> u8[>..] {
    var out: u8[>..] = [];
    format(out, d.id, ":", d.title, "=", d.body);
    return out;
}

fn make(n: i64) -> Stack {
    var s = Stack { name: n, items: [] };
    for i in n { s.items.push(i); }
    return s;
}

fn total(s: Stack) -> i64 {
    var t = s.name;
    for x in s.items { t += x; }
    return t;
}

var gs = Stack { name: 100, items: [1] };

fn main() {
    var s = Stack { name: 1, items: [10, 20] };
    push2(s, 30, 40);
    print(s, " ", s.items.len, " ", tail(s.items), " ", s.items);
    var d = Doc { id: 7, title: "notes", body: [] };
    d.body.append("hello");
    format(d.body, ", ", d.title);
    print(render(d), " ", d.body.len);
    let m = make(4);
    print(m, " ", total(copy(m)), " ", make(2).items);
    var o = Outer { tag: 3, inner: Stack { name: 5, items: [] } };
    push2(o.inner, 1, 2);
    o.inner.items.push(3);
    print(o, " ", o.inner.items[2]);
    s = make(3);
    print(s);
    push2(gs, 2, 3);
    print(gs, " ", total(copy(gs)));
    var r: Stack& = s;
    r.items.push(99);
    print(s.items, " ", r.name);
}
