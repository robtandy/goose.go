// goose-flags: --debug
// Fixed-size values in a debug build: a match or a case function with no
// wildcard checks the tag (GS_E_TAG) where none of its arms has it, and the
// members of elementwise arithmetic are checked for overflow at their own
// types, as every other signed operation is.

struct V2 { x: i32, y: i32 }
enum Dir { N, E, S, W }
enum Op { Add { v: i16 }, Neg, Keep }

fn turn(d: Dir) -> Dir { match d { N => Dir.E, E => Dir.S, S => Dir.W, W => Dir.N } }
fn apply(o: Op.Add, x: i16) -> i16 { x + o.v }
fn apply(o: Op.Neg, x: i16) -> i16 { -x }
fn apply(o: Op.Keep, x: i16) -> i16 { x }

fn main() {
    var d = Dir.N;
    for i in 6 { d = turn(d); print(i, " ", d); }
    let ops: Op[4] = [Op.Add { v: 7 }, Op.Neg, Op.Keep, Op.Add { v: -3 }];
    var x: i16 = 1;
    for o in ops { x = apply(o, x); }
    print(x);
    var v = V2 { x: 1000, y: -1000 };
    v = v * 1000;
    v += V2 { x: 1, y: 1 };
    print(v, " ", -v, " ", v - v);
    let big = V2 { x: 2147483647, y: 0 };
    print(big + V2 { x: 1, y: 0 });
    print("not reached");
}
