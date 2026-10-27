// A member of an extern block binds a function of a shared library at run
// time (§7.10), which a Go program built without cgo cannot load: its call
// is refused, where a plain extern fn calls a Go function of the package.
extern "m" {
    fn cbrt(x: f64) -> f64;
}

fn main() { print(cbrt(27.0)); }
// error: a function of the shared library "m" (an extern block) is not supported by --target=go yet
