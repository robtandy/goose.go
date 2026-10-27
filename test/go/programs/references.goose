// Plain and optional references (§3.8): Go pointers, which the collector
// sees. Reference parameters and results, writes through a reference,
// reference variables and rebinding with .=, identity (.==), optionals with
// null, narrowed by if, guard and while, references held in fields and
// elements, and the text of print, which follows them.

struct Node { v: i64, next: Node? }
struct Pt { x: i64, y: i64 }
struct Holder { p: Pt&, o: Pt? }

fn biggest(a: Pt&, b: Pt&) -> Pt& { if a.x > b.x { a } else { b } }
fn find(nodes: Node[4]&, v: i64) -> Node? {
    for &n in nodes { if n.v == v { return n; } }
    return null;
}
fn length(head: Node?) -> i64 {
    var n = 0;
    var cur = head;
    while cur { n += 1; cur .= cur.next; }
    n
}
fn incr(x: i64&) { x += 1; }
fn first_y(h: Holder&) -> i64 {
    let o = h.o;
    guard o else { return -1; }
    o.y
}

fn main() {
    var nodes = [Node { v: 1 }, Node { v: 2 }, Node { v: 3 }, Node { v: 4 }];
    nodes[0].next .= nodes[1];
    nodes[1].next .= nodes[2];
    nodes[2].next .= nodes[3];
    var n: Node? = nodes[0];
    var t = 0;
    while n { t += n.v; n .= n.next; }
    print(t, " ", n == null, " ", length(nodes[1]), " ", length(null));
    let f = find(nodes, 3);
    if f { f.v = 30; print(f.v, " ", f.next == null); }
    let g = find(nodes, 99);
    print(g == null, " ", g, " ", nodes[2]);

    var p = Pt { x: 1, y: 2 };
    var q = Pt { x: 5, y: 0 };
    let b .= biggest(p, q);
    b.y = 42;
    print(q, " ", b .== q, " ", b .== p, " ", b .!= p);
    var r: Pt& = p;
    r.x += 100;
    r .= q;
    r.x *= 2;
    print(p, " ", q, " ", r, " ", r == q);

    var a = [5, 6, 7];
    var e: i64& = a[2];
    e += 100;
    incr(e);
    e .= a[0];
    incr(a[1]);
    e *= 2;
    print(a);

    var h = Holder { p: p, o: null };
    print(first_y(h), " ", h);
    h.o .= q;
    h.p.y = -7;
    print(first_y(h), " ", h, " ", p);
    let o: Pt? = null;
    print(o, " ", o == null, " ", o != null);
    var cycle = [Node { v: 10 }, Node { v: 20 }];
    cycle[1].next .= cycle[0];
    print(cycle[1]);
}
