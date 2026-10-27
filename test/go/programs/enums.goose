// Fixed-mode ADTs (§3.5, §8.1): a payload-less one is its tag, any other
// the tag (1 byte up to 256 variants) and the largest payload's bytes.
// Variant literals and constants, variants as values of their own type and
// as their ADT, match by value over an ADT and through a reference (binders,
// lists of variants, wildcards, match as a value), overwriting with another
// variant, equality and print.

enum Color { Red, Green, Blue }
enum Shape { Circle { r: f64 }, Rect { w: f64, h: f64 }, Point, Tri { a: i16, b: i16, c: i16 } }
enum Op { Push { v: i64 }, Add, Mul, Neg, Pop }
enum Opt<T> { None, Some { v: T } }
struct Sprite { s: Shape, c: Color, id: u16 }

fn area(s: Shape) -> f64 {
    match s {
        Circle c => c.r * c.r * 3.0,
        Rect r => r.w * r.h,
        Point => 0.0,
        Tri t => t.a + t.b + t.c,
    }
}

fn code(c: Color) -> i64 { match c { Red => 1, Green, Blue => 2 } }

fn grow(s: Shape&) {
    match s {
        Circle c => { s = Shape.Circle { r: c.r * 2.0 }; },
        Rect r => { s = Shape.Rect { w: r.h, h: r.w }; },
        _ => { s = Shape.Point; },
    }
}

fn run(prog: const Op[:]) -> i64 {
    var st: i64[..8] = [];
    for op in prog {
        match op {
            Push p => { st.push(p.v); },
            Add => { let b = st.pop(); let a = st.pop(); st.push(a + b); },
            Mul => { let b = st.pop(); let a = st.pop(); st.push(a * b); },
            Neg => { st.push(-st.pop()); },
            Pop => { st.pop(); },
        }
    }
    st.pop()
}

fn unwrap_or<T>(o: Opt<T>, d: T) -> T { match o { Some s => s.v, None => d } }

fn main() {
    var s: Shape = Shape.Rect { w: 2.0, h: 3.0 };
    print(area(s), " ", area(Shape.Circle { r: 1.0 }), " ", area(Shape.Point), " ",
          area(Shape.Tri { a: 1, b: 2, c: 3 }));
    print(s, " ", Shape.Point, " ", Color.Green, " ", Shape.Tri { a: -1, b: 0, c: 1 });
    var c = Color.Blue;
    print(code(c), " ", code(Color.Red), " ", c == Color.Blue, " ", c != Color.Blue, " ", c);
    c = Color.Green;
    print(c, " ", code(c));

    s = Shape.Circle { r: 4.0 };
    grow(s);
    let big: Shape = Shape.Circle { r: 8.0 };
    print(s, " ", s == big, " ", s != big);
    grow(s);
    grow(s);
    print(s);
    var t: Shape = Shape.Rect { w: 1.0, h: 5.0 };
    grow(t);
    let flat: Shape = Shape.Rect { w: 5.0, h: 1.0 };
    let dot: Shape = Shape.Point;
    print(t, " ", t == flat, " ", t == dot);

    var sp = Sprite { s: Shape.Point, c: Color.Red, id: 7 };
    sp.s = Shape.Rect { w: 1.0, h: 1.0 };
    sp.c = Color.Blue;
    print(sp, " ", sp == Sprite { s: Shape.Rect { w: 1.0, h: 1.0 }, c: Color.Blue, id: 7 });

    let shapes = [Shape.Point, Shape.Circle { r: 0.5 }, Shape.Rect { w: 1.0, h: 2.0 }];
    var tot = 0.0;
    for x in shapes { tot += area(x); }
    print(tot, " ", shapes);

    let k = match c { Red => "red", Green => "green", Blue => "blue" };
    print(k);
    let circle: Shape.Circle = Shape.Circle { r: 2.5 };
    let as_shape: Shape = circle;
    print(circle, " ", as_shape, " ", area(circle), " ", circle.r);

    let prog: Op[6] = [Op.Push { v: 6 }, Op.Push { v: 7 }, Op.Mul, Op.Push { v: 2 }, Op.Neg, Op.Add];
    print(run(prog), " ", prog[0], " ", prog[2]);

    let o: Opt<i64> = Opt<i64>.Some { v: 41 };
    let n: Opt<i64> = Opt<i64>.None {};
    print(unwrap_or(o, 0) + 1, " ", unwrap_or(n, -1), " ", o, " ", n, " ", o == n);
    let of: Opt<f32> = Opt<f32>.Some { v: 1.5 };
    print(unwrap_or(of, 0.0), " ", of);
}
