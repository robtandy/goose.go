// A string held in a variable is a slice, which comes with a later version
// of the Go backend; a literal printed or aborted with is not.
fn main() {
    let name = "goose";
    print("hello ", name);
}
// error: a value of type `const u8[:]` is not supported by --target=go yet
