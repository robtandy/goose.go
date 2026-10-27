// Arrays come with a later version of the Go backend.
fn main() {
    var xs: i64[>..] = [1, 2, 3];
    xs.push(4);
    print(xs.len);
}
// error: is not supported by --target=go yet
