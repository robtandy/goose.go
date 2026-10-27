// Elementwise arithmetic on structs and fixed arrays of numbers (§6.1):
// member by member, a scalar operand meeting every member, compound
// assignment into a value that is an operand too, and negation. The
// members are computed at their own types, which a debug build checks.

struct V3 { x: f32, y: f32, z: f32 }
struct Mixed { n: i32, f: i32, k: i32[2] }
struct Body { pos: V3, vel: V3 }

fn dot(a: V3, b: V3) -> f32 { a.x * b.x + a.y * b.y + a.z * b.z }
fn step(b: Body&, dt: f32) { b.pos += b.vel * dt; b.vel = b.vel * 0.5 - b.pos; }

fn main() {
    let a = V3 { x: 1.0, y: 2.0, z: 3.0 };
    let b = V3 { x: 0.5, y: -1.0, z: 4.0 };
    print(a + b, " ", a - b, " ", a * b, " ", a / b, " ", -a);
    print(a * 2.0, " ", 2.0 * a, " ", a / 4.0, " ", dot(a, b));
    var c = a;
    c += b;
    c *= c;
    c -= a * 0.5;
    print(c);
    var body = Body { pos: a, vel: b };
    step(body, 0.25);
    step(body, 0.25);
    print(body);
    let m = Mixed { n: 7, f: -3, k: [10, 20] };
    var m2 = m + m;
    m2 = m2 * m;
    print(m2, " ", -m, " ", m2 - m);
    var rows: i64[2][3] = [[1, 2], [3, 4], [5, 6]];
    rows += rows;
    rows = rows * 3;
    print(rows, " ", -rows[2]);
}
