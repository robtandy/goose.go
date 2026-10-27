// goose-flags: --debug
// A debug build's `as` from an integer that does not fit the target type.

fn narrow(v: i64) -> u8 { v as u8 }

fn main() {
    print(narrow(200));
    print(narrow(256));
    print("not reached");
}
