# goose.go

A Go compiler backend (`--target=go`) and zero-GC `mmap` Data Stack runtime for the [Goose programming language](https://github.com/aardappel/goose).

## Why `goose.go`?

Goose achieves C-beating performance and compile-time memory safety without a garbage collector or heap allocator (`malloc`/`free`) by placing all dynamic/resizable data on pre-reserved virtual-memory **Data Stacks** (`mmap` regions with guard pages) using compile-time lifetime/root analysis (`typecheck_*.h`), aggressive inlining (`optimize*.h`), bounds-check elimination (`bce.h`), and static stack-slot assignment (`codegen_stacks.h`).

When integrating Goose into **Go** programs, compiling Goose to C hits two walls:
1. **`cgo` call overhead (~40–60 ns per call)**: Switching from a growable Go goroutine stack to the OS thread `g0` stack and locking the scheduler `M`.
2. **Go heap materialization**: Copying C results across `cgo` into Go heap objects re-introduces `runtime.mallocgc` and GC pressure.

`goose.go` solves both by compiling typechecked, lifetime-verified, bounds-check-eliminated Goose AST specializations **directly to pure Go** (`--target=go`) backed by `syscall.Mmap` Data Stacks and `unsafe.Add` / `unsafe.String` / `unsafe.Slice` intrinsics:
- **Zero `cgo` overhead**: Generated code is pure Go functions that inline directly into Go callers.
- **Zero Go GC heap allocations & zero GC write barriers**: Data Stacks live in `syscall.Mmap` anonymous virtual memory outside the Go GC heap, using `noscan` scalar/struct layouts and `unsafe.Add` pointer bumps.
- **Compile-time memory safety**: Goose's typechecker (`src/typecheck_*.h`) enforces all reference root, provenance, aliasing, and lifetime invariants before Go code is emitted.

See [`DESIGN.md`](DESIGN.md) for the full architecture and [`AGENTS.md`](AGENTS.md) for build and development instructions.

## Building

The compiler is C++20 and builds with CMake:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j$(nproc)
./build/goose version
```

The Go backend needs a Go toolchain, 1.21 or later: `go` on `PATH`, or the one `GOOSE_GO` names. The Go it writes uses nothing but Go's standard library and builds with `CGO_ENABLED=0`, on 64-bit Linux and macOS.

## Compiling a program to Go

```bash
goose --target=go -o hello.go hello.goose     # or just: goose -o hello.go hello.goose
go vet hello.go && go build hello.go && ./hello
goose run --target=go hello.goose             # write the Go, build it, run it
goose build --target=go hello.goose           # an executable beside the source (or -o exe)
```

`--target=go` writes one Go source file, `package main`, holding the program and the runtime it needs. It is compiled from the same checked, optimized and bounds-checked program as the C backend's output, and it runs as the C program does: the same output, the same `goose runtime error: ...` messages and exit codes, and the same data stacks (`goose --check --stacks --target=go` reports the counts both backends share). `--debug`, `--stack-reserve`, `-DGS_STACK_RESERVE` and `-DGS_STACK_GAP` work as they do for C. [`docs/command_line.md` §15](docs/command_line.md#15-writing-go---targetgo) has every option and what this version compiles.

## Calling Goose from Go: `export fn`, `--library`, `--go-package`

Each `export fn` gets two exported Go entry points, named after its symbol with each word capitalized (`export fn count_words` is `CountWords`; `export "sym" fn` names the symbol):

- `FuncName(args...)` runs the function on a block of data stacks taken from the runtime's pool, and gives the block back before it returns. Once the pool holds a block, a call allocates nothing on Go's heap.
- `FuncNameWithStack(gs, args...)` runs it on a `*GsStackBlock` the caller holds (from `AcquireStackBlock`/`ReleaseStackBlock`, or `NewStackBlock` and `Close`), and gives the block's stacks back as it found them.

The arguments are Go's own values, viewed in place: a `[]byte` for `u8[:]`, a `string` or a `[]byte` for `const u8[:]`, a `[]T` for any other `T[:]`, a `*T` for `T&`, and Go's types for scalars and fixed-size values. An abort panics with a `*GsError` in the caller's goroutine, and in a library so does `exit()`: a library never exits the process, which is its host's. A file with `export fn`s and no `fn main()` is a library: a package with no `func main` and no `init`. Importing it does nothing -- it maps no memory, leaves every goroutine's fault handling alone and runs no initializer -- and the first call of an entry point starts it, once, initializing the globals; a start that aborts panics with its `*GsError` in that call and every later one. A call that runs on a block turns `debug.SetPanicOnFault` on for itself only, so that a data stack overflowing into its guard gap panics in the caller's goroutine, and gives the caller's setting back. Each block reserves 64 MB of address space for its fixed-frame stack in a library (1 GB in a program), which `--frame-reserve size` changes. `--library` makes any program one, and its `fn main()` is then not run. The package is `goose` unless `--go-package name` names another (`--go-package` names a program's package too, `main` by default).

```goose
// stats.goose
import std;

// The number of words in text: runs of bytes that are not spaces.
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

// The median of xs, sorted in a copy on a data stack: xs is left as it is.
export fn median(xs: i64[:]) -> i64 {
    guard xs.len > 0 else { return 0; }
    var s: i64[>..] = [];
    for x in xs { s.push(x); }
    sort(s);
    return s[s.len / 2];
}
```

```bash
goose --target=go --go-package stats -o host/stats/stats.go stats.goose
```

```go
// host/main.go, in module example.com/host
package main

import (
	"fmt"

	"example.com/host/stats"
)

func main() {
	fmt.Println(stats.CountWords("the quick brown fox")) // 4: a string or a []byte
	fmt.Println(stats.Median([]int64{9, 1, 7, 3, 5}))    // 5: a block from the pool

	// One block kept across calls, its stacks rewound after each.
	gs := stats.AcquireStackBlock(0)
	defer stats.ReleaseStackBlock(gs)
	for _, xs := range [][]int64{{4, 2}, {8, 6, 7}} {
		fmt.Println(stats.MedianWithStack(gs, xs)) // 4, then 7
	}
}
```

Goroutines may call `FuncName` at once: each call runs on a block of its own. In a program without workers the globals are the package's, shared by every call, so an export that writes one is not safe to call from two goroutines at once; in a program with workers each block is a program instance of its own ([`DESIGN.md` §3.5](DESIGN.md#35-export-fn-and-extern-fn-in-go-mode)).

## Calling Go from Goose: `extern fn` and `--include helper.go`

Under `--target=go`, an `extern fn` calls the Go function its symbol names. std's, math's and os's are the Go runtime's and Go's `math`. Any other is a function of the generated file's package, which `--include helper.go` splices in: the file's package clause and build constraints are dropped, its imports join the generated file's, and Go's errors in it name its own lines. A scalar is passed as its Go type, a fixed-size value as its generated type (`Pt_g`), a plain reference as a pointer to it, and a slice as a `GsSlice` (`GsSliceToBytes`, `GsSliceToString` and `GsSliceToSlice[T]` view it). A function that appends to a `u8[>..]&` builder is handed the data stack block first and appends with `GsBldAppend` or `GsBldAppendString`.

```goose
// shout.goose
extern fn shout(s: const u8[:], out: u8[>..]&);
extern fn half(x: i64) -> i64 = "goHalf";

fn main() {
    var b: u8[>..] = [];
    shout("hi", b);
    print(b, " ", half(84));     // HI! 42
}
```

```go
//go:build ignore

package main

import "strings"

func shout(gs *GsStackBlock, s GsSlice, out GsRref) {
	GsBldAppendString(gs, out, strings.ToUpper(GsSliceToString(s))+"!")
}

func goHalf(x int64) int64 { return x / 2 }
```

```bash
goose run --target=go --include helper.go shout.goose
```

`samples/24_call_c.goose` runs this way too, with `samples/call_c.go`, the Go port of its C header, in place of `call_c.h`.

## Testing

```bash
python3 test/run_go_tests.py              # the Go backend against the C backend
python3 test/run_go_tests.py --sweep      # and every fixture of the C suite's own categories
cd runtime/go && CGO_ENABLED=0 go vet ./... && go test -race ./...
```

`test/run_go_tests.py` compiles each program with both backends, at `-O2` and at `-O0` (and with `--debug` where a fixture asks for it), and requires the Go to pass `go vet` (with `CGO_ENABLED=0`) and to run as the C does: the same stdout, stderr and exit code (a sample's stderr, its timings, with the numbers masked), also when built with `-gcflags=all=-d=checkptr`, Go's checks of every `unsafe.Pointer` conversion and of the arithmetic on one. The two backends must report the same data stacks (`--stacks`), and the output must be what the expected files hold. It covers:

- `test/go/programs/`, the Go backend's own programs, with what they print in `test/go/expected/` (`--bless` writes those from the C runs);
- the C suite's `test/stdlib/` and `test/threads/` fixtures;
- every sample in `samples/` but those using the C-only `gfx`, `ui`, `audio` and `physics` modules, run in a directory of their own with their arguments and stdin, against `samples/expected/`;
- the `export fn` libraries of `test/go/export/`, whose Go tests call their entry points (with `testing.AllocsPerRun` checking that a call allocates nothing, and a `TestMain` checking that importing the package mapped nothing);
- programs that never end, which it only builds (`test/go/build_only/`), data stack overflow, the constructs this version refuses (`test/go/unsupported/`), and the command line (`-o out.go`, `--go-package`, `--library`, `--frame-reserve`, `--include`, `goose run`/`goose build --target=go`).

`--sweep` puts the programs of the C suite's other categories (`test/codegen/`, `test/lifetimes/`, `test/optimizer/`, `test/runtime/`, `test/storage/`, `test/syntax/`, `test/typing/`: every fixture that runs, not the ones that must fail to compile) through the same checks, judged against `test/expected/`, aborts included. A fixture using what the Go backend does not compile yet is counted, not failed, and so is one of the few that differ by design (a recursion that exhausts the native stack, which C reports as an abort and Go as a fatal error). `--fuzz N`, `--fuzz-flow N` and `--fuzz-values N` add random programs of scalar arithmetic, control flow and fixed-size values. `-j N` sets the parallelism, `-v` lists every check, and names run only those programs. [`docs/testing.md`](docs/testing.md#the-go-backends-suite) has the details.

The race detector needs cgo on Linux, so `go test -race` is the one command here that runs with it; `CGO_ENABLED=0 go test -gcflags=all=-d=checkptr ./...` runs the same pointer checks without it.
