// A variable-size value lives on a data stack, which comes with a later
// version of the Go backend; a string literal's slice does not.
fn main() {
    let name: u8[] = "goose";
    print("hello ", name);
}
// error: a value of type `u8[]` is not supported by --target=go yet
