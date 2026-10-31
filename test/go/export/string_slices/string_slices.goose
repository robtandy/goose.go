// export fns over a Go []string (issue #42). A Go string is its data and its
// length, the two words of a Goose const u8[:], so a Go []string is laid out
// as a const (const u8[:])[:], read-only at both depths as Go's strings are:
// --target=go takes a parameter of that type as a []string, viewed in place.
// string_slices_test.go checks what each computes through each of its entry
// points, for nil, empty and shared strings too; that a kernel's entry points
// leave a caller's array on its stack, where one keeping the strings on a
// data stack moves it to the heap; that a bad index panics with a *GsError;
// and calls from many goroutines at once (run_go_tests.py runs the tests
// with -d=checkptr, and under -race where cgo builds).
//
// digest and ordered are recursive fns whose leaf helpers, inline fns and a
// small fn, take no data stack: under --target=go they are inlined into them,
// as they are into any other function (DESIGN.md §3.14). run_go_tests.py checks that
// the Go function of each recursive fn an `// inlined:` line annotates calls
// none of the Go functions of the helpers it names (Runner.cycles).
//
// Each string's header is read where the []string holds it with one typed
// load of its words, which was a call of runtime.memmove (issue #62): the
// code Go makes of each export fn an `// asm: no calls` line annotates calls
// nothing, for arm64 and amd64, which run_go_tests.py checks
// (Runner.call_asm).
import std;

// The tags equal to tag.
export fn count_tag(tags: const (const u8[:])[:], tag: const u8[:]) -> i64 {
    var n = 0;
    for t in tags {
        if t == tag { n += 1; }
    }
    n
}

// The bytes of all the tags.
// asm: no calls
export fn total_len(tags: const (const u8[:])[:]) -> i64 {
    var n = 0;
    for t in tags { n += t.len; }
    n
}

// The longest tag, the first of those as long, viewed in place: empty where
// there is none.
// asm: no calls
export fn longest(tags: const (const u8[:])[:]) -> const u8[:] {
    if tags.len == 0 { return ""; }
    var best = 0;
    for t, i in tags {
        if t.len > tags[best].len { best = i; }
    }
    tags[best]
}

fn has(tags: const (const u8[:])[:], tag: const u8[:]) -> bool {
    for t in tags {
        if t == tag { return true; }
    }
    false
}

// Whether every tag of want is one of tags: two []strings, and a function
// of its own taking one.
export fn has_all(tags: const (const u8[:])[:], want: const (const u8[:])[:]) -> bool {
    for w in want {
        if !has(tags, w) { return false; }
    }
    true
}

// The tag at i, which is checked: out of range, an abort.
export fn tag_at(tags: const (const u8[:])[:], i: i64) -> const u8[:] { tags[i] }

// The tags from the first, sliced: their bytes.
export fn tail_len(tags: const (const u8[:])[:], from: i64) -> i64 {
    if from < 0 || from > tags.len { return -1; }
    total_len_of(tags[from..])
}

fn total_len_of(tags: const (const u8[:])[:]) -> i64 {
    var n = 0;
    for t in tags { n += t.len; }
    n
}

// FNV-1a, 64 bits, of s, as Go's hash/fnv computes it.
inline fn fnv(s: const u8[:]) -> u64 {
    var h: u64 = 14695981039346656037;
    for c in s { h = (h ^ c) * 1099511628211; }
    h
}

// The FNV-1a of the tags joined by sep, the text built on a data stack.
export fn joined_hash(tags: const (const u8[:])[:], sep: const u8[:]) -> u64 {
    var b: u8[>..] = [];
    for t, i in tags {
        if i > 0 { b.append(sep); }
        b.append(t);
    }
    fnv(b)
}

// The tags that start with prefix, counted over copies of them pushed onto a
// data stack.
export fn count_prefixed(tags: const (const u8[:])[:], prefix: const u8[:]) -> i64 {
    var kept: (const u8[:])[>..] = [];
    for t in tags { kept.push(t); }
    var n = 0;
    for t in kept {
        if t.len >= prefix.len && t[..prefix.len] == prefix { n += 1; }
    }
    n
}

fn combine(a: u64, b: u64) -> u64 { (a ^ (b >> 7)) * 1099511628211 + b }

// The digest of tags[lo..hi]: the FNV-1a of each tag at the leaves, the
// halves' digests combined above them.
// inlined: fnv, combine
recursive fn digest(tags: const (const u8[:])[:], lo: i64, hi: i64) -> u64 {
    if hi - lo <= 1 {
        if hi == lo { return 0; }
        return fnv(tags[lo]);
    }
    let mid = lo + (hi - lo) / 2;
    combine(digest(tags, lo, mid), digest(tags, mid, hi))
}

// The digest of all the tags.
export fn tags_digest(tags: const (const u8[:])[:]) -> u64 { digest(tags, 0, tags.len) }

// Whether a sorts before b, byte by byte, as Go's < of strings does.
inline fn less(a: const u8[:], b: const u8[:]) -> bool {
    var i = 0;
    while i < a.len && i < b.len {
        if a[i] != b[i] { return a[i] < b[i]; }
        i += 1;
    }
    a.len < b.len
}

// The neighbours of tags[lo..hi] in order, counted where the halves meet.
// inlined: less
recursive fn ordered(tags: const (const u8[:])[:], lo: i64, hi: i64) -> i64 {
    if hi - lo < 2 { return 0; }
    let mid = lo + (hi - lo) / 2;
    var n = 0;
    if !less(tags[mid], tags[mid - 1]) { n = 1; }
    n + ordered(tags, lo, mid) + ordered(tags, mid, hi)
}

// The tags no greater than the next: tags.len - 1 where they are sorted.
export fn sorted_pairs(tags: const (const u8[:])[:]) -> i64 { ordered(tags, 0, tags.len) }
