# goose.go

A Go compiler backend (`--target=go`) and pure-Go `mmap` Data Stack runtime for Wouter van Oortmerssen's [Goose programming language](https://github.com/aardappel/goose).

`goose.go` shares the entire upstream Goose frontend (`vendor/goose`: parser, typechecker, reference root & lifetime analysis, monomorphizer, inliner, bounds-check elimination, and static Data Stack slot assignment) and emits dependency-free, `gofmt`-clean Go (`CGO_ENABLED=0`) instead of C.

---

## Why compile Goose to Go instead of calling `goose-c` via `cgo`?

When integrating Goose into Go services, compiling `.goose` to C and calling it across `cgo` hits two walls:
1. **`cgo` transition overhead (~40–60 ns/call):** Switching from a growable Go goroutine stack to the OS thread `g0` stack and locking the Go scheduler `M` dwarfs short kernels.
2. **Boundary copying & Go GC pressure:** Passing Go `[]T`, `string`, or `[]string` across `cgo` (or materializing C results back into Go) forces copies and `runtime.mallocgc` heap allocations.

`--target=go` compiles typechecked, bounds-check-eliminated Goose directly to pure Go:
- **Zero `cgo` overhead:** Generated functions are ordinary Go functions that inline directly into Go callers (`go build -gcflags=-m`), viewing Go `[]T`, `[]byte`, `string`, and `[]string` in place.
- **Zero Go GC heap allocations or write barriers:** Dynamic Data Stacks live in `syscall.Mmap` virtual memory outside the Go heap and use `noscan` representations (`int64` offsets and raw bytes).
- **Compile-time memory safety:** Goose's typechecker proves all reference roots, aliasing, lifetimes, and slice bounds before Go code is emitted.

---

## How `goose.go` (`--target=go`) Differs from `goose-c` (`--target=c`)

Targeting Go's runtime (moving goroutine stacks, M:N thread scheduling, garbage collector write barriers, and `gc`'s AST-node inlining heuristics) required seven core architectural departures from `goose-c`:

| Area | `goose-c` (`--target=c`) | `goose.go` (`--target=go`) |
|---|---|---|
| **1. Data Stack state** | Global `void *gs_stks[N]` (or OS-thread-local for workers) | Pooled `*GsStackBlock` leased per call (`FuncName`) or caller-held (`FuncNameWithStack(gs, ...)`), safe across goroutine migration |
| **2. Fixed-size locals & moving stacks** | C call stack (never moves) | **Two-tier split:** pure scalars/slices stay in native Go registers; address-taken locals & fixed arrays live on `gs.Frame` (an `mmap` frame stack that never moves when Go reallocates a goroutine stack) |
| **3. Block-free leaf kernels** | Always links C runtime startup | Functions that touch no Data Stack and take no address of a local omit `*GsStackBlock`, `GsEscape`, and fault scopes, compiling to leaf Go functions that `gc` inlines into the caller |
| **4. Pointer & GC representation** | Raw `T*` pointers | Relative references are `GsRref` (`int64` byte offset from stack base); composite frame slots are raw `[N]byte` so Go's GC scans zero pointers and emits zero write barriers |
| **5. Host boundary (`export fn`)** | C ABI (`gs_slice` struct by value; no nested slices; eager `init`) | Idiomatic Go (`[]T`, `[]byte`, generic `~string \| ~[]byte` + monomorphic `String`/`Bytes` variants, zero-copy `[]string` via `const (const u8[:])[:]`), lazy first-call init with zero `init()` side effects |
| **6. Faults & aborts** | Process-wide `exit(1)` or `SIGSEGV` | Goroutine-scoped `panic(*GsError)` + scoped `debug.SetPanicOnFault(true)` so Data Stack guard-page faults and `assert` failures can be recovered by the host Go service |
| **7. Codegen tuned for Go's `gc` compiler** | Relies on `clang`/`gcc -O2` to inline & fold everything | Syntactic `panic(&GsError{...})` in bounds checks (`GsIdx`) to avoid register spills, direct `*(*uint64)(p)` wide loads (`GsLe64`/`GsBe64`) that stay inline inside >5,000-node functions, and dead-code-stripped runtime emission |

### Key Design Highlights

1. **Goroutine-Safe Pooled `*GsStackBlock` (No OS Thread-Local Storage):**
   In `goose-c`, Data Stack pointers live in C globals or OS thread-local storage (`__thread`). Because Go's M:N scheduler multiplexes thousands of goroutines onto OS threads and migrates them at preemption points, OS thread-local storage cannot be used safely. Instead, each `export fn` either leases a `*GsStackBlock` from an on-demand pool for the duration of the call (`FuncName`) or accepts a caller-owned `*GsStackBlock` (`FuncNameWithStack(gs, ...)`). Only goroutines *actively executing* a non-leaf Goose call hold a block—if 10,000 goroutines exist in a Go server but at most 16 run a Goose function concurrently, at most 16 `*GsStackBlock`s are ever mapped.

2. **Two-Tier Local Allocation (`gs.Frame` vs. Register Locals):**
   Go grows a goroutine's stack by allocating a new stack segment and moving existing frames, which invalidates raw `unsafe.Pointer` references to Go stack variables (`-gcflags=all=-d=checkptr`). `goose.go` keeps pure scalar and slice variables in native Go locals (CPU registers), including inlined by-ref scalar parameters, while placing any local whose address escapes into a Goose reference (`&local`, fixed arrays `T[N]`, or composite structs with pointer-bearing fields) onto `gs.Frame`—a single contiguous `mmap` call-frame stack inside `*GsStackBlock` (64 MB virtual reservation by default in libraries, backed by physical RAM only for touched pages) that bumps on function entry (`gs_sp := gs.Frame; gs.Frame += frameBytes`) and rewinds in one instruction on return (`gs.Frame = gs_sp`).

3. **Runtime Fork-Join Concurrency (`worker` / `spawn` / `join` → Go Goroutines):**
   Goose's native concurrency primitives (`worker fn`, `spawn`, and `join`, including spawning a dynamic number of workers in a loop at runtime) compile to Go goroutines (`runtime/go/workers.go`). Each `spawn` allocates a child `*GsStackBlock`, deep-copies the worker's inputs onto the child's Data Stacks, and runs the worker in a new goroutine; `join` copies the result back onto the parent's Data Stack, releases the child block, and re-raises any worker `*GsError` panic in the joining goroutine. Goose's typechecker statically forbids sharing mutable references across `spawn`, guaranteeing data-race freedom (`go test -race`).

4. **Zero-Copy Go Slice & `[]string` Interop:**
   A Goose `const u8[:]` (`GsSlice{Data unsafe.Pointer, Len int64}`) has the exact 16-byte `{ptr, len}` memory layout of a Go `string`. `goose.go` exploits this so `export fn` entry points view Go `string`, `[]byte`, `[]T`, and even `[]string` (`const (const u8[:])[:]`) directly in place without copying or allocating.

5. **`gc`-Friendly Machine Code:**
   Go's compiler (`gc`) uses a strict 80-node inlining budget (which drops to 20 inside functions over 5,000 AST nodes) and spills registers across non-inlined helper calls unless it sees an explicit `panic(...)` statement on the cold branch. `goose.go` emits inline syntactic `panic` checks, direct `*(*GsSlice)(p)` and `*(*uint64)(p)` loads/stores, `CSEL`/`CMOV`-friendly `min`/`max` selects, and static Go array literals for `const` tables.

---

## Quick Start

### 1. Build the compiler

Requires a C++20 compiler, CMake, and Go 1.21+:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/goose version
```

### 2. Compile and run a standalone `.goose` program

```bash
./build/goose --target=go -o hello.go samples/01_hello.goose
go run hello.go

# Or compile and run in one step:
./build/goose run --target=go samples/01_hello.goose
```

### 3. Compile a Goose library into a Go package (`export fn`)

```goose
// stats.goose
import std;

export fn count_words(text: const u8[:]) -> i64 {
    var n = 0;
    var inword = false;
    for c in text {
        let space = c == ' ' || c == '\n' || c == '\t';
        if !space && !inword { n += 1; }
        inword = !space;
    }
    return n;
}

export fn median(xs: i64[:]) -> i64 {
    guard xs.len > 0 else { return 0; }
    var s: i64[>..] = [];
    for x in xs { s.push(x); }
    sort(s);
    return s[s.len / 2];
}
```

Generate the Go package:

```bash
./build/goose --target=go --go-package stats -o stats/stats.go stats.goose
```

Call it from Go:

```go
package main

import (
	"fmt"
	"example.com/stats"
)

func main() {
	// Block-free kernel: inlines directly, accepts string or []byte
	fmt.Println(stats.CountWordsString("the quick brown fox")) // 4

	// Uses a pooled *GsStackBlock automatically
	fmt.Println(stats.Median([]int64{9, 1, 7, 3, 5})) // 5

	// Or reuse one *GsStackBlock across a hot loop:
	gs := stats.AcquireStackBlock(0)
	defer stats.ReleaseStackBlock(gs)
	for _, xs := range [][]int64{{4, 2}, {8, 6, 7}} {
		fmt.Println(stats.MedianWithStack(gs, xs))
	}
}
```

### 4. Useful `--target=go` Flags

| Flag | Description |
|---|---|
| `--go-package <name>` | Package name for generated Go (default: `main` for programs, `goose` for libraries) |
| `--go-build <expr>` | Emit `//go:build <expr>` at the top of the generated file |
| `--go-runtime-file <path>` | Write the shared runtime to `<path>` so multiple `.goose` files can compile into one Go package |
| `--go-runtime=import` | Import `github.com/robtandy/goose.go/runtime/go` instead of embedding the runtime |
| `--go-unexported` | Unexport runtime and internal generated symbols so `go doc` shows only `export fn` entry points |
| `--include <file.go>` | Splice Go `extern fn` helpers into the generated file with `//line` directives |
| `--check` | Verify that the existing `-o` file matches what the compiler would emit (for CI) |

---

## Documentation

- **[`DESIGN.md`](DESIGN.md)** — Full compiler & runtime architecture, Data Stack & `gs.Frame` layout, and codegen details.
- **[`docs/go-integration.md`](docs/go-integration.md)** — Integrating Goose kernels into an existing Go module (build tags, differential & fuzz tests, benchmarks; see [`examples/records/`](examples/records/)).
- **[`docs/command_line.md`](docs/command_line.md)** — Complete CLI reference (`§15` covers all `--target=go` flags).
- **[`docs/releasing.md`](docs/releasing.md)** — Building prebuilt release archives for `go run github.com/robtandy/goose.go/cmd/goose@<version>`.
- **[`AGENTS.md`](AGENTS.md)** — Build, test (`python3 test/run_go_tests.py`), and development workflow.
