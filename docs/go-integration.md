# Using goose.go inside an existing Go module

This guide puts a Goose kernel into a Go package that already exists, and is already tested, without changing the package's default build: where the `.goose` source and the generated Go go, how `go generate` writes the Go, how a build tag switches the package between its Go original and the kernel, how to prove the two equivalent, how to benchmark them fairly, which pitfalls of the generated code to avoid, and what the kernel costs a binary.

Its worked example is [`examples/records/`](../examples/records/), a complete module, which `python3 test/run_go_tests.py example` builds and tests in both builds, as CI would. The [README](../README.md) has the compiler's options and the Go they write, and [`DESIGN.md`](../DESIGN.md) the details.

1. [The worked example](#1-the-worked-example)
2. [Layout](#2-layout)
3. [Off by default](#3-off-by-default)
4. [Proving equivalence](#4-proving-equivalence)
5. [Malformed input](#5-malformed-input)
6. [Benchmarking](#6-benchmarking)
7. [Pitfalls](#7-pitfalls)
8. [Size](#8-size)
9. [CI](#9-ci)

## 1. The worked example

Package `records` counts the records of a segment. A record is a little-endian `uint32` length n, n bytes of payload, and one byte, the XOR of those bytes. `Count(seg)` returns how many records `seg` holds, `ErrChecksum` where one's checksum is wrong, and `ErrTruncated` where `seg` ends inside one. The Go original, in `count.go`, checksums a payload a byte at a time:

<!-- example: count.go -->
```go
func countRecordsGo(seg []byte) int64 {
	var n int64
	for off := 0; off < len(seg); {
		size := int(binary.LittleEndian.Uint32(seg[off:]))
		var x byte
		for _, b := range seg[off+4 : off+4+size] {
			x ^= b
		}
		if x != seg[off+4+size] {
			return -1
		}
		off += 5 + size
		n++
	}
	return n
}
```

It finds a truncated record by failing a bounds check, a panic that `Count` recovers ([§5](#5-malformed-input)). The kernel, in `_goose/records.goose`, is the same loop checksumming 8 bytes at a time, each 8 bytes one load ([§7](#byte-assembled-loads-one-load-helpers)):

<!-- example: _goose/records.goose -->
```goose
// The little-endian u32 at s[i..i + 4], in one load. The slice checks the
// span, which fails where s ends inside it.
fn le32(s: const u8[:], i: i64) -> u32 {
    let c = s[i..i + 4];
    c[0] | (c[1] as u32) << 8 | (c[2] as u32) << 16 | (c[3] as u32) << 24
}

// The little-endian u64 at s[i..i + 8], in one load.
fn le64(s: const u8[:], i: i64) -> u64 {
    let c = s[i..i + 8];
    c[0] | (c[1] as u64) << 8 | (c[2] as u64) << 16 | (c[3] as u64) << 24 |
        (c[4] as u64) << 32 | (c[5] as u64) << 40 | (c[6] as u64) << 48 | (c[7] as u64) << 56
}

// The XOR of p's bytes: 8 at a time, then those of the tail, then the 8
// lanes folded into one.
fn xor_bytes(p: const u8[:]) -> u8 {
    var x: u64 = 0;
    var i = 0;
    while i <= p.len - 8 {
        x ^= le64(p, i);
        i += 8;
    }
    while i < p.len {
        x ^= p[i];
        i += 1;
    }
    x ^= x >> 32;
    x ^= x >> 16;
    x ^= x >> 8;
    x as! u8
}

// The number of records in seg, or -1 where one's checksum is wrong. A
// record is a little-endian u32 length n, n bytes of payload, and the XOR of
// those bytes. A record that seg ends inside fails a bounds check.
export fn count_records(seg: const u8[:]) -> i64 {
    var n = 0;
    var off = 0;
    while off < seg.len {
        let size = le32(seg, off) as i64;
        let p = seg[off + 4..off + 4 + size];
        if xor_bytes(p) != seg[off + 4 + size] { return -1; }
        off += 5 + size;
        n += 1;
    }
    n
}
```

The export fn is a kernel over slices: it takes no block of data stacks, holds no global and keeps its argument in its own variables, so its entry points call its function and nothing else, and Go inlines them into their callers (the README's [kernels](../README.md#kernels-entry-points-that-go-inlines)):

```go
func CountRecordsBytes(seg []byte) int64 {
	return count_records_g(gsSlice{Data: unsafe.Pointer(unsafe.SliceData(seg)), Len: int64(len(seg))})
}
```

## 2. Layout

```
examples/records/           module example.com/records, go 1.21 or later
├── records.go              the API: Count, which calls countRecords
├── count.go                countRecordsGo, the Go original
├── bind.go                 //go:build !goose: countRecords is the original
├── bind_goose.go           //go:build goose: countRecords is the kernel; the //go:generate line
├── records_goose.go        the kernel's Go, which go generate -tags goose writes: checked in
├── _goose/records.goose    the kernel
├── records_test.go         Count's tests, in both builds
├── kernel_test.go          -tags goose: the kernel against the original, table-driven and random
├── fuzz_test.go            -tags goose: the fuzz target
├── guard_test.go           -tags goose: inputs that end at a PROT_NONE page
└── bench_test.go           both builds: the benchmarks, and a plain-Go control
```

(In this repository `records_goose.go` is not checked in but written by the test suite, for the reason [§9](#9-ci) gives.)

**The `.goose` sources** go in a directory the go tool ignores: one whose name starts with `_` or `.`, or `testdata`. `go build ./...`, `go vet ./...` and `go test ./...` see nothing there, whatever it holds, a Go file for `--include` too.

**The generated file** goes in the package, named after its source with `_goose.go`, and is checked in: the package then builds with nothing but Go, and a change in the Go the compiler writes is a change a reviewer sees. Its header records what wrote it -- the compiler's version, the runtime's revision and the options (the README's [Checked-in Go](../README.md#checked-in-go-the-header-and---check)) -- and it is laid out as `gofmt` lays it out, `--go-build`'s constraint included: no `| gofmt` pipe or hand-prepended `//go:build` line is needed.

**The `//go:generate` line** goes next to the binding it serves, in `bind_goose.go`:

<!-- example: bind_goose.go -->
```go
//go:generate goose --target=go --go-build goose --go-package records --go-unexported --go-no-withstack -o records_goose.go _goose/records.goose
```

- `go generate` runs it in the package's directory, so its paths are the package's, and the header and every abort's message name the source as `_goose/records.goose`, the same on every machine.
- `go generate` honors build constraints: a line in a `//go:build goose` file runs under `go generate -tags goose ./...` only. A contributor who runs `go generate ./...` for the module's other generators needs no Goose compiler.
- `--go-build goose` starts the file with `//go:build goose`, so that the default build leaves it out.
- `--go-package records` makes it a file of the package. A file with `export fn`s and no `fn main()` is a library, as `--library` makes any file ([§7](#a-library-not-a-program---library)).
- `--go-unexported` unexports the runtime's names (`gsSlice`, `gsError`), which are otherwise part of the package's API wherever the file is built. The entry points stay exported: with `-tags goose` the package also exports `CountRecords`, `CountRecordsString` and `CountRecordsBytes`. A package whose API must not change at all generates its kernels into an internal package (`internal/kernels`), without `--go-unexported`, and binds to that, a panic's `*kernels.GsError` included.
- `--go-no-withstack` leaves out the `...WithStack` entry points, which take a block of data stacks that a kernel over slices never uses, and the block's type with them.

**`extern fn`s.** The Go functions a kernel's `extern fn`s call can be a normal Go file of the package, with the generated file's constraint (`extern_goose.go`, `//go:build goose`), which `gofmt`, `go vet` and gopls see as it is, and whose changes compile nothing again.

- It names the runtime's types as the generated file declares them: `gsSlice` under `--go-unexported`, `GsSlice` without.
- It can use only what that file holds, the types of the calls the file makes, and no helper the file does not call itself, such as `GsSliceToBytes` or `GsBldAppend`. View a slice in place with `unsafe.Slice((*byte)(s.Data), s.Len)` or `unsafe.String`, for the call only.
- A function that needs the runtime's helpers goes in an `--include` file instead, which the compiler splices into the generated file, renaming the runtime's names as it does its own, and whose `//go:build ignore` keeps it out of the package's own build (the README's [`extern fn`](../README.md#calling-go-from-goose-extern-fn-and---include-helpergo)).
- An `extern fn` handed a slice costs its export the entry points of a kernel: they move their arguments to Go's heap (`GsEscape`) and keep them alive, and Go no longer inlines them.

**Names.** The generated file declares names at the package's level: the runtime's, which start with `gs` or `Gs`; the program's, which end in `_g`; and the entry points. The package must declare none of them. A second `.goose` program in the same package is compiled with `--go-runtime-file`, which puts the runtime both use in a file of its own and gives each program's names its file's name.

**Platforms.** The Go the compiler writes is for 64-bit Linux and macOS (the README's [Building](../README.md#building)). Where a module builds elsewhere too, constrain the generated file and what uses it to those -- `--go-build 'goose && (linux || darwin) && (amd64 || arm64)'`, the same on `bind_goose.go` and the kernel's tests, and its negation on `bind.go` -- so that `-tags goose` anywhere else builds the Go original.

## 3. Off by default

The package keeps the call sites it has. The original is renamed -- here `countRecords` became `countRecordsGo` -- and two small files, one per build, define the name the call sites call:

<!-- example: bind.go -->
```go
//go:build !goose

package records

import "runtime"

// The default build: Count runs the Go original.

func countRecords(seg []byte) int64 { return countRecordsGo(seg) }

// boundsFailure says whether r, recovered from countRecords, is a failed
// bounds check: in Go, a runtime.Error.
func boundsFailure(r any) bool {
	_, ok := r.(runtime.Error)
	return ok
}
```

<!-- example: bind_goose.go -->
```go
//go:build goose

package records

//go:generate goose --target=go --go-build goose --go-package records --go-unexported --go-no-withstack -o records_goose.go _goose/records.goose

// The build with -tags goose: Count runs the Goose kernel of
// _goose/records.goose, which go generate -tags goose compiles to
// records_goose.go.

func countRecords(seg []byte) int64 { return CountRecordsBytes(seg) }

// boundsFailure says whether r, recovered from countRecords, is a failed
// bounds check: in Goose, a *gsError, which is no runtime.Error.
func boundsFailure(r any) bool {
	_, ok := r.(*gsError)
	return ok
}
```

(`boundsFailure` is [§5](#5-malformed-input)'s.) A binding is a function, not a `var` holding one: Go inlines it, and `Count` makes the one call of the kernel's function, `count_records_g`, that a call of `CountRecordsBytes` makes (`go build -tags goose -gcflags=-m` says `inlining call to CountRecordsBytes`). It calls `CountRecordsBytes` rather than the generic `CountRecords`, which reads its argument through its address, at the cost of a store and a load.

- **The default build is the Go it was.** It compiles neither the generated file nor the kernel's tests, and needs no Goose compiler: `go build`, `go vet` and `go test` are what they were. `go build -tags goose` switches every call site at once.
- **The original stays,** in both builds: what the default build runs, and the reference every test of the kernel holds it to. It is the specification; the kernel is an optimization of it.
- **A tag is the whole build's.** `-tags goose` switches every package that uses the tag, a dependency's too. A tag of the package's own (`records_goose`) switches its kernels alone.
- **Tools see the default build.** `go doc` shows the API without the kernel's entry points; gopls takes `"build.buildFlags": ["-tags=goose"]` to work on the files the tag builds.

## 4. Proving equivalence

Every test of the kernel holds it to the original, input by input: the same result, and a panic wherever the original panics. `kernel_test.go`'s `same` is the one comparison all of them make:

<!-- example: kernel_test.go -->
```go
// same fails t unless the kernel does with seg what the original does.
func same(t *testing.T, seg []byte) {
	t.Helper()
	want, wr := run(countRecordsGo, seg)
	got, gr := run(countRecords, seg)
	if got != want {
		t.Fatalf("% x: the kernel's %+v (%v), the original's %+v (%v)", seg, got, gr, want, wr)
	}
	if want.panicked {
		// A failed bounds check: a runtime.Error in Go, a *gsError in Goose.
		if _, ok := wr.(runtime.Error); !ok {
			t.Fatalf("% x: the original panicked with %v", seg, wr)
		}
		if _, ok := gr.(*gsError); !ok {
			t.Fatalf("% x: the kernel panicked with %v", seg, gr)
		}
	}
}
```

(`run` calls a count and recovers what it panics with.) The tests that use it are `//go:build goose`: in the default build there is no kernel to compare.

**Differential tests, table-driven and randomized.** A table of the inputs where a kernel and its original part ways: the empty input, each size either side of a word (payloads of 7, 8 and 9 bytes, and of 15, 16 and 17), the input cut short at every byte, and with each byte changed in turn, which here makes a checksum wrong or a length run past the end. Then random inputs, of random shapes, from a fixed seed so that a failure can be run again (`TestKernelRandom`: 10,000 segments of up to 7 records of up to 39 bytes, half of them with a byte changed or cut short).

**One fuzz target per kernel,** which `go test -tags goose` runs on its seeds, and `-fuzz` on whatever the fuzzer finds:

<!-- example: fuzz_test.go -->
```go
func FuzzCountRecords(f *testing.F) {
	f.Add([]byte{})
	f.Add(segment("goose", "", "a gaggle of geese"))
	f.Add(segment(payload(8), payload(9)))
	f.Add(segment("goose")[:7])
	f.Fuzz(func(t *testing.T, seg []byte) { same(t, seg) })
}
```

```bash
go test -tags goose -run '^$' -fuzz '^FuzzCountRecords$' -fuzztime 1m
```

An input that fails is written to `testdata/fuzz/FuzzCountRecords/`: check it in, and it is a seed of every `go test` from then on.

**Guard pages,** for a kernel that reads or writes more than a byte at a time. The Go the compiler writes reaches memory through `unsafe.Pointer` wherever bounds-check elimination proved an index in range (`binary.LittleEndian.Uint64((*[8]byte)(p)[:])`), so Go checks none of it. A read past the end of a slice on Go's heap reads its allocation's slack or the next object's bytes, and seldom faults: no other test sees it, and `-d=checkptr` sees it only where it crosses into another allocation. An input that ends where a `PROT_NONE` page starts makes any read past its end fault, at once:

<!-- example: guard_test.go -->
```go
// guarded is n bytes that end where a PROT_NONE page starts, their capacity
// included.
func guarded(t *testing.T, n int) []byte {
	page := syscall.Getpagesize()
	size := (n + page - 1) / page * page
	mem, err := syscall.Mmap(-1, 0, size+page, syscall.PROT_READ|syscall.PROT_WRITE,
		syscall.MAP_ANON|syscall.MAP_PRIVATE)
	if err != nil {
		t.Fatal(err)
	}
	t.Cleanup(func() { syscall.Munmap(mem) })
	if err := syscall.Mprotect(mem[size:], syscall.PROT_NONE); err != nil {
		t.Fatal(err)
	}
	return mem[size-n : size : size]
}

func TestKernelGuardPage(t *testing.T) {
	// A fault panics, which same reports, rather than ending the test.
	defer debug.SetPanicOnFault(debug.SetPanicOnFault(true))
	g := guarded(t, 64)
	for n := 0; n <= 17; n++ {
		seg := segment(payload(n), payload(17-n))
		for cut := 0; cut <= len(seg); cut++ {
			buf := g[len(g)-cut:]
			copy(buf, seg)
			same(t, buf)
		}
	}
}
```

Every input ends at the page, at each length that puts a load near the end: here two payloads of 17 bytes between them, cut short at every byte. An output buffer is guarded the same way (`dst := guarded(t, n)`), and a kernel that reads backwards takes the page in front of its input (`mem[:page]` `PROT_NONE`, the input from `mem[page:]`). `guard_test.go` is `//go:build goose && (linux || darwin)`, where `syscall.Mmap` is.

What it buys is the fourth mutant below: a kernel whose 4-byte load reads 8 bytes is right, since only the 4 bytes it keeps count, and passes the table, the random tests, the fuzz target's seeds and `Count`'s tests, `-d=checkptr` and all. Only the guard page fails it:

```
--- FAIL: TestKernelGuardPage (0.00s)
    guard_test.go:41: 00 00 00 00: the kernel panicked with runtime error: invalid memory address or nil pointer dereference
```

**Mutation checks.** Break the kernel on purpose, compile it again, and check that a test fails; then `go generate` it back. A test that no mutant fails tests nothing. `test/run_go_tests.py` makes these mutants of the example, each in a copy of its own, and fails where the tests pass:

| Mutant | Where | Caught by |
|---|---|---|
| `x ^= x >> 8;` deleted: one lane of the checksum left unfolded | `_goose/records.goose` | every test of the kernel, `TestCount` and two of the fuzz target's seeds |
| `off += 4 + size;`: each record's checksum read as the next one's length | `_goose/records.goose` | every test of the kernel, `TestCount` and two of the fuzz target's seeds |
| `r.(*gsError)` made `r.(interface{ RuntimeError() })`: the original's recover ([§5](#5-malformed-input)) | `bind_goose.go` | `TestCount` |
| the 4-byte load of `le32` made an 8-byte load | `records_goose.go` | the guard page alone |

Break the generated Go where what is to be tested is the compiler's work -- its wide loads, its bounds checks -- and the `.goose` source where it is the kernel's logic.

## 5. Malformed input

Go's bounds checks panic with a `runtime.Error`; Goose's panic with a `*GsError` (`*gsError` under `--go-unexported`), which every abort panics with. It is an `error`, but not a `runtime.Error`, whose `RuntimeError` method it lacks. Where the original fails a check on malformed input, the kernel fails one too, with the other kind of panic:

```
runtime.boundsError: runtime error: index out of range [3] with length 2
*records.gsError: goose runtime error: slice bounds out of range (_goose/records.goose:8)
```

So follow each call of the kernel up to every `recover` that can catch what it panics with, the package's own and its callers', and read what each one tests: `r.(runtime.Error)` lets a `*GsError` go on, where `r.(error)` takes both. `Count` recovers what `countRecords` panics with, turns a failed bounds check into `ErrTruncated` and lets anything else go on:

<!-- example: records.go -->
```go
// Count returns the number of records in seg.
func Count(seg []byte) (n int, err error) {
	// countRecords leaves finding a truncated record to its bounds checks,
	// whose panic boundsFailure knows.
	defer func() {
		if r := recover(); r != nil {
			if !boundsFailure(r) {
				panic(r)
			}
			n, err = 0, ErrTruncated
		}
	}()
	c := countRecords(seg)
	if c < 0 {
		return 0, ErrChecksum
	}
	return int(c), nil
}
```

and each binding defines `boundsFailure` for its own kind ([§3](#3-off-by-default)). `records_test.go` is in both builds: its truncated segments go through `runtime.Error` under `go test` and through `*gsError` under `go test -tags goose`, so that the recover is tested both ways. The third mutant of [§4](#4-proving-equivalence) is the bug this catches: the original's test, `runtime.Error`, kept in the Goose build, where `Count` then panics on a truncated segment. And `same` checks that the two panic alike, the original with a `runtime.Error` and the kernel with a `*gsError`.

What else differs:

- **The text.** Go's `runtime error: index out of range [20] with length 20` is Goose's `goose runtime error: index 20 out of bounds (length 20) (_goose/records.goose:47)`. A test that matches a message matches one of them.
- **Every abort is a `*GsError`:** an `assert`, a limited array's capacity, an empty array's `pop`, and in a library `exit()`, which panics where a program would end ([§7](#a-library-not-a-program---library)).
- **A data stack that overflows** -- in an export that runs on a block of them, which a kernel over slices does not -- panics with Go's own fault, a `runtime.Error` (`invalid memory address or nil pointer dereference`), not a `*GsError`.
- **A block after a panic.** A call that runs on a block gives its stacks back as it returns, not as it panics: after recovering one of a block you keep (`...WithStack`), `Reset` it, or what the call pushed stays on its stacks until you do. A pooled call (`Name`) that panics does not give its block back to the pool: Go's collector closes it, and a call that finds the pool empty makes another, mapping its stacks again. Where malformed input is common, check it first, or keep a block and `Reset` it after each panic.

## 6. Benchmarking

`bench_test.go` is in both builds, so the same benchmarks measure the original under `go test` and the kernel under `go test -tags goose`:

<!-- example: bench_test.go -->
```go
// bench measures count over inputs distinct segments of each shape, a
// different one each time round.
func bench(b *testing.B, count func([]byte) int64, inputs int) {
	for _, s := range []struct {
		name   string
		lo, hi int
	}{
		{"payload=0-7", 0, 7},         // every byte in a tail, at its payload's end
		{"payload=8-64", 8, 64},       // words, and tails
		{"payload=1K-4K", 1024, 4096}, // nearly every byte in a word, inside its payload
	} {
		segs, mean := segments(inputs, s.lo, s.hi)
		b.Run(s.name, func(b *testing.B) {
			b.SetBytes(mean)
			b.ReportAllocs()
			for i := 0; i < b.N; i++ {
				if count(segs[i%len(segs)]) < 0 {
					b.Fatal("a bad checksum")
				}
			}
		})
	}
}

func BenchmarkCount(b *testing.B)      { bench(b, countRecords, 64) }
func BenchmarkCountWords(b *testing.B) { bench(b, countRecordsWords, 64) }
```

(`segments` makes `inputs` random segments of about 8 KB, of records whose payloads are `lo` to `hi` bytes long.) `benchstat` lines the two builds up:

```bash
go test -run '^$' -bench . -count 10 > go.txt
go test -tags goose -run '^$' -bench . -count 10 > goose.txt
benchstat go.txt goose.txt
```

`-count 10` gives `benchstat` ten samples of each (it gives no confidence interval for fewer than six). They are only as good as the machine was while they ran:

- **Interleave the builds.** One build's ten runs, then the other's, put whatever changed in between -- another job, the clock, the temperature -- on one side. Two builds of this kernel, written two ways whose machine code differed in one compare's operand order, measured 25–35% apart run so, a few minutes apart, with other jobs on the machine; interleaved, they were within 0.5% of each other. Build each test binary once and alternate:

  ```bash
  go test -c -o go.test . && go test -c -tags goose -o goose.test .
  for i in $(seq 10); do
      ./go.test -test.run '^$' -test.bench . >> go.txt
      ./goose.test -test.run '^$' -test.bench . >> goose.txt
  done
  benchstat go.txt goose.txt
  ```

- **Cycle through many distinct inputs,** so that the branch predictor cannot learn one. A kernel's branches follow its data -- here how many words and tail bytes each payload has -- and a predictor learns all of an input it sees over and over, 8 KB of records and more. `bench` takes the next of 64 distinct segments each time round; `BenchmarkCountOneInput` is `BenchmarkCount` on one segment, to show what that hides.
- **Measure inside and at the end of buffers.** A kernel that reads 8 bytes at a time takes its fast path only where 8 bytes are left, and its last bytes, at the end of a buffer, take the slow one: here every payload's tail. Measure inputs whose work is inside, in whole words, and inputs whose work is at the end, in the proportions real inputs have, or each on its own: `payload=1K-4K` is nearly all words, `payload=0-7` all tails. A parser called at positions in a buffer is measured at positions inside it and near its end.
- **Include a plain-Go control** with the kernel's algorithm. The kernel checksums 8 bytes at a time where the original takes one; `countRecordsWords` is the original doing what the kernel does, in Go. Against the original, the kernel's gain is the algorithm's and the generated code's together; against the control, the generated code's alone. Write the control as well as Go allows, or the generated code gets credit that is Go's: the control that slices its way along (`x ^= binary.LittleEndian.Uint64(p); p = p[8:]`) is up to 20% faster than one that indexes the words (`p[i : i+8]`). In one build, `benchstat -col .name goose.txt` puts the benchmarks side by side.

Interleaved as above, on an arm64 Neoverse N1 with Go 1.24, the time to count an 8 KB segment (all ± 1% or less):

| Payloads | Go original | Plain-Go control | Goose kernel | One input: original | One input: kernel |
|---|---|---|---|---|---|
| 0–7 bytes | 11.4 µs | 13.1 µs | 12.9 µs | 6.18 µs | 9.70 µs |
| 8–64 bytes | 6.47 µs | 4.82 µs | 4.30 µs | 5.11 µs | 2.48 µs |
| 1–4 KB | 6.09 µs | 1.62 µs | 1.04 µs | 6.23 µs | 1.04 µs |

Inside long payloads the kernel takes a sixth of the original's time: the algorithm takes it from 6.09 µs to the control's 1.62, and the generated code from 1.62 to 1.04, 36% less. At 8–64 bytes the kernel takes a third less than the original, 11% less than the control. At the end of payloads too short for a word, it is 13% slower than the original -- and the control 15% slower: the loss is the algorithm's, a word loop's test and a fold per record for a few bytes, and a kernel that is to win on such data checksums them a byte at a time. One input, over and over, would have told another story: the kernel taking 51% less time than the original at 8–64 bytes, where 64 inputs make it 34% less, and 57% more at 0–7, where it is 13% more; only long payloads, with few branches a byte, take the same time either way.

Both builds report 0 B/op and 0 allocs/op: a kernel's call allocates nothing.

## 7. Pitfalls

### Byte-assembled loads: one-load helpers

An integer ORed together from consecutive bytes is one load, of 2, 4 or 8 bytes, only where every byte's bounds check is gone: anything else is a load and a check per byte (the README's [Integers in bytes](../README.md#integers-in-bytes-one-load-one-store)). Write each width once, as a helper that slices its span and indexes the slice by constants, as `le32` and `le64` do: the slice's one check proves every byte, wherever the helper is called, at a position proven or not. A helper of indices instead (`s[i] | (s[i + 1] as u64) << 8 | ...`) is one load only where the compiler proves `i` through `i + 7` in range at its every call, from a loop's condition or a guard, and eight checked bytes elsewhere. Then check the Go: the example's file holds one `binary.LittleEndian.Uint32` and one `binary.LittleEndian.Uint64`, a load each (`test/run_go_tests.py` counts them).

### Shift width: cast before shifting

A shift computes at its left operand's type and masks its count to that type's width, so with `c` a slice of `u8`, `c[1] << 8` is `c[1] << 0` and `c[3] << 24` is `c[3]`: the bytes of a word, uncast, all land on its lowest byte, and what was to be one load is not one. Cast each byte to the word's type first, `(c[1] as u32) << 8`. `--target=go` warns of a shift by a constant count outside its type's width, with the cast that keeps the bits:

```
shift.goose:4: warning: shift of u8 by 8 is a shift by 0: at u8 the count is masked to 0..7 (§6.2); cast the operand first: `(c[1] as u16) << 8`
```

It is a warning, on stderr, and the compile goes on: a CI step that is to fail on one looks for `: warning:` in what the compile prints ([§9](#9-ci)). A count that is no constant (`c[k] << (8 * k)`) is not warned of.

### By-reference scalars in inlined helpers: prefer return values

A helper that takes a position by reference (`fn skip(s: const u8[:], i: i64&)`) leaves its caller's `i` a Go local, in a register, where the optimizer inlines the helper and the body uses `i` as a place only ([`DESIGN.md` §3.6](../DESIGN.md#36-the-fixed-frame-stack-gsframe)). Where it does not inline it -- at `-O0`, or a helper too big to inline at `-O2` -- `i`'s address is taken: `i` moves to a frame on the fixed-frame stack, the export takes a block of data stacks for it, and its entry points take one from a pool and move their arguments to Go's heap, where it was a kernel whose entry points Go inlined. A helper that returns the new position (`fn skipped(s: const u8[:], i: i64) -> i64`) keeps the export a kernel whatever is inlined. Check the generated Go: a kernel's entry points call its function and nothing else, and none takes a block (`AcquireStackBlock`).

### Globals in exports: pass slices instead

- **Any global with an initializer gives the library a start,** a `let` table of constants too: every entry point checks it (`gsLibStart`, whose slow path costs 57 of Go's inlining budget of 80, so that only the smallest entry points still inline), and the first call starts the runtime and maps the package's block. A table built in the function (`let hexval: u8[4] = [...]`, built at each call), or one handed in as a slice the Go side owns, leaves the kernel without a start.
- **A global on a data stack of its own** (a growable arena), which an export's calls grow or hand a reference to, makes every block a program instance ([`DESIGN.md` §3.5](../DESIGN.md#35-export-fn-and-extern-fn-in-go-mode)): a pooled call copies the package's globals into its block before it runs (90 ns for a few elements, where a call on a block that keeps them takes 16 ns), and a result that views them is copied to Go's heap. Pass the arena as a slice, or keep a block (`...WithStack`), which keeps its copy across calls.
- **A `var` an export writes** is shared by every call, in a program without instances: such an export is not safe to call from two goroutines at once.

### A library, not a program: `--library`

A `.goose` file with `export fn`s and no `fn main()` is a library: the package has no `init` and no `func main`, importing it does nothing -- it maps no memory and leaves every goroutine's fault handling alone -- and the first call of an entry point starts it, the globals' initializers included. Keep a `fn main()` in the source, to run the kernel on its own, and the file is a program unless `--library` says otherwise: its globals are initialized by a `func main` that a host never runs, so that an export reading one computed by an initializer reads its zero value, and `exit()` ends the host's process. Compile a kernel with `--library` whenever its source has a `fn main()`; the `goose-flags` line of the header says `--library` of every file that is one. What a library's runtime does, it does in its calls, on the caller's goroutine: the start at the first call, where there is one, and in each call that runs on a block, `debug.SetPanicOnFault` turned on for the call alone, the caller's setting given back as it returns or panics. A kernel over slices has neither.

### The `go` line of `go.mod`

The generated Go uses Go 1.21's builtin `min` and `max` wherever the program uses std's: a module whose `go.mod` says `go 1.20` or less fails to build it (`built-in max requires go1.21 or later`).

## 8. Size

What the generated file adds to a binary is its program's code and the part of the runtime that code uses. On linux/arm64 with Go 1.24, a program that prints `records.Count` of a file, and one that calls the README's [`stats`](../README.md#calling-goose-from-go-export-fn---library---go-package) in place of a Go `stats` of `strings.Fields` and `slices.Sort`, built each way (`size`'s text, data and bss, what the binary loads; and the file):

| Library | Without the tag | With `-tags goose` | The file, without and with |
|---|---|---|---|
| `examples/records`, a kernel over slices | 1,660,720 bytes | 1,663,940 bytes (+3.1 KB) | 2,361,214 and 2,363,430 bytes |
| `stats`, which runs on blocks of data stacks | 1,584,525 bytes | 1,603,889 bytes (+18.9 KB) | 2,258,399 and 2,336,214 bytes |

A kernel over slices holds next to none of the runtime: no data stacks, pools or text forms (`test/go/kernels/`), here a slice type, an error type and its messages, and the checks' failures, 286 lines of Go with its own function, whose code is 560 bytes. A library that runs on blocks holds their runtime: the regions and their guard gaps, the pool, the fault scope, which imports `runtime/debug` -- 1,612 lines for `stats`. The file's size moves with its debug information and its padding too: stripped (`-ldflags='-s -w'`), it grows in whole 64 KB segments on arm64 Linux, `stats`'s by one, `records`'s by none. To see it for yourself:

```bash
go build -o app . && go build -tags goose -o app-goose . && size app app-goose
go tool nm -size -sort size app-goose | grep ' example.com/records\.'
diff <(go list -deps . | sort) <(go list -tags goose -deps . | sort)
```

Two programs in one package share one runtime with `--go-runtime-file`, and every package of a binary one with `--go-runtime=import` (the README's [runtime](../README.md#the-runtime-in-a-go-package---go-runtime---go-runtime-file---go-unexported)).

## 9. CI

```bash
# The default build: the Go original, as it always was.
go vet ./... && go test ./...

# The checked-in Go is its source's, by the compiler the header names (a
# --check by another compiler finds every file stale: pin the version), and
# the compile warns of nothing.
set -o pipefail
goose --target=go --check --go-build goose --go-package records --go-unexported --go-no-withstack \
    -o records_goose.go _goose/records.goose 2>&1 | tee goose-check.txt
! grep ': warning:' goose-check.txt

# The Goose build: every test, with Go's pointer checks too, and some fuzzing.
go vet -tags goose ./... && go test -tags goose ./...
go test -tags goose -gcflags=all=-d=checkptr ./...
go test -tags goose -run '^$' -fuzz '^FuzzCountRecords$' -fuzztime 30s
```

The `--check` takes the `//go:generate` line's options; `go generate -tags goose ./... && git diff --exit-code` checks the same with the line itself. `test/run_go_tests.py` runs all of this on the example (but fuzzing 2,000 inputs, not 30 seconds' worth), and the mutation checks of [§4](#4-proving-equivalence). It writes the example's generated file with `go generate`, rather than finding it checked in: the compiler is built from each commit here, and the header of a file checked in would name the commit before.
