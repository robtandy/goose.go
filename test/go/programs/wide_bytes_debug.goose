// goose-flags: --debug
// wide_bytes.goose's checks with --debug (issue #23): a byte a store takes
// with `as` is masked to 255 first, and every index's arithmetic is checked,
// in a wide load or store as in a byte's.
import modules.wide_bytes;

fn main() { wide_bytes_run(); }
