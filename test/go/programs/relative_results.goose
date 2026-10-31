// Results holding relative references (§3.9, issue #59): built where the
// caller receives them, so that a link from one into the receiver's storage
// is measured from there, as C's result locations do -- a pushed element, an
// element assigned, the first of several results, a case function's result,
// a function value's, one forwarded through another call, and one a
// long-distance return delivers -- at -O0, where every call stays a call, as
// at -O2, where most are inlined.
struct Node { id: i64, next: Node&<u32>? }
enum Kind { A, B }

fn make(first: Node&, id: i64) -> Node { Node { id: id, next: first } }
fn forward(first: Node&, id: i64) -> Node { make(first, id) }
fn two(first: Node&) -> Node, i64 { return make(first, 20), 21; }
fn two_via(first: Node&) -> Node, i64 { return two(first); }
fn pick(k: Kind.A, first: Node&) -> Node { make(first, 30) }
fn pick(k: Kind.B, first: Node&) -> Node { make(first, 31) }
fn build<F>() -> Node { F() }
fn invoke<F>() { F(); }
fn distant(first: Node&, far: bool) -> Node {
    invoke() { if far { return make(first, 40) from distant; } };
    make(first, 41)
}
fn chosen(first: Node&, flag: bool) -> Node {
    loop { if flag { break make(first, 50); } break forward(first, 51); }
}

fn main() {
    var xs: Node[>..] = [];
    let first .= xs.push(Node { id: 1 });
    xs.push(make(first, 2));
    xs.push(forward(first, 3));
    xs.push(Node { id: 4 });
    xs[3] = forward(first, 5);
    xs.push(two_via(first));
    var k = Kind.A;
    for i in 2 {
        xs.push(pick(k, first));
        k = Kind.B;
    }
    xs.push(build() { make(first, 60) });
    xs.push(distant(first, true));
    xs.push(distant(first, false));
    xs.push(chosen(first, true));
    xs.push(chosen(first, false));
    for &x in xs {
        let next = x.next;
        print(x.id, " ", if next { next.id } else { 0 });
    }
}
