package goose

import (
	"testing"
	"unsafe"
)

// testPool is a reusable[] slice pool as the generated code keeps one
// (src/codegen_builtins.h, EmitSlicePool): elements of esz bytes on one
// data stack of gs, the freelist on another, and the code around the
// GsSpans* calls that grows the element array and fills in defaults.
type testPool struct {
	gs      *GsStackBlock
	elemStk int64
	flStk   int64
	elems   GsRhdr // the element array
	fl      GsRhdr // the freelist: its base, and its node count
	esz     int64
}

func newTestPool(gs *GsStackBlock, elemStk, flStk, esz int64) *testPool {
	return &testPool{gs: gs, elemStk: elemStk, flStk: flStk, esz: esz,
		elems: GsRhdr{Base: gs.Stks[elemStk]}, fl: GsRhdr{Base: gs.Stks[flStk]}}
}

func (p *testPool) elem(i int64) unsafe.Pointer { return unsafe.Add(p.elems.Base, i*p.esz) }

// placeRun is PlaceRun: past the end while nothing was ever freed.
func (p *testPool) placeRun(n int64) int64 {
	if p.fl.Len == 0 {
		return p.elems.Len
	}
	return GsSpansAlloc(p.fl.Base, &p.fl.Len, &p.gs.Stks[p.flStk], p.elems.Len, n)
}

// extend is EmitSliceExtend: the element array grown to end elements.
func (p *testPool) extend(end int64) {
	if ext := end - p.elems.Len; ext > 0 {
		p.elems.Len += ext
		p.gs.Stks[p.elemStk] = unsafe.Add(p.gs.Stks[p.elemStk], ext*p.esz)
	}
}

// fill writes v into every byte of count elements from first; 0 is the
// default value that EmitDefaultElems clears new elements to.
func (p *testPool) fill(first, count int64, v byte) {
	b := unsafe.Slice((*byte)(p.elem(first)), count*p.esz)
	for i := range b {
		b[i] = v
	}
}

func (p *testPool) get(i int64) int64    { return *(*int64)(p.elem(i)) }
func (p *testPool) set(i int64, v int64) { *(*int64)(p.elem(i)) = v }

// alloc is alloc_slice(n), returning the run's index.
func (p *testPool) alloc(n int64) int64 {
	i := p.placeRun(n)
	p.extend(i + n)
	p.fill(i, n, 0)
	return i
}

// free is free_slice of the n elements from i.
func (p *testPool) free(i, n int64) {
	GsSpansFree(p.fl.Base, &p.fl.Len, &p.gs.Stks[p.flStk], i, n)
}

// realloc is realloc_slice of the ol elements from i to n, returning where
// they start now.
func (p *testPool) realloc(i, ol, n int64) int64 {
	if n <= ol {
		GsSpansFree(p.fl.Base, &p.fl.Len, &p.gs.Stks[p.flStk], i+n, ol-n)
		return i
	}
	if ol == 0 {
		i = p.placeRun(n)
	} else if i+ol != p.elems.Len {
		ni := GsSpansRegrow(p.fl.Base, &p.fl.Len, &p.gs.Stks[p.flStk], p.elems.Len, i, ol, n)
		if ni != i {
			GsMemMove(p.elem(ni), p.elem(i), ol*p.esz)
		}
		i = ni
	}
	p.extend(i + n)
	p.fill(i+ol, n-ol, 0)
	return i
}

// head is the freelist's header, once a span has been freed.
func (p *testPool) head() *gsSpHead {
	f := gsSpans{base: p.fl.Base}
	return f.head()
}

func (p *testPool) i64s(i, n int64) []int64 {
	return GsSliceToSlice[int64](GsSlice{Data: p.elem(i), Len: n})
}

// checkSpans checks the structure of a pool's freelist, and returns its
// spans in index order: every node is in the tree or chained for reuse,
// once; every inner entry holds the first index and the largest count
// under it; leaves below the root are never empty, nor an inner root of
// fewer than two children; and the spans are ordered, none touching the
// next, and inside the pool.
func checkSpans(t testing.TB, p *testPool) (idx, cnt []int64) {
	t.Helper()
	n, base, top := p.fl.Len, p.fl.Base, p.gs.Stks[p.flStk]
	if n == 0 {
		if top != base {
			t.Fatalf("an empty freelist's top %p is not its base %p", top, base)
		}
		return nil, nil
	}
	if top != unsafe.Add(base, n*GsSpanNode) {
		t.Fatalf("the freelist's top is %d bytes past its base, want %d nodes'",
			uintptr(top)-uintptr(base), n)
	}
	f := gsSpans{base: base}
	hd := f.head()
	if hd.root <= 0 || hd.root >= n || hd.height < 0 || hd.height >= gsSpanDepth {
		t.Fatalf("header: root %d, height %d, of %d nodes", hd.root, hd.height, n)
	}
	seen := make([]bool, n)
	seen[0] = true
	for i := hd.chain; i != 0; i = f.leaf(i).link {
		if i <= 0 || i >= n || seen[i] {
			t.Fatalf("the chain of emptied nodes reaches node %d again or out of range", i)
		}
		seen[i] = true
	}
	var walk func(i, depth int64) (key, max int64)
	walk = func(i, depth int64) (int64, int64) {
		if i <= 0 || i >= n || seen[i] {
			t.Fatalf("node %d is out of range, chained, or reached twice", i)
		}
		seen[i] = true
		if depth == hd.height {
			lf := f.leaf(i)
			if lf.n < 0 || lf.n > gsSpanLeaf || (lf.n == 0 && depth > 0) {
				t.Fatalf("leaf %d holds %d spans", i, lf.n)
			}
			m := int64(0)
			for k := int64(0); k < lf.n; k++ {
				if lf.cnt[k] <= 0 {
					t.Fatalf("leaf %d: span (%d, %d)", i, lf.idx[k], lf.cnt[k])
				}
				if j := len(idx) - 1; j >= 0 && idx[j]+cnt[j] >= lf.idx[k] {
					t.Fatalf("span (%d, %d) is not before (%d, %d), apart", idx[j], cnt[j], lf.idx[k], lf.cnt[k])
				}
				idx, cnt = append(idx, lf.idx[k]), append(cnt, lf.cnt[k])
				if lf.cnt[k] > m {
					m = lf.cnt[k]
				}
			}
			return lf.idx[0], m
		}
		in := f.inner(i)
		if in.n < 1 || in.n > gsSpanFan || (depth == 0 && in.n < 2) {
			t.Fatalf("inner node %d at depth %d has %d children", i, depth, in.n)
		}
		m := int64(0)
		for k := int64(0); k < in.n; k++ {
			key, max := walk(in.child[k], depth+1)
			if in.key[k] != key || in.max[k] != max {
				t.Fatalf("entry %d of node %d holds key %d and largest count %d; its subtree's are %d and %d",
					k, i, in.key[k], in.max[k], key, max)
			}
			if max > m {
				m = max
			}
		}
		return in.key[0], m
	}
	walk(hd.root, 0)
	for i, s := range seen {
		if !s {
			t.Fatalf("node %d is neither in the tree nor chained", i)
		}
	}
	if k := len(idx) - 1; k >= 0 && idx[k]+cnt[k] > p.elems.Len {
		t.Fatalf("span (%d, %d) is past the pool's %d elements", idx[k], cnt[k], p.elems.Len)
	}
	return idx, cnt
}

func spansEqual(idx, cnt, starts, counts []int64) bool {
	if len(idx) != len(starts) {
		return false
	}
	for k := range idx {
		if idx[k] != starts[k] || cnt[k] != counts[k] {
			return false
		}
	}
	return true
}

// spanModel is the plain model of §5.4 that slice_pool_model.goose checks
// the C runtime with: the free spans in index order, and the pool's
// length. A run goes at the front of the first span that holds it, else at
// the end of the pool, starting with a free span that reaches the end;
// freed elements merge with the spans they touch; growth stays in place
// where a free span after the slice holds the difference or reaches the end.
type spanModel struct {
	starts, counts []int64
	size           int64
}

func (m *spanModel) insert(k int, at, n int64) {
	m.starts = append(m.starts, 0)
	m.counts = append(m.counts, 0)
	copy(m.starts[k+1:], m.starts[k:])
	copy(m.counts[k+1:], m.counts[k:])
	m.starts[k], m.counts[k] = at, n
}

func (m *spanModel) remove(k int) {
	m.starts = append(m.starts[:k], m.starts[k+1:]...)
	m.counts = append(m.counts[:k], m.counts[k+1:]...)
}

func (m *spanModel) take(k int, n int64) {
	if m.counts[k] <= n {
		m.remove(k)
	} else {
		m.starts[k] += n
		m.counts[k] -= n
	}
}

func (m *spanModel) alloc(n int64) int64 {
	for k := range m.starts {
		if m.counts[k] >= n {
			at := m.starts[k]
			m.take(k, n)
			return at
		}
	}
	at := m.size
	if last := len(m.starts) - 1; last >= 0 && m.starts[last]+m.counts[last] == m.size {
		at = m.starts[last]
		m.remove(last)
	}
	m.size = at + n
	return at
}

// upto is how many spans start at or before i.
func (m *spanModel) upto(i int64) int {
	k := 0
	for k < len(m.starts) && m.starts[k] <= i {
		k++
	}
	return k
}

func (m *spanModel) free(i, n int64) {
	if n == 0 {
		return
	}
	k := m.upto(i)
	prev := k > 0 && m.starts[k-1]+m.counts[k-1] == i
	next := k < len(m.starts) && m.starts[k] == i+n
	switch {
	case prev && next:
		m.counts[k-1] += n + m.counts[k]
		m.remove(k)
	case prev:
		m.counts[k-1] += n
	case next:
		m.starts[k] = i
		m.counts[k] += n
	default:
		m.insert(k, i, n)
	}
}

func (m *spanModel) realloc(i, ol, n int64) int64 {
	if n <= ol {
		m.free(i+n, ol-n)
		return i
	}
	if i+ol == m.size {
		m.size = i + n
		return i
	}
	k := m.upto(i+ol) - 1
	if k >= 0 && m.starts[k] == i+ol && (m.counts[k] >= n-ol || m.starts[k]+m.counts[k] == m.size) {
		if m.starts[k]+m.counts[k] == m.size && m.counts[k] < n-ol {
			m.size = i + n
		}
		m.take(k, n-ol)
		return i
	}
	m.free(i, ol)
	return m.alloc(n)
}

type poolRun struct{ at, n int64 }

// test/runtime/slice_pool_model.goose, step for step: short runs of a byte
// pool allocated, freed and resized many times over, as a cache of strings
// does, with free spans enough for a tree of several levels. Every
// placement is the model's, and the counts are those the C runtime prints
// (test/expected/slice_pool_model.out).
func TestSlicePoolModel(t *testing.T) {
	gs := NewStackBlock(2, 64<<20, 64<<10)
	defer gs.Close()
	pool := newTestPool(gs, 0, 1, 1)
	var model spanModel
	seed := lcg(99)
	length := func() int64 {
		switch r := seed.next(20); r {
		case 0:
			return 1 + seed.next(4)
		case 1:
			return 15 + seed.next(50)
		}
		return 5 + seed.next(10)
	}
	const slots = 2000
	runs := make([]poolRun, slots)
	tags := make([]byte, slots)
	for q := range runs {
		runs[q] = poolRun{pool.alloc(0), 0}
	}
	placed, wrong, deepest, height := 0, 0, 0, int64(0)
	for step := 0; step < 60000; step++ {
		q := seed.next(slots)
		s := runs[q]
		what := seed.next(10)
		switch {
		case s.n == 0:
			n := length()
			at := pool.alloc(n)
			placed++
			if at != model.alloc(n) {
				wrong++
			}
			tag := byte(1 + seed.next(250))
			pool.fill(at, n, tag)
			runs[q], tags[q] = poolRun{at, n}, tag
		case what < 4: // all of it back
			model.free(s.at, s.n)
			pool.free(s.at, s.n)
			runs[q] = poolRun{s.at, 0}
		case what < 5: // the front part back
			k := 1 + seed.next(s.n)
			model.free(s.at, k)
			pool.free(s.at, k)
			runs[q] = poolRun{s.at + k, s.n - k}
		case what < 6 && s.n >= 3: // a middle part back, then the back part, which merges with it
			a := 1 + seed.next(s.n-2)
			b := a + 1 + seed.next(s.n-a-1)
			model.free(s.at+a, b-a)
			pool.free(s.at+a, b-a)
			model.free(s.at+b, s.n-b)
			pool.free(s.at+b, s.n-b)
			runs[q] = poolRun{s.at, a}
		default: // resized: shrunk, grown by a little, or doubled
			ol := s.n
			var n int64
			switch r := seed.next(3); r {
			case 0:
				n = seed.next(ol)
			case 1:
				n = ol + 1 + seed.next(4)
			default:
				n = ol * 2
			}
			at := pool.realloc(s.at, ol, n)
			mat := model.realloc(s.at, ol, n)
			if n > 0 {
				placed++
				if at != mat {
					wrong++
				}
			}
			pool.fill(at, n, tags[q])
			runs[q] = poolRun{at, n}
		}
		if len(model.starts) > deepest {
			deepest = len(model.starts)
		}
		if step%7 == 0 || step < 2000 {
			idx, cnt := checkSpans(t, pool)
			if !spansEqual(idx, cnt, model.starts, model.counts) {
				t.Fatalf("step %d: free spans %v %v, the model's %v %v", step, idx, cnt, model.starts, model.counts)
			}
			if pool.fl.Len > 0 {
				height = max(height, pool.head().height)
			}
		}
	}
	if placed != 34932 || wrong != 0 {
		t.Errorf("placements %d, not where the model put them %d; the C runtime's are 34932 and 0", placed, wrong)
	}
	if pool.elems.Len != 21480 || model.size != 21480 || deepest != 656 {
		t.Errorf("pool %d model %d, at most %d free spans at once; the C runtime's are 21480, 21480 and 656",
			pool.elems.Len, model.size, deepest)
	}
	if height < 1 {
		t.Errorf("the freelist never grew past one leaf")
	}
	// Every run still holds its own byte.
	bad := 0
	for q, r := range runs {
		for _, v := range unsafe.Slice((*byte)(pool.elem(r.at)), r.n) {
			if v != tags[q] {
				bad++
			}
		}
	}
	if bad != 0 {
		t.Errorf("bytes overwritten: %d", bad)
	}
	// All of it back, from the last slot down: one span again.
	for q := slots - 1; q >= 0; q-- {
		pool.free(runs[q].at, runs[q].n)
	}
	if idx, cnt := checkSpans(t, pool); len(idx) != 1 || idx[0] != 0 || cnt[0] != pool.elems.Len {
		t.Fatalf("all freed: spans %v %v", idx, cnt)
	}
	if hd := pool.head(); hd.height != 0 {
		t.Errorf("a freelist of one span is %d levels high", hd.height)
	}
	length0 := pool.elems.Len
	if at := pool.alloc(length0); at != 0 || pool.elems.Len != length0 {
		t.Errorf("one span: %d, pool %d", at, pool.elems.Len)
	}
	if idx, _ := checkSpans(t, pool); len(idx) != 0 {
		t.Errorf("after taking the one span: %v", idx)
	}
}

// test/runtime/slice_pool_many_spans.goose, step for step: lists grown an
// element at a time in a scattered order leave a free span behind most
// moves, and whole lists and parts of lists are freed and new runs placed
// among thousands of free spans. The counts and checksum are those the C
// runtime prints (test/expected/slice_pool_many_spans.out).
func TestSlicePoolManySpans(t *testing.T) {
	gs := NewStackBlock(2, 64<<20, 64<<10)
	defer gs.Close()
	pool := newTestPool(gs, 0, 1, 8)
	seed := lcg(12345)
	const n = 3000
	lists := make([]poolRun, n)
	for i := range lists {
		lists[i] = poolRun{pool.alloc(0), 0}
	}
	// Each element holds its list's number and its place in the list.
	for step := 0; step < n*20; step++ {
		i := seed.next(n)
		l := lists[i]
		at := pool.realloc(l.at, l.n, l.n+1)
		lists[i] = poolRun{at, l.n + 1}
		pool.set(at+l.n, i*1000+l.n)
	}
	spans := 0
	for _, l := range lists {
		if l.n > 0 {
			spans++
		}
	}
	if pool.elems.Len != 67452 || spans != 3000 {
		t.Errorf("grown: pool %d, lists holding elements %d; the C runtime's are 67452 and 3000", pool.elems.Len, spans)
	}
	checkSpans(t, pool)
	// Every third list goes back whole, the front half of every fifth and
	// the back of every seventh.
	for i := int64(0); i < n; i++ {
		s := lists[i]
		switch {
		case i%3 == 0:
			pool.free(s.at, s.n)
			lists[i] = poolRun{s.at, 0}
		case i%5 == 0 && s.n >= 2:
			pool.free(s.at, s.n/2)
			lists[i] = poolRun{s.at + s.n/2, s.n - s.n/2}
		case i%7 == 0 && s.n >= 2:
			lists[i] = poolRun{pool.realloc(s.at, s.n, s.n-s.n/3), s.n - s.n/3}
		}
	}
	idx, _ := checkSpans(t, pool)
	if len(idx) < 1000 {
		t.Errorf("only %d free spans at once", len(idx))
	}
	// New runs of every size from 1 to 40 land in the first span that holds
	// them, and some lists grow again.
	placed := int64(0)
	var extra []poolRun
	for k := 0; k < 4000; k++ {
		cnt := 1 + seed.next(40)
		at := pool.alloc(cnt)
		for j := int64(0); j < cnt; j++ {
			pool.set(at+j, -1)
		}
		placed = (placed*31 + at) % 1000000007
		extra = append(extra, poolRun{at, cnt})
		if i := seed.next(n); i%3 != 0 {
			l := lists[i]
			first := int64(0)
			if l.n > 0 {
				first = pool.get(l.at) % 1000
			}
			at := pool.realloc(l.at, l.n, l.n+1)
			lists[i] = poolRun{at, l.n + 1}
			pool.set(at+l.n, i*1000+first+l.n)
		}
	}
	if pool.elems.Len != 123505 || placed != 103237829 {
		t.Errorf("placed: pool %d, checksum %d; the C runtime's are 123505 and 103237829", pool.elems.Len, placed)
	}
	checkSpans(t, pool)
	// Every list still holds exactly its own elements, in order.
	bad := 0
	for i, l := range lists {
		s := pool.i64s(l.at, l.n)
		for j := range s {
			if s[j]/1000 != int64(i) || (j > 0 && s[j] != s[j-1]+1) {
				bad++
			}
		}
	}
	for _, e := range extra {
		for _, v := range pool.i64s(e.at, e.n) {
			if v != -1 {
				bad++
			}
		}
	}
	if bad != 0 {
		t.Errorf("elements out of place: %d", bad)
	}
	// All of it back, in a scattered order: the freed runs merge into one span.
	for i := 0; i < n; i++ {
		l := lists[(i*1777)%n]
		pool.free(l.at, l.n)
	}
	for _, e := range extra {
		pool.free(e.at, e.n)
	}
	if at := pool.alloc(pool.elems.Len); at != 0 {
		t.Errorf("one span: %d", at)
	}
	checkSpans(t, pool)
}

func i64sEqual(xs []int64, want ...int64) bool {
	if len(xs) != len(want) {
		return false
	}
	for i := range xs {
		if xs[i] != want[i] {
			return false
		}
	}
	return true
}

// The scenarios of test/runtime/slice_pool.goose, each placement as the
// comments there, and test/expected/slice_pool.out, give it.
func TestSlicePoolScenarios(t *testing.T) {
	gs := NewStackBlock(2, 1<<20, 64<<10)
	defer gs.Close()
	newPool := func() *testPool {
		gs.Reset()
		return newTestPool(gs, 0, 1, 8)
	}
	fill := func(p *testPool, at, n, from int64) {
		for i := int64(0); i < n; i++ {
			p.set(at+i, from+i)
		}
	}
	expect := func(t *testing.T, what string, got, want []int64) {
		t.Helper()
		if !i64sEqual(got, want...) {
			t.Errorf("%s: %v, want %v", what, got, want)
		}
	}

	t.Run("first fit", func(t *testing.T) {
		p := newPool()
		a, _, c, _, e, f, _ := p.alloc(4), p.alloc(1), p.alloc(2), p.alloc(1), p.alloc(3), p.alloc(2), p.alloc(1)
		p.free(a, 4)
		p.free(c, 2)
		p.free(e, 3)
		p.free(f, 2) // (0, 4) (5, 2) (8, 5)
		h, k, m, n, o := p.alloc(2), p.alloc(3), p.alloc(5), p.alloc(1), p.alloc(2)
		expect(t, "first fit", []int64{h, k, m, n, o, p.elems.Len}, []int64{0, 8, 14, 2, 5, 19})
		checkSpans(t, p)
	})

	t.Run("merging", func(t *testing.T) {
		p := newPool()
		var s [6]int64
		for i := range s[:5] {
			s[i] = p.alloc(2)
		}
		s[5] = p.alloc(1)
		for _, i := range []int{0, 1, 4, 3, 2} {
			p.free(s[i], 2)
		}
		if idx, cnt := checkSpans(t, p); !i64sEqual(idx, 0) || !i64sEqual(cnt, 10) {
			t.Errorf("merged spans %v %v, want (0, 10)", idx, cnt)
		}
		expect(t, "merging", []int64{p.alloc(10), p.elems.Len}, []int64{0, 11})
	})

	t.Run("tail and dangling", func(t *testing.T) {
		p := newPool()
		a, b := p.alloc(3), p.alloc(4)
		fill(p, a, 3, 1)
		fill(p, b, 4, 100)
		p.free(b, 4) // (3, 4) reaches the end
		c := p.alloc(6)
		expect(t, "tail", []int64{c, p.elems.Len}, []int64{3, 9})
		expect(t, "tail c", p.i64s(c, 6), []int64{0, 0, 0, 0, 0, 0})
		expect(t, "tail b", p.i64s(b, 4), []int64{0, 0, 0, 0})
		p.set(c+1, 42)
		expect(t, "dangling", append([]int64{p.get(b + 1)}, p.i64s(a, 3)...), []int64{42, 1, 2, 3})
	})

	t.Run("growth", func(t *testing.T) {
		p := newPool()
		a := p.alloc(2)
		fill(p, a, 2, 1)
		a = p.realloc(a, 2, 4) // last in the array
		expect(t, "a", append([]int64{a, p.elems.Len}, p.i64s(a, 4)...), []int64{0, 4, 1, 2, 0, 0})
		b := p.alloc(3)
		fill(p, b, 3, 10)
		c := p.alloc(2)
		fill(p, c, 2, 20)
		p.free(c, 2)           // (7, 2)
		b = p.realloc(b, 3, 4) // into the free span after it, leaving (8, 1)
		expect(t, "b into a span", append([]int64{b, p.elems.Len}, p.i64s(b, 4)...), []int64{4, 9, 10, 11, 12, 0})
		b = p.realloc(b, 4, 7) // (8, 1) ends the array: it and 2 more
		expect(t, "b to the end", append([]int64{b, p.elems.Len}, p.i64s(b, 7)...), []int64{4, 11, 10, 11, 12, 0, 0, 0, 0})
		a = p.realloc(a, 4, 5) // b follows a, and nothing free holds 5: the end
		expect(t, "a moved", append([]int64{a, p.elems.Len}, p.i64s(a, 5)...), []int64{11, 16, 1, 2, 0, 0, 0})
		d := p.alloc(4) // where a was
		expect(t, "d", append([]int64{d, p.elems.Len}, p.i64s(d, 4)...), []int64{0, 16, 0, 0, 0, 0})
		b = p.realloc(b, 7, 2) // frees (6, 5)
		e := p.alloc(5)
		expect(t, "b shrunk", append([]int64{b, e, p.elems.Len}, p.i64s(b, 2)...), []int64{4, 6, 16, 10, 11})
		none := p.alloc(0) // takes nothing
		p.free(0, 0)       // and gives nothing back
		p.realloc(e, 5, 0) // all of e back: (6, 5)
		f := p.alloc(5)
		expect(t, "none", []int64{none, f, p.elems.Len}, []int64{16, 6, 16})
		tailing := p.realloc(0, 0, 2) // empty: placed like a new run, at the end
		expect(t, "tailing", []int64{tailing, p.elems.Len}, []int64{16, 18})
		// A slice moved past a free span at the end absorbs it.
		p.alloc(3)
		h := p.alloc(3)
		p.free(h, 3)                // (21, 3) reaches the end
		moved := p.realloc(d, 4, 5) // d is followed by b: moved into (21, 3) and 2 more
		expect(t, "moved", []int64{moved, p.elems.Len}, []int64{21, 26})
		checkSpans(t, p)
	})

	// A slice grows in place into a free span that ends the array, though
	// the span holds less than the growth and an earlier one holds the
	// whole grown run (gs_spans_regrow's "or reaches the end").
	t.Run("growth to the end", func(t *testing.T) {
		p := newPool()
		a, b, c := p.alloc(10), p.alloc(2), p.alloc(3)
		fill(p, b, 2, 7)
		p.free(a, 10) // (0, 10), which would hold 8
		p.free(c, 3)  // (12, 3) reaches the end
		b = p.realloc(b, 2, 8)
		expect(t, "b", append([]int64{b, p.elems.Len}, p.i64s(b, 8)...), []int64{10, 18, 7, 8, 0, 0, 0, 0, 0, 0})
		if idx, cnt := checkSpans(t, p); !i64sEqual(idx, 0) || !i64sEqual(cnt, 10) {
			t.Errorf("spans %v %v, want (0, 10) alone", idx, cnt)
		}
	})

	t.Run("moves", func(t *testing.T) {
		p := newPool()
		a, _, c, _, e, _ := p.alloc(3), p.alloc(1), p.alloc(8), p.alloc(1), p.alloc(2), p.alloc(1)
		fill(p, e, 2, 40)
		p.free(a, 3)
		p.free(c, 8)             // (0, 3) (4, 8)
		e2 := p.realloc(e, 2, 6) // f follows e: into (4, 8), not the end
		expect(t, "e2", append([]int64{e2, p.elems.Len}, p.i64s(e2, 6)...), []int64{4, 16, 40, 41, 0, 0, 0, 0})
		checkSpans(t, p)

		p2 := newPool()
		x, y, _ := p2.alloc(2), p2.alloc(4), p2.alloc(1)
		fill(p2, y, 4, 60)
		p2.free(x, 2)             // (0, 2)
		y2 := p2.realloc(y, 4, 6) // z follows y: y's place and (0, 2) hold it, moving over itself
		expect(t, "y2", append([]int64{y2, p2.elems.Len}, p2.i64s(y2, 6)...), []int64{0, 7, 60, 61, 62, 63, 0, 0})
		checkSpans(t, p2)
	})
}

func TestSpansEdges(t *testing.T) {
	gs := NewStackBlock(2, 1<<20, 64<<10)
	defer gs.Close()
	p := newTestPool(gs, 0, 1, 8)
	top := &gs.Stks[p.flStk]
	// Nothing freed yet: every run goes at the end, and an empty run frees
	// nothing, leaving the freelist without even its header.
	if at := GsSpansAlloc(p.fl.Base, &p.fl.Len, top, 7, 3); at != 7 {
		t.Errorf("from an empty freelist: %d, want the end", at)
	}
	GsSpansFree(p.fl.Base, &p.fl.Len, top, 3, 0)
	GsSpansFree(p.fl.Base, &p.fl.Len, top, 3, -1)
	if p.fl.Len != 0 || *top != p.fl.Base {
		t.Fatalf("freeing nothing made a freelist of %d nodes", p.fl.Len)
	}
	p.extend(10)
	// The first span freed lays out the header and a root leaf.
	GsSpansFree(p.fl.Base, &p.fl.Len, top, 2, 3)
	if p.fl.Len != 2 || *top != unsafe.Add(p.fl.Base, 2*GsSpanNode) {
		t.Fatalf("a freelist of one span has %d nodes", p.fl.Len)
	}
	if at := GsSpansAlloc(p.fl.Base, &p.fl.Len, top, 10, 0); at != 10 {
		t.Errorf("an empty run: %d, want the end", at)
	}
	if at := GsSpansAlloc(p.fl.Base, &p.fl.Len, top, 10, 4); at != 10 {
		t.Errorf("a run no span holds, with none at the end: %d, want the end", at)
	}
	if at := GsSpansAlloc(p.fl.Base, &p.fl.Len, top, 10, 3); at != 2 {
		t.Errorf("a run a span holds exactly: %d, want 2", at)
	}
	if idx, _ := checkSpans(t, p); len(idx) != 0 {
		t.Fatalf("spans %v left", idx)
	}
	// A freelist that empties keeps its nodes, and an empty root leaf.
	if p.fl.Len != 2 {
		t.Fatalf("%d nodes", p.fl.Len)
	}
	// Regrowing while nothing was ever freed: the slice goes back on the
	// freelist, and is placed again as a new run.
	p2 := newTestPool(NewStackBlock(2, 1<<20, 64<<10), 0, 1, 8)
	defer p2.gs.Close()
	p2.extend(6)
	if at := GsSpansRegrow(p2.fl.Base, &p2.fl.Len, &p2.gs.Stks[1], 6, 0, 2, 5); at != 6 {
		t.Errorf("regrow from an empty freelist: %d, want the end, the slice freed", at)
	}
	if idx, cnt := checkSpans(t, p2); !i64sEqual(idx, 0) || !i64sEqual(cnt, 2) {
		t.Errorf("spans %v %v, want (0, 2)", idx, cnt)
	}
}

// Thousands of spans make a tree several levels high, whose leaves the
// regrowth and freeing of one run at a time reach across: into a span
// that starts the next leaf, and merging the last span of one leaf with
// the first of the next. Freeing everything collapses it to one leaf.
func TestSpansDeepTree(t *testing.T) {
	gs := NewStackBlock(2, 64<<20, 64<<10)
	defer gs.Close()
	p := newTestPool(gs, 0, 1, 1)
	const n = 20000
	for k := 0; k < 4*n; k++ {
		p.alloc(1)
	}
	// Free [4k, 4k + 2) for every k: n spans of two, with allocated pairs
	// between them.
	free := make([]bool, 4*n)
	for k := int64(0); k < n; k++ {
		p.free(4*k, 2)
		free[4*k], free[4*k+1] = true, true
	}
	expectFree := func(what string) {
		t.Helper()
		idx, cnt := checkSpans(t, p)
		var starts, counts []int64
		for i := 0; i < len(free); i++ {
			if free[i] && (i == 0 || !free[i-1]) {
				starts, counts = append(starts, int64(i)), append(counts, 0)
			}
			if free[i] {
				counts[len(counts)-1]++
			}
		}
		if !spansEqual(idx, cnt, starts, counts) {
			t.Fatalf("%s: %d spans, want %d", what, len(idx), len(starts))
		}
	}
	expectFree("every other pair freed")
	if h := p.head().height; h < 2 {
		t.Fatalf("%d spans make a tree of height %d", n, h)
	}
	// Grow the run [4k + 2, 4k + 4) of every odd k by one, into the span
	// after it, whose front it takes, in this leaf or the next.
	for k := int64(1); k < n-1; k += 2 {
		at := 4*k + 2
		if got := p.realloc(at, 2, 3); got != at {
			t.Fatalf("regrowing [%d, %d): moved to %d", at, at+2, got)
		}
		free[at+2] = false
	}
	expectFree("odd runs grown in place")
	// Free every allocated element in a scattered order: each merges the
	// spans on both sides, across leaves where they are in two.
	var order []int64
	for i := range free {
		if !free[i] {
			order = append(order, int64(i))
		}
	}
	seed := lcg(7)
	for j := len(order) - 1; j > 0; j-- {
		k := seed.next(int64(j + 1))
		order[j], order[k] = order[k], order[j]
	}
	for j, i := range order {
		p.free(i, 1)
		free[i] = true
		if j%2500 == 0 {
			expectFree("part freed")
		}
	}
	expectFree("all freed")
	f := gsSpans{base: p.fl.Base}
	if hd := f.head(); hd.height != 0 || f.leaf(hd.root).n != 1 {
		t.Fatalf("one span in a tree of height %d", hd.height)
	}
	if at := p.alloc(4 * n); at != 0 || p.elems.Len != 4*n {
		t.Fatalf("the whole pool as one run: at %d of %d", at, p.elems.Len)
	}
}
