// Globals (§11.1): named constants, `let`, `var` and `const` globals of
// every scalar type, initialized in declaration order before main runs, by
// initializers that may call functions, print, and read earlier globals.

let SIZE = 12;                       // A named constant.
let SCALE: f64 = 2.5;
const LIMIT: i32 = 1000;             // Read-only static data.
const NEGZERO: f64 = -0.0;
const BIG: u64 = 18446744073709551615;
const ON: bool = true;
var counter: i64 = 0;
var ratio: f32 = 0.5;
var flag = false;
var narrow: i8 = -7;

fn next_id() -> i64 {
    counter += 1;
    print("next_id -> ", counter);
    counter * 100
}

let FIRST = next_id();               // Runs at startup, in order.
let SECOND = next_id() + FIRST;
var derived: i64 = SIZE * 3 + FIRST;
let MIXED = { let t = SECOND / 100; t * t };

fn bump(by: i64) -> i64 {
    counter += by;
    counter
}

fn main() {
    print(SIZE, " ", SCALE, " ", LIMIT, " ", NEGZERO, " ", BIG, " ", ON);
    print(FIRST, " ", SECOND, " ", derived, " ", MIXED, " ", counter);
    print(bump(5), " ", bump(-2), " ", counter);
    ratio *= 3.0;
    flag = !flag;
    narrow -= 121;
    narrow -= 1;
    print(ratio, " ", flag, " ", narrow);
    derived = derived / 2;
    print(derived);
    var local = SIZE;
    local += LIMIT as i64;
    print(local, " ", LIMIT + 24, " ", BIG / 3);
}
