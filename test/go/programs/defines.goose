// goose-flags: -DGS_DEBUG=0x0 -DGS_STACK_RESERVE=0x10000 -DGS_STACK_GAP=0x100000
// The runtime's configuration as C reads a -D, here in hex: GS_DEBUG=0x0
// leaves the debug checks off, so arithmetic wraps as in a release build,
// and 0x10000 reserves 64 KB per data stack. Both backends must run alike
// and report the same stacks (the runner's command-line checks read the
// values themselves).

fn main() {
    let n: i8 = 127;
    print(n + 1);
    let u: u8 = 0;
    print(u - 1);
    var w: i32 = 2147483647;
    w += 1;
    print(w);
}
