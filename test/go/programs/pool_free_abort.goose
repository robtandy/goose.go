// free of a slot index the pool does not have aborts, as indexing it would:
// the freelist would hand it out unchecked.
fn main() {
    reusable var pool: i64[>..] = [];
    let a = pool.alloc_index(5);
    pool.free(a);
    print(pool.alloc_index(6), " ", pool.len);
    pool.free(3);
    print("not reached");
}
