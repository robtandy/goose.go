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
5. **Large values.** A fixed-size value of more than 4096 bytes is one the C backend holds on a data stack wherever it is a local or a temporary (`FixedLocal`). Those stacks are counted in the `--stacks` report both backends share, so this version supports such values in globals (built in place, counting the one stack C's initializer takes, or none where C makes the global static data) and leaves locals and temporaries to the data-stack work of the resizable values.

### 3.3 Variable-Size Byte Images (`Bytes` Class)
- Represented as `unsafe.Pointer` pointing to the start of the self-describing byte image on a Data Stack.
- Fields after a variable-size field are reached via generated `gs_size_<T>(p unsafe.Pointer) int64` walkers and `unsafe.Add(p, offset)`, identical to C Goose (`FieldPtr`, `SizeX`).

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
- For every `export fn foo(a: u8[:], x: i64) -> i64`:
  1. Emits `func Foo_with_stack(gs *GsStackBlock, a []byte, x int64) int64` for callers reusing a `GsStackBlock`.
  2. Emits `func Foo(a []byte, x int64) int64` which acquires a `*GsStackBlock` from the pool, calls `Foo_with_stack`, and releases the block on return.
- For `--include helper.go` + `extern fn`: allows Goose code in `--target=go` mode to call hand-written Go functions in the same package directly (zero FFI overhead).

### 3.6 The Fixed-Frame Stack (`gs.Frame`)
A Goose local whose address the program never takes is a Go local, which Go keeps in registers. One whose address it does take (`&x`, `x[..]`, a by-reference argument, a field access of a value held as bytes) cannot be: Go may move its goroutine's stack while the bytes of a value, a data stack or a frame hold its address, and the collector scans none of those. Each `GsStackBlock` therefore has one more `mmap` region, the fixed-frame stack: a function holding such locals (or parameters) takes its frame there as it starts, `gs_fr := gs.Frame; gs.Frame = unsafe.Add(gs_fr, gs_fo<n>)`, each local a pointer to its slot, and gives the frame back at every return and where its body's end is reached. Fixed-size locals never grow, so the frames obey a strict call discipline, recursion included, and a local declared in a loop has the same slot every iteration, as C's does. Each slot is 8-aligned and at least a byte, so that two variables never share an address, a zero-length array's or a payload-less variant's included. `FinishBody` (`src/codegen_go_emit.h`) finds those locals once a body is generated and rewrites their declarations and uses; whether a function then takes the block is settled once every body is (`ResolveGsArgs`). The region's 1 GB reservation is address space only; a frame that does not fit is reported as C reports its native stack's overflow.
