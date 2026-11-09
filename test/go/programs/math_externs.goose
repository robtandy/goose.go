// math's extern fns (stdlib/math.goose) and the C library's own, bound by
// their C names, at both widths, which the Go runtime and Go's math package
// stand in for under --target=go (issue #5). Only values both libraries
// give exactly are printed: the roots, the roundings and the exact cases,
// and the rest within a tolerance. math has hypot, cbrt, exp2, expm1,
// log1p, sinh, cosh, tanh and copysign of its own since upstream's 13712d6a
// (issue #1), which a program binding the C library's binds under names of
// its own; and the rest of C99's <math.h> by C's names, which a run
// in-process resolves since upstream's e4a73fb9.
import math;

extern fn fabs(x: f64) -> f64;
extern fn fabsf(x: f32) -> f32;
extern fn fmod(x: f64, y: f64) -> f64;
extern fn sqrtf(x: f32) -> f32;
extern fn c_hypot(x: f64, y: f64) -> f64 = "hypot";
extern fn c_cbrt(x: f64) -> f64 = "cbrt";
extern fn c_copysign(x: f64, y: f64) -> f64 = "copysign";
extern fn c_expm1(x: f64) -> f64 = "expm1";
extern fn c_log1pf(x: f32) -> f32 = "log1pf";
// The rest of C99's <math.h>, which Go's math has otherwise or not at all.
extern fn fmin(x: f64, y: f64) -> f64;
extern fn fmaxf(x: f32, y: f32) -> f32;
extern fn fdim(x: f64, y: f64) -> f64;
extern fn nextafter(x: f64, y: f64) -> f64;
extern fn lgamma(x: f64) -> f64;
extern fn modf(x: f64, ip: f64&) -> f64;
extern fn remquo(x: f64, y: f64, q: i32&) -> f64;
extern fn remquof(x: f32, y: f32, q: i32&) -> f32;
extern fn rint(x: f64) -> f64;
extern fn scalbnf(x: f32, n: i32) -> f32;
extern fn ilogb(x: f64) -> i32;

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
    print(c_hypot(3.0, 4.0), " ", fmod(xs[7], 2.0), " ", fmod(xs[8], 2.0), " ", c_copysign(3.0, xs[2]), " ",
          fabs(c_cbrt(xs[0] * 4.0) - 4.0) < 1e-12, " ", c_expm1(xs[2]), " ", c_log1pf(fs[2]));
    print(sin(0.0), " ", cos(0.0), " ", atan2(0.0, 1.0), " ", tan(0.0), " ", asin(0.0), " ", acos(1.0));
    // x * y + z rounded once: what both give where the product is exact.
    print(mul_add(xs[0], xs[1], xs[6]), " ", mul_add(fs[0], fs[1], fs[6]));
    let big = mul_add(xs[3], 2.0, -xs[3]);
    print(big == 1e308 || is_inf(big));
    // math's own, at both widths: exact where C's Annex F fixes the value
    // (signed zeros, the infinities, exact powers and sums of squares).
    var sp: f64[>..] = [0.0, -0.0, -1.0, 3.0, 4.0, 10.0];
    var sf: f32[>..] = [0.0, -0.0, -1.0, 3.0, 4.0, 10.0];
    let inf = INF;
    print(expm1(sp[1]), " ", log1p(sp[1]), " ", sinh(sp[1]), " ", tanh(sp[1]), " ", cbrt(sp[1]), " ",
          cosh(sp[0]), " ", exp2(sp[0]), " ", exp2(sp[5]), " ", hypot(sp[3], sp[4]));
    print(expm1(sf[1]), " ", log1p(sf[1]), " ", sinh(sf[1]), " ", tanh(sf[1]), " ", cbrt(sf[1]), " ",
          cosh(sf[0]), " ", exp2(sf[0]), " ", exp2(sf[5]), " ", hypot(sf[3], sf[4]));
    print(tanh(inf), " ", tanh(-inf), " ", cosh(-inf), " ", sinh(-inf), " ", expm1(-inf), " ", exp2(-inf), " ",
          log1p(sp[2]), " ", cbrt(-inf), " ", hypot(-inf, NAN), " ", is_nan(hypot(sp[3], NAN)));
    let finf: f32 = INF;
    print(tanh(finf), " ", cosh(-finf), " ", expm1(-finf), " ", log1p(sf[2]), " ", hypot(sf[3], -finf));
    print(copysign(sp[3], sp[1]), " ", copysign(sp[1], sp[4]), " ", copysign(sf[3], sf[2]), " ",
          copysign(inf, sp[2]), " ", copysign(finf, sf[1]));
    // Within a tolerance where libms differ in the last place.
    print(fabs(cbrt(sp[3] * 9.0) - 3.0) < 1e-12, " ", fabsf(cbrt(sf[3] * 9.0) - 3.0) < 1e-5, " ",
          fabs(sinh(-sp[2]) - (E - 1.0 / E) / 2.0) < 1e-12, " ", fabs(cosh(-sp[2]) - (E + 1.0 / E) / 2.0) < 1e-12, " ",
          fabsf(tanh(sf[3]) - sinh(sf[3]) / cosh(sf[3])) < 1e-6, " ", fabsf(expm1(sf[3]) - (exp(sf[3]) - 1.0)) < 1e-4, " ",
          fabs(log1p(sp[4]) - log(5.0)) < 1e-12);
    // C99's: fmin and fmax of a quiet NaN are the number, fdim of equal
    // infinities +0.0, nextafter of equal operands y, lgamma of -inf +inf,
    // modf of an infinity a zero and the infinity, and remquo's quotient
    // bits glibc's.
    let nan = NAN;
    let fnan: f32 = NAN;
    print(fmin(nan, sp[3]), " ", fmin(sp[3], -sp[4]), " ", fmaxf(fnan, sf[3]), " ", fdim(inf, inf), " ",
          fdim(sp[5], sp[3]), " ", nextafter(sp[0], sp[1]), " ", nextafter(1.0, 2.0) - 1.0 == 1.0 / 4503599627370496.0,
          " ", lgamma(-inf), " ", lgamma(-sp[2]), " ", rint(2.5), " ", rint(-0.5), " ", scalbnf(sf[3], 4), " ",
          ilogb(1000.0));
    var ip = 0.0;
    let frac = modf(-inf, ip);
    let frac2 = modf(-3.25, ip);
    var q: i32 = 0;
    var qf: i32 = 0;
    let r = remquo(29.0, sp[4], q);
    let rf = remquof(-10.0, sf[3], qf);
    print(frac, " ", frac2, " ", ip, " ", r, " ", q, " ", rf, " ", qf);
}
