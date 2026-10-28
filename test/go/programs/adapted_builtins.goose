// Builtin results the checker adapted to an ADT they do not arrive as
// (§3.5): a queue's fixed-mode value as the variable-mode ADT, bound and
// passed, its variable-mode value as the fixed-mode one, a default value as
// the variable-mode ADT, and a variant pushed (the reference push gives
// back) and popped as its ADT in either mode.
enum E { A { n: i64 }, B { s: i64 }, C }

thread_fn worker(n: i64) {
    let e: E = E.B { s: n };
    qput(e);
    let f: E.. = E.A { n: n + 1 };
    qput(f);
    let g: E = E.C;
    qput(g);
}

fn show_v(e: E..) -> i64 { return match e { A a => a.n, B b => b.s * 10, C => -1 }; }

fn main() {
    let id = thread_spawn(worker, 7);
    thread_wait(id);
    let v: E.. = qget<E>();
    let w: E = qget<E..>();
    print(v, " ", w, " ", show_v(qget<E>()));
    let d: E.. = default<E>();
    print(d, " ", show_v(default<E>()));
    var ring: E.B[>..] = [];
    ring.push(E.B { s: 1 });
    let top: E = ring.push(E.B { s: 2 });
    let popped: E = ring.pop();
    ring.push(E.B { s: 3 });
    let vp: E.. = ring.pop();
    print(top, " ", popped, " ", vp, " ", show_v(ring.pop()), " ", ring.len);
}
