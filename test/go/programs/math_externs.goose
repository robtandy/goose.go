// math's extern fns (stdlib/math.goose) and the C library's own, bound by
// their C names, at both widths, which the Go runtime and Go's math package
// stand in for under --target=go (issue #5). Only values both libraries
// give exactly are printed: the roots, the roundings and the exact cases.
import math;

extern fn hypot(x: f64, y: f64) -> f64;
extern fn fabs(x: f64) -> f64;
extern fn fabsf(x: f32) -> f32;
extern fn fmod(x: f64, y: f64) -> f64;
extern fn copysign(x: f64, y: f64) -> f64;
extern fn cbrt(x: f64) -> f64;
extern fn sqrtf(x: f32) -> f32;

fn main() {
    // Read from arrays, so that no value is known to the optimizer.
    var xs: f64[>..] = [16.0, 2.25, -0.0, 1e308, 0.5, -2.5, 2.5, 7.75, -7.75];
    var fs: f32[>..] = [16.0, 2.25, -0.0, 3e38, 0.5, -2.5, 2.5, 7.75, -7.75];
    for x in xs {
        print(x, ": ", sqrt(fabs(x)), " ", floor(x), " ", ceil(x), " ", round(x), " ", trunc(x));
    }
    for x in fs {
        print(x, ": ", sqrt(fabsf(x)), " ", floor(x), " ", ceil(x), " ", round(x), " ", trunc(x), " ",
              sqrtf(fabsf(x)));
    }
    print(is_nan(sqrt(xs[5])), " ", is_nan(sqrt(fs[5])), " ", sqrt(xs[2]), " ", sqrt(fs[2]));
    print(pow(xs[0], 0.5), " ", pow(2.0, 10.0), " ", exp(0.0), " ", log(1.0), " ", log2(8.0));
    print(pow(fs[0], fs[4]), " ", exp(fs[2]), " ", log2(fs[0]));
    print(hypot(3.0, 4.0), " ", fmod(xs[7], 2.0), " ", fmod(xs[8], 2.0), " ", copysign(3.0, xs[2]), " ",
          cbrt(27.0));
    print(sin(0.0), " ", cos(0.0), " ", atan2(0.0, 1.0), " ", tan(0.0), " ", asin(0.0), " ", acos(1.0));
    // x * y + z rounded once: what both give where the product is exact.
    print(mul_add(xs[0], xs[1], xs[6]), " ", mul_add(fs[0], fs[1], fs[6]));
    let big = mul_add(xs[3], 2.0, -xs[3]);
    print(big == 1e308 || is_inf(big));
}
