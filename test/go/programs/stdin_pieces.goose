// os's read_stdin(out, max) (upstream's 2c5fcd77, issue #1): up to max bytes
// of the standard input a call, waiting for them or the input's end,
// appended to what the builder holds; false, with nothing appended, where
// no bytes are left or max is 0 or less. It reads stdin_pieces.stdin, from
// the buffer read_line takes its lines from (GsOsReadStdinMax).
import os;

fn main() {
    var line: u8[>..] = [];
    print(read_line(line), " [", line, "]");
    var got: u8[>..] = [];
    print(read_stdin(got, 0), " ", read_stdin(got, -3), " ", got.len);
    // Pieces of 7 bytes after the line, the last one short.
    var pieces = 0;
    while read_stdin(got, 7) {
        pieces += 1;
        print(pieces, ": ", got.len, " [", got[got.len - 1], "]");
    }
    print("[", got, "]");
    // The input's end: false, and nothing appended, for either.
    print(read_stdin(got, 100), " ", got.len, " ", read_line(line));
}
