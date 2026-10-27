// Structs come with a later version of the Go backend: the compile stops
// at the first one, naming it, rather than writing Go that would be wrong.
struct Point { x: i64, y: i64 }

fn main() {
    let p = Point { x: 1, y: 2 };
    print(p.x + p.y);
}
// error: a value of type `Point` is not supported by --target=go yet
