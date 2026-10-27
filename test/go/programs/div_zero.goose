// Division by zero aborts in every build (§6.2), with its file and line;
// so does the remainder.

fn divide(a: i32, b: i32) -> i32 { a / b }
fn rem(a: u8, b: u8) -> u8 { a % b }

fn main() {
    print(divide(100, 7), " ", rem(200, 7), " ", divide(-100, 7));
    print(rem(5, 0));
    print("not reached");
}
