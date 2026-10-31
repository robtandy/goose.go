// Slices held in memory, read and written where they lie (issue #62). The Go
// backend reads one with Go's own load where its address is aligned as Go
// aligns a slice, as a Go []string's strings and a local array's are, and
// copies its bytes anywhere else; it writes one's bytes wherever it lies. A
// slice of slices can view a run that Go would not align -- a field of a
// packed struct, the elements of a variable array after its length, a data
// stack's top after an odd number of bytes -- which -d=checkptr would take a
// typed load of for a misaligned pointer. Each, read, written, compared and
// copied, through both backends.
import std;

// Its slices at byte 1, and a reference to one at byte 49.
struct Rec { tag: u8, names: (const u8[:])[3], pick: (const u8[:])&? }

fn total(xs: const (const u8[:])[:]) -> i64 {
    var n = 0;
    for x in xs { n += x.len; }
    n
}

fn show(what: const u8[:], xs: const (const u8[:])[:]) {
    print(what, ": ", xs.len, " slices, ", total(xs), " bytes");
    for x, i in xs { print("  ", i, " [", x, "] ", x.len); }
}

// The longest of xs, the first of those as long: a reference to it.
fn longest(xs: (const u8[:])[:]) -> (const u8[:])& {
    var best = 0;
    for x, i in xs {
        if x.len > xs[best].len { best = i; }
    }
    xs[best]
}

// The slices of xs in reverse order, in place.
fn reverse(xs: (const u8[:])[:]) {
    var i = 0;
    var j = xs.len - 1;
    while i < j {
        let t = xs[i];
        xs[i] = xs[j];
        xs[j] = t;
        i += 1;
        j -= 1;
    }
}

fn main() {
    // A local's fixed array, and a slice of it.
    var names: (const u8[:])[3] = ["ab", "cde", ""];
    show("local", names[..]);
    reverse(names[..]);
    show("local reversed", names[..]);

    // A packed struct's: the slices at byte 1.
    var r = Rec { tag: 7, names: ["xy", "zzz", "w"], pick: null };
    show("field", r.names[..]);
    r.names[1] = "quux";
    let s = r.names[..];
    r.names[2] = s[1];
    show("field written", r.names[..]);
    print("longest ", longest(r.names[..]));
    r.pick .= names[1];
    let p = r.pick;
    if p != null { print("pick ", p, " ", p.len); }
    reverse(r.names[..]);
    show("field reversed", r.names[..]);
    print("tag ", r.tag, ", equal ", s[2] == "xy", " ", s[1] == names[1], " ", s[0] == s[1], " ", s[0] == "quux");

    // A variable array's: the slices after its 4-byte length.
    let v: (const u8[:])[] = ["one", "three", "five", "seven"];
    show("variable", v);
    let at = longest_of(v);
    print("longest at ", at, ": ", v[at]);

    // A data stack's, after one byte.
    var bytes: u8[>..] = [];
    bytes.push(1);
    var kept: (const u8[:])[>..] = [];
    for x in s { kept.push(x); }
    kept.push(names[0]);
    show("kept", kept[..]);
    reverse(kept[..]);
    show("kept reversed", kept[..]);
    var copies: (const u8[:])[4] = ["", "", "", ""];
    for x, i in kept {
        if i < 4 { copies[i] = x; }
    }
    show("copies", copies[..]);
    print("bytes ", bytes.len, " ", copies[0] == kept[0], " ", copies[1] == "quux");
}

// Where the longest of xs is, the first of those as long; -1 for none.
fn longest_of(xs: const (const u8[:])[:]) -> i64 {
    if xs.len == 0 { return -1; }
    var best = 0;
    for x, i in xs {
        if x.len > xs[best].len { best = i; }
    }
    best
}
