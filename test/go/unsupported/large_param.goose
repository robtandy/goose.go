// A fixed-size value of more than 4096 bytes is handed to a function, and
// returned from one, through a copy on a data stack by the C backend, which
// comes with a later version of the Go backend (globals, locals, the
// temporaries of literals and defaults, queue values and a thread_fn's
// arguments are supported).
recursive fn third(grid: u8[5000], n: i64) -> u8 {
    if n == 0 { return grid[3]; }
    third(grid, n - 1)
}

fn main() {
    var grid: u8[5000] = [0; 5000];
    grid[3] = 1;
    print(third(grid, 2));
}
// error: a parameter or result of `u8[5000]`
