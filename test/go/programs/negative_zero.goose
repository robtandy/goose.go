// Negative zero, which Go's constants do not have: a literal that is -0.0,
// a negative one too small for its type, which rounds to -0.0 (an f32's
// from -1e-100, an f64's from -1e-400), and those negated or divided by.
// The positive ones round to 0.0.

fn main() {
    let a: f32 = -1e-100;
    let b: f32 = 1e-100;
    let c: f32 = -1e-46;
    let d: f32 = -1.4e-45;
    let e: f32 = -0.0;
    let f: f64 = -1e-400;
    let g: f64 = -0.0;
    print(a, " ", b, " ", c, " ", d, " ", e, " ", f, " ", g);
    let h = -(1e-100 as f32);
    let i: f32 = -(0.0 as f32);
    let j = 0.0 * -1.0;
    print(h, " ", i, " ", j, " ", -1e-100 as f32, " ", 1.0 / (-1e-100 as f32), " ", 1.0 / a);
}
