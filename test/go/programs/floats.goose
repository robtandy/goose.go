// Floats (§6.3, §3.7): f32 and f64 arithmetic rounded per operation (no
// fused multiply-add), `%` as fmod, infinities, NaN and negative zero, the
// shortest text that reads back, and conversions to and from integers.

fn f(x: f64) -> f64 { x }
fn g(x: f32) -> f32 { x }

// A sum of products, the shape a fused multiply-add would round once.
fn dot(a: f64, b: f64, c: f64, d: f64) -> f64 { a * b + c * d }
fn dot32(a: f32, b: f32, c: f32, d: f32) -> f32 { a * b + c * d }

fn main() {
    print(f(0.1) + f(0.2), " ", f(1.0) / f(3.0), " ", f(2.0) / f(3.0), " ", f(1e300) * f(1e10));
    print(g(0.1) + g(0.2), " ", g(1.0) / g(3.0), " ", g(16777216.0) + g(1.0), " ", g(3.4e38) * g(10.0));
    print(f(1.0) / f(0.0), " ", f(-1.0) / f(0.0), " ", f(0.0) / f(0.0), " ", -f(0.0), " ", f(0.0) * f(-1.0));
    print(g(1.0) / g(0.0), " ", g(-1.0) / g(0.0), " ", -g(0.0), " ", f(-0.0) == f(0.0));
    let nan = f(0.0) / f(0.0);
    print(nan == nan, " ", nan != nan, " ", nan < 1.0, " ", f(1.0) < f(2.0), " ", g(2.5) >= g(2.5));

    // Products that fused would round differently.
    print(dot(0.1, 10.0, -1.0, 1.0), " ", dot(1.0 / 3.0, 3.0, -1.0, 1.0), " ",
          dot(1e16, 1.0000000000000002, -1e16, 1.0));
    print(dot32(0.1, 10.0, -1.0, 1.0), " ", dot32(1.0 / 3.0, 3.0, -1.0, 1.0));
    var acc = 0.0;
    for i in 1..20 { acc = acc * 1.1 + 1.0 / (i as f64); }
    print(acc);

    // Text forms: the fewest digits that read back, %g's layout.
    print(f(1.0), " ", f(100.0), " ", f(1e15), " ", f(1e16), " ", f(1.5e-5), " ", f(0.0001), " ",
          f(123456789012345680.0), " ", f(5e-324), " ", f(1.7976931348623157e308));
    print(g(1.0), " ", g(0.1), " ", g(1e-10), " ", g(16777217.0), " ", g(3.4028235e38), " ", g(1e-45));
    print(f(-2.5), " ", f(3.14159265358979), " ", f(2.0) / f(7.0), " ", f(1e21), " ", f(1e22) / f(3.0));

    // `%` is fmod: the sign of the dividend.
    print(f(7.5) % f(2.0), " ", f(-7.5) % f(2.0), " ", f(7.5) % f(-2.0), " ", g(5.25) % g(1.5), " ",
          f(1.0) % f(0.0), " ", f(1e300) % f(3.0));

    // Integer to float rounds; float to integer truncates (as!) or wraps.
    print(9007199254740993 as f64, " ", 16777217 as f32, " ", 18446744073709551615 as f64, " ",
          -9223372036854775807 as f32);
    print(f(2.9) as! i64, " ", f(-2.9) as! i64, " ", f(1e20) as! i64, " ", f(-1e20) as! i64, " ",
          f(3e9) as! i32, " ", f(-1.5) as! u8, " ", nan as! i64, " ", f(1e19) as! u64);
    print(g(2.5) as! i32, " ", g(-100.75) as! i16, " ", g(4e9) as! u32, " ", f(1.0) / f(0.0) as! i64);
    print(f(42.0) as i64, " ", f(-1.0) as i8, " ", g(255.0) as u8, " ", f(1e18) as u64);

    // Mixed: an integer meeting a float converts to it.
    let n = 7;
    let half = n as f64 / 2.0;
    print(half, " ", half * 2.0 == 7.0, " ", n as f32 * 0.5);

    // Compound assignment.
    var x = 1.0;
    x += 0.5;
    x *= 4.0;
    x -= 1.0;
    x /= 3.0;
    print(x);
    var y: f32 = 1.0;
    y += 0.1;
    y *= y;
    print(y);
}
