// A check failing inside a standard library function reports the place the
// program called the library (issue #59), at -O0, where nothing is inlined,
// as at -O2: slot_pool's free of a slot it does not have, called through a
// function of the program's, beside std's sort, which nothing inlines.
import std;
import slot_pool;

fn release(p: slot_pool<i64>&, i: i64) {
    p.free(i);
}

fn main() {
    var p = slot_pool<i64> {};
    let i = p.alloc_index(7);
    release(p, i);
    print(p.alloc_index(8), " ", p.items[0]);
    var xs: i64[>..] = [3, 1, 2];
    sort(xs);
    print(xs);
    release(p, 5);
    print("not reached");
}
