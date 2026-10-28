// A call's reference to a variable-mode ADT whose pointee the checker
// adapted to the fixed-mode ADT (§3.5): the call is made once, its pointee
// read from a copy, as C reads it, whichever variant it holds -- bound by a
// let, passed, stored in a field and matched on -- and a variable-mode
// result adapted the same way. -O0 keeps every call a call, which a print
// in it shows; -O2 inlines them, and both backends take the same stacks.
enum Shape { Circle { r: i64 }, Square { s: i64 }, Dot }
struct Holder { s: Shape, n: i64 }

var calls = 0;

fn pick(a: Shape..&) -> Shape..& {
    calls += 1;
    print("pick ", calls);
    return a;
}

fn make(k: i64) -> Shape.. {
    calls += 1;
    if k == 0 { return Shape.Circle { r: 3 }; }
    if k == 1 { return Shape.Square { s: 4 }; }
    return Shape.Dot;
}

fn area(s: Shape) -> i64 {
    return match s { Circle c => 3 * c.r * c.r, Square q => q.s * q.s, Dot => 0 };
}

fn main() {
    var shapes: Shape..[>..] = [];
    shapes.push(Shape.Circle { r: 2 });
    shapes.push(Shape.Square { s: 5 });
    shapes.push(Shape.Dot);
    for sh in shapes {
        let s: Shape = pick(sh);
        let h = Holder { s: pick(sh), n: calls };
        let a = area(pick(sh));
        let m = match pick(sh) { Circle c => c.r, Square q => q.s, Dot => -1 };
        print(area(s), " ", a, " ", area(h.s), " ", h.n, " ", m, " ", calls);
    }
    for k in 3 {
        let s: Shape = make(k);
        print(area(s), " ", area(make(k)), " ", calls);
    }
    print("calls ", calls);
}
