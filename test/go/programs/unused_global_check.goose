// goose-flags: --debug
// A global no function reads is still initialized, in order, before main,
// where its initializer can fail: the negation of the least i64 is an
// overflow under --debug, and the program stops before main runs. Only an
// initializer that cannot fail, nor do anything but give its value, is left
// out of the Go (CodeGenGo::PureInit).

let MIN: i64 = -9223372036854775807 - 1;
let unused = -MIN;

fn main() {
    print("main ran");
}
