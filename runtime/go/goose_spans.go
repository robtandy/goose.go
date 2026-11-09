package goose

import (
	"math"
	"syscall"
	"unsafe"
)

// Slice pools (goose_spec.md §5.4, reusable[]), ported from the gs_spans_*
// functions of src/runtime/runtime_impl.h. A pool's freelist holds its
// free (index, count) spans, no two of them touching, in a B+ tree ordered
// by index: leaves hold spans, and every inner entry holds the first index
// and the largest count in the subtree under it, which is what lets first
// fit, the span at or before an index, and the last span each be found by
// one descent.
//
// The tree lives on a data stack of its own, in nodes of GsSpanNode bytes
// numbered from the freelist's base: node 0 is the header, and the others
// come off a chain of emptied nodes or past the last one, so the node count
// is the freelist's length, and the tree, in node numbers, can be copied
// whole. A freelist starts empty, without a header, until a span is freed.
// Every span comes from a slice of the pool, so spans stay inside the
// pool's length whatever the program frees: freeing a slice twice leaves
// overlapping spans that hand the same elements out twice, never anything
// outside the pool.
//
// The generated code keeps the element array's length and top and fills
// in elements; these functions keep only the spans. Each takes the
// freelist's base, a pointer to its node count (its header's Len) and a
// pointer to its stack's top, which stays past the last node: the stack's
// slot in GsStackBlock.Stks, or a local the generated code caches it in.
// Indices and counts are in elements.

// GsSpanNode is the size of a freelist node in bytes (GS_SPAN_NODE).
const GsSpanNode = 512

const (
	gsSpanLeaf = (GsSpanNode - 16) / 16 // spans in a leaf
	gsSpanFan  = (GsSpanNode - 16) / 24 // children of an inner node
	// A full node splits into halves and an empty one leaves its parent,
	// so every level a tree gains takes about gsSpanFan / 2 times the
	// insertions of the one before: no freelist gets this deep.
	gsSpanDepth = 24
)

// The nodes as they lie on the stack; link chains emptied nodes.
type gsSpLeaf struct {
	n, link int64
	idx     [gsSpanLeaf]int64
	cnt     [gsSpanLeaf]int64
}

type gsSpInner struct {
	n, link int64
	key     [gsSpanFan]int64
	max     [gsSpanFan]int64
	child   [gsSpanFan]int64
}

type gsSpHead struct {
	root, height, chain int64
}

// gsSpans is a freelist as the functions are handed it.
type gsSpans struct {
	base unsafe.Pointer
	n    *int64
	top  *unsafe.Pointer
}

// gsSpPath is a path down a freelist: the node and the entry taken at
// each level, the leaf's last.
type gsSpPath struct {
	h    int64
	node [gsSpanDepth]int64
	pos  [gsSpanDepth]int64
}

func (f *gsSpans) head() *gsSpHead { return (*gsSpHead)(f.base) }

func (f *gsSpans) leaf(i int64) *gsSpLeaf {
	return (*gsSpLeaf)(unsafe.Add(f.base, i*GsSpanNode))
}

func (f *gsSpans) inner(i int64) *gsSpInner {
	return (*gsSpInner)(unsafe.Add(f.base, i*GsSpanNode))
}

func (f *gsSpans) newNode() int64 {
	hd := f.head()
	if i := hd.chain; i != 0 {
		hd.chain = f.leaf(i).link
		return i
	}
	*f.top = unsafe.Add(*f.top, GsSpanNode)
	i := *f.n
	*f.n = i + 1
	return i
}

func (f *gsSpans) dropNode(i int64) {
	hd := f.head()
	f.leaf(i).link = hd.chain
	hd.chain = i
}

// gsSpOpen moves a leaf's entries from position s on up one, making room
// at s; gsSpClose moves those after s down onto it. Loops over the arrays
// together rather than copies of each: a node holds a few entries, and the
// calls cost more than moving them.
func gsSpOpen(lf *gsSpLeaf, s int64) {
	for j := lf.n; j > s; j-- {
		lf.idx[j] = lf.idx[j-1]
		lf.cnt[j] = lf.cnt[j-1]
	}
	lf.n++
}

func gsSpClose(lf *gsSpLeaf, s int64) {
	lf.n--
	for j := s; j < lf.n; j++ {
		lf.idx[j] = lf.idx[j+1]
		lf.cnt[j] = lf.cnt[j+1]
	}
}

func gsSpOpenIn(in *gsSpInner, s int64) {
	for j := in.n; j > s; j-- {
		in.key[j] = in.key[j-1]
		in.max[j] = in.max[j-1]
		in.child[j] = in.child[j-1]
	}
	in.n++
}

func gsSpCloseIn(in *gsSpInner, s int64) {
	in.n--
	for j := s; j < in.n; j++ {
		in.key[j] = in.key[j+1]
		in.max[j] = in.max[j+1]
		in.child[j] = in.child[j+1]
	}
}

// gsSpMost is the largest of the counts v, or stop as soon as one reaches
// it.
func gsSpMost(v []int64, stop int64) int64 {
	m := int64(0)
	for k := 0; k < len(v) && m < stop; k++ {
		if v[k] > m {
			m = v[k]
		}
	}
	return m
}

// max is the largest count under node i, a leaf or an inner node, or stop
// as soon as one reaches it.
func (f *gsSpans) max(i int64, leaf bool, stop int64) int64 {
	if leaf {
		lf := f.leaf(i)
		return gsSpMost(lf.cnt[:lf.n], stop)
	}
	in := f.inner(i)
	return gsSpMost(in.max[:in.n], stop)
}

func (f *gsSpans) key(i int64, leaf bool) int64 {
	if leaf {
		return f.leaf(i).idx[0]
	}
	return f.inner(i).key[0]
}

// fix brings the entries above level l of the path (the leaf being level
// h) up to date with the node below each, after one count in the node at
// level l went from was to now (0 for an entry added or removed), stopping
// at the first entry that is unchanged. A node's largest count is rescanned
// only where the count that held it shrank, and only until another as large
// turns up; a was of math.MaxInt64 rescans the node at level l whole,
// whatever changed in it.
func (f *gsSpans) fix(p *gsSpPath, l, was, now int64) {
	for ; l > 0; l-- {
		in := f.inner(p.node[l-1])
		k, c := p.pos[l-1], p.node[l]
		m := in.max[k]
		leaf := l == p.h
		key := f.key(c, leaf)
		var nm int64
		switch {
		case now >= m:
			nm = now
		case was < m:
			nm = m
		default:
			nm = f.max(c, leaf, was)
		}
		if in.key[k] == key && nm == m {
			return
		}
		in.key[k] = key
		in.max[k] = nm
		was = m
		now = nm
	}
}

// find descends to the leaf holding the last span that starts at or before
// idx, and returns its position there, or -1 where no span does.
func (f *gsSpans) find(p *gsSpPath, idx int64) int64 {
	hd := f.head()
	i := hd.root
	p.h = hd.height
	for l := int64(0); l < p.h; l++ {
		in := f.inner(i)
		k := int64(1)
		for k < in.n && in.key[k] <= idx {
			k++
		}
		p.node[l] = i
		p.pos[l] = k - 1
		i = in.child[k-1]
	}
	p.node[p.h] = i
	lf := f.leaf(i)
	k := int64(0)
	for k < lf.n && lf.idx[k] <= idx {
		k++
	}
	p.pos[p.h] = k - 1
	return k - 1
}

// newRoot puts a node full at the root, split into a and b, under a new
// root of the two halves.
func (f *gsSpans) newRoot(a, b int64, leaf bool) {
	r := f.newNode()
	rn := f.inner(r)
	rn.n = 2
	rn.key[0] = f.key(a, leaf)
	rn.max[0] = f.max(a, leaf, math.MaxInt64)
	rn.child[0] = a
	rn.key[1] = f.key(b, leaf)
	rn.max[1] = f.max(b, leaf, math.MaxInt64)
	rn.child[1] = b
	hd := f.head()
	hd.root = r
	hd.height++
}

// addChild puts the entry for node child at position k of the inner node
// at level l of the path, splitting full nodes on the way up.
func (f *gsSpans) addChild(p *gsSpPath, l, k, child int64, leaf bool) {
	for {
		in := f.inner(p.node[l])
		o := in
		key, max := f.key(child, leaf), f.max(child, leaf, math.MaxInt64)
		s, nn := k, int64(0)
		if in.n == gsSpanFan {
			const half = gsSpanFan / 2
			nn = f.newNode()
			r := f.inner(nn)
			r.n = in.n - half
			copy(r.key[:r.n], in.key[half:])
			copy(r.max[:r.n], in.max[half:])
			copy(r.child[:r.n], in.child[half:])
			in.n = half
			if k > half {
				o = r
				s = k - half
			}
		}
		gsSpOpenIn(o, s)
		o.key[s] = key
		o.max[s] = max
		o.child[s] = child
		if nn == 0 {
			f.fix(p, l, 0, max)
			return
		}
		if l == 0 {
			f.newRoot(p.node[0], nn, false)
			return
		}
		// The node keeps its place above, and its new right half goes
		// after it.
		f.fix(p, l, math.MaxInt64, 0)
		child = nn
		leaf = false
		k = p.pos[l-1] + 1
		l--
	}
}

// insert puts span (idx, cnt) at position k of the path's leaf.
func (f *gsSpans) insert(p *gsSpPath, k, idx, cnt int64) {
	lf := f.leaf(p.node[p.h])
	o := lf
	s, nn := k, int64(0)
	if lf.n == gsSpanLeaf {
		const half = gsSpanLeaf / 2
		nn = f.newNode()
		r := f.leaf(nn)
		r.n = lf.n - half
		copy(r.idx[:r.n], lf.idx[half:])
		copy(r.cnt[:r.n], lf.cnt[half:])
		lf.n = half
		if k > half {
			o = r
			s = k - half
		}
	}
	gsSpOpen(o, s)
	o.idx[s] = idx
	o.cnt[s] = cnt
	switch {
	case nn == 0:
		f.fix(p, p.h, 0, cnt)
	case p.h == 0:
		f.newRoot(p.node[0], nn, true)
	default:
		f.fix(p, p.h, math.MaxInt64, 0)
		f.addChild(p, p.h-1, p.pos[p.h-1]+1, nn, true)
	}
}

// delete removes the span at position k of the path's leaf. An emptied
// node leaves its parent, and a root left with one child hands the root
// down.
func (f *gsSpans) delete(p *gsSpPath, k int64) {
	lf := f.leaf(p.node[p.h])
	c := lf.cnt[k]
	gsSpClose(lf, k)
	if lf.n > 0 || p.h == 0 {
		f.fix(p, p.h, c, 0)
		return
	}
	f.dropNode(p.node[p.h])
	for l := p.h - 1; ; l-- {
		in := f.inner(p.node[l])
		e := p.pos[l]
		m := in.max[e]
		gsSpCloseIn(in, e)
		if in.n > 0 {
			f.fix(p, l, m, 0)
			break
		}
		// Never the root: a root of one child is handed down below, so
		// every root has two or more.
		f.dropNode(p.node[l])
	}
	hd := f.head()
	for hd.height > 0 && f.inner(hd.root).n == 1 {
		r := hd.root
		hd.root = f.inner(r).child[0]
		hd.height--
		f.dropNode(r)
	}
}

// take takes cnt elements off the front of the span at position k of the
// path's leaf, or the whole span where it holds no more.
func (f *gsSpans) take(p *gsSpPath, k, cnt int64) {
	lf := f.leaf(p.node[p.h])
	c := lf.cnt[k]
	if c <= cnt {
		f.delete(p, k)
		return
	}
	lf.idx[k] += cnt
	lf.cnt[k] = c - cnt
	f.fix(p, p.h, c, c-cnt)
}

// GsSpansAlloc is where a run of cnt elements goes in a pool of length
// elements, taken off the freelist (gs_spans_alloc): for alloc_slice, and
// for a slice realloc_slice moves. That is the front of the first free span
// that holds cnt, in index order, else the end of the array, where a free
// span that reaches the end starts the run. The caller grows the array to
// the returned index plus cnt. First fit rather than best: in index order
// it packs runs toward the start and leaves the end free.
func GsSpansAlloc(base unsafe.Pointer, n *int64, top *unsafe.Pointer, length, cnt int64) int64 {
	if cnt == 0 || *n == 0 {
		return length
	}
	f := gsSpans{base, n, top}
	var p gsSpPath
	var before [gsSpanDepth]int64
	var lf *gsSpLeaf
	var l, k, m int64
	hd := f.head()
	p.h = hd.height
	i := hd.root
	// Down the first entry at each level whose largest count holds cnt,
	// noting the largest count of the ones before it, all smaller than
	// cnt, for bringing the entries up to date once the span is taken
	// from. Where the root holds none, down to the last span.
	for l = 0; l < p.h; l++ {
		in := f.inner(i)
		for k, m = 0, 0; k < in.n && in.max[k] < cnt; k++ {
			if in.max[k] > m {
				m = in.max[k]
			}
		}
		if k == in.n {
			break
		}
		p.node[l] = i
		p.pos[l] = k
		before[l] = m
		i = in.child[k]
	}
	if l == p.h {
		lf = f.leaf(i)
		for k, m = 0, 0; k < lf.n && lf.cnt[k] < cnt; k++ {
			if lf.cnt[k] > m {
				m = lf.cnt[k]
			}
		}
		before[l] = m
	}
	if l < p.h || k == lf.n {
		for l, i = 0, hd.root; l < p.h; l++ {
			in := f.inner(i)
			p.node[l] = i
			p.pos[l] = in.n - 1
			i = in.child[in.n-1]
		}
		p.node[p.h] = i
		lf = f.leaf(i)
		k = lf.n - 1
		if k < 0 || lf.idx[k]+lf.cnt[k] != length {
			return length
		}
		idx := lf.idx[k]
		f.take(&p, k, cnt)
		return idx
	}
	p.node[p.h] = i
	p.pos[p.h] = k
	idx, c := lf.idx[k], lf.cnt[k]
	var now, j int64
	switch {
	case c > cnt:
		now = c - cnt
		lf.idx[k] = idx + cnt
		lf.cnt[k] = now
		j = k + 1
	case lf.n > 1 || p.h == 0:
		gsSpClose(lf, k)
		now = 0
		j = k
	default:
		f.delete(&p, k)
		return idx
	}
	// As fix, with what the descent noted: the count on the path at level
	// l went from c to now, and the node's other entries are the ones
	// before, smaller than cnt, and those from j on, which need scanning
	// only where c was the node's largest.
	for l = p.h; l > 0; l-- {
		in := f.inner(p.node[l-1])
		e := p.pos[l-1]
		mx := in.max[e]
		nm := mx
		key := f.key(p.node[l], l == p.h)
		if c == mx {
			var v []int64
			if l == p.h {
				v = lf.cnt[:lf.n]
			} else {
				below := f.inner(p.node[l])
				v = below.max[:below.n]
			}
			nm = before[l]
			if now > nm {
				nm = now
			}
			for ; j < int64(len(v)) && nm < c; j++ {
				if v[j] > nm {
					nm = v[j]
				}
			}
		}
		if in.key[e] == key && nm == mx {
			break
		}
		in.key[e] = key
		in.max[e] = nm
		c = mx
		now = nm
		j = e + 1
	}
	return idx
}

// gsSpNear is the spans around a run [idx, end): the leaf holding the last
// span that starts at or before idx (at position k, or -1), and the span
// after that one, where it follows in the same leaf, or where it starts
// the next leaf and at end (nl and nk, nl nil where there is none). Where
// that span is in the next leaf, the path to it is the other path near
// fills (inQ).
type gsSpNear struct {
	lf, nl *gsSpLeaf
	k, nk  int64
	inQ    bool
}

// near finds the spans around the run [idx, end), with the path to the one
// before it in p. A next leaf's first index is the key of the entry that
// leads to it, so a path to it, in q, is only made where its first span
// touches the run.
func (f *gsSpans) near(p, q *gsSpPath, idx, end int64) gsSpNear {
	var s gsSpNear
	s.k = f.find(p, idx)
	s.lf = f.leaf(p.node[p.h])
	s.nk = -1
	if s.k+1 < s.lf.n {
		s.nl = s.lf
		s.nk = s.k + 1
		return s
	}
	l := p.h - 1
	for l >= 0 && p.pos[l]+1 >= f.inner(p.node[l]).n {
		l--
	}
	if l < 0 || f.inner(p.node[l]).key[p.pos[l]+1] != end {
		return s
	}
	q.h = p.h
	copy(q.node[:l+1], p.node[:l+1])
	copy(q.pos[:l+1], p.pos[:l+1])
	for q.pos[l]++; l < q.h; l++ {
		q.node[l+1] = f.inner(q.node[l]).child[q.pos[l]]
		q.pos[l+1] = 0
	}
	s.inQ = true
	s.nl = f.leaf(q.node[q.h])
	s.nk = 0
	return s
}

// release puts [idx, idx + cnt) back on the freelist beside the spans near
// found around it, merged with the ones it touches.
func (f *gsSpans) release(p, q *gsSpPath, s *gsSpNear, idx, cnt int64) {
	lf, nl := s.lf, s.nl
	k, nk := s.k, s.nk
	np := p
	if s.inQ {
		np = q
	}
	prev := k >= 0 && lf.idx[k]+lf.cnt[k] == idx
	next := nl != nil && idx+cnt == nl.idx[nk]
	switch {
	case prev:
		c := lf.cnt[k]
		m := c + cnt
		if next {
			m += nl.cnt[nk]
		}
		lf.cnt[k] = m
		// The merged count is larger than both it replaces, so one fix
		// covers the span after leaving the leaf as well.
		if next && nl == lf {
			gsSpClose(lf, nk)
		}
		f.fix(p, p.h, c, m)
		if next && nl != lf {
			f.delete(np, nk)
		}
	case next:
		c := nl.cnt[nk]
		nl.idx[nk] = idx
		nl.cnt[nk] = c + cnt
		f.fix(np, np.h, c, c+cnt)
	default:
		f.insert(p, k+1, idx, cnt)
	}
}

// GsSpansFree puts [idx, idx + cnt) back on the freelist, merged with the
// spans it touches (gs_spans_free): free_slice, and what realloc_slice lets
// go of. The first span freed lays out the freelist's header.
func GsSpansFree(base unsafe.Pointer, n *int64, top *unsafe.Pointer, idx, cnt int64) {
	if cnt <= 0 {
		return
	}
	f := gsSpans{base, n, top}
	if *n == 0 {
		hd := f.head()
		*n = 1
		*top = unsafe.Add(base, GsSpanNode)
		hd.chain = 0
		hd.height = 0
		hd.root = f.newNode()
		f.leaf(hd.root).n = 0
	}
	var p, q gsSpPath
	s := f.near(&p, &q, idx, idx+cnt)
	f.release(&p, &q, &s, idx, cnt)
}

// GsSpansRegrow is where the non-empty slice [idx, idx + ol), which does
// not end the array, starts once realloc_slice grows it to cnt elements
// (gs_spans_regrow). That is in place where a free span starts where the
// slice ends and holds the difference or reaches the end of the array;
// otherwise the slice's elements go back on the freelist and the run goes
// where GsSpansAlloc places one of cnt elements, and the caller copies the
// elements there. The span after the slice is the one after the last span
// before it, so one descent finds both what growth takes from and what the
// elements merge with.
func GsSpansRegrow(base unsafe.Pointer, n *int64, top *unsafe.Pointer, length, idx, ol, cnt int64) int64 {
	if *n == 0 {
		GsSpansFree(base, n, top, idx, ol)
		return GsSpansAlloc(base, n, top, length, cnt)
	}
	f := gsSpans{base, n, top}
	end := idx + ol
	var p, q gsSpPath
	s := f.near(&p, &q, idx, end)
	if s.nl != nil && s.nl.idx[s.nk] == end &&
		(s.nl.cnt[s.nk] >= cnt-ol || end+s.nl.cnt[s.nk] == length) {
		np := &p
		if s.inQ {
			np = &q
		}
		f.take(np, s.nk, cnt-ol)
		return idx
	}
	f.release(&p, &q, &s, idx, ol)
	return GsSpansAlloc(base, n, top, length, cnt)
}

// ---------------------------------------------------------------------------
// std's slice_pool (stdlib/slice_pool_internal.goose) keeps the span tree
// above in nodes of its own, a node[>..], and a `tree` field for the header,
// both of which the program can write: these are its externs' functions,
// gs_slice_pool_place, _free and _regrow of src/runtime/runtime_os.h, the
// functions above taking the nodes as a GsRref and the header apart from
// them, with everything read from the tree checked before it is used. A node
// number must be one of the nodes past node 0, which stays zero and ends
// the chain of emptied nodes; an entry count must fit the node it is read
// from as a leaf or an inner node; a height must fit a path; and an entry
// moved into or out of a node must be one it has room for or holds. A check
// that fails aborts (GsESpanTree) at the call where the program called
// slice_pool (at); a wrong tree that passes them hands out wrong spans,
// which slice_pool checks against its length. Sums of stored indices and
// counts wrap. New nodes start zeroed, as a pushed `node { .. }` would.
// Where C reads on past an inner node's entries, in a tree edited so that one
// node is both an inner node and a leaf of one path, Go's arrays end, and
// that is a corrupt tree too.

// GsLoc is a place in the program's source: where a standard library
// function called from there reports its failures (C's gs_loc).
type GsLoc struct {
	File string
	Line int
}

// gsSpcPath is a path down a slice pool's tree, with the tree it is in.
type gsSpcPath struct {
	hd   *gsSpHead
	base unsafe.Pointer
	n    *int64
	top  *unsafe.Pointer
	at   *GsLoc
	h    int64
	node [gsSpanDepth]int64
	pos  [gsSpanDepth]int64
}

func gsSpcStart(gs *GsStackBlock, nodes GsRref, tree unsafe.Pointer, at *GsLoc) gsSpcPath {
	return gsSpcPath{hd: (*gsSpHead)(tree), base: nodes.Hdr.Base, n: &nodes.Hdr.Len,
		top: &gs.Stks[nodes.Stk], at: at}
}

func (p *gsSpcPath) leaf(i int64) *gsSpLeaf {
	return (*gsSpLeaf)(unsafe.Add(p.base, i*GsSpanNode))
}

func (p *gsSpcPath) inner(i int64) *gsSpInner {
	return (*gsSpInner)(unsafe.Add(p.base, i*GsSpanNode))
}

func (p *gsSpcPath) corrupt() { GsAbort(GsESpanTree, p.at.File, p.at.Line) }

// Whether node number i is none of the nodes past node 0; whether it is no
// inner node, or no leaf, whose count fits; and whether a height fits a
// path.
func (p *gsSpcPath) badNum(i int64) bool { return uint64(i)-1 >= uint64(*p.n)-1 }
func (p *gsSpcPath) badIn(i int64) bool {
	return p.badNum(i) || uint64(p.inner(i).n)-1 >= gsSpanFan
}
func (p *gsSpcPath) badLeaf(i int64) bool {
	return p.badNum(i) || uint64(p.leaf(i).n) > gsSpanLeaf
}
func gsSpcBadH(h int64) bool { return uint64(h) >= gsSpanDepth }

func (p *gsSpcPath) newNode() int64 {
	if i := p.hd.chain; i != 0 {
		if p.badNum(i) {
			p.corrupt()
		}
		p.hd.chain = p.leaf(i).link
		return i
	}
	i := *p.n
	*p.n = i + 1
	*p.leaf(i) = gsSpLeaf{}
	*p.top = unsafe.Add(*p.top, GsSpanNode)
	return i
}

func (p *gsSpcPath) dropNode(i int64) {
	p.leaf(i).link = p.hd.chain
	p.hd.chain = i
}

func (p *gsSpcPath) open(lf *gsSpLeaf, s int64) {
	if uint64(lf.n) >= gsSpanLeaf || uint64(s) > uint64(lf.n) {
		p.corrupt()
	}
	gsSpOpen(lf, s)
}

func (p *gsSpcPath) close(lf *gsSpLeaf, s int64) {
	if uint64(s) >= uint64(lf.n) {
		p.corrupt()
	}
	gsSpClose(lf, s)
}

func (p *gsSpcPath) openIn(in *gsSpInner, s int64) {
	if uint64(in.n) >= gsSpanFan || uint64(s) > uint64(in.n) {
		p.corrupt()
	}
	gsSpOpenIn(in, s)
}

func (p *gsSpcPath) closeIn(in *gsSpInner, s int64) {
	if in.n > gsSpanFan || uint64(s) >= uint64(in.n) {
		p.corrupt()
	}
	gsSpCloseIn(in, s)
}

func (p *gsSpcPath) max(i int64, leaf bool, stop int64) int64 {
	if leaf {
		lf := p.leaf(i)
		return gsSpMost(lf.cnt[:lf.n], stop)
	}
	in := p.inner(i)
	if uint64(in.n) > gsSpanFan {
		p.corrupt()
	}
	return gsSpMost(in.max[:in.n], stop)
}

func (p *gsSpcPath) key(i int64, leaf bool) int64 {
	if leaf {
		return p.leaf(i).idx[0]
	}
	return p.inner(i).key[0]
}

func (p *gsSpcPath) fix(l, was, now int64) {
	for ; l > 0; l-- {
		in := p.inner(p.node[l-1])
		k, c := p.pos[l-1], p.node[l]
		m := in.max[k]
		leaf := l == p.h
		key := p.key(c, leaf)
		var nm int64
		switch {
		case now >= m:
			nm = now
		case was < m:
			nm = m
		default:
			nm = p.max(c, leaf, was)
		}
		if in.key[k] == key && nm == m {
			return
		}
		in.key[k] = key
		in.max[k] = nm
		was = m
		now = nm
	}
}

func (p *gsSpcPath) find(idx int64) int64 {
	i := p.hd.root
	p.h = p.hd.height
	if gsSpcBadH(p.h) {
		p.corrupt()
	}
	for l := int64(0); l < p.h; l++ {
		if p.badIn(i) {
			p.corrupt()
		}
		in := p.inner(i)
		k := int64(1)
		for k < in.n && in.key[k] <= idx {
			k++
		}
		p.node[l] = i
		p.pos[l] = k - 1
		i = in.child[k-1]
	}
	p.node[p.h] = i
	if p.badLeaf(i) {
		p.corrupt()
	}
	lf := p.leaf(i)
	k := int64(0)
	for k < lf.n && lf.idx[k] <= idx {
		k++
	}
	p.pos[p.h] = k - 1
	return k - 1
}

func (p *gsSpcPath) newRoot(a, b int64, leaf bool) {
	r := p.newNode()
	rn := p.inner(r)
	rn.n = 2
	rn.key[0] = p.key(a, leaf)
	rn.max[0] = p.max(a, leaf, math.MaxInt64)
	rn.child[0] = a
	rn.key[1] = p.key(b, leaf)
	rn.max[1] = p.max(b, leaf, math.MaxInt64)
	rn.child[1] = b
	p.hd.root = r
	p.hd.height++
}

func (p *gsSpcPath) addChild(l, k, child int64, leaf bool) {
	for {
		in := p.inner(p.node[l])
		o := in
		key, max := p.key(child, leaf), p.max(child, leaf, math.MaxInt64)
		s, nn := k, int64(0)
		if in.n == gsSpanFan {
			const half = gsSpanFan / 2
			nn = p.newNode()
			if nn == p.node[l] {
				p.corrupt()
			}
			r := p.inner(nn)
			r.n = in.n - half
			copy(r.key[:r.n], in.key[half:])
			copy(r.max[:r.n], in.max[half:])
			copy(r.child[:r.n], in.child[half:])
			in.n = half
			if k > half {
				o = r
				s = k - half
			}
		}
		p.openIn(o, s)
		o.key[s] = key
		o.max[s] = max
		o.child[s] = child
		if nn == 0 {
			p.fix(l, 0, max)
			return
		}
		if l == 0 {
			p.newRoot(p.node[0], nn, false)
			return
		}
		p.fix(l, math.MaxInt64, 0)
		child = nn
		leaf = false
		k = p.pos[l-1] + 1
		l--
	}
}

func (p *gsSpcPath) insert(k, idx, cnt int64) {
	lf := p.leaf(p.node[p.h])
	o := lf
	s, nn := k, int64(0)
	if lf.n == gsSpanLeaf {
		const half = gsSpanLeaf / 2
		nn = p.newNode()
		if nn == p.node[p.h] {
			p.corrupt()
		}
		r := p.leaf(nn)
		r.n = lf.n - half
		copy(r.idx[:r.n], lf.idx[half:])
		copy(r.cnt[:r.n], lf.cnt[half:])
		lf.n = half
		if k > half {
			o = r
			s = k - half
		}
	}
	p.open(o, s)
	o.idx[s] = idx
	o.cnt[s] = cnt
	switch {
	case nn == 0:
		p.fix(p.h, 0, cnt)
	case p.h == 0:
		p.newRoot(p.node[0], nn, true)
	default:
		p.fix(p.h, math.MaxInt64, 0)
		p.addChild(p.h-1, p.pos[p.h-1]+1, nn, true)
	}
}

func (p *gsSpcPath) delete(k int64) {
	lf := p.leaf(p.node[p.h])
	if uint64(k) >= uint64(lf.n) {
		p.corrupt()
	}
	c := lf.cnt[k]
	p.close(lf, k)
	if lf.n > 0 || p.h == 0 {
		p.fix(p.h, c, 0)
		return
	}
	p.dropNode(p.node[p.h])
	for l := p.h - 1; ; l-- {
		in := p.inner(p.node[l])
		e := p.pos[l]
		m := in.max[e]
		p.closeIn(in, e)
		if in.n > 0 {
			p.fix(l, m, 0)
			break
		}
		// A root of one child, which the functions never leave.
		if l == 0 {
			p.corrupt()
		}
		p.dropNode(p.node[l])
	}
	for p.hd.height > 0 {
		r := p.hd.root
		if p.badNum(r) {
			p.corrupt()
		}
		if p.inner(r).n != 1 {
			break
		}
		p.hd.root = p.inner(r).child[0]
		p.hd.height--
		p.dropNode(r)
	}
}

func (p *gsSpcPath) take(k, cnt int64) {
	lf := p.leaf(p.node[p.h])
	c := lf.cnt[k]
	if c <= cnt {
		p.delete(k)
		return
	}
	lf.idx[k] += cnt
	lf.cnt[k] = c - cnt
	p.fix(p.h, c, c-cnt)
}

// GsSlicePoolPlace is GsSpansAlloc of a slice pool's tree
// (gs_slice_pool_place): where a run of cnt elements goes in a pool of
// length elements, first fit, else the end.
func GsSlicePoolPlace(gs *GsStackBlock, nodes GsRref, tree unsafe.Pointer, length, cnt int64, at *GsLoc) int64 {
	if cnt == 0 || nodes.Hdr.Len == 0 {
		return length
	}
	p := gsSpcStart(gs, nodes, tree, at)
	var before [gsSpanDepth]int64
	var lf *gsSpLeaf
	var l, k, m int64
	p.h = p.hd.height
	if gsSpcBadH(p.h) {
		p.corrupt()
	}
	i := p.hd.root
	for l = 0; l < p.h; l++ {
		if p.badIn(i) {
			p.corrupt()
		}
		in := p.inner(i)
		for k, m = 0, 0; k < in.n && in.max[k] < cnt; k++ {
			if in.max[k] > m {
				m = in.max[k]
			}
		}
		if k == in.n {
			break
		}
		p.node[l] = i
		p.pos[l] = k
		before[l] = m
		i = in.child[k]
	}
	if l == p.h {
		if p.badLeaf(i) {
			p.corrupt()
		}
		lf = p.leaf(i)
		for k, m = 0, 0; k < lf.n && lf.cnt[k] < cnt; k++ {
			if lf.cnt[k] > m {
				m = lf.cnt[k]
			}
		}
		before[l] = m
	}
	if l < p.h || k == lf.n {
		for l, i = 0, p.hd.root; l < p.h; l++ {
			if p.badIn(i) {
				p.corrupt()
			}
			in := p.inner(i)
			p.node[l] = i
			p.pos[l] = in.n - 1
			i = in.child[in.n-1]
		}
		p.node[p.h] = i
		if p.badLeaf(i) {
			p.corrupt()
		}
		lf = p.leaf(i)
		k = lf.n - 1
		if k < 0 || lf.idx[k]+lf.cnt[k] != length {
			return length
		}
		idx := lf.idx[k]
		p.take(k, cnt)
		return idx
	}
	p.node[p.h] = i
	p.pos[p.h] = k
	idx, c := lf.idx[k], lf.cnt[k]
	var now, j int64
	switch {
	case c > cnt:
		now = c - cnt
		lf.idx[k] = idx + cnt
		lf.cnt[k] = now
		j = k + 1
	case lf.n > 1 || p.h == 0:
		p.close(lf, k)
		now = 0
		j = k
	default:
		p.delete(k)
		return idx
	}
	for l = p.h; l > 0; l-- {
		in := p.inner(p.node[l-1])
		e := p.pos[l-1]
		mx := in.max[e]
		nm := mx
		key := p.key(p.node[l], l == p.h)
		if c == mx {
			var v []int64
			if l == p.h {
				v = lf.cnt[:lf.n]
			} else {
				below := p.inner(p.node[l])
				if uint64(below.n) > gsSpanFan {
					p.corrupt()
				}
				v = below.max[:below.n]
			}
			nm = before[l]
			if now > nm {
				nm = now
			}
			for ; j < int64(len(v)) && nm < c; j++ {
				if v[j] > nm {
					nm = v[j]
				}
			}
		}
		if in.key[e] == key && nm == mx {
			break
		}
		in.key[e] = key
		in.max[e] = nm
		c = mx
		now = nm
		j = e + 1
	}
	return idx
}

// gsSpcNear is gsSpNear in a slice pool's tree: np is the path to nl.
type gsSpcNear struct {
	lf, nl *gsSpLeaf
	k, nk  int64
	np     *gsSpcPath
}

func (p *gsSpcPath) near(q *gsSpcPath, idx, end int64) gsSpcNear {
	var s gsSpcNear
	s.k = p.find(idx)
	s.lf = p.leaf(p.node[p.h])
	s.nk = -1
	s.np = p
	if s.k+1 < s.lf.n {
		s.nl = s.lf
		s.nk = s.k + 1
		return s
	}
	l := p.h - 1
	for l >= 0 && p.pos[l]+1 >= p.inner(p.node[l]).n {
		l--
	}
	if l < 0 {
		return s
	}
	if p.pos[l]+1 >= gsSpanFan {
		p.corrupt()
	}
	if p.inner(p.node[l]).key[p.pos[l]+1] != end {
		return s
	}
	q.hd, q.base, q.n, q.top, q.at, q.h = p.hd, p.base, p.n, p.top, p.at, p.h
	copy(q.node[:l+1], p.node[:l+1])
	copy(q.pos[:l+1], p.pos[:l+1])
	for q.pos[l]++; l < q.h; l++ {
		c := q.inner(q.node[l]).child[q.pos[l]]
		if l+1 < q.h && q.badIn(c) || l+1 == q.h && q.badLeaf(c) {
			q.corrupt()
		}
		q.node[l+1] = c
		q.pos[l+1] = 0
	}
	s.np = q
	s.nl = q.leaf(q.node[q.h])
	s.nk = 0
	return s
}

func (p *gsSpcPath) release(s *gsSpcNear, idx, cnt int64) {
	lf, nl := s.lf, s.nl
	k, nk := s.k, s.nk
	prev := k >= 0 && lf.idx[k]+lf.cnt[k] == idx
	next := nl != nil && idx+cnt == nl.idx[nk]
	switch {
	case prev:
		c := lf.cnt[k]
		m := c + cnt
		if next {
			m += nl.cnt[nk]
		}
		lf.cnt[k] = m
		if next && nl == lf {
			p.close(lf, nk)
		}
		p.fix(p.h, c, m)
		if next && nl != lf {
			s.np.delete(nk)
		}
	case next:
		c := nl.cnt[nk]
		nl.idx[nk] = idx
		nl.cnt[nk] = c + cnt
		s.np.fix(s.np.h, c, nl.cnt[nk])
	default:
		p.insert(k+1, idx, cnt)
	}
}

// GsSlicePoolFree is GsSpansFree of a slice pool's tree
// (gs_slice_pool_free): [idx, idx + cnt) back among the free spans. The
// first span freed makes node 0, zero, and the root's leaf.
func GsSlicePoolFree(gs *GsStackBlock, nodes GsRref, tree unsafe.Pointer, idx, cnt int64, at *GsLoc) {
	if cnt <= 0 {
		return
	}
	p := gsSpcStart(gs, nodes, tree, at)
	if *p.n == 0 {
		*p.leaf(0) = gsSpLeaf{}
		*p.n = 1
		*p.top = unsafe.Add(p.base, GsSpanNode)
		p.hd.chain = 0
		p.hd.height = 0
		p.hd.root = p.newNode()
	}
	var q gsSpcPath
	s := p.near(&q, idx, idx+cnt)
	p.release(&s, idx, cnt)
}

// GsSlicePoolRegrow is GsSpansRegrow of a slice pool's tree
// (gs_slice_pool_regrow): where the slice [idx, idx + ol) starts once grown
// to cnt elements.
func GsSlicePoolRegrow(gs *GsStackBlock, nodes GsRref, tree unsafe.Pointer, length, idx, ol, cnt int64,
	at *GsLoc) int64 {
	end := idx + ol
	if nodes.Hdr.Len == 0 {
		GsSlicePoolFree(gs, nodes, tree, idx, ol, at)
		return GsSlicePoolPlace(gs, nodes, tree, length, cnt, at)
	}
	p := gsSpcStart(gs, nodes, tree, at)
	var q gsSpcPath
	s := p.near(&q, idx, end)
	if s.nl != nil && s.nl.idx[s.nk] == end && (s.nl.cnt[s.nk] >= cnt-ol || end+s.nl.cnt[s.nk] == length) {
		s.np.take(s.nk, cnt-ol)
		return idx
	}
	p.release(&s, idx, ol)
	return GsSlicePoolPlace(gs, nodes, tree, length, cnt, at)
}

// GsSlicePoolTrim is slice_pool's trim (gs_slice_pool_trim): the complete
// pages inside every free span of items, the pool's array viewed as bytes
// of elements of elem bytes, given back to the OS, which reads them back as
// zeros, as GsTrimMemory gives back a stack's. The span (idx, cnt) covers the
// bytes [idx * elem, (idx + cnt) * elem): free spans never touch, a free
// merging with its neighbours, so each is whole, and a page is kept only
// where it holds a live element too, at either end. The walk visits each
// node once, so a tree whose children lead back to nodes it has seen is
// corrupt, as is a span outside the array: both abort before anything past
// them is discarded. Only pages below the array's stack top are discarded.
// False where the OS refused.
func GsSlicePoolTrim(gs *GsStackBlock, nodes GsRref, tree unsafe.Pointer, items GsSlice, elem int64, at *GsLoc) bool {
	page := int64(syscall.Getpagesize())
	if nodes.Hdr.Len == 0 || items.Len <= 0 || elem <= 0 || page <= 0 {
		return true
	}
	count := items.Len / elem
	p := gsSpcStart(gs, nodes, tree, at)
	p.h = p.hd.height
	if gsSpcBadH(p.h) {
		p.corrupt()
	}
	p.node[0] = p.hd.root
	p.pos[0] = 0
	if p.h > 0 && p.badIn(p.node[0]) || p.h == 0 && p.badLeaf(p.node[0]) {
		p.corrupt()
	}
	// The pages from the first boundary at or above the span's first byte to
	// the last one at or below its end, as offsets from the array's start.
	base := int64(uintptr(items.Data) % uintptr(page))
	ok := true
	visits := int64(0)
	for l := int64(0); l >= 0; {
		if l == p.h {
			lf := p.leaf(p.node[l])
			if visits++; visits >= *p.n {
				p.corrupt()
			}
			for k := int64(0); k < lf.n; k++ {
				idx, cnt := lf.idx[k], lf.cnt[k]
				if idx < 0 || cnt <= 0 || idx > count || cnt > count-idx {
					p.corrupt()
				}
				start := (base+idx*elem+page-1)/page*page - base
				end := (base+(idx+cnt)*elem)/page*page - base
				if start < end && syscall.Madvise(unsafe.Slice((*byte)(unsafe.Add(items.Data, start)), end-start),
					syscall.MADV_DONTNEED) != nil {
					ok = false
				}
			}
			l--
			continue
		}
		in := p.inner(p.node[l])
		if p.pos[l] == 0 {
			if visits++; visits >= *p.n {
				p.corrupt()
			}
		}
		if p.pos[l] >= in.n {
			l--
			continue
		}
		c := in.child[p.pos[l]]
		p.pos[l]++
		if l+1 < p.h && p.badIn(c) || l+1 == p.h && p.badLeaf(c) {
			p.corrupt()
		}
		l++
		p.node[l] = c
		p.pos[l] = 0
	}
	return ok
}
