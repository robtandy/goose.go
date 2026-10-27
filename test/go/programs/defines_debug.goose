// goose-flags: -DGS_DEBUG=0X1
// GS_DEBUG=0X1 turns the debug checks on, as --debug does: the overflow
// aborts in both backends.

fn main() {
    let n: i8 = 127;
    print("before");
    print(n + 1);
    print("not reached");
}
