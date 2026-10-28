// Values of every class through the typed queues (§11.2), to a worker and
// back: a nested frame object, a variable-mode ADT, static and runtime
// capacity limited arrays, fixed arrays, a grow-shrink array; then a missed
// qget<T>(ms) and qpoll, which yield T's zero value.
struct Bag { id: i64, data: i64[>..] }
struct Nested { tag: u8, bag: Bag }
enum Shape { Circle { r: f64 }, Poly { pts: i32[] } }
struct Lim { n: i64, xs: i32[..5] }
struct Go { n: i64 }

thread_fn echo() {
    let a = qget<Nested>();
    let c = qget<Shape..>();
    let d = qget<Lim>();
    let e = qget<u8[..10]>();
    let f = qget<i64[4]>();
    var g = qget<i64[>..<]>();
    g.push(1);
    let h = qget<u8[]>();
    qget<Go>();
    qput(a);
    qput(c);
    qput(d);
    qput(e);
    qput(f);
    qput(g);
    qput(h);
}

fn main() {
    let id = thread_spawn(echo);
    qput(Nested { tag: 3, bag: Bag { id: 9, data: [1, 2, 3] } });
    let s: Shape.. = Shape.Poly { pts: [1, 2, 3, 4] };
    qput(s);
    qput(Lim { n: 2, xs: [7, 8] });
    let e: u8[..10] = "abc";
    qput(e);
    let f: i64[4] = [4, 3, 2, 1];
    qput(f);
    var g: i64[>..<] = [9, 8];
    qput(g);
    let h: u8[] = "bytes";
    qput(h);
    qput(Go { n: 0 });
    thread_wait(id);
    let a = qget<Nested>();
    print(a.tag, " ", a.bag.id, " ", a.bag.data);
    let c = qget<Shape..>();
    match c {
        Circle x => print("circle ", x.r),
        Poly p => print("poly ", p.pts),
    }
    let d = qget<Lim>();
    print(d.n, " ", d.xs);
    print(qget<u8[..10]>(), " ", qget<i64[4]>(), " ", qget<i64[>..<]>(), " ", qget<u8[]>());
    let y, ok = qget<Nested>(1);
    print(ok, " ", y.tag, " ", y.bag.data.len);
    let z, ok2 = qpoll<Shape..>();
    match z {
        Circle x => print(ok2, " circle ", x.r),
        Poly p => print(ok2, " poly ", p.pts),
    }
    let w, ok3 = qpoll<u8[]>();
    print(ok3, " ", w.len);
}
