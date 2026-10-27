// A fixed-size value of more than 4096 bytes is held on a data stack by the
// C backend wherever it is a local or a temporary, which comes with a later
// version of the Go backend (globals are supported).
fn main() {
    var grid: u8[5000] = [0; 5000];
    grid[3] = 1;
    print(grid[3]);
}
// error: a local or temporary value of `u8[5000]`
