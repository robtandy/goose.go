# `goose.go` Architecture & Design

## 1. Compiler Pipeline Integration

The Goose compiler (`src/main.cpp`) executes six passes over the AST (`src/ast.h`):
1. **Parse** (`src/lexer.h`, `src/parser.h`) -> `Ast`
2. **Resolve** (`src/resolve.h`) -> type aliases & namespaces resolved
3. **Typecheck & Monomorphize** (`src/typecheck*.h`) -> clones one `FnSpec` per live specialization, enforces all reference root, provenance, writability, shrinking, and lifetime rules (§3–§11 of `docs/goose_spec.md`)
4. **Optimize** (`src/optimize*.h`) -> function inlining, constant folding, base-case inlining, tail-recursion elimination, loop strip-mining
5. **Bounds-Check Elimination (BCE)** (`src/bce.h`) -> marks `Index::nobc = true` and `SliceExpr::nobc = true` wherever bounds are statically proven safe, plus loop hoisting (`hoistrefs`, `hoistfields`)
6. **Codegen**:
   - Existing C backend: `src/codegen*.h` (`CodeGen`)
   - **New Go backend**: `src/codegen_go*.h` (`CodeGenGo`), selected via `--target=go` (or automatically when `-o <file>.go` ends in `.go`).

Because `CodeGenGo` runs **after** `TypeCheck`, `Optimizer`, `BCE`, and `BoundStacks` (`src/codegen_stacks.h`), it receives:
- Fully monomorphized `FnSpec`s with exact static stack counts (`fn->numstacks`, `BoundStacks`)
- Already-inlined hot functions (bypassing Go `cmd/compile`'s 80-node inlining budget)
- Proven `nobc` flags on array/slice indexing (allowing `CodeGenGo` to emit raw `unsafe.Add` loads/stores without Go slice bounds checks).

---

## 2. Pure-Go Data Stack Runtime (`runtime/go/`)

### 2.1 Virtual-Memory Data Stacks (`syscall.Mmap`)
- Each Data Stack reserves `GS_STACK_RESERVE` bytes (default 2 GB, configurable) + `GS_STACK_GAP` (1 MB) of anonymous virtual address space via:
  ```go
  syscall.Mmap(-1, 0, reserve+gap, syscall.PROT_READ|syscall.PROT_WRITE, syscall.MAP_ANON|syscall.MAP_PRIVATE)
  ```
- The trailing `GS_STACK_GAP` is marked `syscall.Mprotect(gapSlice, syscall.PROT_NONE)` so runaway stack growth triggers an immediate OS fault at the guard page rather than corrupting memory.
- If the OS refuses a 2 GB reservation (e.g., strict `ulimit -v` or `vm.overcommit_memory=2`), `gs_reserve_region` halves the reservation size down to a minimum of 1 MB, matching C Goose (`src/runtime/runtime_impl.h`).
- Physical RAM pages are demand-paged by the OS kernel only as the stack top (`Top`) advances.

### 2.2 Goroutine-Safe `GsStackBlock` & `sync.Pool`
Unlike C threads (`__thread`), Go goroutines multiplex across OS threads and a process may have tens of thousands of idle goroutines.
- Instead of OS thread-local storage, `goose.go` encapsulates a thread/worker program's Data Stacks in a `GsStackBlock`:
  ```go
  type GsStackBlock struct {
      Stks []unsafe.Pointer // current stack tops, indexed by gs_sp + k
      Base []unsafe.Pointer // base addresses of each reserved mmap stack
      Cap  []int64          // reserved byte capacity of each stack
  }
  ```
- Every generated Go function that uses Data Stacks (or calls one that does) takes `(gs *GsStackBlock, gs_sp int64, ...)` as its leading parameters—mirroring C Goose's `int64_t gs_sp` hidden parameter (`docs/implementation.md` §6.2).
- A global `sync.Pool` of `*GsStackBlock` instances (`AcquireStackBlock(numStacks int)` / `ReleaseStackBlock(gs *GsStackBlock)`) allows exported Go entry points (`export fn`) to check out a `GsStackBlock` in ~15 ns, run the entire CPU-bound Goose computation with 0 GC allocations, rewind stack tops to `Base`, and return the `GsStackBlock` to the pool.
- Callers can also manage a `*GsStackBlock` explicitly (`NewStackBlock` / `Reset` / `Close`) to keep parsed DOMs or zero-copy structures alive across multiple calls on the Data Stack before rewinding in $O(1)$.
- A block also carries the program instance it runs, in a program with workers (`Gl`, §2.3), and which worker runs on it.

### 2.3 Workers, Queues and Program Instances (`goose_spec.md` §11.2)
`src/codegen_go_threads.h` compiles Goose's concurrency as the C backend does (`codegen_builtins.h`, `runtime_threads.h`, `docs/implementation.md` §6.3, §6.8); the runtime it calls (`go_runtime_threads` in `src/codegen_go_runtime.h`, and `runtime/go/goose_threads.go`) is written into a file that uses it.
- **A worker** is a goroutine running its thread program on a `GsStackBlock` of its own, which it reserves as it starts (`GsThreadSpawn`, `gsThreadMain`): the data stacks its program counts, then the dedicated ones its arguments and globals land on, its fixed-frame stack, its program instance, and a private copy of its spawn packet, which the spawn takes, all outside the Go heap. The goroutine releases all of them (`Close`) before it publishes its end, which is what `thread_wait` waits for; ids increase from 0 and are never reused, and a wait on an unissued id or on the waiting worker itself aborts (`GS_E_THREADID`, `GS_E_THREADSELF`).
- **`thread_spawn`** packs the arguments contiguously on a scratch stack, then each global the worker's program reaches as `[int64 size][image]`, as C's does: a fixed-size value's bytes, a bytes value's image, a resizable's count, a frame object's fixed fields (packed) and its elements, a pool's freelist after its elements. The worker's thunk (`gsTmain_<worker>`) unpacks them onto its block's dedicated stacks and into its instance and runs the body with its stacks from 0; a fixed-size argument of more than 4096 bytes stays in the packet, which the worker owns, as C passes one. `--stacks` reports each worker's stacks and the thread cap as C's does.
- **Aborts.** Every abort panics with a `*GsError`; each thread program's goroutine recovers it. A contained worker (`thread_spawn_contained`) keeps the message of one with a location for the first `thread_wait(id, msg)` that asks, and its waits answer `false`; anything else -- an abort in main or in a worker that is not contained, the runtime's own failures, a fault in a guard gap (`debug.SetPanicOnFault` is set per goroutine) -- ends the process, with one report, from whichever goroutine (`gsEnd`), as `exit(code)` does.
- **Program instances.** In a program that spawns workers, every global that is not read-only static data (`CStaticInit`, which every instance shares as it is) is a member of `gsGlobals`, one instance per thread program: main's is the static `gsGlobalsMain`, a worker's the copy its spawn took. A function that names one reads the instance from its block once, `gs_gl := (*gsGlobals)(gs.Gl)`, and so takes the block; a global's dedicated stack is the block's stack its instance names (`gs_gl.gsGStk_<name>`), and the channels of `return ... from` are members too. A program without workers keeps its globals in package variables, at no cost.
- **Queues** are a `GsQueue` per element type, a FIFO of value images (the same images a spawn packs) under a mutex: `qput` copies before it locks and never waits, refusing a put at the queue's limit (`qlimit`, `GS_E_QBOUND` below one); each waiting receiver waits on a channel of its own, so a timed `qget<T>(ms)` waits for a monotonic deadline; a miss yields `T`'s zero value; a value of no bytes is a message of none.
- **`hardware_threads()`** is `runtime.GOMAXPROCS(0)`, capped at the workers `GS_STACK_BUDGET` holds beside the main program, at least one (`gsRtStart`, as `gs_rt_start` computes it).
- What differs: recursion deeper than a goroutine's stack holds is a fatal error of the Go runtime at 1 GB, where C reports its native stack's overflow as an abort, in a worker as in main (`test/threads/thread_stack_overflow.goose`).

---

## 3. Value Representations in `--target=go` (`docs/implementation.md` §6.1)

### 3.1 Scalars & Integer Semantics
- `i8`, `i16`, `i32`, `i64` -> `int8`, `int16`, `int32`, `int64`
- `u8`, `u16`, `u32`, `u64` -> `uint8`, `uint16`, `uint32`, `uint64`
- `f32`, `f64` -> `float32`, `float64`
- `bool` -> `bool` (1 byte, 0 or 1 in binary layouts)
- In release mode (`GS_DEBUG == 0`), arithmetic and casts emit native Go operators (`+`, `-`, `*`, casts), with `gs_f2iwrap` for float-to-int wrapping and zero-checked division/Euclidean modulo (`gs_div_*`, `gs_mod_*`).
- In debug mode (`--debug`), arithmetic and narrowing `as` casts call checked helpers (`gs_add_i32`, `gs_range_*`, etc.) that report the `.goose` file and line on overflow.

### 3.2 Fixed-Size Types (`Fixed` Class)
Goose specifies exact byte layouts (`FixedSize`, `LayoutFields` in `src/codegen_types.h`) with `#pragma pack(1)` semantics. `src/codegen_go_types.h` ports them and keeps every fixed-size value in its C layout, byte for byte, its references and slices included:
1. **Naturally Aligned Structs holding no address** (where every field offset in `LayoutFields` already matches Go's natural alignment and total size matches `FixedSize`, such as `struct YyVal { tag: u64, uni: u64 }`):
   - Emitted as a native Go `struct` so field reads/writes are direct `s.tag_g` / `s.uni_g`; `pad` and `pad n` become blank `_ [n]byte` fields, which Go's `==` ignores as Goose's does.
2. **Every other Fixed Struct & Fixed-Mode ADT** (unaligned offsets, no tail padding, or a reference or slice among the fields):
   - Emitted as `type Name_g [FixedSize]byte`, each field read and written in place at its exact byte offset from `LayoutFields`: `*(*uint32)(unsafe.Add(unsafe.Pointer(&s), off))`.
   - On `arm64` and `amd64`, Go's SSA compiler lowers that to a single unaligned load/store instruction (`LDR`/`STR` or `MOV`).
   - A payload-less ADT is its tag, a named `uint8`/`uint16`; any other fixed-mode ADT the bytes of its tag (`TagStore`) and its largest payload, the payloads overlapping behind the tag as C's union. A static-capacity limited array `T[..k]` is its length (`LenStore`) and its `k` slots, as a native struct where Go aligns the slots and they hold no address, and as bytes otherwise; a fixed array `T[k]` is a Go array.
3. **References and slices.** A plain or optional reference is a typed Go pointer (`*T`, `nil` for null) and a slice a `GsSlice { Data unsafe.Pointer, Len int64 }`; a reference to a value whose Go type itself holds Go pointers (a slice, an array of references) is its `unsafe.Pointer` address. Every address a program holds points outside the Go heap and never moves: into a data stack, into the fixed-frame stack (§3.6), into a global or into a literal's static data. So a value's bytes can hold one where the collector does not look, which needs no write barrier either. A value of a Go type holding Go pointers at an address that may be unaligned (a slice at byte 1 of a struct's bytes, a reference packed on a data stack) is read and written through a copy of its bytes, `gsLd_<T>`/`gsSt_<T>` (`*(*[16]byte)(unsafe.Pointer(&v)) = *(*[16]byte)(p)`), which has alignment 1: `checkptr` accepts it where a typed access would be a misaligned pointer conversion. A self-relative reference is the signed offset C stores, at its width, from the offset C gives its field.
4. **Equality** (`==`, §4.5) is Go's own `==` wherever that is the structural one (native structs of scalars, the bytes of a value whose bytes are its equality, references and slices, arrays of them, tags), and a generated `gsEq_<T>` elsewhere, which compares a limited array's live elements and an ADT's live payload only, and floats by value.
5. **Large values.** A fixed-size value of more than 4096 bytes is one the C backend holds on a data stack wherever it is a local or a temporary (`FixedLocal`). Those stacks are counted in the `--stacks` report both backends share, so this version supports such values where it takes the stacks C does: in globals (built in place, counting the one stack C's initializer takes, or none where C makes the global static data), in a recursive cycle's locals (which take none, in the function's frame), and on a data stack where C takes one too (`CodeGenGo::FixedLocal`): a named local, a literal's or a `default()`'s temporary, a queue's value, and a `thread_fn`'s argument, which stays in its spawn packet. A parameter or result of one, and its other temporaries, are not supported yet.

### 3.3 Variable-Size Byte Images (`Bytes` Class)
- Represented as `unsafe.Pointer` pointing to the start of the self-describing byte image on a Data Stack.
- Fields after a variable-size field are reached via generated `gs_size_<T>(p unsafe.Pointer) int64` walkers and `unsafe.Add(p, offset)`, identical to C Goose (`FieldPtr`, `SizeX`).
- **An ADT adapted from another type.** Where the checker adapts the result of a call -- a function's, a case function's, a builtin's or a function value's -- or of an inlined body to an ADT it does not arrive as (a variant to its ADT, or an ADT to its other mode, `goose_spec.md` §3.5), the value is built as the type it arrives as and converted, as C builds it (`CodeGenGo::AdtFrom`, `GenAdtAdapted`, after `CodeGen`'s): a variant becoming the variable-mode ADT is built behind the ADT's tag; a fixed-mode ADT becoming the variable-mode one is built in a local and written out as the image of the variant it holds; a variable-mode value becoming the fixed-mode ADT, a reference's pointee included, is built on a temporary stack, taken before the call as C takes it, and read from there. A result a return forwards is adapted at the function's destination for it the same way (`GenForward`). Each call is made once, and both backends take the same stacks. A fixed-size value holding relative references adapted so is not supported yet.

### 3.4 Resizable Data-Stack Values (`Resizable` Class) & Slices
- **Slices (`T[:]`)**:
  ```go
  type GsSlice struct {
      Data unsafe.Pointer
      Len  int64
  }
  ```
  (with zero-copy conversion helpers to/from Go `[]T`, `[]byte`, and `string` via `unsafe.Slice` and `unsafe.String`).
- **Resizable Headers (`GsRhdr`)**:
  ```go
  type GsRhdr struct {
      Base unsafe.Pointer
      Len  int64
  }
  ```
  Elements live contiguously on `gs.Stks[gs_sp + k]`. Pushing an element writes `*(*T)(gs.Stks[stk]) = elem` and advances `gs.Stks[stk] = unsafe.Add(gs.Stks[stk], sizeof_T)` + `hdr.Len++`.
- **Fat References (`GsRref`) & Pool References (`GsPref`)**:
  ```go
  type GsRref struct {
      Hdr *GsRhdr
      Stk int64
  }
  type GsPref struct {
      Hdr     *GsRhdr
      Stk     int64
      FreeHdr *GsRhdr
      FreeStk int64
  }
  ```

### 3.5 `export fn` and `extern fn` in Go Mode
`src/codegen_go_export.h` holds the foreign boundary.

- **`export fn`.** Every `export fn foo(a: u8[:], x: i64) -> i64` gets two exported Go entry points, named by its C symbol's words capitalized (`parse_json` is `ParseJson`, `export "sym" fn` renames it):
  1. `func FooWithStack(gs *GsStackBlock, a []byte, x int64) int64` runs it on the data stacks of a block the caller holds (from `NewStackBlock`, or `AcquireStackBlock`), which it gives back as it found them: a caller may keep one block across calls.
  2. `func Foo(a []byte, x int64) int64` takes a block from the runtime's pool (`AcquireStackBlock(n)`, `n` the export's own stack count), calls `FooWithStack`, and rewinds and returns the block (`ReleaseStackBlock`) before returning. Once the pool holds a block, a call allocates nothing on Go's heap (`testing.AllocsPerRun == 0`, `test/go/export/`). An export whose code needs no block calls the function directly.

  The parameters are Go's own values, viewed in place with no copy: `u8[:]` is a `[]byte`; `const u8[:]` either a `string` or a `[]byte` (a type parameter `T ~string | ~[]byte`, `GsBytes`); any other `T[:]` a `[]T`; `T&` a `*T`; a scalar or a fixed-size value its Go type. A `const u8[:]` result is a `[]byte` viewing what the slice does (the arguments, a global, static data): no export's result can view the block's stacks, which Goose's lifetimes keep a result from pointing into, so `Foo` always gives the block back. What the entry points are handed may end up where only the data stacks see it, so they keep it off Go's moving goroutine stacks (`gsEscape`: a caller's buffer is on the heap, or static). An abort panics with a `*GsError` in the caller's goroutine. `--library` (or `export fn`s and no `fn main()`) makes the file a package with no `func main`, `package goose` unless `--go-package` says otherwise, whose `init` initializes the globals on a block of its own.
- **Concurrency.** Each call of `Foo` runs on a block of its own, so goroutines may call exports at once; making blocks is goroutine-safe, and a pooled block the collector drops is closed. In a program without workers, Goose's globals are the package's, shared by every call, as are `return ... from`'s channels: an export that writes a global is not safe to call from two goroutines at once. An export whose calls reach a global that lives on a data stack of its own (growing it, or handing on a reference to it) is refused at compile time, since that stack is in the program's block, not the caller's.
- **Concurrency with workers.** In a program with workers (§2.3), a call runs on a program instance of its block's own, so no two goroutines share a global, as no two Goose threads do. `FooWithStack(gs, ...)` gives a block that has none a copy of the package's instance (`gsEnter`: main's, which the package's `init` or `fn main()` leaves), the values on data stacks onto the block's own dedicated stacks at the main program's indices; the block keeps it, and what its calls write, until `Reset` or `Close`. `Foo(...)` copies the globals its calls reach into the pooled block for the call (`gsEnter_<fn>`), so it always starts from the package's, and hands back a result viewing them as a copy on Go's heap (`GsKeep`), its block going back to the pool. Such an export may grow a global's stack, which is its block's; one whose calls reach a global holding a reference into a data stack is refused. The queues are the process's, one per element type, shared by every instance (§11.2).
- **`extern fn`.** A call calls the Go function its C symbol names, directly. The C runtime's functions that std, math and os bind are the Go runtime's (`gs_find_any` is `GsFindAny`, `gs_os_read_file` `GsOsReadFile`; the OS layer behind `stdlib/os.goose` is written into a file that calls it), and the C library's that Go's `math` has are those (`floor`, `sinf`, ...; `mul_add` is `math.FMA`, rounded once, as C's is on arm64). Any other symbol is a Go function of the generated file's package, from an `--include helper.go` (or a file beside the generated one): scalars are Go's types, a fixed-size value its generated type (`Pt_g`), a plain reference a pointer to it, a slice a `GsSlice` (`GsSliceToBytes`, `GsSliceToString` view it), and a `u8[>..]&` builder a `GsRref`, after the block, which such a function is handed first to append with (`GsBldAppend(gs, b, data)`, `GsBldAppendString`). It may not shrink the builder or keep what it is handed past the call.
- **`--include helper.go`** is Go source spliced into the file: its package clause and build constraints are dropped, its imports join the file's own, and the rest follows the program, each run of its lines between blank ones under a `//line helper.go:N` directive naming the run's line (`GoIncludeLines`), so Go's errors in it and its panics' frames name its own lines whatever `gofmt` makes of the blank lines between the runs. A run that opens with a declaration's doc comment takes its directive at the comment's end, after an empty comment line, where `gofmt` keeps a directive in a doc comment.

### 3.6 The Fixed-Frame Stack (`gs.Frame`)
A Goose local whose address the program never takes is a Go local, which Go keeps in registers. One whose address it does take (`&x`, `x[..]`, a by-reference argument, a field access of a value held as bytes) cannot be: Go may move its goroutine's stack while the bytes of a value, a data stack or a frame hold its address, and the collector scans none of those. Each `GsStackBlock` therefore has one more `mmap` region, the fixed-frame stack: a function holding such locals (or parameters) takes its frame there as it starts, `gs_fr := gs.Frame; gs.Frame = unsafe.Add(gs_fr, gs_fo<n>)`, each local a pointer to its slot, and gives the frame back at every return and where its body's end is reached. Fixed-size locals never grow, so the frames obey a strict call discipline, recursion included, and a local declared in a loop has the same slot every iteration, as C's does. Each slot is 8-aligned and at least a byte, so that two variables never share an address, a zero-length array's or a payload-less variant's included. `FinishBody` (`src/codegen_go_emit.h`) finds those locals once a body is generated and rewrites their declarations and uses; whether a function then takes the block is settled once every body is (`ResolveGsArgs`). The region's 1 GB reservation is address space only; a frame that does not fit is reported as C reports its native stack's overflow.

### 3.7 The Layout: `gofmt`'s
A generated file is laid out as `gofmt` lays it out, so that `gofmt -l` finds nothing to change in it (`src/codegen_go_format.h`). The runtime's text is `gofmt`'s already, and the imports are one group in `gofmt`'s order. The program's Go is written a statement or a declaration to a line with `gofmt`'s indentation, and `GoFormat` lays the finished text out as `go/printer` prints the same Go: it parses each line's expressions as `go/parser` does and spaces each binary operator as `go/printer` does, from the depth and the precedences around it (`a + b` at a statement's top, `f(a+b, c)`, `x[i+1]`, `a*b + c`), which is not known where an expression is generated, inside out; leaves out the parentheses `gofmt` does not print (the outer of two pairs, those around a control clause's expression); aligns the fields of a struct, the specs of a `const` or `var` group and the comments ending consecutive lines in `text/tabwriter`'s columns; and keeps one blank line where `gofmt` keeps one, adding the one it puts before a comment or a declaration of another kind at the package level. `--go-build 'expr'` starts the file with `//go:build expr`, the expression checked and written as `gofmt` writes it (`GoBuildConstraint`), and a blank line. An export fn's Go documentation is its Goose documentation's text as one paragraph, which `go/doc/comment` leaves as it is.

---

## 4. Using the Go Backend

The command line is in [`docs/command_line.md` §15](docs/command_line.md#15-writing-go---targetgo); in short:

| Command | What it does |
|---|---|
| `goose --target=go -o out.go prog.goose` (or `goose -o out.go prog.goose`) | Writes one Go source file holding the program and the runtime it uses (§2), `package main`, which `go build out.go` builds with nothing but Go's standard library and `CGO_ENABLED=0`, laid out as `gofmt` lays it out (§3.7). |
| `goose run --target=go prog.goose [-- args]` | Writes the Go to the program's build directory under `GOOSE_HOME` (as `main.go`), builds it with `go build` and runs it, exiting with its exit code. `goose --target=go prog.goose` does the same. |
| `goose build --target=go prog.goose [-o exe]` | Builds the executable beside the root file, or where `-o` says. |
| `--go-package name` | The file's package: `main` for a program, `goose` for a library by default. |
| `--library` | A package with no `func main`, whose `init` initializes the globals on a block of its own; a `fn main()` the program has is not run. A file with `export fn`s and no `fn main()` is a library without it. |
| `--include helper.go` | Go source spliced into the file for the `extern fn`s the program calls (§3.5): its package clause and build constraints dropped, its imports merged, its lines kept (`//line helper.go:N` before each run of lines). A C header is refused. |
| `--go-build expr` | Starts the file with the build constraint `//go:build expr` (§3.7), which `go build` honors as it builds the file's package: `--go-build goose_x` makes a file built with `-tags goose_x` only. For the Go `-o` writes; `goose run` and `goose build` refuse it. |
| `--debug`, `--stack-reserve`, `-DGS_STACK_RESERVE`, `-DGS_STACK_GAP` | As for C: the checked arithmetic and casts of §3.1, and the size of each data stack's reservation and guard gap (§2.1), which the compiler reads as integer constants and writes into the file (`gsStackReserve`, `gsStackGap`). |

An `export fn name` is two Go entry points (§3.5), named after its symbol with each word capitalized: `Name(args...)`, on a block from the runtime's pool, and `NameWithStack(gs, args...)`, on a `*GsStackBlock` the caller holds. A Go program imports the library's package and calls them:

```go
n := stats.CountWords("the quick brown fox")   // export fn count_words(text: const u8[:]) -> i64
gs := stats.AcquireStackBlock(0)                // or stats.NewStackBlock, and gs.Close()
defer stats.ReleaseStackBlock(gs)
m := stats.MedianWithStack(gs, []int64{9, 1, 7}) // export fn median(xs: i64[:]) -> i64
```

The README has the whole example, and one of `--include`.

---

## 5. Testing: `test/run_go_tests.py`

`python3 test/run_go_tests.py` checks the Go backend against the C backend. Every program it runs is compiled by both, at `-O2` and, but for the generated overflow and `--fuzz` programs, at `-O0` too, where nothing is inlined and every call stays a call. The Go must pass `go vet` with `CGO_ENABLED=0` (the `unsafeptr` analyzer included) and `gofmt -l`, which must find nothing to change in any file the suite writes (§3.7), build, and run exactly as the C does -- stdout, stderr and exit code -- also when built with `-gcflags=all=-d=checkptr` (at `-O2`), which checks every `unsafe.Pointer` conversion and the arithmetic on one at run time (what `go test -race` turns on, without cgo). Both backends must print the same `--stacks` report, which their shared accounting gives (`src/codegen_stacks.h`), and the runs must print what the expected files hold. Its sections:

| Section | What runs |
|---|---|
| `test/go/programs/` | The Go backend's own programs, one or more per construct and per runtime check, judged against `test/go/expected/` (`<name>.out`, `.stderr`, `.exit`), which `--bless` writes from the C runs. |
| `test/stdlib/`, `test/threads/` | The C suite's fixtures of the standard library and of workers and queues, also with `--debug` where a fixture's first line asks for a `GS_DEBUG` run, with the `-D` it names, each run in a directory of its own, judged against `test/expected/` as `run_tests.py` judges a C run. |
| `samples/` | Every sample but those importing the C-only `gfx`, `ui`, `audio` and `physics` modules: run with `data/<name>.args` and `data/<name>.stdin` in a directory of its own whose `data/` is the samples', the C with its header (`call_c.h`) and the Go with its Go port (`call_c.go`) spliced in by `--include`. Stderr is the samples' timings, compared with every number masked; stdout must be what `samples/expected/` holds. |
| `test/go/export/<name>/` | Libraries of `export fn`s compiled with `--target=go --library` (and the directory's other `.go` files by `--include`), with Go tests that call the entry points: `go vet` and `go test`, also with `-d=checkptr`, including `testing.AllocsPerRun` checks that a call allocates nothing on Go's heap, and calls from many goroutines at once. |
| `test/go/build_only/`, overflow, `test/go/unsupported/`, the command line | Programs that never end, only built; data stack overflow at a 64 KB reservation; programs using what this version refuses, which must name it; `-o out.go`, `--go-package`, `--library`, `--include` (the lines an error, `runtime.Caller` and a panic's frame name in it), `--go-build` (`go build` skipping the file without the tag and building it with it), `goose run` and `goose build --target=go`. |
| `--sweep` | Every program of the C suite's other categories (`test/codegen/`, `test/lifetimes/`, `test/optimizer/`, `test/runtime/`, `test/storage/`, `test/syntax/`, `test/typing/`) through the checks of the `test/stdlib/` section. One the Go backend refuses as not supported yet, at a level or at all, is counted, not failed, and so is one that differs by design (`SWEEP_KNOWN`: a recursion that exhausts the native stack is an abort in C and a fatal error of the Go runtime). |
| `--fuzz N`, `--fuzz-flow N`, `--fuzz-values N` | Random programs of scalar arithmetic and conversions, of control flow, and of fixed-size values, which both backends must run alike. |

`-j N` runs N checks at once, `-v` prints each one, `--workdir` names where the generated files go (`build/go-tests` by default), `--jit-reference` takes the C reference from TinyCC in the compiler's process, and names run only those programs. [`docs/testing.md`](docs/testing.md#the-go-backends-suite) has the details. The runtime's own tests are `cd runtime/go && CGO_ENABLED=0 go vet ./... && go test -race ./...` (the race detector needs cgo on Linux; `CGO_ENABLED=0 go test -gcflags=all=-d=checkptr ./...` runs the pointer checks without it).
