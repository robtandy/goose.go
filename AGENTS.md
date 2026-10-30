# AGENTS.md — Developer & Agent Guide for `goose.go`

## Overview
This repository contains the Goose compiler (`src/`) extended with a **Go code generation backend (`--target=go`)** and its companion **pure-Go Data Stack runtime (`runtime/go/`)**.

Read [`DESIGN.md`](DESIGN.md) first for the complete architecture of the Go backend and runtime, and [`docs/implementation.md`](docs/implementation.md) + [`docs/goose_spec.md`](docs/goose_spec.md) for the Goose language specification and middle-end compiler passes.

## Building the Goose Compiler (`./build/goose`)

The compiler is a C++20 single-translation-unit binary (`src/main.cpp` includes the pass headers in `src/*.h`).

- **Fast incremental developer build (`RelWithDebInfo` / `-O1`)**:
  ```bash
  cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
  cmake --build build -j$(nproc)
  ```
- **Release build**:
  ```bash
  cmake -B build -DCMAKE_BUILD_TYPE=Release
  cmake --build build -j$(nproc)
  ```
- **Verify compiler binary**:
  ```bash
  ./build/goose version
  ```
- **The Go runtime is the compiler's**: the build embeds the source of `runtime/go/` (all of it but `goose_package.go` and `goose_extra.go`, `cmake/go_runtime.cmake`), which is the one copy of the runtime a `--target=go` file holds or imports. After editing it, rebuild the compiler; `test/run_go_tests.py` fails where the compiler's runtime is not `runtime/go`'s. A name the generated code calls must be exported there, marked `//goose:internal` where a file holding the runtime keeps it to itself (DESIGN.md §2, §3.11).

## Running Tests

1. **Pure-Go Runtime Tests (`runtime/go/`)**:
   ```bash
   cd runtime/go && go vet ./... && go test -v -race ./...
   ```
2. **Go Backend End-to-End Tests (`test/run_go_tests.py`)**:
   ```bash
   python3 test/run_go_tests.py
   ```
3. **Existing C Backend Tests**:
   ```bash
   python3 test/run_tests.py
   ```
4. **The command that runs a release's compiler (`cmd/goose`, a module of its own)**:
   ```bash
   cd cmd/goose && go vet ./... && go test ./...
   ```
   `test/run_go_tests.py release` runs these, and `go generate` through it with nothing but Go (issue #29). Releases themselves are made with `scripts/release.py` ([`docs/releasing.md`](docs/releasing.md)).

## Critical Go `unsafe` & Runtime Rules

Every Go file in `runtime/go/` and every `.go` file emitted by the `--target=go` backend **must** obey Go's `unsafe.Pointer` rules so that `go vet` (`unsafeptr` analyzer) and `go test -race` (`checkptr` instrumentation) pass without warnings or panics:

1. **Always use `unsafe.Add(ptr, offset)` for pointer arithmetic**:
   - **NEVER** store an intermediate `uintptr(ptr) + offset` in a variable and cast back to `unsafe.Pointer`.
   - Always write `unsafe.Add(ptr, offset)` or `*(*T)(unsafe.Add(ptr, offset))`.
2. **Use `unsafe.String` and `unsafe.Slice` (Go 1.20+)**:
   - **NEVER** use deprecated `reflect.SliceHeader` or `reflect.StringHeader`.
   - Use `unsafe.String((*byte)(ptr), n)` and `unsafe.Slice((*T)(ptr), n)`.
3. **Keep Data Stack structs `noscan` (pointer-free from Go GC's perspective)**:
   - Memory allocated on Data Stacks via `syscall.Mmap` is outside Go's GC heap (`mheap`) and is **not scanned by the Go garbage collector**.
   - Therefore, structs stored inside `mmap` Data Stacks must **never** hold pointers to Go-heap-allocated objects (`*T` pointing to `new(T)`, Go heap `string`, or Go heap `[]byte`).
   - Represent addresses inside Data Stack values as `uintptr` (when stored in a struct field on the Data Stack) or relative offsets (`int32`/`int64`), or `unsafe.Pointer` only when pointing to `mmap` Data Stack memory or caller-pinned input buffers. Calling `unsafe.Pointer(uintptrField)` when reading a `mmap` Data Stack address is valid when suppressed or kept in the runtime helper `goose.PtrFromAddr(addr uintptr) unsafe.Pointer`.
4. **No `cgo`**:
   - Both `runtime/go/` and all generated `.go` files must compile with `CGO_ENABLED=0` (pure Go + `syscall` / `unsafe`).
