# Views of Go data in export fns: Go slices and structs, read in place

<details>
<summary>Table of contents</summary>

- [1. Decisions](#1-decisions)
- [2. The boundary today](#2-the-boundary-today)
  - [2.1 What crosses](#21-what-crosses)
  - [2.2 An argument lived only to its last use (fixed by this change)](#22-an-argument-lived-only-to-its-last-use-fixed-by-this-change)
  - [2.3 An export fn may keep its argument past the call](#23-an-export-fn-may-keep-its-argument-past-the-call)
  - [2.4 `const` stops at a held slice](#24-const-stops-at-a-held-slice)
- [3. The view type](#3-the-view-type)
- [4. The entry point](#4-the-entry-point)
- [5. What must be guaranteed, and who checks it](#5-what-must-be-guaranteed-and-who-checks-it)
- [6. gsEscape, the race detector and checkptr](#6-gsescape-the-race-detector-and-checkptr)
- [7. Alternatives considered](#7-alternatives-considered)
- [8. Measurements](#8-measurements)
- [9. The follow-up issue](#9-the-follow-up-issue)
- [10. Open questions](#10-open-questions)

</details>

Design note for issue #30, October 2026, written against `25ab409`. The
question was whether an `export fn` can read a Go caller's structs that hold
Go slices where they are, without copying them; what the checker and the Go
backend must then guarantee for Go's collector to stay correct; and what the
alternatives cost. The answer proposed here is yes, with no new syntax: a
view is a Goose struct laid out as Go lays out the caller's type, read
through a `const` slice or reference parameter, which the entry point checks
against the caller's type once and keeps alive for the call.

Working out what the collector needs turned up two gaps in the boundary the
Go backend already has, for the slices and references that cross it today:
an argument lived only to its last use (§2.2), which this change fixes, and
an export fn may keep an argument past the call (§2.3), which needs the
front end. Nothing else here is implemented. The measurements come from a
prototype that is not part of this change: the compiler with the boundary's
check relaxed and a view parameter taken as the host's `[]R` (§8). "Spec"
sections are those of `goose_spec.md` (`vendor/goose/docs/`), and DESIGN.md
is this repository's.

## 1. Decisions

* **A view is a Goose struct laid out as Go lays out the caller's.** Goose
  structs are packed, with explicit pads (spec §3.2), so a Goose struct can
  state Go's layout byte for byte. A Go slice `[]T` is three words: data,
  length and capacity. A Goose `const T[:]` is the first two, so the Go
  slice is the Goose slice followed by `pad 8`, which skips the capacity. A
  Go `string` is two words, exactly a `const u8[:]`. Nothing new is needed
  in the language: the field *is* a `const T[:]` in Goose (`for t in
  r.tags`), and the slice with its pad is the declaration that it is a Go
  slice (§3).
* **A view crosses only as a `const S[:]` or `const S&` parameter of an
  export fn.** The entry point is generic in the caller's element type, so
  a host passes its own `[]Record`, from its own package, with its tags,
  methods and unexported fields. The first call with each type checks, with
  `reflect`, that the type is laid out as the view says (§4).
* **A view is read-only at every depth, and nothing of it outlives the
  call.** These are the checker's two guarantees, and today's boundary has
  neither: a `const` parameter's held slices are writable (§2.4), and an
  export fn may store an argument in a global (§2.3).
* **A view may sit on a data stack during the call.** With those two
  guarantees, and an entry point that keeps its argument alive, a copy of a
  view where the collector does not look is a copy of a pointer the
  collector sees in the caller's own memory. Goose makes such copies on its
  own (§5).
* **One `gsEscape` and one `runtime.KeepAlive` per parameter cover all a
  view reaches.** Go never lets its heap point into a goroutine's stack,
  and the caller's records keep their slices' arrays alive (§6).
* **A view is as fast as Go's own loop, if the backend reads its fields
  with typed loads.** Go aligns every field, so the byte copies the backend
  uses for Goose's packed layouts, which may be unaligned, are not needed.
  With typed loads the prototype's kernel runs at the speed of Go's own
  loop: about 2× faster than today's split at 4 tags a record, 1.25× at 16,
  and the same at 128 (§8).

## 2. The boundary today

### 2.1 What crosses

The Go backend's export entry points take Go's own values in place
(DESIGN.md §3.5): a `[]byte` for `u8[:]`, a `string` or `[]byte` for
`const u8[:]`, a `[]T` for any other `T[:]`, a `*T` for `T&`, and Go's types
for scalars and fixed-size values. The checker lets only flat values cross
(spec §7.10, §7.11, `ExternParamOk`), so a host record that holds a Go
slice cannot cross, whatever Goose type stands for it:

```go
type Record struct {
	ID   int64
	Tags []int64
}
```

```goose
struct Record { id: i64, tags: const i64[:], pad 8 }

export fn self_tagged(recs: const Record[:]) -> i64 { ... }
// error: export fn self_tagged: parameter recs of type const Record[:]
//        cannot cross to C: values holding references do not cross
```

A slice of slices is refused the same way ("nested references and slices do
not cross"). Without the pad, a Goose struct would be 24 bytes where Go's is
32, since a Goose slice is two words and a Go slice three. So the validation
routine of the issue keeps its outer loop in Go and makes one export call
per record for the inner loop over that record's flat `Tags`:

```go
// export fn has_tag(tags: const i64[:], id: i64) -> bool
for i := range recs {
	if lib.HasTag(recs[i].Tags, recs[i].ID) {
		n++
	}
}
```

### 2.2 An argument lived only to its last use (fixed by this change)

Since Go 1.8 a function's argument is dead after its last use (the Go 1.8
release notes, *Argument Liveness*), and a caller's last use of a slice may
be the very call that hands it to an entry point. The entry point hands its
address to the Goose code, which may copy it where the collector does not
look: onto a data stack (`kept.push(xs)`), into the fixed-frame stack, or
into a struct whose Go value is its bytes (`let h = Held { n: 1, xs: xs }`
is a `[24]byte`). Once the Goose code's own Go-typed copy is dead as well,
nothing the collector sees refers to the caller's memory, and the collector
frees it while the call is still reading it.

`test/go/export/keepalive/` reproduces this for every kind of parameter that
views Go's memory, through both entry points. Each export fn keeps its
argument only in such a place, calls back into Go to run the collector, and
then reads the argument again. Before this change every case failed, at -O2
and at -O0: the finalizer of the argument's memory ran during the call. Built
with `-d=checkptr`, the call died of a bad pointer in Go's heap. Each entry
point now ends its call with a `runtime.KeepAlive` of every such argument,
which allocates nothing (`test/go/export/`'s `testing.AllocsPerRun` checks
still pass):

```go
func KeptSumWithStack(gs *GsStackBlock, xs []int64) int64 {
	gsLibStart()
	defer debug.SetPanicOnFault(debug.SetPanicOnFault(true))
	gs.EnsureStacks(1)
	gs_r := kept_sum_g(gs, 0, GsSliceOf(xs))
	runtime.KeepAlive(xs)
	return gs_r
}
```

### 2.3 An export fn may keep its argument past the call

The checker checks an export fn as it checks any function that no call
reaches (`CheckUnreached`, `CheckStandalone` in `typecheck.h`): each
reference, slice or holder parameter is rooted "at a global of its own". So
the body may store the argument in a global, and this compiles under both
backends:

```goose
struct Held { n: i64, xs: const i64[:] }
var backing: i64[1] = [0];
var held: Held = Held { n: 0, xs: backing };

export fn hold(xs: const i64[:]) { held.xs = xs; }
export fn held_sum() -> i64 { var s = 0; for x in held.xs { s += x; } s }
```

In Go, `held` is a `[24]byte`. Once `Hold(buf)` has returned and the host has
dropped `buf`, the collector frees it, and `HeldSum()` reads freed memory: in
the experiment, a garbage sum, and with `-d=checkptr`, a bad pointer in Go's
heap. In a program with workers an export call's globals live in its
block's program instance, which is `mmap`ed, so there a global of any type
hides the pointer this way. A global that Go does scan (`var keep: const
i64[:]`, a `GsSlice` package variable) keeps the memory alive instead, but
then the Goose code reads whatever the host has written there since.

The fix belongs to the front end: root an export fn's parameters at its
caller, in a variable that every global outlives, so that storing one in a
global is the same error it is for any function called with a local ("does
not outlive the destination", spec §9.2). That would serve C hosts as well,
since a global can outlive their buffers just the same. Spec §7.10 asks C
not to keep what Goose lends it, but nothing asks Goose not to keep what C
lends it.

### 2.4 `const` stops at a held slice

```goose
struct Record { id: i64, tags: i64[:], pad 8 }

fn poke(recs: const Record[:]) { recs[0].tags[0] = 5; }   // compiles, and writes
```

A `const` parameter makes the records' own bytes read-only (re-pointing
`recs[0].tags` is "cannot assign through this path"), but not what a slice
held in them views: a slice is as writable as its own type says (spec
§9.5). Through a view, such a write lands in the caller's memory. It may
land in a Go string's bytes, which Go never writes (a literal's are in
read-only memory). Where the elements hold slices of their own, it may
re-point one of those: the backend stores a slice as a byte copy, so that is
a Go pointer written with no write barrier, which the collector may never
see, or a view of Goose's own memory, which the caller keeps after the call.
So a view type must be `const` at every depth, and the boundary has to check
that itself (§5).

## 3. The view type

```goose
// Go: struct { ID int64; Tags []int64; Name string; OK bool }
struct Record {
    id: i64,
    tags: const i64[:], pad 8,   // a Go []int64: data, length, and the capacity, skipped
    name: const u8[:],           // a Go string: data, length
    ok: bool, pad 7,             // Go's tail padding
}
```

Go lays a struct out in declaration order, puts each field at the next
offset its alignment divides, and rounds the size up to a multiple of the
largest alignment. Goose lays a struct out packed: `pad n` inserts n bytes,
and a bare `pad` aligns the next field to its width (a slice to 8). A view
type spells Go's gaps out as pads, and the backend checks that it did: each
field must sit at an offset that its Go alignment divides, and the size must
be a multiple of the struct's alignment. The backend already makes most of
this test when it decides whether a struct is a native Go struct (DESIGN.md
§3.2). A pad also stands for Go fields the Goose code does not read: a map,
an interface, a mutex, or a field the kernel ignores.

| A view's field | The caller's Go field |
|---|---|
| `i8` … `i64`, `u8` … `u64`, `f32`, `f64`, `bool` | a Go type of that kind and width, named or not: `int64` or `int` for `i64`, `uint64`, `uint` or `uintptr` for `u64`, … |
| `const T[:]`, then `pad 8` | `[]E`, where E is a view of T, of the same size |
| `const u8[:]` | `string` (followed by `pad 8`, a `[]byte`) |
| `T[k]` | `[k]E`, where E is a view of T |
| a view struct | a struct, named or embedded |
| `pad n` | anything, n bytes of it |

Nothing else may appear in a view, at any depth. That rules out:

* a slice or reference that is not `const` (§2.4);
* a plain reference `T&`, since a Go pointer may be nil. A `const T?` for a
  Go `*E`, whose nil is a null, can come later;
* enums and ADTs, since a Go integer can hold a value that tags no variant;
* limited arrays, since Go keeps no invariant on their length;
* varints, relative references and index types, as at every foreign
  boundary (spec §3.11).

A view crosses behind a `const` slice or reference, never by value (§6.1),
and is never a result: no result holds a reference but a `const u8[:]`
(spec §7.11), which reaches the host as a `[]byte` the collector sees.

Inside Goose a view is an ordinary value of an ordinary struct. Everything
Goose does with a `const` slice it can do with `r.tags`: iterate over it,
index it (with a bounds check, or none where BCE proves it), slice it,
compare it, print it, hand it to a function, push it onto a data stack.
Nothing changes in the optimizer, in BCE or in the stack analysis. The
backend should change one thing, how it reads a view's fields (§8).

## 4. The entry point

```go
func SelfTaggedWithStack[T0 any](gs *GsStackBlock, recs []T0) int64 {
	gsLibStart()
	gsViewOf[T0](&gsView_Record) // at the first call with T0: is it laid out as Record?
	_ = gs
	gs_r := self_tagged_g(GsSliceOf(recs))
	runtime.KeepAlive(recs)
	return gs_r
}
```

The parameter's element type is a type parameter because no generated Go
type could be the host's: `[]host.Record` converts to no other package's
slice type, and the Goose compiler does not know any Go type to name.
`gsViewOf` is a runtime function that uses `reflect`. At the first call with
each type, it checks that the type is a struct of the view's size; that at
each of the view's offsets the type has a field of the kind the table of §3
pairs with the Goose field, recursively; and that the field is named as the
Goose field is, ignoring case and underscores (`ID` for `id`, `UserIDs` for
`user_ids`). Without the names, two swapped `int64` fields would match the
layout and deliver the wrong data. A mismatch panics with a `*GsError` that
names the field ("goose.swapped's field at offset 0 is Tags, Goose's id"),
at every call with that type. A match is cached. With the cache looked up
through `reflect`, as prototyped, each later call pays 13 ns for the lookup;
comparing the type's descriptor word instead makes it 1.7 ns.

`GsSliceOf` hands the slice's data to `gsEscape`, and `runtime.KeepAlive`
(§2.2) keeps the caller's records alive for the call, and through them the
arrays of their slices. A view needs nothing more of its own (§6).

## 5. What must be guaranteed, and who checks it

| Guarantee | Today | With views | Checked by |
|---|---|---|---|
| A view is read-only at every depth | a `const` parameter's held slices are writable (§2.4) | every slice and reference in a view type is `const`, as is the parameter | the Go backend, at the boundary |
| Nothing of an argument outlives the call | an export fn may store it in a global (§2.3) | an export fn's parameters are rooted at its caller | the front end (`CheckStandalone`) |
| Nor through a result, a worker, a queue or an extern fn | a result holds at most a `const u8[:]`, which reaches Go as a `[]byte` the collector sees; only flat values reach workers and queues; an extern fn takes slices and references of flat values only, and keeps none past its call (DESIGN.md §3.5) | as today | the front end |
| The collector keeps alive what the call reads | an argument lived to its last use (§2.2) | `runtime.KeepAlive` of each argument (this change) | the Go backend |
| No view points into a goroutine's stack | `gsEscape` | as today, once per parameter (§6.1) | the Go backend |
| The caller's type has the view's layout | (nothing crosses) | `gsViewOf`, once per type (§4) | the runtime |

**Data stacks.** The issue proposes that views should not reach data
stacks. Given the first two rows and `KeepAlive`, a view on a data stack, in
the fixed-frame stack or in a struct's bytes is a copy of a pointer that the
collector sees where the caller keeps it, in memory nothing changes during
the call. No such copy is read once the call is over, since nothing global
can hold one. A rule against them would cost the language much and buy
nothing. Goose makes such copies on its own (the backend copies the binder
of `for r in recs` into the fixed-frame stack, §8), and code that pushes a
record's slice onto a data stack is ordinary code.

**The collector's invariant**, then: every Go pointer the Goose code holds is
either in memory the collector scans (a Go local's `GsSlice` or pointer), or
a copy of a pointer it scans in the caller's memory, which the entry point
keeps alive and which nothing writes during the call. The Goose code writes
no Go pointer into the heap or into a global the collector scans (views are
read-only, and no global can hold one). It writes them only to its
goroutine's stack, which needs no write barrier, and to memory the collector
never scans, so no write barrier is ever missing.

**The host's obligations**, like C's in spec §7.10: no goroutine writes the
memory a call views while the call runs (that is a data race, §6.2), and
neither does any extern fn the call reaches. An `append` that moves a
record's `Tags` would leave the call's copy of the old header pointing at
memory that nothing keeps alive.

## 6. gsEscape, the race detector and checkptr

### 6.1 gsEscape

A goroutine's stack moves when it grows, and Go adjusts only the pointers it
knows about, which excludes every pointer on a data stack. `gsEscape` makes
Go's escape analysis keep what an entry point is handed off the stack. One
call per parameter is enough, because Go never lets its heap point into a
stack: once the caller's outer array is on the heap, every slice and string
header in it views the heap or static data. In the prototype, a host's
`[]Record{{ID: 2, Tags: []int64{1, 2}}, ...}` and both of its `[]int64`
literals move to the heap where they are handed to an entry point, and stay
on the stack where they are handed to a Go function that only reads them. A
view passed by value would need a `gsEscape` of each pointer it holds, which
is one reason views cross only behind `[:]` or `&`.

### 6.2 The race detector

`-race` instruments the generated Go like any other Go, so it checks every
load through a view of the caller's memory. In the prototype, a goroutine
that writes a record's `ID` while a call reads the records is reported at
the kernel's read, and the calls with no writer are not reported. Data
stacks and the fixed-frame stack are not checked, as today: the race runtime
checks only addresses in Go's heap arena and data segment (`isvalidaddr`),
and those stacks are the block's own. `-race` also turns `checkptr` on.

### 6.3 checkptr

`-d=checkptr` checks each conversion of an `unsafe.Pointer` to a `*T`: the
pointer must be aligned for `T` if `T` holds pointers, and the converted
memory must not straddle two heap objects. `unsafe.Add` itself is not
checked; only a conversion from `uintptr` is. Go aligns every field of a
view, so a typed load of a view's slice header, `*(*GsSlice)(p)`, passes,
while the byte copies the backend uses for Goose's own packed layouts
(`gsLd_sl_i64`) have no alignment to check. A load from a freed heap object
is the checker's "found bad pointer in Go heap", which is how §2.2 and §2.3
showed up under `-d=checkptr`. So the suite's `-d=checkptr` runs catch a
view that the collector freed, but only where a test runs the collector
during the call, as `keepalive/`'s `collect` does.

## 7. Alternatives considered

**Today's split.** One export call per record for the inner loop. It needs
no new rule, and costs a call per record: 1.6× Go's own loop at 4 tags a
record, 1.2× at 16, and nothing measurable at 128 (§8). The outer loop's
logic stays in Go.

**Adapters that flatten the caller's data.** Generated Go walks the records
and copies every id and tag into flat arrays (in CSR form: the ids, the end
of each record's tags, and all the tags), and one export call takes those
arrays. If the arrays are on a data stack, the Goose code holds no Go
pointer at all, and only today's rules apply. But every call copies
everything, whatever the kernel reads, and the kernel is written for the
flat shape, not for the record. It runs at 1.6–2.7× Go's loop with arrays
kept across calls, and at 13–16× with fresh arrays, which allocate 2 to 7 MB
a call (§8). The adapter also has to be written from the caller's Go type,
which the Goose compiler does not know: the problem that naming the
caller's type at compile time (below) has to solve, without its benefit.

**Adapters that copy headers.** Copy each record's scalars and slice headers
into Goose's own layout (24 bytes a record), and leave the elements where
they are. That is one pass over the records (1.0–1.4× Go's loop with a fast
kernel), and the Goose code then holds Go pointers in memory the collector
does not scan, so it needs every rule of §5 anyway. A view is the same thing
with the copy left out.

**Go types generated for the host.** The backend could emit `type Record
struct { Id int64; Tags []int64 }` and take a `[]Record` of that type,
which Go's compiler checks with no reflection. But the host's own type
is a different one, and a `[]host.Record` does not convert: existing code
would have to switch its type to the library's (an alias, on which the
host's package cannot declare methods), or copy.

**Naming the caller's type at compile time.** A flag such as `--go-view
Record=example.com/model.Record` would let the generated package import the
host's type and check its layout as Go compiles it (`unsafe.Offsetof`, and
assignments of its fields to the view's types), at no cost per call. But it
needs the Go field names as well, which the Goose names do not give (`ID`
for `id`); the type's package must not import the library; and unexported
fields cannot be named from another package. It could later complement §4
for a host that wants the error at compile time.

**New syntax.** A slice type with Go's layout (`const T[:go]`), or a struct
in std, `go::slice<T> { s: const T[:], pad 8 }`. Either is explicit, but the
first changes upstream's parser, and the second makes every use
`r.tags.s`. The pad says the same thing with what Goose already has.

## 8. Measurements

The prototype is this change's compiler with two edits: the check of
`CheckExport` skipped for a `const` slice or reference of a struct, and such
a parameter taken as the host's `[]T0` or `*T0`, with `gsEscape` and
`KeepAlive` as above. The kernel counts the records that list their own id
among their tags, as Go's loop of §2.1 does, over a host's `[]Record` of
random ids and tags, where one record in 8 has its id last. It ran on an
arm64 Neoverse N1 with Go 1.24, as the median of 6 runs, and every variant
allocates nothing per call unless noted. The prototype's tests also passed
at -O2 and -O0, with `-d=checkptr` and under `-race`. They covered the
variants agreeing with Go's loop, views on a data stack while the collector
runs, records the host builds where it calls, and the layout check's
messages.

Each figure is the time of a call relative to Go's own loop over the same
records. All of them come from one test binary, in which the two kernels
with typed loads had those loads substituted by hand into the generated Go:

| A call over N records of K tags | 10000 × 4 | 10000 × 16 | 1000 × 128 |
|---|---|---|---|
| Go's own loop, per record | 5.7 ns | 15.4 ns | 108 ns |
| Today's split: an export call per record | 1.56 | 1.16 | 1.01 |
| Flattened into arrays kept across calls | 2.51 | 2.73 | 1.59 |
| Flattened into fresh arrays (2.2, 7.3, 5.3 MB a call) | 13.4 | 14.4 | 15.9 |
| Headers copied into Goose's layout, typed loads | 1.36 | 1.15 | 1.02 |
| View: `for r in recs`, as the backend compiles it today | 3.71 | 4.42 | 4.02 |
| View in place, headers byte-copied (`gsLd_sl_i64`) | 1.54 | 1.18 | 1.02 |
| View in place, typed loads | 0.80 | 0.93 | 0.99 |

Two things in the backend's code decide the outcome:

* **`for r in recs`** copies each record into the fixed-frame stack, because
  reading a field of a value held as bytes takes its address (DESIGN.md
  §3.6). It then re-reads `r.tags` from that copy with a 16-byte byte copy
  at every step of the inner loop. Reading the record in place, as `for &r
  in recs` or `let ts = recs[i].tags` does, takes neither. The follow-up
  should compile a `for` over a view whose body writes nothing of `r` the
  way it compiles `for &r`.
* **`gsLd_sl_i64`**, the byte copy the backend uses to read a slice that
  Goose's packed layouts may leave unaligned, costs 1.9× at 4 tags and 1.3×
  at 16. A view's slices are where Go aligns them, so `*(*GsSlice)(p)` is
  safe there and passes `checkptr`.

The layout check costs nothing measurable at these sizes. Go's own loop
varies by about 10% between test binaries with code layout alone, which is
why every figure is a ratio within one binary.

## 9. The follow-up issue

The text for the follow-up issue that the acceptance criteria ask for. It is
here because this change cannot open an issue.

> **Go backend: read-only views of a Go caller's structs that hold slices,
> in export fn parameters**
>
> `docs/design/go_views.md` (issue #30) is the design. This issue builds it.
>
> **Front end** (`vendor/goose`: upstream, or patched copies of
> `typecheck.h` and `typecheck_calls.h` in `src/`, which `main.cpp`'s include
> order lets shadow upstream's):
>
> 1. Root an export fn's reference, slice and holder parameters at its
>    caller, in a variable that every global outlives, instead of "at a
>    global of its own" (`CheckStandalone`), so that an export fn storing an
>    argument in a global is refused (design §2.3). This applies to every
>    target.
> 2. Add a `TypeCheck` option, set by the driver for `--target=go`, that
>    lets a `const S[:]` or `const S&` parameter of an export fn be of a
>    struct `S` holding slices (`CheckExport`), and refuses every other
>    value that is not flat, as today. The Go backend checks `S` against
>    design §3.
>
> **Go backend** (`src/codegen_go_export.h`, `codegen_go_types.h`):
>
> 3. View types: check design §3's rules at each export fn parameter that
>    is a view, with errors that name the field and the pad Go's layout
>    needs ("tags: a Go slice is three words, so follow it with pad 8").
> 4. Entry points: take the parameter as `[]T0` or `*T0` for a type
>    parameter `T0 any`, call `gsViewOf[T0](&gsView_<S>)` first, and keep
>    `gsEscape` and `runtime.KeepAlive` as today. Emit a `gsView_<S>` table
>    for each view type: each field's offset, kind, element table and
>    normalized name.
> 5. Read a view's fields with typed loads, `*(*GsSlice)(p)` rather than
>    `gsLd_`, since Go aligns every field of a view (design §8: 1.9× at 4
>    tags a record).
> 6. Compile a `for r in recs` over a view whose body writes nothing of
>    `r` as `for &r in recs` is compiled, reading the element in place
>    (design §8: 3.7–4.4× today).
>
> **Runtime** (`go_runtime` in `codegen_go_runtime.h`):
>
> 7. Add `gsViewOf[T any](v *gsViewType)`, the check of design §4: at the
>    first call with each type, using `reflect`, cached by the type's
>    descriptor word. A mismatch panics with a `*GsError` that names the
>    type, the field and the offset.
>
> **Tests**:
>
> * `test/go/export/views/`: view types with every kind of field in design
>   §3's table (scalars of every width; `const T[:]` with `pad 8`, of
>   scalars and of view structs; strings and `[]byte`; fixed arrays; nested
>   and embedded structs; pads over a map, an interface and a mutex), viewing
>   host types with exported and unexported fields, struct tags and methods.
>   Export fns over `const S[:]` and `const S&`, checked against Go's own
>   loops at -O2 and -O0 and with `-d=checkptr`, by the runner. Both entry
>   points allocate nothing (`testing.AllocsPerRun`). Views are kept on a
>   data stack, in the frame and in a struct while `collect` runs the
>   collector, as in `test/go/export/keepalive/`. Many goroutines call at
>   once over shared records. Nil and empty slices. Each layout mismatch (a
>   wrong offset, a missing `pad 8`, a wrong element type or width, two
>   same-typed fields swapped, a wrong size) panics with a `*GsError` that
>   names it.
> * The refusals, in `test/go/unsupported/` or a section of their own: a
>   view parameter that is not `const`; a slice in a view type that is not
>   `const`; a view by value; an enum, a `T&`, a limited array or an index
>   type in a view; a field Go would align elsewhere; and, from the front
>   end, an export fn that stores its argument in a global.
> * Benchmarks of design §8's kernel as a view, as today's split and as
>   Go's own loop.
> * The C suite (`vendor/goose/test/run_tests.py`) for the front end's
>   changes, which reach the C backend too.
>
> **Docs**: DESIGN.md §3.5, and the README's "Calling Goose from Go".
>
> **Not in scope**: optional references for Go pointers (`const T?` for a
> `*E`), writes through a view, views as extern fn parameters, and a
> binding checked at compile time (design §7, "Naming the caller's type at
> compile time").

## 10. Open questions

* **The front end.** Should the two changes of §9 go upstream to Goose, or
  live in this repository as shadowing copies of the two headers? The
  rooting change of §2.3 is a fix for C hosts too.
* **Names.** Should `gsViewOf` require the Go field names to match the Goose
  names, as proposed, or check only offsets and kinds?
* **Reflection in the runtime.** Is `reflect` in the generated runtime
  acceptable, or should the compile-time binding come first?
