// export fn comes with a later version of the Go backend.
export fn twice(x: i64) -> i64 { x * 2 }
fn main() { print(twice(21)); }
// error: export fn is not supported by --target=go yet
