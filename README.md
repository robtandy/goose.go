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
