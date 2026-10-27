// Names (§11.1) that Go reserves or predeclares, used as Goose names: every
// Goose name carries a suffix in the generated Go, so none of them collides
// with Go's keywords, its predeclared names, the runtime's or the
// temporaries'; a namespaced function is apart from a global one of the
// same name; shadowed locals and unused ones are each a Go variable.
import std;
import modules.names_geo;

var len: i64 = 3;
let nil: i64 = 4;
var gs: i64 = 5;

fn panic(x: i64) -> i64 { x + 1 }
fn func(chan: i64, map: i64) -> i64 { chan * map }
fn init() -> i64 { 7 }
fn string(x: i64) -> i64 { x * 10 }
fn t0(x: i64) -> i64 { x - 1 }
fn norm(x: i64) -> i64 { x * 100 }

fn main() {
    let range = 1;
    let select = 2;
    let interface = 3;
    let fallthrough = 4;
    let package = 5;
    let goto = 6;
    let switch = 7;
    let case = 8;
    let default = 9;
    let go = 10;
    let int = 11;
    let byte = 12;
    let append = 13;
    let copy_ = 14;
    let math = 15;
    let os = 16;
    let unsafe = 17;
    let strconv = 18;
    let syscall = 19;
    let gs_sp = 20;
    let t1 = 21;
    let L0 = 22;
    print(range + select + interface + fallthrough + package + goto + switch + case + default + go);
    print(int + byte + append + copy_ + math + os + unsafe + strconv + syscall + gs_sp + t1 + L0);
    print(len, " ", nil, " ", gs, " ", panic(1), " ", func(3, 4), " ", init(), " ", string(5),
          " ", t0(9));
    print(norm(2), " ", geo::norm(2), " ", geo::scale(3), " ", geo::SCALE);

    // Shadowing: each a variable of its own.
    let x = 1;
    {
        let x = 2;
        {
            let x = x * 10;
            print("inner ", x);
        }
        print("middle ", x);
    }
    print("outer ", x);

    // Locals nothing reads, or that are only assigned.
    let unused = 42;
    var written = 0;
    written = 5;
    var bumped = 0;
    bumped += 1;
    for i in 3 { let twice = i * 2; }
    print("done ", min(abs(-3), 9), " ", max(len, 2));
}
