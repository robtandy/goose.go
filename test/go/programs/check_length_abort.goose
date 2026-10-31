// check_length of a count no data stack could hold aborts (issue #59): a
// negative one, an unsigned one that would wrap, one too large, each with
// the message it is given or the runtime's.
fn main() {
    var a: i64[>..] = [];
    let n: u64 = 3;
    print(a.check_length(n), " ", a.check_length(0, "a: bad count"));
    let huge: u64 = 18446744073709551615;
    print(a.check_length(2, "a: bad count"));
    print(a.check_length(huge, "a: bad count"));
    print("not reached");
}
