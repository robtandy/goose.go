// Integers assembled from the bytes of consecutive elements of a u8 array or
// slice, and taken apart into them (issue #23): with their bounds proven, the
// Go backend makes each one load or one store of 2, 4 or 8 bytes, little- or
// big-endian. The checks are modules/wide_bytes.goose's, which
// wide_bytes_debug.goose runs with --debug.
import modules.wide_bytes;

fn main() { wide_bytes_run(); }
