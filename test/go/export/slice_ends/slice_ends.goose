// export fns that slice what their Go caller hands them at its end (issue
// #39), which may be where its allocation on Go's heap ends: an address one
// past that is no valid Go pointer, which -d=checkptr (and -race) aborts on
// and Go's collector may take for a bad pointer into its heap. So an empty
// slice at the end of what it views starts at an address of the runtime's
// own, where no slice of what it views starts (GsSub). slice_ends_test.go
// hands each one Go slices and strings that end where their allocations do.
import std;

// Where the slice it is handed starts, and its length, for the test to
// read (helper.go).
extern fn note(s: const u32[:]);

// xs[xs.len..], xs[xs.len..xs.len] and xs[^0..], each noted: empty, at
// xs's end.
export fn note_tails(xs: const u32[:]) {
    note(xs[xs.len..]);
    note(xs[xs.len..xs.len]);
    note(xs[^0..]);
}

// The same of text's bytes, each a view of text.
export fn tail(text: const u8[:]) -> const u8[:] { text[text.len..] }

export fn tail_to_len(text: const u8[:]) -> const u8[:] { text[text.len..text.len] }

export fn tail_from_end(text: const u8[:]) -> const u8[:] { text[^0..] }

// text[k..], for any k up to text.len.
export fn rest(text: const u8[:], k: i64) -> const u8[:] { text[k..] }

// The sum of xs[k..], and of xs[k..k] a thousand times, which is none.
export fn sum_from(xs: const u32[:], k: i64) -> i64 {
    var s = 0;
    for x in xs[k..] { s += x; }
    for x in xs[k..k] { s += 1000 * x; }
    s
}

// How many pieces text's commas cut it into, the empty one after a last
// comma included, which std's each_split slices at text's end.
export fn pieces(text: const u8[:]) -> i64 {
    var n = 0;
    each_split(text, ',') { n += 1; };
    n
}
