// Self-relative references (§3.9): a slot holds the signed distance from
// itself to its target, at the width it declares, with 0 the null of the
// optional form. Links between the elements of one array, in a fixed
// array, a limited one (pushed, popped and resized) and the fields of one
// value, `self`, optional and non-optional links, every width, and the
// range checks a store makes where an offset may not fit its width.

struct Node { v: i64, next: Node&<u8>? }
struct Ring { k: i64, prev: Ring&<u16>, next: Ring&<u16> }
struct List { head: Node&<u32>?, nodes: Node[4] }
struct Tree { nodes: Node[..8], links: (Node&<u16>?)[..8] }
struct Wide { val: i32, far: Wide&<u64>? }

fn total(l: List&) -> i64 {
    var t = 0;
    var n = l.head;
    while n { t += n.v; n .= n.next; }
    t
}

fn show(n: Node&?) {
    guard n else { print("null"); return; }
    print(n.v);
}

fn main() {
    var l = List { head: null, nodes: [Node { v: 1 }, Node { v: 2 }, Node { v: 3 }, Node { v: 4 }] };
    print(total(l));
    l.head .= l.nodes[0];
    l.nodes[0].next .= l.nodes[2];
    l.nodes[2].next .= l.nodes[3];
    print(total(l));
    l.nodes[3].next .= l.nodes[1];
    l.nodes[1].next .= null;
    print(total(l));
    let first = l.nodes[0].next;
    guard first else { return; }
    print(first.v, " ", l.nodes[2].next .== l.nodes[3], " ", l.nodes);
    l.nodes[2].v = 300;
    print(total(l));

    var r = Ring { k: 7, prev: self, next: self };
    print(r.next.k, " ", r.prev.next.k, " ", r.next .== r);

    var t = Tree { nodes: [], links: [] };
    t.nodes.push(Node { v: 10 });
    t.nodes.push(Node { v: 20 });
    t.links.push(t.nodes[1]);
    t.links.push(null);
    t.links.push(t.nodes[0]);
    for x in t.links { show(x); }
    t.links.resize(5, t.nodes[1]);
    print(t.links.len, " ", t.links);
    show(t.links.pop());
    t.links.resize(2);
    show(t.links.pop());
    show(t.links[0]);
    t.links[0] .= t.nodes[0];
    show(t.links[0]);
    print(t.nodes, " ", t.links);

    var w = [Wide { val: 1 }, Wide { val: 2 }];
    w[0].far .= w[1];
    w[1].far .= w[0];
    let f0 = w[0].far;
    let f1 = w[1].far;
    guard f0 else { return; }
    guard f1 else { return; }
    print(f0.val, " ", f1.val);
}
