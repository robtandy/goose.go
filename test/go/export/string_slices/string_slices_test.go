package goose

import (
	"fmt"
	"hash/fnv"
	"strings"
	"sync"
	"testing"
	"unsafe"
)

// The entry points of string_slices.goose (issue #42): export fns taking a
// Go []string, which is a const (const u8[:])[:] to Goose, viewed in place.

// fnv64a is FNV-1a, 64 bits, of s: the fnv of string_slices.goose.
func fnv64a(s string) uint64 {
	h := fnv.New64a()
	h.Write([]byte(s))
	return h.Sum64()
}

// digest is string_slices.goose's, in Go.
func digest(tags []string) uint64 {
	switch len(tags) {
	case 0:
		return 0
	case 1:
		return fnv64a(tags[0])
	}
	mid := len(tags) / 2
	a, b := digest(tags[:mid]), digest(tags[mid:])
	return (a^(b>>7))*1099511628211 + b
}

// less and ordered are string_slices.goose's, in Go: whether a sorts before
// b, byte by byte, and the neighbours in order where the halves of tags
// meet, and within each.
func less(a, b string) bool {
	for i := 0; i < len(a) && i < len(b); i++ {
		if a[i] != b[i] {
			return a[i] < b[i]
		}
	}
	return len(a) < len(b)
}

func ordered(tags []string) int64 {
	if len(tags) < 2 {
		return 0
	}
	mid := len(tags) / 2
	var n int64
	if !less(tags[mid], tags[mid-1]) {
		n = 1
	}
	return n + ordered(tags[:mid]) + ordered(tags[mid:])
}

// The lists every test takes the export fns over: nil and empty ones, empty
// strings, strings sharing their bytes, strings built as the test runs, and
// a long list.
func lists() map[string][]string {
	shared := "hello, world"
	built := make([]string, 300)
	for i := range built {
		built[i] = fmt.Sprintf("tag-%d", i%37) + strings.Repeat("x", i%5)
	}
	return map[string][]string{
		"nil":    nil,
		"empty":  {},
		"one":    {"go"},
		"blanks": {"", "a", "", "bb", ""},
		"repeat": {"x", "y", "x", "x", "zz", "y"},
		"shared": {shared[:5], shared[7:], shared[3:3], shared, shared[7:]},
		"built":  built,
	}
}

func count(tags []string, tag string) (n int64) {
	for _, t := range tags {
		if t == tag {
			n++
		}
	}
	return
}

func TestKernels(t *testing.T) {
	gs := AcquireStackBlock(4)
	defer ReleaseStackBlock(gs)
	for name, tags := range lists() {
		probes := []string{"", "x", "a", "go", "world", "tag-3", "tag-36xxxx", "absent"}
		for _, p := range probes {
			want := count(tags, p)
			for _, c := range []struct {
				form string
				got  int64
			}{
				{"CountTag(string)", CountTag(tags, p)},
				{"CountTag([]byte)", CountTag(tags, []byte(p))},
				{"CountTagWithStack", CountTagWithStack(nil, tags, p)},
				{"CountTagString", CountTagString(tags, p)},
				{"CountTagBytes", CountTagBytes(tags, []byte(p))},
			} {
				if c.got != want {
					t.Errorf("%s: %s(%q) = %d, want %d", name, c.form, p, c.got, want)
				}
			}
		}
		var total int64
		long := ""
		for _, s := range tags {
			total += int64(len(s))
			if len(s) > len(long) {
				long = s
			}
		}
		if got := TotalLen(tags); got != total {
			t.Errorf("%s: TotalLen = %d, want %d", name, got, total)
		}
		if got := TotalLenWithStack(nil, tags); got != total {
			t.Errorf("%s: TotalLenWithStack = %d, want %d", name, got, total)
		}
		if got := Longest(tags); string(got) != long {
			t.Errorf("%s: Longest = %q, want %q", name, got, long)
		}
		if got := TagsDigest(tags); got != digest(tags) {
			t.Errorf("%s: TagsDigest = %x, want %x", name, got, digest(tags))
		}
		if got := TagsDigestWithStack(nil, tags); got != digest(tags) {
			t.Errorf("%s: TagsDigestWithStack = %x, want %x", name, got, digest(tags))
		}
		var pairs int64
		for i := 1; i < len(tags); i++ {
			if tags[i-1] <= tags[i] {
				pairs++
			}
		}
		if got := SortedPairs(tags); got != pairs {
			t.Errorf("%s: SortedPairs = %d, want %d", name, got, pairs)
		}
		for from := int64(-1); from <= int64(len(tags))+1; from++ {
			want := int64(-1)
			if from >= 0 && from <= int64(len(tags)) {
				want = TotalLen(tags[from:])
			}
			if got := TailLen(tags, from); got != want {
				t.Errorf("%s: TailLen(%d) = %d, want %d", name, from, got, want)
			}
		}
		for i := range tags {
			if got := TagAt(tags, int64(i)); string(got) != tags[i] {
				t.Errorf("%s: TagAt(%d) = %q, want %q", name, i, got, tags[i])
			}
		}
		if !HasAll(tags, tags) || !HasAll(tags, nil) || (len(tags) > 0 && HasAll(nil, tags)) {
			t.Errorf("%s: HasAll of itself, of nil or in nil", name)
		}
		if HasAll(tags, []string{"absent"}) {
			t.Errorf("%s: HasAll absent", name)
		}
		for _, sep := range []string{"", ",", " | "} {
			want := fnv64a(strings.Join(tags, sep))
			for _, c := range []struct {
				form string
				got  uint64
			}{
				{"JoinedHash", JoinedHash(tags, sep)},
				{"JoinedHashWithStack", JoinedHashWithStack(gs, tags, []byte(sep))},
				{"JoinedHashString", JoinedHashString(tags, sep)},
				{"JoinedHashBytes", JoinedHashBytes(tags, []byte(sep))},
			} {
				if c.got != want {
					t.Errorf("%s: %s(%q) = %x, want %x", name, c.form, sep, c.got, want)
				}
			}
		}
		for _, prefix := range []string{"", "x", "tag-1", "hel"} {
			var want int64
			for _, s := range tags {
				if strings.HasPrefix(s, prefix) {
					want++
				}
			}
			if got := CountPrefixed(tags, prefix); got != want {
				t.Errorf("%s: CountPrefixed(%q) = %d, want %d", name, prefix, got, want)
			}
			if got := CountPrefixedWithStack(gs, tags, prefix); got != want {
				t.Errorf("%s: CountPrefixedWithStack(%q) = %d, want %d", name, prefix, got, want)
			}
		}
	}
}

// A result viewing one of the strings views its bytes where they are.
func TestViews(t *testing.T) {
	s := "abc defgh ij"
	tags := []string{s[:3], s[4:9], s[10:]}
	if got := Longest(tags); len(got) != 5 || unsafe.SliceData(got) != unsafe.StringData(tags[1]) {
		t.Errorf("Longest = %q, not a view of %q", got, tags[1])
	}
	if got := TagAt(tags, 2); len(got) != 2 || unsafe.SliceData(got) != unsafe.StringData(tags[2]) {
		t.Errorf("TagAt(2) = %q, not a view of %q", got, tags[2])
	}
}

// An index out of range aborts: a panic with a *GsError in the caller's
// goroutine.
func TestBadIndexPanics(t *testing.T) {
	for _, i := range []int64{3, -1} {
		func() {
			defer func() {
				r := recover()
				e, ok := r.(*GsError)
				if !ok || !strings.Contains(e.Error(), fmt.Sprintf("index %d out of bounds (length 3)", i)) {
					t.Errorf("TagAt(%d): recovered %v", i, r)
				}
			}()
			TagAt([]string{"a", "b", "c"}, i)
			t.Errorf("TagAt(%d) returned", i)
		}()
	}
}

// A kernel's entry points hand the caller's []string on as it is, so that an
// array of strings that nothing else lets escape stays on its caller's stack:
// the call allocates nothing. Where the export fn keeps the strings on a data
// stack, its entry points move the array to the heap, which is an allocation
// each call (gsEscape), and keep it alive for the call.
func TestCallerArrays(t *testing.T) {
	var sink int64
	stay := []struct {
		name string
		f    func()
	}{
		{"CountTagString", func() {
			var a [4]string
			a[0], a[1], a[3] = "x", "y", "x"
			sink += CountTagString(a[:], "x")
		}},
		{"CountTag", func() {
			var a [4]string
			a[2] = "q"
			sink += CountTag(a[:], "q")
		}},
		{"TotalLen", func() {
			var a [4]string
			a[1] = "four"
			sink += TotalLen(a[:])
		}},
		{"TotalLenWithStack", func() {
			var a [4]string
			a[1] = "four"
			sink += TotalLenWithStack(nil, a[:])
		}},
		{"Longest", func() {
			var a [3]string
			a[0], a[2] = "ab", "abc"
			sink += int64(len(Longest(a[:])))
		}},
		{"HasAll", func() {
			var a, b [2]string
			a[0], a[1], b[0] = "u", "v", "v"
			if HasAll(a[:], b[:]) {
				sink++
			}
		}},
		{"TailLen", func() {
			var a [3]string
			a[2] = "tail"
			sink += TailLen(a[:], 1)
		}},
		{"TagsDigest", func() {
			var a [5]string
			a[0], a[4] = "first", "last"
			sink += int64(TagsDigest(a[:]) & 1)
		}},
		{"SortedPairs", func() {
			var a [5]string
			a[1], a[2] = "b", "a"
			sink += SortedPairs(a[:])
		}},
	}
	for _, c := range stay {
		if n := testing.AllocsPerRun(100, c.f); n != 0 {
			t.Errorf("%s: %v allocs/op: the caller's array left its stack", c.name, n)
		}
	}
	gs := AcquireStackBlock(4)
	defer ReleaseStackBlock(gs)
	moved := []struct {
		name string
		f    func()
	}{
		{"JoinedHashString", func() {
			var a [3]string
			a[0], a[1] = "j", "k"
			sink += int64(JoinedHashString(a[:], "-") & 1)
		}},
		{"CountPrefixedString", func() {
			var a [3]string
			a[0], a[2] = "pre", "prefix"
			sink += CountPrefixedString(a[:], "pre")
		}},
		{"CountPrefixedWithStack", func() {
			var a [3]string
			a[0], a[2] = "pre", "prefix"
			sink += CountPrefixedWithStack(gs, a[:], "pre")
		}},
	}
	for _, c := range moved {
		if n := testing.AllocsPerRun(100, c.f); n < 1 {
			t.Errorf("%s: %v allocs/op: the caller's array stayed on its stack", c.name, n)
		}
	}
	_ = sink
}

// raceEnabled is set by string_slices_race_test.go under the race detector,
// where sync.Pool drops a quarter of what it is given.
var raceEnabled bool

// A call in a loop over a list on the heap allocates nothing, a pooled
// block's included, once the pool holds one.
func TestZeroAllocs(t *testing.T) {
	tags := lists()["built"]
	gs := AcquireStackBlock(4)
	defer ReleaseStackBlock(gs)
	var sink uint64
	for _, c := range []struct {
		name   string
		pooled bool
		f      func()
	}{
		{"CountTagString", false, func() { sink += uint64(CountTagString(tags, "tag-5")) }},
		{"TagsDigest", false, func() { sink += TagsDigest(tags) }},
		{"JoinedHashWithStack", false, func() { sink += JoinedHashWithStack(gs, tags, ",") }},
		{"JoinedHashString", true, func() { sink += JoinedHashString(tags, ",") }},
		{"CountPrefixed", true, func() { sink += uint64(CountPrefixed(tags, "tag-1")) }},
	} {
		if c.pooled && raceEnabled {
			continue
		}
		if n := testing.AllocsPerRun(100, c.f); n != 0 {
			t.Errorf("%s: %v allocs/op, want 0", c.name, n)
		}
	}
	_ = sink
}

// Goroutines call the entry points at once over one shared list, each pooled
// call on a block of its own: what each computes is what one alone does, and
// the race detector sees only reads of the list.
func TestConcurrentCalls(t *testing.T) {
	tags := lists()["built"]
	wantDigest, wantJoined := digest(tags), fnv64a(strings.Join(tags, "/"))
	var wg sync.WaitGroup
	errs := make(chan string, 64)
	for g := 0; g < 16; g++ {
		wg.Add(1)
		go func(g int) {
			defer wg.Done()
			gs := AcquireStackBlock(4)
			defer ReleaseStackBlock(gs)
			for i := 0; i < 50; i++ {
				if got := TagsDigest(tags); got != wantDigest {
					errs <- fmt.Sprintf("goroutine %d: TagsDigest = %x", g, got)
					return
				}
				if got := JoinedHashString(tags, "/"); got != wantJoined {
					errs <- fmt.Sprintf("goroutine %d: JoinedHashString = %x", g, got)
					return
				}
				if got := JoinedHashWithStack(gs, tags, "/"); got != wantJoined {
					errs <- fmt.Sprintf("goroutine %d: JoinedHashWithStack = %x", g, got)
					return
				}
				if got, want := CountTagString(tags, "tag-7"), count(tags, "tag-7"); got != want {
					errs <- fmt.Sprintf("goroutine %d: CountTagString = %d, want %d", g, got, want)
					return
				}
			}
		}(g)
	}
	wg.Wait()
	close(errs)
	for e := range errs {
		t.Error(e)
	}
}

var benchSink uint64

// The recursive digest, whose leaf helpers --target=go inlines into it, and
// the same in Go.
func BenchmarkDigest(b *testing.B) {
	for _, n := range []int{16, 1024} {
		tags := make([]string, n)
		for i := range tags {
			tags[i] = fmt.Sprintf("key-%d", i)
		}
		b.Run(fmt.Sprintf("goose/%d", n), func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				benchSink += TagsDigest(tags)
			}
		})
		b.Run(fmt.Sprintf("go/%d", n), func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				benchSink += digest(tags)
			}
		})
	}
}

// The recursive count of the neighbours in order, whose comparator
// --target=go inlines into it, and the same in Go.
func BenchmarkSortedPairs(b *testing.B) {
	for _, n := range []int{16, 1024} {
		tags := make([]string, n)
		for i := range tags {
			tags[i] = fmt.Sprintf("key-%06d", i*7919%n)
		}
		b.Run(fmt.Sprintf("goose/%d", n), func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				benchSink += uint64(SortedPairs(tags))
			}
		})
		b.Run(fmt.Sprintf("go/%d", n), func(b *testing.B) {
			for i := 0; i < b.N; i++ {
				benchSink += uint64(ordered(tags))
			}
		})
	}
}

func BenchmarkCountTag(b *testing.B) {
	tags := lists()["built"]
	for i := 0; i < b.N; i++ {
		benchSink += uint64(CountTagString(tags, "tag-11x"))
	}
}
