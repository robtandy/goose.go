// alloc_slice of a negative length aborts before anything changes
// (GS_E_SLICELEN).
fn grab(n: i64) -> i64 {
    reusable[] var pool: i64[>..] = [];
    let s = pool.alloc_slice(n);
    return s.len + pool.len;
}

fn main() {
    print(grab(3));
    print(grab(-2));
    print("not reached");
}
