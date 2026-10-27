// goose-flags: --debug
// A debug build checks `as` conversions that could change the value
// (§6.3): the value, the target type, the cast's line. `as!` never checks.

fn narrow(v: i64) -> u8 { v as u8 }
fn trunc(v: f64) -> i32 { v as i32 }

fn main() {
    print(narrow(255), " ", narrow(0), " ", trunc(-12.0), " ", 300 as! u8, " ", 2.5 as! i32);
    print(trunc(3.5));
    print("not reached");
}
