// A struct with a variable-size field is a variable-size value, which comes
// with a later version of the Go backend: the compile stops at the first
// one, naming it, rather than writing Go that would be wrong.
struct Named { id: i64, name: u8[] }

fn main() {
    let n = Named { id: 1, name: "goose" };
    print(n.id);
}
// error: a value of type `Named` is not supported by --target=go yet
