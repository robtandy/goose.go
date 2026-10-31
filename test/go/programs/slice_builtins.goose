// The checked array builtins of issue #59: move_from, a slice's elements
// copied over another's of the same length as memmove copies them, either
// way an overlap goes; range_index, where a slice of an array starts in it,
// checked at run time where the checker cannot prove the slice the array's;
// and check_length, a count checked to be one the array's elements can have
// on a data stack. Each on fixed, limited and resizable arrays, through
// references, and with elements of every size, none included.

struct P { x: i32, y: u8 }
struct Empty {}

fn index_in(a: i64[>..]&, s: i64[:]) -> i64 { a.range_index(s) }
fn index_fixed(a: P[4]&, s: P[:]) -> i64 { a.range_index(s, "index_fixed: not a run of a") }
fn shift_left(s: i64[:], by: i64) { s[..s.len - by].move_from(s[by..]); }
fn shift_right(s: i64[:], by: i64) { s[by..].move_from(s[..s.len - by]); }

fn main() {
    var a: i64[>..] = [1, 2, 3, 4, 5, 6];
    shift_left(a[..], 2);
    print(a);
    shift_right(a[..], 3);
    print(a);
    a[1..3].move_from(a[1..3]);
    a[..0].move_from(a[6..]);
    print(a);
    var ps: P[4] = [P { 1, 1 }, P { 2, 2 }, P { 3, 3 }, P { 4, 4 }];
    ps[1..].move_from(ps[..3]);
    print(ps[0].x, " ", ps[1].x, " ", ps[2].x, " ", ps[3].y);
    var bs: u8[>..] = "abcdef";
    bs[..3].move_from(bs[3..]);
    print(bs);
    var es: Empty[3] = [Empty {}, Empty {}, Empty {}];
    es[..2].move_from(es[1..]);

    // Where a slice of each array starts, proven or checked.
    print(a.range_index(a[2..5]), " ", a.range_index(a[6..]), " ", index_in(a, a[3..4]), " ",
          index_in(a, a[..0]));
    print(index_fixed(ps, ps[2..]), " ", ps.range_index(ps[1..2]), " ", es.range_index(es[1..]));
    var lim: u8[..8] = [9, 8, 7];
    print(lim.range_index(lim[1..]), " ", bs.range_index(bs[4..]));
    var b: i64[>..] = [7, 8];
    print(index_in(a, b[..0]));

    // Counts a data stack can hold; a call's own counts, a u8's and an i64's.
    print(a.check_length(0), " ", a.check_length(1000000), " ", bs.check_length(5),
          " ", a.check_length(3, "a: bad count"));
    var big: P[>..] = [];
    print(big.check_length(268435455), " ", big.len);
}
