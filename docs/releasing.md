# Releasing `goose.go`: prebuilt compilers (issue #29)

A release is a version, `vMAJOR.MINOR.PATCH` (or one with a pre-release, `v0.2.0-rc.1`), and the GitHub release of its tag, which holds the compiler prebuilt for three platforms and the checksums of the archives:

| File | What it holds |
|---|---|
| `goose-v0.2.0-linux-amd64.tar.gz`, `goose-v0.2.0-linux-arm64.tar.gz` | The compiler for Linux, linked statically against musl: it runs on any Linux, glibc's or musl's (Alpine's, a `golang:alpine` CI image's). |
| `goose-v0.2.0-darwin-arm64.tar.gz` | The compiler for macOS on Apple silicon, 13.3 and later (where the system's C++ library has the floating-point `std::to_chars` it prints numbers with). |
| `SHA256SUMS` | `sha256sum`'s lines, one for each archive, which [`cmd/goose`](../cmd/goose) checks what it downloads against. |

Each archive is one directory, `goose-v0.2.0-<os>-<arch>/`: `goose`, the compiler; `stdlib/`, the standard library (what `vendor/goose` tracks), which the compiler finds beside itself; `LICENSE`; and `licenses/`, those of the code it is linked from that has its own (TinyCC's, and musl's in the Linux ones). An archive is the same bytes each time the same compiler is packaged.

Three tags name the release's commit: `v0.2.0`, an annotated tag, the release's own and the compiler's version; and `cmd/goose/v0.2.0` and `runtime/go/v0.2.0`, the versions of the two Go modules in the repository, which the go command finds a nested module's versions by:

- `go run github.com/robtandy/goose.go/cmd/goose@v0.2.0` runs the release's compiler (the README's [Prebuilt compilers](../README.md#prebuilt-compilers-go-generate-with-nothing-but-go));
- `go get github.com/robtandy/goose.go/runtime/go@v0.2.0` is the runtime that the Go `--go-runtime=import` writes imports, as the release's compiler embeds it.

The modules' tags are lightweight, and the release's annotated, because `git describe`, which gives a build its version (`vendor/goose/cmake/version.cmake`), names a commit by an annotated tag before the lightweight ones on it, and by the first of those in the order of their names otherwise: `cmd/goose/v0.2.0`. So a compiler built from a checkout of the tag is `goose v0.2.0`, as the release's are, and the Go it writes is theirs byte for byte, its header too (`// goose-version: v0.2.0 (runtime/go ...)`). A checkout without the tag (a shallow clone) builds `goose <commit>`; `cmake -B build -DGOOSE_VERSION=v0.2.0` names its version.

## What it takes

- A clean checkout of the commit, `vendor/goose` checked out (`git submodule update --init vendor/goose`).
- Python 3 and git.
- For the Linux compilers, Docker: each is built in an `alpine:3.20` container of its architecture (`docker run --platform linux/amd64`, `linux/arm64`), which fetches its compiler and CMake with `apk` and musl's license from musl's repository. Docker Desktop runs the other architecture's container in emulation; on Linux, that takes QEMU's handlers (`docker run --privileged --rm tonistiigi/binfmt --install arm64`, or `amd64`), and a build in emulation takes many times as long as a native one.
- For the macOS compiler, an Apple silicon Mac, with the Xcode command line tools and CMake.
- For publishing, `gh`, logged in to GitHub with the right to make releases, and the right to push tags to `origin`.

An Apple silicon Mac with Docker Desktop builds all three.

## The procedure

1. **Tag the commit**, on `main`, with an annotated tag:

   ```bash
   git tag -a v0.2.0 -m "goose.go v0.2.0"
   ```

2. **Build the compilers.** `scripts/release.py build` builds the compiler of the work tree, with the version `git describe` gives it, for each platform, and writes the archives and `SHA256SUMS` to `dist/v0.2.0/`; it refuses a tree with changes, and a release tag that is lightweight. Each build's log is in `build-release/v0.2.0/<os>-<arch>/build.log`.

   ```bash
   python3 scripts/release.py build                    # all three, on an Apple silicon Mac with Docker
   python3 scripts/release.py build --platform linux/amd64 --platform linux/arm64   # or the Linux ones here,
   python3 scripts/release.py build --platform darwin/arm64                         # the macOS one on a Mac,
   ```

   and the archive of the one built elsewhere copied into this `dist/v0.2.0/` (`publish` writes `SHA256SUMS` again from the archives there). `scripts/release.py package --exe path/to/goose` packages a compiler built some other way: for the platform its header names, of the version it reports (`--version` names it where it cannot run here).

3. **Check them** against a compiler built here from the tag, which must write the same Go:

   ```bash
   cmake -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j$(nproc)
   ./build/goose version                                     # goose v0.2.0 (runtime ...)
   python3 test/run_go_tests.py release --release-dist dist/v0.2.0
   ```

   The suite's `release` section then serves this platform's compiler of the release as `cmd/goose` downloads one, runs it as `go generate` does, with nothing but Go on `PATH`, on a library and on every sample, and requires the Go it writes to be the local compiler's, byte for byte (on a Mac, the macOS one's; on Linux, the Linux one of its architecture). The whole suite (`python3 test/run_go_tests.py`) checks the compiler of the tag as ever.

4. **Publish.** `scripts/release.py publish` releases the tag at `HEAD`: it checks that `dist/v0.2.0/` holds the three archives, and nothing else, writes their `SHA256SUMS` again, tags the commit `cmd/goose/v0.2.0` and `runtime/go/v0.2.0`, pushes the three tags to `origin`, and makes the GitHub release of the tag with the archives and `SHA256SUMS` (`gh release create v0.2.0 --verify-tag`, a pre-release for a version with one). `--dry-run` prints what it would run, and runs none of it:

   ```bash
   python3 scripts/release.py publish --dry-run
   python3 scripts/release.py publish
   ```

5. **Try it**, anywhere with Go:

   ```bash
   go run github.com/robtandy/goose.go/cmd/goose@v0.2.0 version   # goose v0.2.0 (runtime ...)
   ```

   While the repository is private, the go command fetches the module with git, not through the public proxy (`GOPRIVATE=github.com/robtandy/goose.go`, with git's credentials for GitHub), and `cmd/goose` downloads the release through GitHub's API, with a token that can read the repository in `GH_TOKEN` (or `GITHUB_TOKEN`).

A release is not made again: the module proxy keeps a version's source for good, and a version's archives are the ones its users have checked. A broken one is followed by the next version.

## What a prebuilt compiler does

All a compiler built on its machine does -- `--target=go`, `goose run --target=go` and `goose build --target=go` with the Go toolchain on `PATH` (or `GOOSE_GO`), `-o out.c`, and `goose build` with a C compiler on the machine -- but run a program's C in its own process (`goose run` without `--target=go`, the default): TinyCC, which does those runs, takes the headers of the machine's C library and that library loaded in the compiler, which a compiler linked statically, or built on another machine, does not have. The blessed packages (`gfx`, `ui`, `audio`, `physics`) are not compiled in.
