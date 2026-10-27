// Patterns one arm lists under several names, which the checker allows as
// one value's aliases, and ranges such a name overlaps or touches: the Go
// tests each value once, since go vet reports the same comparison twice in
// one `||`.

let A = 1;
let B = 1;
let C: i64 = 3;
let NEG: i64 = -2;
let MINUS_TWO: i64 = -2;
let BIG: u64 = 18446744073709551615;
let TOP: u64 = 18446744073709551615;

fn small(n: i64) -> i64 {
    match n {
        A, B => 10,
        C, 2..5 => 20,
        NEG, MINUS_TWO, -1 => 30,
        _ => 40,
    }
}

fn wide(n: u64) -> i64 {
    match n {
        BIG, TOP => 1,
        0, 1 => 2,
        _ => 3,
    }
}

fn main() {
    print(small(1), " ", small(2), " ", small(3), " ", small(4), " ", small(5), " ", small(-2), " ",
          small(-1), " ", small(0), " ", small(-3));
    print(wide(18446744073709551615), " ", wide(18446744073709551614), " ", wide(0), " ",
          wide(1), " ", wide(2));
}
