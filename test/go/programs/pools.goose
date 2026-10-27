// reusable pools (§5.4): a grow-only array whose freelist lives on a stack
// of its own -- free slot indices, or a slice pool's tree of free spans --
// handed around by a pool reference (GsPref), and `in pool` links measured
// from a global pool's base (§3.9).

struct Node { v: i64 = -1, next: Node&<u16 in nodes>? }
reusable var nodes: Node[>..] = [];

struct Cell { v: i64 = 7, w: i32 }

// A pool reference parameter carries the freelist with it.
fn take(pool: Cell[>..]&, v: i64) -> i64 { pool.alloc_index(Cell { v: v, w: 1 }) }

fn slots() {
    reusable var pool: Cell[>..] = [];
    let a = take(pool, 10);
    let b = take(pool, 20);
    let c = pool.alloc_index(Cell { v: 30, w: 2 });
    pool.free(b);
    let d = take(pool, 40);   // b's slot again
    let r: Cell& = pool.alloc_ref(Cell { v: 50, w: 3 });
    r.w = 33;
    print(a, " ", b, " ", c, " ", d, " ", pool.len, " ", pool);
    pool.free(a);
    pool.free(c);
    print(pool.alloc_index(Cell { v: 1, w: 1 }), " ", pool.alloc_index(Cell { v: 2, w: 2 }), " ",
          pool.alloc_index(Cell { v: 3, w: 3 }), " ", pool.len);
}

fn links() {
    let a: Node& = nodes.alloc_ref(Node { v: 1 });
    let b: Node& = nodes.alloc_ref(Node { v: 2, next: a });
    let c: Node& = nodes.alloc_ref(Node { v: 3, next: b });
    var n: Node&<u16 in nodes>? = c;
    var s = 0;
    while n {
        s = s * 10 + n.v;
        n .= n.next;
    }
    print(s, " ", nodes.len, " ", nodes.index_of(c));
}

fn fill(s: i64[:], from: i64) { for i in s.len { s[i] = from + i; } }

reusable[] var runs: i64[>..] = [];

fn spans() {
    reusable[] var pool: Cell[>..] = [];
    let x = pool.alloc_slice(3);
    x[1].w = 9;
    let y = pool.alloc_slice(2);
    pool.free_slice(x);
    let z = pool.alloc_slice(2);         // the front of x's span: defaults again
    print(z, " ", y.len, " ", pool.len, " ", pool.index_of(z[0]), " ", pool.index_of(y[0]));
    var a = runs.alloc_slice(2);
    fill(a, 1);
    let blocker = runs.alloc_slice(1);
    a = runs.realloc_slice(a, 4);        // moved past the blocker
    print(a, " ", runs.index_of(a[0]), " ", runs.len, " ", blocker.len);
    a = runs.realloc_slice(a, 1);        // shrunk in place, the rest freed
    let b = runs.alloc_slice(3);
    print(a, " ", runs.index_of(b[0]), " ", runs.len);
    runs.free_slice(b);
    runs.free_slice(a);
    let all = runs.alloc_slice(4);
    print(runs.index_of(all[0]), " ", runs.len, " ", all);
}

fn main() {
    slots();
    links();
    spans();
}
