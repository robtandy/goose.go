// Case functions (§8.2): an overload per variant type, which a call with
// the ADT dispatches to on its tag, the arguments evaluated once and in
// order, the payload handed on by value, also from behind a reference.

enum Shape { Circle { r: f64 }, Rect { w: f64, h: f64 }, Point }
enum Cmd { Move { dx: i32, dy: i32 }, Stop, Say { code: u16 } }

var trail = 0;
fn note(n: i64) -> i64 { trail = trail * 10 + n; n }

fn area(c: Shape.Circle) -> f64 { c.r * c.r * 3.0 }
fn area(r: Shape.Rect) -> f64 { r.w * r.h }
fn area(p: Shape.Point) -> f64 { 0.0 }

fn scaled(k: i64, c: Shape.Circle, bias: i64) -> f64 { k * c.r + bias }
fn scaled(k: i64, r: Shape.Rect, bias: i64) -> f64 { k * (r.w + r.h) + bias }
fn scaled(k: i64, p: Shape.Point, bias: i64) -> f64 { k + bias }

fn apply(pos: i32[2]&, m: Cmd.Move) -> i64 { pos[0] += m.dx; pos[1] += m.dy; 1 }
fn apply(pos: i32[2]&, s: Cmd.Stop) -> i64 { 0 }
fn apply(pos: i32[2]&, s: Cmd.Say) -> i64 { print("say ", s.code); 2 }

fn both(c: Shape.Circle) -> f64, i64 { return c.r, 1; }
fn both(r: Shape.Rect) -> f64, i64 { return r.w, 2; }
fn both(p: Shape.Point) -> f64, i64 { return 0.0, 3; }
fn forward(s: Shape) -> f64, i64 { return both(s); }

fn total(shapes: const Shape[:]) -> f64 {
    var t = 0.0;
    for s in shapes { t += area(s); }
    t
}

fn main() {
    let shapes: Shape[3] = [Shape.Circle { r: 1.0 }, Shape.Rect { w: 2.0, h: 3.5 }, Shape.Point];
    print(total(shapes), " ", area(shapes[1]));
    for s in shapes { print(scaled(note(2), s, note(3)), " ", trail); }
    var pos: i32[2] = [0, 0];
    let cmds: Cmd[4] = [Cmd.Move { dx: 3, dy: -1 }, Cmd.Say { code: 7 }, Cmd.Stop, Cmd.Move { dx: 1, dy: 1 }];
    var score = 0;
    for &c in cmds { score += apply(pos, c); }
    print(pos, " ", score);
    let x, k = both(shapes[1]);
    let y, j = forward(shapes[0]);
    print(x, " ", k, " ", y, " ", j);
    let sr: Shape& = shapes[2];
    print(area(sr));
}
