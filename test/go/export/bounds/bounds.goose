// Bounds checks that bounds-check elimination leaves in place (issue #40),
// through the Go entry points of export fns. Each is GsIdx, which Go
// inlines, and whose failure is a panic statement, which Go knows does not
// return: so the values a loop keeps in registers stay there across every
// check, and past one Go knows the index is in range. bounds_test.go checks
// what each kernel computes against the same written in Go, and that an
// index out of range panics with its *GsError, and benchmarks the kernels
// against the Go, and the sort and the scan against twins whose indices
// assert(...)s prove in range first, which BCE then leaves unchecked: what
// kept checks out of a loop's way before. run_go_tests.py reads the code Go
// makes of each annotated one for arm64 and amd64, at -O2 and -O0
// (Runner.spill_asm): `asm: no spills` is a function that keeps its values
// in registers, moving none to its frame on Go's stack and back; `asm: no
// index checks` one that leaves Go no check of its own to make of an index.

// ---- a sorting kernel: heapsort, whose sift reads the children of i, at
// 2 * i + 1 and the one after, of which BCE proves nothing

// Moves v[start] down the heap v[..n] past every child greater than it.
// asm: no spills
export fn sift(v: i64[:], start: i64, n: i64) {
    var i = start;
    let x = v[i];
    loop {
        let l = 2 * i + 1;
        if l >= n { break; }
        var m = l;
        if l + 1 < n && v[l] < v[l + 1] { m = l + 1; }
        if v[m] <= x { break; }
        v[i] = v[m];
        i = m;
    }
    v[i] = x;
}

export fn heap_sort(v: i64[:]) {
    var i = v.len / 2;
    while i > 0 {
        i -= 1;
        sift(v, i, v.len);
    }
    var end = v.len;
    while end > 1 {
        end -= 1;
        let t = v[0];
        v[0] = v[end];
        v[end] = t;
        sift(v, 0, end);
    }
}

// The same, each index asserted in range before it is used.
// asm: no spills
export fn sift_asserted(v: i64[:], start: i64, n: i64) {
    var i = start;
    assert(0 <= i && i < v.len);
    let x = v[i];
    loop {
        let l = 2 * i + 1;
        if l >= n { break; }
        assert(0 <= l && l < v.len);
        var m = l;
        if l + 1 < n {
            assert(l + 1 < v.len);
            if v[l] < v[l + 1] { m = l + 1; }
        }
        assert(0 <= m && m < v.len);
        if v[m] <= x { break; }
        assert(0 <= i && i < v.len);
        v[i] = v[m];
        i = m;
    }
    assert(0 <= i && i < v.len);
    v[i] = x;
}

export fn heap_sort_asserted(v: i64[:]) {
    var i = v.len / 2;
    while i > 0 {
        i -= 1;
        sift_asserted(v, i, v.len);
    }
    var end = v.len;
    while end > 1 {
        end -= 1;
        let t = v[0];
        v[0] = v[end];
        v[end] = t;
        sift_asserted(v, 0, end);
    }
}

// ---- a scanning loop: the neighbours of each node of a graph in CSR form,
// node r's at cols[rowptr[r]] up to rowptr[r + 1], each a node's index into
// x: two checks a step, cols's and x's, of indices that are data

// The sum of x over each node's neighbours, the sums hashed in order.
// asm: no spills
export fn row_sums(rowptr: const i64[:], cols: const i64[:], x: const i64[:]) -> i64 {
    var h = 0;
    for r in rowptr.len - 1 {
        var s = 0;
        var k = rowptr[r];
        let e = rowptr[r + 1];
        while k < e {
            s += x[cols[k]];
            k += 1;
        }
        h = h * 31 + s;
    }
    h
}

// asm: no spills
export fn row_sums_asserted(rowptr: const i64[:], cols: const i64[:], x: const i64[:]) -> i64 {
    var h = 0;
    for r in rowptr.len - 1 {
        var s = 0;
        var k = rowptr[r];
        let e = rowptr[r + 1];
        while k < e {
            assert(0 <= k && k < cols.len);
            let c = cols[k];
            assert(0 <= c && c < x.len);
            s += x[c];
            k += 1;
        }
        h = h * 31 + s;
    }
    h
}

// ---- slices in a loop: each one's bounds checked before GsSub makes it,
// the check failing through GsAbort, a panic statement once Go inlines it

// The fields of a text between the offsets starts holds, each hashed by
// its length and first byte.
// asm: no spills
export fn fields(text: const u8[:], starts: const i64[:]) -> i64 {
    var h = 0;
    for j in starts.len - 1 {
        let f = text[starts[j]..starts[j + 1]];
        h = h * 31 + f.len;
        if f.len > 0 { h += f[0]; }
    }
    h
}

// ---- a Go local's own array, indexed by data: after GsIdx, Go's own check
// of the index (DESIGN.md §3.6) is one it proves passes

// asm: no index checks
export fn tally(xs: const u8[:]) -> i64 {
    var counts: i64[16] = [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0];
    for x in xs { counts[x] += 1; }
    var h = 0;
    for c in counts { h = h * 31 + c; }
    h
}
