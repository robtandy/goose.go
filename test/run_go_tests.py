#!/usr/bin/env python3
"""Go backend test runner (--target=go).

Every program of the suite, test/go/programs/*.goose (the modules they
import are in programs/modules/), is

1. compiled to Go with `goose --target=go -o <name>.go`, with the flags its
   first line names (`// goose-flags: --debug`), at -O2 and at -O0, where
   nothing is inlined;
2. checked with `go vet`, which must find nothing;
3. built with `go build` and run;
4. compiled with the C backend as well, with the same flags and at the same
   level (`goose -o <name>.c --standalone` and the C compiler
   scripts/toolchain.py finds, or TinyCC in the compiler's own process where
   there is none), and run: --debug checks the operations as the
   optimizations leave them, so a program can fail at one level only;
5. judged: the two runs at each level must agree on stdout, stderr and the
   exit code, and the C run at -O2 match what test/go/expected/ holds for
   the program (<name>.out, and <name>.stderr and <name>.exit where it
   writes to stderr or does not exit with 0);
6. reported on with `goose --check --stacks` for both targets, at both
   levels, which must agree: both backends count data stacks with the same
   analysis (BoundStacks, src/codegen_stacks.h);
7. built again at -O2 with -gcflags=all=-d=checkptr, Go's checks of every
   unsafe.Pointer conversion and of the arithmetic on one (which `go test
   -race` turns on), and run, which must match the C run as well;
8. where its source declares the warnings its compile prints, with `//
   warning:` lines as the C suite's fixtures declare theirs, checked with
   `goose --check` for both targets: each must print exactly those, one
   warning line each, in any order, as run_tests.py judges a fixture's
   (issue #41).

The programs of test/go/build_only/ never end (a loop that only stopping
them leaves): each is compiled to Go at both levels, vetted and built, and
with the C backend, as above, and reported on with --stacks, but neither is
run.

Data stack overflow is checked on programs generated for it: a print whose
line, at --stack-reserve 64K, runs past its stack by a literal or inside a
value's text, or just fits, must run in Go as it does in C, built with
-d=checkptr as well.

The programs of test/go/unsupported/ use what this version of the Go backend
does not compile yet. Each must stop the compile with every `// error:`
message its source names, where the C backend compiles it.

The programs of test/go/warnings/ hold what the Go backend warns of where
the C backend does not yet: a shift by a constant count outside its type's
width (issue #27). Compiled to Go at -O2 and at -O0, and checked with
`--check --target=go`, each must print the warnings its `// warning:`
lines declare, one warning line each, in any order, and no other, as
run_tests.py judges a fixture's; the C backend must check it
(Runner.warnings). Step 6 leaves those warnings out of the two backends'
`--check --stacks` reports, which must agree on the rest (GO_WARNING).

The C suite's fixtures of test/stdlib/ and test/threads/ run through both
backends as the suite's programs do, at -O2 and at -O0 (and with --debug
where their first line asks run_tests.py for a GS_DEBUG run), with the -D
their first line names, and must print what test/expected/ holds for them,
as run_tests.py judges a C run (Runner.fixture); one that differs by design
(SWEEP_KNOWN) is counted, not run. Every sample of samples/ but those
importing a module only the C backend has (gfx, ui, audio and physics)
runs through both backends at both levels, as samples/run_samples.py runs
one -- with its arguments, its stdin, and its C header, which the Go
backend takes a Go port of -- each run in a directory of its own: the runs
must agree on stdout and the exit code, and on stderr, the sample's
timings, once its numbers are masked, and print what samples/expected/
holds (Runner.sample). A program with workers needs a C compiler for its
reference, which a JIT run cannot build; without one it is counted, not
run.

A few checks of the command line follow: the target -o out.go implies, `goose
run --target=go` and `goose build --target=go`, --go-package and --library.

The code Go makes of the wide loads and stores of test/go/export/wide/
(issue #23) is read for arm64 and for amd64, which Go cross-compiles to with
nothing but its toolchain (`go build -gcflags=-S`): each export fn an `//
asm:` line annotates must reach memory through a pointer as it names, with
one load or one store of its width, or not at all (Runner.wide_asm). So is
the code of the selects of test/go/export/selects/ (issue #26): each export
fn an `// asm: select, branches m` line annotates must have a conditional
select (CSEL and its kin on arm64, CMOV on amd64) and exactly m conditional
branches, those of its loop and its guard, so that no choice it makes is a
branch (Runner.select_asm). So is the code of the bounds checks of
test/go/export/bounds/ (issue #40): each export fn an `// asm: no spills`
line annotates must keep its values in registers across every check,
moving none to a slot of its frame and back, and each one an `// asm: no
index checks` line must leave Go none of its own bounds checks to make;
and Go must inline every check, GsIdx and GsSub, into its caller
(Runner.spill_asm).

The reproducers of the gaps in the front end's bounds-check elimination
that issue #26 found, test/go/bce_gaps/ (whose README.md writes them up),
are checked with `goose --check --bce-test` at -O1 and -O2: each `//
bce:keep` and `// bce:elide` line must have the outcome it names, so that
the write-up says what BCE does (Runner.bce_gaps).

The Go of the export fns of test/go/export/inline_refs/ (issue #24) is read
at -O2, where the functions they call are inlined: each one a `// frame:`
line annotates must take as many slots on the fixed-frame stack as it
names, `none` none at all, a local handed to an inlined body by reference
staying a Go local unless its address outlives the body (Runner.frames).

The Go of test/go/programs/cycle_inlining.goose and of
test/go/export/string_slices/ (issue #42) is read at -O2, where the helpers
of a recursive fn that take no data stack are inlined into it: the Go
function of each function an `// inlined:` line annotates must call none of
the helpers it names, and must call each one a `// calls:` line names, which
takes one (Runner.cycles). The export libraries of EXPORT_RACE run their
tests under the race detector too, where cgo builds.

The entry points of the kernels of test/go/export/inlining/ (issue #22) are
read as Go compiles them, at -O2 and at -O0 (`go test -c -gcflags=-m=2`):
each export fn an `// entry:` line annotates must have entry points Go can
inline (`inline`), and whose slice and pointer parameters do not escape (`no
escape`) or all do (`escape`); and each callX function of its tests, a call
of an entry point, must be the same instructions for arm64 and for amd64 as
directX, the call of its function written by hand (Runner.entry_points).

The library of test/go/export/arena/ (issue #25), whose export fns grow
globals on data stacks of their own in a program without workers, keeps its
globals in program instances, one of each block's own: compiled at -O2 and
at -O0, its file must hold none of the workers' and queues' runtime
(runtime/go/goose_threads.go), and its tests, which call the entry points
from many goroutines at once, must pass under the race detector, where cgo
builds (Runner.instances).

The runtime (issue #19): every program (at -O2), fixture, sample, overflow
case and export library is compiled again with --go-runtime=import, into a
module that requires the runtime's package, runtime/go, and built with
-d=checkptr, and every program at -O0 with --go-unexported, which must run
as the C does. runtime/go's own tests run (vet, test, -race where cgo
builds, checkptr), and the runtime the compiler embedded must be
runtime/go's (goose --emit-go-runtime). test/go/kernels/ is a library of
kernels whose file must hold none of the runtime's data stacks, pools or
text forms (KERNEL_ABSENT), test/go/unexported/ one compiled with
--go-unexported whose go doc must list its entry points alone, and
test/go/shared/ two programs compiled into one package with
--go-runtime-file, whose tests pass together in each mode.

Reproducible files (issue #21, the section reproducible): a generated
file's header keeps Go's `// Code generated ... DO NOT EDIT.` line and
records the compiler's version, the runtime's revision and the options its
Go depends on, spelled one way whatever the compile's spelling; `--check
-o out.go` passes a file that is what the compile would write and fails one
that is stale or missing, with a short diff; and the same programs compiled
in two temporary directories, each with a standard library of its own, are
the same Go, byte for byte (Runner.reproducible, Runner.same_go). So the Go
names a file of the standard library by its place in it, stdlib/std.goose,
where an abort there names it, and C by its whole path: every C run's
stderr is compared with the Go's so named (go_file_names).

Prebuilt compilers (issue #29, the section release): scripts/release.py
packages the compiler under test as a release holds it, an archive of it,
the standard library and the licenses that is the same bytes each time,
and SHA256SUMS; cmd/goose, the wrapper `go run
github.com/robtandy/goose.go/cmd/goose@<version>` runs, passes go vet and
its own tests; and run so, from a module proxy of the suite's, with
nothing but Go on PATH, the wrapper downloads the release of its version
from a local server, once, checks it against SHA256SUMS, keeps it and runs
it: `go generate` with it writes the file the compiler under test writes,
byte for byte, and so does every sample (Runner.release). With
--release-dist dist/<version>, the release it runs is the one there, as
scripts/release.py build made it, whose Go must be that of the compiler
under test, built here at the same version.

--sweep also runs the C suite's own programs (test/<category>/*.goose) of
its other categories through both backends as the test/stdlib/ fixtures
run: at both levels, with --debug where they ask for it, vetted, built with
-d=checkptr as well, and run as their C runs, printing what test/expected/
holds for them, with the same --stacks report. One the Go backend refuses
as not supported yet, at all or at a level (a value -O0 passes where -O2
inlines it away), is counted there, not failed, and so is one that differs
by design (SWEEP_KNOWN, with the reason). --fuzz N generates N programs of
random scalar arithmetic, conversions and comparisons (from --fuzz-seed on,
every other one built with --debug), which both backends must run alike,
the Go built with -d=checkptr as well. --fuzz-flow N generates N programs
of random control flow, whose returns, breaks, continues and guards end it
in every position Go's flow analysis follows (conditions, arguments, loop
bounds, values), with code after them that is never reached: their Go must
vet and build, and run as their C does at both levels (and at -O2 with
-d=checkptr), with the same --stacks report. --fuzz-values
N generates N programs of fixed-size values: random structs (packed, padded,
nested), ADTs and fixed and limited arrays, built from literals, written in
place, compared, copied, matched, handed to functions by value and by
reference and sliced, each printed; they run as --fuzz-flow's do, the Go
built with -d=checkptr as well. --bless writes test/go/expected/ from the C
backend's runs instead of comparing with it.

  python3 test/run_go_tests.py [--exe build/goose] [-j N] [--sweep] [--fuzz N]
                               [--fuzz-flow N] [--fuzz-values N] [--bless] [-v]
                               [--release-dist dist/<version>] [name...]
"""

import argparse
import concurrent.futures
import contextlib
import difflib
import functools
import hashlib
import http.server
import importlib.util
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import threading
import zipfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
UPSTREAM = REPO / "vendor" / "goose"
UPSTREAM_TEST = UPSTREAM / "test"
sys.path.insert(0, str(UPSTREAM / "scripts"))
import toolchain as tc

UNSUPPORTED = "is not supported by --target=go yet"

# What the Go backend warns of where the C backend does not yet: a shift by a
# constant count outside its type's width (issue #27, src/codegen_go_shifts.h),
# which the C suite's fixtures of the masking do on purpose. The one line of
# each is left out of the two backends' --stacks reports, which must agree on
# the rest (Runner.stacks); test/go/warnings/ checks the warnings.
GO_WARNING = re.compile(r"^\S.*?: warning: shift of [iu]\d+ by -?\d+ is a shift by \d+: .*\n",
                        re.MULTILINE)

# C suite fixtures --sweep counts as differing by design, with the reason: a
# recursion that exhausts the native stack, which the C runtime reports as a
# Goose abort, overflows the goroutine's stack in Go only at 1 GB, and that
# is a fatal Go runtime error, which no Go program can recover from.
SWEEP_KNOWN = {
    "runtime/native_stack_overflow.goose": "native stack exhaustion",
    "runtime/render_reference_cycle.goose": "native stack exhaustion",
    "threads/thread_stack_overflow.goose": "native stack exhaustion, in a worker",
}
FLOW_TIMEOUT = 60     # Seconds a --fuzz-flow program may run.
SAMPLE_TIMEOUT = 300  # Seconds a sample may run.

# The Go runtime's package (issue #19), which the Go --go-runtime=import
# writes imports, and the files of it that are its alone, which the compiler
# does not embed: every other file of runtime/go is the compiler's runtime.
RUNTIME = REPO / "runtime" / "go"
RUNTIME_MODULE = "github.com/robtandy/goose.go/runtime/go"
RUNTIME_PACKAGE_ONLY = {"goose_package.go", "goose_extra.go"}
# Runtime functions a library of kernels has no use for (issue #19): the
# data stacks', the slice pools' and the text forms'; and since its entry
# points hand their arguments on as they are (issue #22), what moves them off
# a caller's stack and keeps them alive.
KERNEL_ABSENT = ["func NewStackBlock", "func AcquireStackBlock", "func ReleaseStackBlock",
                 "func gsNewBlock", "func gsReserveRegion", "func gsMap", "func (gs *GsStackBlock)",
                 "func GsSpans", "gsSpLeaf", "func GsFmt", "func GsPutStr", "func gsAppendFloat",
                 "func GsOut", "type gsProgram", "gsLibStart", "gsInitGlobals", '"syscall"', '"sync"',
                 '"os"', "gsEscape", "func GsSliceOf", "func GsBytesOf", "KeepAlive"]
KERNEL_MAX_LINES = 450
# The workers' and queues' runtime (§2.3), which a program that spawns no
# workers and puts on no queue has no use for, though its globals be in
# program instances (issue #25): every declaration of the file, and its
# section's comment.
THREADS_RUNTIME = RUNTIME / "goose_threads.go"
THREADS_SECTION = "Workers and typed queues"
# A top-level declaration of Go source as gofmt lays it out: a method's
# receiver type and name, a func's, a type's or a var's name.
GO_DECL = re.compile(r"^(?:func \(\w+ \*?(\w+)[^)]*\) (\w+)|func (\w+)|type (\w+)|var (\w+))", re.MULTILINE)


def go_declared(text):
    """The top-level declarations of Go source (GO_DECL): each name, a
    method's as Type.Method, with its first letter lowercase, as a file
    holding the runtime unexports some of the runtime's names and
    --go-unexported every one."""
    def low(n):
        return n[:1].lower() + n[1:]
    return {f"{low(recv)}.{method}" if recv else low(fn or ty or var)
            for recv, method, fn, ty, var in GO_DECL.findall(text)}

# The export libraries whose Go tests run under the race detector as well,
# where cgo can build it (issue #42): those of a Go []string and of what the
# collector must keep alive, whose tests leave out under it what sync.Pool,
# which drops a quarter of what it is given there, makes untrue.
EXPORT_RACE = {"string_slices", "keepalive"}

# The line Go's tools know generated code by (go help generate), which a
# generated file's header keeps as it was (issue #21).
GO_GENERATED = re.compile(r"^// Code generated .* DO NOT EDIT\.$")

# Prebuilt compilers (issue #29): the script that lays out a release of the
# compiler, and the module of cmd/goose, the wrapper `go run` runs, which
# downloads one and runs it. The suite serves the wrapper from a module
# proxy of its own at WRAPPER_TEST_VERSION, and the compiler under test as
# the release of that name (Runner.release).
RELEASE_SCRIPT = REPO / "scripts" / "release.py"
WRAPPER = REPO / "cmd" / "goose"
WRAPPER_MODULE = "github.com/robtandy/goose.go/cmd/goose"
WRAPPER_TEST_VERSION = "v0.0.0-test"
# The platforms there are prebuilt compilers for (GOOS, GOARCH), and
# TinyCC's license files, which a release holds, as the one of what goose
# is linked from that has its own.
RELEASE_PLATFORMS = {("linux", "amd64"), ("linux", "arm64"), ("darwin", "arm64")}
RELEASE_LICENSES = {"licenses/tinycc/COPYING": UPSTREAM / "third_party" / "tinycc" / "COPYING",
                    "licenses/tinycc/RELICENSING": UPSTREAM / "third_party" / "tinycc" / "RELICENSING"}
# The README's library, which `go generate` compiles with the wrapper, and
# a test of its entry points.
STATS_GOOSE = """\
import std;

// The number of words in text: runs of bytes that are not spaces.
export fn count_words(text: const u8[:]) -> i64 {
    var n = 0;
    var inword = false;
    for c in text {
        let space = c == ' ' || c == '\\n' || c == '\\t';
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
"""
STATS_TEST = """\
package stats

import "testing"

func TestStats(t *testing.T) {
	if n := CountWords("the quick brown fox"); n != 4 {
		t.Errorf("CountWords = %d, want 4", n)
	}
	xs := []int64{9, 1, 7, 3, 5}
	if m := Median(xs); m != 5 || xs[0] != 9 {
		t.Errorf("Median = %d, xs = %v, want 5 and xs as it was", m, xs)
	}
}
"""


def release_script():
    """scripts/release.py as a module (issue #29), which the suite packages
    releases of the compiler under test with (Runner.release)."""
    spec = importlib.util.spec_from_file_location("goose_release", RELEASE_SCRIPT)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def tar_contents(path):
    """The members of a gzipped tar: {name: (TarInfo, bytes, or None for
    what is not a file)}."""
    with tarfile.open(path, "r:gz") as t:
        return {m.name: (m, t.extractfile(m).read() if m.isfile() else None) for m in t.getmembers()}


def go_runtime_revision():
    """The runtime's revision a generated file's header records (issue #21):
    a hash of the text of the files of runtime/go the compiler embeds (every
    one but its tests and RUNTIME_PACKAGE_ONLY), in the order of their
    names, each its name, a NUL, its text without carriage returns and a
    NUL; FNV-1a's 64 bits, as `goose version` hashes the C runtime's text,
    the high 48 of them in hex."""
    h = 0xcbf29ce484222325
    for p in sorted(RUNTIME.glob("*.go"), key=lambda p: p.name):
        if p.name.endswith("_test.go") or p.name in RUNTIME_PACKAGE_ONLY:
            continue
        for b in p.name.encode() + b"\0" + p.read_bytes().replace(b"\r", b"") + b"\0":
            h = ((h ^ b) * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return f"{h >> 16:012x}"


# Where an abort's message names its place: (file:line), at its end.
ABORT_PLACE = re.compile(r"\(([^()\n]+\.goose):(\d+)\)")


def go_file_names(text):
    """text, what a C run printed, with each place an abort names in a file
    of the standard library named as the Go backend names it (issue #21):
    by its place in the library, stdlib/std.goose, where the C backend
    names it by its whole path, which is this machine's. A C run is
    compared with the Go's so (Runner.c_run)."""
    libs = [os.path.normpath(UPSTREAM / "stdlib")]
    if os.environ.get("GOOSE_STDLIB"):
        libs.append(os.path.normpath(os.path.abspath(os.environ["GOOSE_STDLIB"])))

    def name(m):
        path = os.path.normpath(m[1])
        for lib in libs:
            if os.path.isabs(m[1]) and path.startswith(lib + os.sep):
                return f"(stdlib/{Path(path).relative_to(lib).as_posix()}:{m[2]})"
        return m[0]
    return ABORT_PLACE.sub(name, text)


def first_difference(a, b):
    """Where two texts first differ: the line, and each one's text of it."""
    la, lb = a.splitlines(), b.splitlines()
    for i in range(max(len(la), len(lb))):
        x = la[i] if i < len(la) else "<end>"
        y = lb[i] if i < len(lb) else "<end>"
        if x != y:
            return f"line {i + 1}:\n    {x}\n    {y}"
    return "in their line breaks"



def parity_samples():
    """The samples the Go backend's runs must match the C backend's on
    (Runner.sample): every one but those importing a module only the C
    backend has, which draws, plays sound or simulates physics through a C
    library (gfx, ui, audio and physics, tc.NATIVE_MODULES)."""
    return [f for f in sorted((UPSTREAM / "samples").glob("*.goose"))
            if not tc.native_imports(f.read_text(encoding="utf-8"))]


def run_capture(argv, cwd=None, timeout=None, stdin_path=None):
    """tc.run_capture, or with a limit on the time it takes: what runs on
    past it is stopped, and the result is (None, "", ""). The program reads
    the file stdin_path names as its stdin where given."""
    if timeout is None:
        return tc.run_capture(argv, cwd=cwd, stdin_path=stdin_path)
    try:
        with open(stdin_path, "rb") if stdin_path else contextlib.nullcontext() as f:
            r = subprocess.run([str(a) for a in argv], cwd=cwd, stdin=f, capture_output=True,
                               timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "", ""
    return r.returncode, tc.decode(r.stdout), tc.decode(r.stderr)


# Every number in a text: a sample's timings, masked where its stderr is
# compared (Runner.sample).
NUMBER = re.compile(r"\d+(?:\.\d+)?(?:e[+-]?\d+)?")


def masked(text):
    return NUMBER.sub("N", text)


def first_line(path):
    with open(path, encoding="utf-8", errors="replace") as f:
        return f.readline()


def goose_flags(path):
    """The compile flags a program's first line asks for, for both backends."""
    m = re.match(r"^// goose-flags:(.*)$", first_line(path).rstrip("\n"))
    return m[1].split() if m else []


def runtime_defines(path):
    """The -D a C suite fixture's first line asks every compile of it for
    (run_tests.py's runtime_defines)."""
    return ["-D" + d for d in re.findall(r"^// runtime-define: (\w+=\w+)$", first_line(path))]


def error_markers(path):
    return re.findall(r"^// error: (.+)$", path.read_text(encoding="utf-8"), re.MULTILINE)


def warning_markers(path):
    """The warnings a program declares it is compiled with, as run_tests.py
    reads them: a substring of each warning's message per `// warning:`
    line."""
    return re.findall(r"^// warning: (.+)$", path.read_text(encoding="utf-8"), re.MULTILINE)


def warning_lines(text):
    """The warnings a compile printed: (line, message) for each line that
    starts with a place and `warning:`, as run_tests.py reads them."""
    return [(m[0], m[1]) for m in (re.match(r"\S.*?: warning: (.*)$", line)
                                   for line in text.splitlines()) if m]


def unmatched_warnings(markers, warnings):
    """The markers no warning was printed for and the warning lines no marker
    declares, as run_tests.py pairs them: each marker with a warning line of
    its own whose message holds it, as many as can be paired (a maximum
    bipartite matching, since a marker can be part of several messages),
    a line repeating an earlier one offered last, so that it is the one
    left over where a warning prints twice."""
    lines = [line for line, _ in warnings]
    order = sorted(range(len(lines)), key=lambda w: lines[w] in lines[:w])
    owner = [None] * len(lines)

    def place(m, tried):
        for w in order:
            if w not in tried and markers[m] in warnings[w][1]:
                tried.add(w)
                if owner[w] is None or place(owner[w], tried):
                    owner[w] = m
                    return True
        return False

    missing = [marker for m, marker in enumerate(markers) if not place(m, set())]
    extra = [line for w, line in enumerate(lines) if owner[w] is None]
    return missing, extra


# --- random programs (--fuzz) --------------------------------------------------

FUZZ_INTS = ["i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64"]
FUZZ_FLOATS = ["f32", "f64"]
FUZZ_RANGE = {"i8": (-128, 127), "i16": (-32768, 32767), "i32": (-2**31, 2**31 - 1),
              "i64": (-2**63, 2**63 - 1), "u8": (0, 255), "u16": (0, 65535),
              "u32": (0, 2**32 - 1), "u64": (0, 2**64 - 1)}


def fuzz_literal(t, r):
    if t in FUZZ_FLOATS:
        v = r.choice([0.0, 1.0, -1.0, 0.5, 3.25, 1e10, -2.5e-3, 123456.789, 1e300, -0.0])
        return f"(({v!r}) as {t})" if t == "f32" else f"({v!r})"
    lo, hi = FUZZ_RANGE[t]
    v = r.choice([lo, hi, 0, 1, 2, r.randint(lo, hi), r.randint(max(lo, -100), 200)])
    # The type's least value has no positive literal to negate.
    return f"(({v + 1} as {t}) - 1)" if v == lo and lo < 0 else f"({v} as {t})"


def fuzz_expr(t, depth, names, r):
    """A random expression of type t over the variables `names`: (name, type)."""
    if depth <= 0 or r.random() < 0.25:
        own = [n for n, nt in names if nt == t]
        return r.choice(own) if own and r.random() < 0.8 else fuzz_literal(t, r)
    sub = lambda st=t: fuzz_expr(st, depth - 1, names, r)
    k = r.random()
    if t in FUZZ_FLOATS:
        if k < 0.6:
            return f"({sub()} {r.choice(['+', '-', '*', '/'])} {sub()})"
        if k < 0.75:
            return f"(-({sub()}))"
        return f"({sub(r.choice(FUZZ_INTS + FUZZ_FLOATS))} as {t})"
    if k < 0.45:
        return f"({sub()} {r.choice(['+', '-', '*', '&', '|', '^'])} {sub()})"
    if k < 0.6:
        # A divisor from 1 to 127, so that the program runs on.
        return f"({sub()} {r.choice(['/', '%'])} (({sub()} & (127 as {t})) | (1 as {t})))"
    if k < 0.7:
        return f"({sub()} {r.choice(['<<', '>>'])} {sub(r.choice(FUZZ_INTS))})"
    if k < 0.78:
        return f"(-({sub()}))" if t.startswith("i") else f"(~({sub()}))"
    return f"({sub(r.choice(FUZZ_INTS + FUZZ_FLOATS))} as! {t})"


def fuzz_program(seed):
    r = random.Random(seed)
    types = FUZZ_INTS + FUZZ_FLOATS
    params = [(f"p{i}", r.choice(types)) for i in range(6)]
    lines = [f"// Generated by run_go_tests.py --fuzz, seed {seed}.",
             f"fn compute({', '.join(f'{n}: {t}' for n, t in params)}) {{"]
    names = list(params)
    for i in range(25):
        t = r.choice(types)
        lines.append(f"    let v{i}: {t} = {fuzz_expr(t, r.randint(1, 4), names, r)};")
        names.append((f"v{i}", t))
        a = r.choice(names)
        b = r.choice([v for v in names if v[1] == a[1]])
        cmp = r.choice(["<", ">", "<=", ">=", "==", "!="])
        lines.append(f'    print("v{i} ", v{i}, " ", {a[0]} {cmp} {b[0]});')
    lines += ["}", "fn main() {"]
    for _ in range(3):
        lines.append(f"    compute({', '.join(fuzz_literal(t, r) for _, t in params)});")
    lines.append("}")
    return "\n".join(lines) + "\n"


# --- random control flow (--fuzz-flow) ------------------------------------------


class FlowProgram:
    """A random program whose control flow ends in every position Go's flow
    analysis follows: returns, breaks, continues and guards in conditions,
    scrutinees, arguments, loop bounds, print arguments and values, with
    code after them that is never reached, and defers, labels and loops
    around them. Every loop is bounded; `trail` records the calls made."""

    def __init__(self, seed):
        self.r = random.Random(seed)
        self.n = 0
        self.fns = []

    def fresh(self, prefix):
        self.n += 1
        return f"{prefix}{self.n}"

    @staticmethod
    def sub(ctx, **kw):
        # ctx: vars [(name, type, mutable)]; brk, the innermost break target
        # (None, "plain" or "valued"); cont, whether a continue may be made
        # (never in a while's own condition, which it would run again);
        # noexit, for a value's statements: none may always leave, which
        # would make the value void; depth, of the constructs left to nest.
        c = dict(ctx)
        c["vars"] = list(ctx["vars"])
        c.update(kw)
        return c

    def block(self, ctx, lo=0, hi=3, noexit=False):
        c = self.sub(ctx, depth=ctx["depth"] - 1, noexit=noexit)
        return " ".join(self.stmt(c) for _ in range(self.r.randint(lo, hi)))

    def exit(self, ctx):
        k = self.r.choice(["return"] + ["break"] * bool(ctx["brk"]) + ["continue"] * ctx["cont"])
        if k == "return":
            return f"return {self.expr(ctx, 1)};"
        if k == "break":
            return f"break {self.expr(ctx, 1)};" if ctx["brk"] == "valued" else "break;"
        return "continue;"

    def stmt(self, ctx):
        r, d, ne = self.r, ctx["depth"], ctx.get("noexit", False)
        kinds = ["let", "let", "var", "note", "print", "defer", "assert"]
        kinds += [] if ne else ["exit", "exit"]
        if any(m for _, t, m in ctx["vars"] if t == "i64"):
            kinds += ["assign"]
        if d > 0:
            kinds += ["if", "if", "guard", "while", "while", "loop", "for", "block", "scope",
                      "match", "letb"]
        k = r.choice(kinds)
        if k in ("let", "var", "letb"):
            v = self.fresh({"let": "x", "var": "m", "letb": "b"}[k])
            init = self.cond(ctx, 2) if k == "letb" else self.expr(ctx, 2)
            ctx["vars"].append((v, "bool" if k == "letb" else "i64", k == "var"))
            return f"{'var' if k == 'var' else 'let'} {v} = {init};"
        if k == "assign":
            v = r.choice([n for n, t, m in ctx["vars"] if t == "i64" and m])
            return f"{v} = ({self.expr(ctx, 2)}) % 1000;"
        if k == "note":
            return f"note({self.expr(ctx, 2)});"
        if k == "print":
            return f'print("p ", {self.expr(ctx, 2)}, " ", {self.cond(ctx, 1)});'
        if k == "exit":
            return self.exit(ctx)
        if k == "defer":
            return f"defer note({r.randint(1, 9)});"
        if k == "assert":
            return f"assert({self.cond(ctx, 2)} || true);"
        if k == "if":
            s = f"if {self.cond(ctx, 2)} {{ {self.block(ctx)} }}"
            # Under noexit, the else may not always leave: the if would.
            return s + (f" else {{ {self.block(ctx, noexit=ne)} }}" if r.random() < 0.5 else "")
        if k == "guard":
            return f"guard {self.cond(ctx, 2)} else {{ {self.block(ctx, 0, 2)} {self.exit(ctx)} }}"
        if k in ("while", "loop"):
            kv, m = self.fresh("k"), r.randint(0, 3)
            body = self.sub(ctx, brk="plain", cont=True, noexit=False)
            if k == "loop":
                return (f"var {kv} = 0; loop {{ {kv} += 1; if {kv} > {m} {{ break; }} "
                        f"{self.block(body)} }}")
            cond = f"{kv} < {m}"
            if r.random() < 0.5:
                # A condition that runs statements: it may break the loop or
                # return, as the statements in its ifs do.
                cc = self.sub(ctx, brk="plain", cont=False, depth=d - 1, noexit=True)
                cond = f"{{ {' '.join(self.stmt(cc) for _ in range(r.randint(0, 2)))} {cond} }}"
            return f"var {kv} = 0; while {cond} {{ {kv} += 1; {self.block(body)} }}"
        if k == "for":
            iv = self.fresh("ix")
            hi = self.expr(self.sub(ctx, depth=d - 1), 2)
            body = self.sub(ctx, brk="plain", cont=True, noexit=False)
            body["vars"].append((iv, "i64", False))
            return f"for {iv} in 0..(({hi}) % 4) {{ {self.block(body)} }}"
        if k == "block":
            return f"block {{ {self.block(self.sub(ctx, brk='plain'), noexit=ne)} }}"
        if k == "scope":
            return f"{{ {self.block(ctx, noexit=ne)} }}"
        arms = ", ".join(f"{p} => {{ {self.block(ctx, 0, 2, noexit=ne and p == '_')} }}"
                         for p in ("0", "1", "_"))
        return f"match ({self.expr(ctx, 2)}) % 3 {{ {arms} }}"

    def expr(self, ctx, d):
        r = self.r
        names = [n for n, t, _ in ctx["vars"] if t == "i64"]
        if d <= 0 or r.random() < 0.3:
            return r.choice(names) if names and r.random() < 0.6 else str(r.randint(-3, 9))
        kinds = ["note", "add", "block", "exits", "exits", "if", "match"]
        kinds += ["loop", "early"] if ctx["depth"] > 0 else []
        kinds += ["call"] if self.fns else []
        kinds += ["apply"] if names else []
        k = r.choice(kinds)
        e = lambda c=ctx: self.expr(c, d - 1)
        if k == "note":
            return f"note({e()})"
        if k == "add":
            return f"({e()} + {e()})"
        if k == "block":
            b = self.sub(ctx, depth=ctx["depth"] - 1)
            return f"({{ {self.block(b, 0, 2, noexit=True)} {e(b)} }})"
        if k == "exits":
            return f"({{ if {self.cond(ctx, d - 1)} {{ {self.exit(ctx)} }} {e()} }})"
        if k == "if":
            return f"(if {self.cond(ctx, d - 1)} {{ {e()} }} else {{ {e()} }})"
        if k == "match":
            return f"(match ({e()}) % 3 {{ 0 => {e()}, 1 => {e()}, _ => {e()} }})"
        if k == "loop":
            kv = self.fresh("k")
            body = self.sub(ctx, brk="valued", cont=True, depth=ctx["depth"] - 1, noexit=False)
            # The break's value may break the loop too, but a continue would
            # skip the break on every iteration from then on.
            value = e(self.sub(body, cont=False))
            return (f"({{ var {kv} = 0; loop {{ {kv} += 1; if {kv} > {r.randint(0, 2)} "
                    f"{{ break {value}; }} {self.block(body, 0, 2)} }} }})")
        if k == "early":
            b = self.sub(ctx, brk="valued", depth=ctx["depth"] - 1)
            return f"(block {{ {self.block(b, 0, 2, noexit=True)} {e(b)} }})"
        if k == "call":
            return f"{r.choice(self.fns)}({e()}, {self.cond(ctx, d - 1)})"
        return f"(apply({e()}) {{ it + {r.choice(names)} }})"

    def cond(self, ctx, d):
        r = self.r
        names = [n for n, t, _ in ctx["vars"] if t == "bool"]
        if d <= 0 or r.random() < 0.3:
            return r.choice(names) if names and r.random() < 0.6 else r.choice(["true", "false"])
        k = r.choice(["<", "==", "&&", "||", "!", "block", "exits"])
        if k in ("<", "=="):
            return f"({self.expr(ctx, d - 1)} {k} {self.expr(ctx, d - 1)})"
        if k in ("&&", "||"):
            return f"({self.cond(ctx, d - 1)} {k} {self.cond(ctx, d - 1)})"
        if k == "!":
            return f"!({self.cond(ctx, d - 1)})"
        if k == "block":
            b = self.sub(ctx, depth=ctx["depth"] - 1)
            return f"({{ {self.block(b, 0, 2, noexit=True)} {self.cond(b, d - 1)} }})"
        return f"({{ if {self.cond(ctx, d - 1)} {{ {self.exit(ctx)} }} {self.cond(ctx, d - 1)} }})"

    def text(self, seed):
        lines = [f"// Generated by run_go_tests.py --fuzz-flow, seed {seed}.",
                 "var trail: i64 = 0;",
                 "fn note(n: i64) -> i64 { trail = (trail * 7 + n) % 1000003; n }",
                 "fn apply<F>(x: i64) -> i64 { F(x) }"]
        for i in range(self.r.randint(2, 4)):
            ctx = {"vars": [("a", "i64", False), ("c", "bool", False)], "brk": None,
                   "cont": False, "noexit": False, "depth": 3}
            body = " ".join(self.stmt(ctx) for _ in range(self.r.randint(1, 5)))
            lines.append(f"fn f{i}(a: i64, c: bool) -> i64 {{ {body} {self.expr(ctx, 2)} }}")
            self.fns.append(f"f{i}")
        calls = [f'trail = 0; print({f}({a}, {c}), " ", trail);'
                 for f in self.fns for a in (0, 1, 5) for c in ("true", "false")]
        lines.append("fn main() { " + " ".join(calls) + " }")
        return "\n".join(lines) + "\n"


def flow_program(seed):
    return FlowProgram(seed).text(seed)


# --- random fixed-size values (--fuzz-values) ----------------------------------


class ValueProgram:
    """A random program of fixed-size values: structs packed and padded,
    nested, ADTs, fixed and limited arrays, built from literals, written
    field by field and element by element, compared, copied, matched,
    handed to functions by value and by reference, sliced, and printed."""

    SCALARS = ["i8", "i16", "i32", "i64", "u8", "u16", "u32", "u64", "f32", "f64", "bool"]
    RANGES = {"i8": (-128, 127), "i16": (-32768, 32767), "i32": (-2**31, 2**31 - 1),
              "i64": (-2**63 + 1, 2**63 - 1), "u8": (0, 255), "u16": (0, 65535),
              "u32": (0, 2**32 - 1), "u64": (0, 2**63)}
    FLOATS = ["0.0", "-0.0", "1.5", "-2.25", "1e10", "3.0e-5", "1024.0", "0.1"]
    STRINGS = ['""', '"a"', '"goose"', '"tab\\there"', '"q\\"d"', '"two words"']
    # Leaves that are no scalar: a string literal's slice, and an optional
    # reference to one of the program's scalar globals (TARGETS), which
    # outlive anything holding a reference to them.
    TARGETS = {"i64": ["t0", "t1"], "f64": ["t2"], "u8": ["t3"]}

    def __init__(self, seed):
        self.r = random.Random(seed)
        self.structs = {}   # name -> [(field or None for a pad, type, pad text)]
        self.enums = {}     # name -> [(variant, [(field, type)])]
        self.order = []     # declarations, in order
        self.n = 0

    def fresh(self, p):
        self.n += 1
        return f"{p}{self.n}"

    # Types: a scalar name, ("s", name), ("e", name), ("a", elem, k), ("l", elem, k).
    def spell(self, t):
        if isinstance(t, str):
            return t
        if t[0] == "str":
            return "const u8[:]"
        if t[0] == "opt":
            return f"{t[1]}?"
        if t[0] in ("s", "e"):
            return t[1]
        inner = self.spell(t[1])
        if not isinstance(t[1], str) and t[1][0] in ("a", "l", "str", "opt"):
            inner = f"({inner})"
        return f"{inner}[{t[2]}]" if t[0] == "a" else f"{inner}[..{t[2]}]"

    def rtype(self, depth, elem=False, top=False):
        r = self.r
        k = r.random()
        if k < 0.06 and not top:
            return ("str",)
        if k < 0.1 and not top:
            return ("opt", r.choice(list(self.TARGETS)))
        if depth <= 0 or k < 0.45:
            return r.choice(self.SCALARS)
        if k < 0.62 and self.structs:
            return ("s", r.choice(list(self.structs)))
        if k < 0.72 and self.enums and not elem:
            return ("e", r.choice(list(self.enums)))
        if k < 0.86:
            return ("a", self.rtype(depth - 1, True), r.randint(1, 4))
        return ("l", self.rtype(depth - 1, True), r.randint(1, 5))

    def declare(self):
        r = self.r
        for _ in range(r.randint(2, 5)):
            if r.random() < 0.3:
                name = self.fresh("E")
                variants = []
                for v in range(r.randint(1, 4)):
                    nf = r.choice([0, 0, 1, 2, 3])
                    variants.append((f"V{v}", [(f"p{i}", self.rtype(1)) for i in range(nf)]))
                self.enums[name] = variants
                self.order.append(("e", name))
            else:
                name = self.fresh("S")
                fields = []
                for i in range(r.randint(1, 5)):
                    if r.random() < 0.15:
                        fields.append((None, None, r.choice(["pad", "pad 1", "pad 3"])))
                    fields.append((f"f{i}", self.rtype(2), None))
                self.structs[name] = fields
                self.order.append(("s", name))

    def decl_text(self):
        out = []
        for kind, name in self.order:
            if kind == "s":
                parts = [p if f is None else f"{f}: {self.spell(t)}" for f, t, p in self.structs[name]]
                out.append(f"struct {name} {{ {', '.join(parts)} }}")
            else:
                vs = []
                for v, fs in self.enums[name]:
                    vs.append(f"{v} {{ {', '.join(f'{f}: {self.spell(t)}' for f, t in fs)} }}" if fs else v)
                out.append(f"enum {name} {{ {', '.join(vs)} }}")
        return out

    def lit(self, t):
        r = self.r
        if not isinstance(t, str) and t[0] == "str":
            return r.choice(self.STRINGS)
        if not isinstance(t, str) and t[0] == "opt":
            return r.choice(["null"] + self.TARGETS[t[1]])
        if isinstance(t, str):
            if t == "bool":
                return r.choice(["true", "false"])
            if t in ("f32", "f64"):
                return r.choice(self.FLOATS)
            lo, hi = self.RANGES[t]
            v = r.choice([lo, hi, 0, 1, r.randint(lo, hi), r.randint(max(lo, -50), 50)])
            return f"({v + 1} - 1)" if v == lo and lo < 0 else str(v)
        if t[0] == "s":
            inits = [f"{f}: {self.lit(ft)}" for f, ft, _ in self.structs[t[1]] if f is not None]
            return f"{t[1]} {{ {', '.join(inits)} }}"
        if t[0] == "e":
            v, fs = r.choice(self.enums[t[1]])
            if not fs:
                return f"{t[1]}.{v}"
            return f"{t[1]}.{v} {{ {', '.join(f'{f}: {self.lit(ft)}' for f, ft in fs)} }}"
        n = t[2] if t[0] == "a" else r.randint(0, t[2])
        if t[0] == "a" and n > 2 and r.random() < 0.3:
            return f"[{self.lit(t[1])}; {n}]"
        return f"[{', '.join(self.lit(t[1]) for _ in range(n))}]"

    def paths(self, t, base, depth=3):
        """Assignable leaf paths into a value of type t, with their types."""
        out = []
        if isinstance(t, str) or t[0] in ("str", "opt"):
            return [(base, t)]
        if depth <= 0:
            return out
        if t[0] == "s":
            for f, ft, _ in self.structs[t[1]]:
                if f is not None:
                    out += self.paths(ft, f"{base}.{f}", depth - 1)
        elif t[0] == "a":
            out += self.paths(t[1], f"{base}[{self.r.randrange(t[2])}]", depth - 1)
        return out

    def stmt(self, var, t, fns):
        r = self.r
        k = r.random()
        if k < 0.25:
            return f"print({var});"
        if k < 0.45:
            ps = self.paths(t, var)
            if ps:
                p, pt = r.choice(ps)
                if not isinstance(pt, str):
                    # A reference is rebound; a slice is assigned.
                    return f"{p} {'.=' if pt[0] == 'opt' else '='} {self.lit(pt)}; print({p});"
                if pt not in ("bool",) and r.random() < 0.5:
                    op = r.choice(["+=", "-=", "*="]) if pt not in ("f32", "f64") else r.choice(["+=", "*="])
                    return f"{p} {op} {self.lit(pt)};"
                return f"{p} = {self.lit(pt)};"
            return f"print({var});"
        if k < 0.55:
            w = self.fresh("w")
            ps = self.paths(t, w)
            change = ""
            if ps:
                p, pt = r.choice(ps)
                op = ".=" if not isinstance(pt, str) and pt[0] == "opt" else "="
                change = f" {p} {op} {self.lit(pt)};"
            return (f"var {w} = {var}; print({w} == {var}, \" \", {w} != {var});{change} "
                    f"print({w} == {var}, \" \", {w});")
        if k < 0.62:
            return f"{var} = {self.lit(t)}; print({var});"
        if k < 0.72 and not isinstance(t, str) and t[0] == "e":
            arms = []
            for v, fs in self.enums[t[1]]:
                if fs and r.random() < 0.8:
                    b = self.fresh("b")
                    arms.append(f"{v} {b} => print(\"{v} \", {b}, \" \", {b}.{fs[0][0]})")
                else:
                    arms.append(f"{v} => print(\"{v}\")")
            if r.random() < 0.4 and len(arms) > 1:
                arms = arms[:-1] + ["_ => print(\"other\")"]
            return f"match {var} {{ {', '.join(arms)} }}"
        if k < 0.82 and not isinstance(t, str) and t[0] == "l":
            e = t[1]
            return (f"if {var}.len < {var}.cap {{ {var}.push({self.lit(e)}); }} "
                    f"if {var}.len > 0 {{ print({var}.pop(), \" \", {var}.len); }} "
                    f"print({var}, \" \", {var}.len);")
        if k < 0.88 and not isinstance(t, str) and t[0] in ("a", "l"):
            return (f"if {var}.len > 1 {{ print(first({var}[1..]), \" \", {var}[..1], \" \", "
                    f"{var}[1..] == {var}[1..]); }}")
        if k < 0.94:
            show, mut = fns[self.spell(t)]
            return f"{show}({var}); {mut}({var}); print({var});"
        # A literal compared needs a type of its own where it is an array's.
        c = self.fresh("c")
        return f"let {c}: {self.spell(t)} = {self.lit(t)}; print({var} == {c}, \" \", {c} != {var});"

    def text(self, seed):
        r = self.r
        self.declare()
        lines = [f"// Generated by run_go_tests.py --fuzz-values, seed {seed}."]
        lines += self.decl_text()
        lines.append("fn first<T>(s: const T[:]) -> T { s[0] }")
        for tt, names in self.TARGETS.items():
            for i, n in enumerate(names):
                lines.append(f"var {n}: {tt} = {self.lit(tt) if tt != 'f64' else '0.5'};")
        vars_ = [(f"v{i}", self.rtype(3, top=True)) for i in range(r.randint(3, 6))]
        globs = [(f"g{i}", self.rtype(2, top=True)) for i in range(r.randint(0, 2))]
        fns = {}
        for _, t in vars_ + globs:
            sp = self.spell(t)
            if sp in fns:
                continue
            show, mut = self.fresh("show"), self.fresh("mut")
            fns[sp] = (show, mut)
            lines.append(f"fn {show}(x: {sp}) {{ print(\"show \", x); }}")
            ps = self.paths(t, "x")
            body = ""
            if ps:
                p, pt = r.choice(ps)
                op = ".=" if not isinstance(pt, str) and pt[0] == "opt" else "="
                body = f"{p} {op} {self.lit(pt)};"
            lines.append(f"fn {mut}(x: {sp}&) {{ {body} }}")
        for g, t in globs:
            lines.append(f"var {g}: {self.spell(t)} = {self.lit(t)};")
        lines.append("fn main() {")
        for v, t in vars_:
            lines.append(f"    var {v}: {self.spell(t)} = {self.lit(t)};")
        every = vars_ + globs
        for _ in range(r.randint(10, 25)):
            v, t = r.choice(every)
            lines.append("    " + self.stmt(v, t, fns))
        for v, _ in every:
            lines.append(f"    print({v});")
        lines.append("}")
        return "\n".join(lines) + "\n"


def value_program(seed):
    return ValueProgram(seed).text(seed)


# --- data stack overflow -----------------------------------------------------

# A line print renders that runs past its stack's region, with 64 KB
# regions (no page is larger), by its literal or inside a value's text, and
# lines that just fit, beside the cases that just do not: name -> (the
# literal's length, the value's type and text and its text's length).
OVERFLOW_REGION = 65536
OVERFLOW_CASES = {"literal": (70000, None), "fits": (OVERFLOW_REGION - 1, None),
                  "newline": (OVERFLOW_REGION, None)}
for kind, value, n in (("i64", "-9223372036854775807 - 1", 20),
                       ("u64", "18446744073709551615", 20),
                       ("f64", "2.5", 3), ("f32", "2.5", 3), ("bool", "true", 4)):
    OVERFLOW_CASES[f"{kind}-fits"] = (OVERFLOW_REGION - 1 - n, (kind, value))
    OVERFLOW_CASES[kind] = (OVERFLOW_REGION - n + 1, (kind, value))


def overflow_program(name):
    n, value = OVERFLOW_CASES[name]
    decl = f"    let v: {value[0]} = {value[1]};\n" if value else ""
    return (f"// Generated by run_go_tests.py: data stack overflow, {name}.\n"
            f"fn main() {{\n{decl}    print(\"start\");\n"
            f"    print(\"{'x' * n}\"{', v' if value else ''});\n    print(\"end\");\n}}\n")


# --- the code Go makes of wide loads and stores (issue #23) -------------------

# The targets whose code Runner.wide_asm reads, which Go cross-compiles to
# with nothing but its toolchain.
WIDE_ARCHES = ("arm64", "amd64")
# The bytes each move of a target's moves.
WIDE_WIDTHS = {
    "arm64": {"MOVD": 8, "MOVW": 4, "MOVWU": 4, "MOVH": 2, "MOVHU": 2, "MOVB": 1, "MOVBU": 1,
              "FMOVD": 8, "FMOVS": 4, "FMOVQ": 16, "LDP": 16, "STP": 16, "LDPW": 8,
              "STPW": 8},
    "amd64": {"MOVQ": 8, "MOVL": 4, "MOVW": 2, "MOVB": 1, "MOVBLZX": 1, "MOVBQZX": 1,
              "MOVBLSX": 1, "MOVBQSX": 1, "MOVWLZX": 2, "MOVWQZX": 2, "MOVWLSX": 2,
              "MOVWQSX": 2, "MOVLQZX": 4, "MOVLQSX": 4, "MOVSD": 8, "MOVSS": 4, "MOVUPS": 16,
              "MOVOU": 16},
}
# An operand that is memory reached through a general register: not the
# function's own stack (SP, RSP, FP), static data (SB) or g's (g, R14).
WIDE_MEM = {
    "arm64": re.compile(r"(?<![$\w])-?\d*\(R\d+\)"),
    "amd64": re.compile(r"(?<![$\w])-?\d*\((?:AX|BX|CX|DX|SI|DI|BP|R8|R9|R10|R11|R12|R13|R15)\)"),
}


def wide_annotations(src):
    """The export fns of src an `// asm:` comment line above names what Go must
    make of: name -> ("load" or "store", its bytes), or ("none", 0)."""
    want, pending = {}, None
    for line in src.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^// asm: (?:(load|store) (\d+)|(none))$", line)
        if m:
            pending = ("none", 0) if m[3] else (m[1], int(m[2]))
            continue
        m = re.match(r'^export (?:"\w+" )?fn (\w+)\(', line)
        if m and pending:
            want[m[1]] = pending
        if not line.startswith("//"):
            pending = None
    return want


def wide_accesses(arch, asm, fn):
    """The memory accesses through a register of the function fn in the
    output of `go build -gcflags=-S` for arch: ("load" or "store", bytes,
    the instruction) for each, in order, or None where fn is not there. A
    move is a store where its last operand, the destination, is the memory."""
    mem = WIDE_MEM[arch]
    found = None
    for line in asm.splitlines():
        m = re.match(r"^(\S+) STEXT", line)
        if m:
            if found is not None:
                break
            if m[1].endswith("." + fn):
                found = []
            continue
        if found is None:
            continue
        parts = line.split("\t")
        if not line.startswith("\t0x") or len(parts) < 3:
            if line and not line.startswith("\t"):
                break
            continue
        op, args = parts[2].strip(), "\t".join(parts[3:]).strip()
        # An address computed, or jumped to (arm64's `RET (R30)`), is no access.
        if op.startswith("LEA") or op in ("PCDATA", "FUNCDATA", "RET", "JMP", "CALL") or \
                not mem.search(args):
            continue
        operands, depth, cur = [], 0, ""
        for c in args:
            depth += c in "(["
            depth -= c in ")]"
            if c == "," and not depth:
                operands.append(cur.strip())
                cur = ""
            else:
                cur += c
        operands.append(cur.strip())
        kind = "store" if len(operands) > 1 and mem.search(operands[-1]) else "load"
        found.append((kind, WIDE_WIDTHS[arch].get(op.split(".")[0], 0), f"{op} {args}"))
    return found


# --- the code Go makes of selects (issue #26) ---------------------------------

# A target's conditional selects, which Runner.select_asm counts: arm64's
# CSEL and its kin, which choose between two registers on the flags (CSINC,
# CSINV and CSNEG take the second one stepped, inverted or negated; CSET,
# CSETM, CINC, CINV and CNEG are their aliases), and amd64's CMOVs.
SELECT_OPS = {
    "arm64": re.compile(r"^(?:(?:CSEL|CSINC|CSINV|CSNEG|CSET|CSETM|CINC|CINV|CNEG)W?|FCSEL[SD])$"),
    "amd64": re.compile(r"^CMOV[QLW]"),
}
# Its conditional branches: on the flags, or on a register's zero or bit.
BRANCH_OPS = {
    "arm64": re.compile(r"^(?:B(?:EQ|NE|CS|HS|CC|LO|MI|PL|VS|VC|HI|LS|GE|LT|GT|LE)|CBN?ZW?|TBN?Z)$"),
    "amd64": re.compile(r"^J(?:CC|CS|CXZ[LQ]|EQ|GE|GT|HI|LE|LS|LT|MI|NE|OC|OS|PC|PL|PS)$"),
}


def select_annotations(src):
    """The export fns of src an `// asm: select, branches m` comment line above
    names: name -> m."""
    want, pending = {}, None
    for line in src.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^// asm: select, branches (\d+)$", line)
        if m:
            pending = int(m[1])
            continue
        m = re.match(r'^export (?:"\w+" )?fn (\w+)\(', line)
        if m and pending is not None:
            want[m[1]] = pending
        if not line.startswith("//"):
            pending = None
    return want


def select_counts(arch, asm, fn):
    """The conditional selects and the conditional branches of the function
    fn in the output of `go build -gcflags=-S` for arch, each a list of its
    instructions, or None where fn is not there."""
    found = None
    for line in asm.splitlines():
        m = re.match(r"^(\S+) STEXT", line)
        if m:
            if found is not None:
                break
            if m[1].endswith("." + fn):
                found = ([], [])
            continue
        if found is None:
            continue
        parts = line.split("\t")
        if not line.startswith("\t0x") or len(parts) < 3:
            if line and not line.startswith("\t"):
                break
            continue
        op, args = parts[2].strip(), "\t".join(parts[3:]).strip()
        if SELECT_OPS[arch].match(op):
            found[0].append(f"{op} {args}")
        elif BRANCH_OPS[arch].match(op):
            found[1].append(f"{op} {args}")
    return found


# --- the code Go makes of bounds checks (issue #40) ---------------------------

# A named slot of a function's frame on Go's stack, which Go spills a value
# to and restores it from: a local's or a temporary's (`pkg.i_g-56(SP)`,
# `pkg..autotmp_3+8(SP)`), or an argument's in its caller's frame
# (`pkg.v_g(FP)`). Not the address of one (`$pkg.a_g-128(SP)`), an element of
# a Go array there (`pkg.a_g+8(SP)(AX*8)`), nor the frame's own words, which
# have no name: the return address, the frame pointer, the arguments
# morestack saves.
SPILL_SLOT = re.compile(r"^[^$\s]\S*[A-Za-z_]\S*\((?:SP|FP)\)$")
# The calls of Go's own bounds checks' failures: runtime.panicIndex and its
# kin, runtime.panicBounds in later Go.
INDEX_PANIC = re.compile(r"\bruntime\.panic(?:Index|Bounds)\w*\(SB\)")


def spill_annotations(src):
    """The export fns of src an `// asm: no spills` or `// asm: no index
    checks` comment line above names: name -> "spills" or "index checks"."""
    want, pending = {}, None
    for line in src.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^// asm: no (spills|index checks)$", line)
        if m:
            pending = m[1]
            continue
        m = re.match(r'^export (?:"\w+" )?fn (\w+)\(', line)
        if m and pending:
            want[m[1]] = pending
        if not line.startswith("//"):
            pending = None
    return want


def asm_insns(asm, fn):
    """The instructions of the function fn in the output of `go build
    -gcflags=-S`, each its operation and its operands, or None where fn is
    not there."""
    found = None
    for line in asm.splitlines():
        m = re.match(r"^(\S+) STEXT", line)
        if m:
            if found is not None:
                break
            if m[1].endswith("." + fn):
                found = []
            continue
        if found is None:
            continue
        parts = line.split("\t")
        if not line.startswith("\t0x") or len(parts) < 3:
            if line and not line.startswith("\t"):
                break
            continue
        operands, depth, cur = [], 0, ""
        for c in "\t".join(parts[3:]).strip():
            depth += c in "(["
            depth -= c in ")]"
            if c == "," and not depth:
                operands.append(cur.strip())
                cur = ""
            else:
                cur += c
        operands.append(cur.strip())
        found.append((parts[2].strip(), [o for o in operands if o]))
    return found


def spill_moves(asm, fn):
    """The instructions of the function fn in the output of `go build
    -gcflags=-S` that move a value between a register and a named slot of
    its frame (SPILL_SLOT), each as text, or None where fn is not there:
    every spill and every restore, but an argument's spill to its slot in
    its caller's frame, which no loop holds: Go places a spill no deeper in
    loops than the value's definition, an argument's the entry."""
    insns = asm_insns(asm, fn)
    if insns is None:
        return None
    out = []
    for op, operands in insns:
        if op.startswith("LEA") or op in ("PCDATA", "FUNCDATA"):
            continue
        slots = [k for k, o in enumerate(operands) if SPILL_SLOT.match(o)]
        if not slots or (slots == [len(operands) - 1] and len(operands) > 1 and operands[-1].endswith("(FP)")):
            continue
        out.append(f"{op} {', '.join(operands)}")
    return out


def index_panics(asm, fn):
    """The calls of the function fn, in the output of `go build -gcflags=-S`,
    to the failure of a bounds check of Go's own (INDEX_PANIC), or None
    where fn is not there."""
    insns = asm_insns(asm, fn)
    if insns is None:
        return None
    return [f"{op} {', '.join(o)}" for op, o in insns if op == "CALL" and INDEX_PANIC.search(" ".join(o))]


def not_inlined(gosrc, mout, names):
    """The calls of the runtime functions names in the generated Go gosrc,
    `line:col name` each, that the output of `go build -gcflags=-m` of its
    package, mout, does not say Go inlines: Go places a call at its
    parenthesis, or at the name. Their declarations and comments are no
    calls."""
    inlined = set(re.findall(r"^\./[\w.]+:(\d+:\d+): inlining call to (\w+)", mout, re.M))
    out = []
    for i, line in enumerate(gosrc.splitlines(), 1):
        if line.lstrip().startswith("//"):
            continue
        for m in re.finditer(r"\b(\w+)\(", line):
            if m[1] not in names or line[:m.start()].endswith("func "):
                continue
            if (f"{i}:{m.end()}", m[1]) not in inlined and (f"{i}:{m.start() + 1}", m[1]) not in inlined:
                out.append(f"{i}:{m.end()} {m[1]}")
    return out


# --- the frames of locals handed to inlined bodies by reference (issue #24) ---

def frame_annotations(src):
    """The export fns of src a `// frame:` comment line above names the frame
    of at -O2: name -> the slots its Go function's frame on the fixed-frame
    stack holds, 0 for `none`."""
    want, pending = {}, None
    for line in src.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^// frame: (?:(\d+)|(none))$", line)
        if m:
            pending = 0 if m[2] else int(m[1])
            continue
        m = re.match(r'^export (?:"\w+" )?fn (\w+)\(', line)
        if m and pending is not None:
            want[m[1]] = pending
        if not line.startswith("//"):
            pending = None
    return want


def frame_slots(gosrc, fn):
    """The Go function fn of the generated Go gosrc, and the slots its frame
    on the fixed-frame stack holds: the offsets FinishBody declares past
    gs_fo0, one for each (DESIGN.md §3.6). None where fn is not there."""
    m = re.search(rf"^func {re.escape(fn)}\(.*?^}}$", gosrc, re.M | re.S)
    if not m:
        return None
    return m[0], len(re.findall(r"^\t\tgs_fo\d+ += gs_fo\d+ \+", m[0], re.M))


# --- helpers inlined into recursive cycles (issue #42) -------------------------

# The sources whose `// inlined:` and `// calls:` lines Runner.cycles checks,
# and the flags each compiles with besides -O2 --target=go.
CYCLE_SOURCES = [("programs/cycle_inlining.goose", []),
                 ("export/string_slices/string_slices.goose", ["--library"])]


def cycle_annotations(src):
    """The functions of src that `// inlined:` and `// calls:` comment lines
    above name helpers of: name -> (the helpers its Go function must call
    none of, those it must call)."""
    want, pending = {}, None
    for line in src.read_text(encoding="utf-8").splitlines():
        line = line.strip()
        m = re.match(r"^// (inlined|calls): (\w+(?:, \w+)*)$", line)
        if m:
            pending = pending or (set(), set())
            pending[0 if m[1] == "inlined" else 1].update(m[2].split(", "))
            continue
        m = re.match(r"^(?:recursive )?fn (\w+)[<(]", line)
        if m and pending is not None:
            want[m[1]] = pending
        if not line.startswith("//"):
            pending = None
    return want


def go_function(gosrc, name):
    """The Go function of the Goose function name in the generated Go gosrc:
    the text of the first whose Go name is name's with the _g of the
    program's own names (name_g, name_g2, or a nested function's
    outer_name_g), or None."""
    m = re.search(rf"^func (?:\w+_)?{re.escape(name)}_g\d*\(.*?^}}$", gosrc, re.M | re.S)
    return m[0] if m else None


def go_calls(text, name):
    """Whether the Go text calls the Go function of the Goose function name
    (name_g, name_g2, ...)."""
    return re.search(rf"\b(?:\w+_)?{re.escape(name)}_g\d*\(", text) is not None


# --- the entry points of pure kernels (issue #22) -----------------------------

# What an `// entry:` line can say of an export fn's entry points: Go can
# inline each; none lets a parameter viewing its caller's memory escape; each
# lets every one escape.
ENTRY_PROPS = {"inline", "no escape", "escape"}


def go_export_name(sym):
    """The Go name of an export fn's symbol (GoExportName): its words
    between the underscores, each capitalized."""
    return "".join(w[:1].upper() + w[1:] for w in sym.split("_"))


def entry_annotations(src):
    """The export fns of src an `// entry:` comment line above names what Go
    makes of their entry points: the Go name of each -> the set of what its
    line lists, of ENTRY_PROPS."""
    want, pending = {}, None
    for line in src.read_text(encoding="utf-8").splitlines():
        m = re.match(r"^// entry: (.+)$", line)
        if m:
            pending = set(m[1].split(", "))
            continue
        m = re.match(r'^export (?:"(\w+)" )?fn (\w+)\(', line)
        if m and pending is not None:
            want[go_export_name(m[1] or m[2])] = pending
        if not line.startswith("//"):
            pending = None
    return want


def go_entry_points(gosrc, name):
    """The entry points of the export fn whose Go name is name in the
    generated Go gosrc -- Name, NameWithStack, NameString and NameBytes, those
    it has -- each as (its name, its line, its parameters that view a
    caller's memory: slices, strings, pointers and the type parameters that
    stand for a string or a []byte, and whether it is generic)."""
    found = []
    for n, line in enumerate(gosrc.splitlines(), 1):
        m = re.match(rf"^func ({re.escape(name)}(?:WithStack|String|Bytes)?)(\[.*?\])?\((.*?)\)[^()]* \{{$", line)
        if not m:
            continue
        tparams = set(re.findall(r"(\w+) ~", m[2] or ""))
        views = []
        for p in m[3].split(", ") if m[3] else []:
            pn, _, pt = p.partition(" ")
            if pn != "gs" and (pt.startswith(("[]", "*")) or pt == "string" or pt in tparams):
                views.append(pn)
        found.append((m[1], n, views, bool(tparams)))
    return found


def call_pairs(testsrc):
    """The names X of the test source's functions callX that a directX
    follows: a call of an entry point, and the call of its function that
    entry point makes, which Runner.entry_points compares."""
    calls = re.findall(r"^func call(\w+)\(", testsrc, re.M)
    return [x for x in calls if re.search(rf"^func direct{x}\(", testsrc, re.M)]


def asm_instructions(asm, fn):
    """The instructions of the function fn in the output of `go build
    -gcflags=-S`, as text naming nothing of the function's own: without the
    TEXT line, the collector's data and the inline marks (NOP, arm64's HINT
    $0, amd64's XCHGL AX, AX), with each branch's target the index of the
    instruction it goes to, and the compiler's temporaries unnumbered. None
    where fn is not there."""
    insts, on = [], False
    for line in asm.splitlines():
        m = re.match(r"^(\S+) STEXT", line)
        if m:
            if on:
                break
            on = m[1].endswith("." + fn)
            continue
        if not on:
            continue
        parts = line.split("\t")
        if not line.startswith("\t0x") or len(parts) < 3:
            if line and not line.startswith("\t"):
                break
            continue
        op, args = parts[2].strip(), "\t".join(parts[3:]).strip()
        if op in ("TEXT", "PCDATA", "FUNCDATA", "NOP") or (op, args) in (("HINT", "$0"), ("XCHGL", "AX, AX")):
            continue
        insts.append((int(parts[1].split()[1]), op, re.sub(r"autotmp_\d+", "autotmp", args)))
    if not on:
        return None
    pcs = [pc for pc, _, _ in insts]
    out = []
    for _, op, args in insts:
        # A branch's target, a PC, is its last operand, a bare number.
        head, sep, last = args.rpartition(", ")
        if re.fullmatch(r"\d+", last):
            at = next((i for i, pc in enumerate(pcs) if pc >= int(last)), len(pcs))
            args = f"{head}{sep}->{at}"
        out.append(f"{op} {args}")
    return out


def go_env():
    """The Go tool's environment: the module and build flags of whoever runs
    the suite play no part in building one generated file, which builds,
    and is vetted, without cgo, as it must (AGENTS.md)."""
    env = dict(os.environ)
    env["GOFLAGS"] = ""
    env["GO111MODULE"] = "auto"
    env["CGO_ENABLED"] = "0"
    return env


class Runner:
    def __init__(self, args):
        default_exe = REPO / "build" / f"goose{tc.EXE_SUFFIX}"
        self.exe = tc.find_goose(args.exe or (str(default_exe) if default_exe.exists() else None))
        self.go = os.environ.get("GOOSE_GO") or shutil.which("go")
        self.gofmt = None
        self.verbose = args.verbose
        self.bless = args.bless
        self.work = Path(args.workdir).resolve() if args.workdir else \
            REPO / "build" / "go-tests"
        self.work.mkdir(parents=True, exist_ok=True)
        self.cc = None if args.jit_reference else tc.test_cc()
        self.lock = threading.Lock()
        self.failures = []
        self.oks = 0
        self.skips = []
        self.known = []
        self.release_dist = Path(args.release_dist).resolve() if args.release_dist else None

    # --- processes -----------------------------------------------------------

    def goose(self, *args, timeout=None, stdin_path=None):
        """The compiler, run from the repository's root (or UPSTREAM when a
        fixture lives under vendor/goose/), where a source named relative to
        it is named as the messages of what it compiles say: the same on
        every machine."""
        in_upstream = any(isinstance(a, Path) and a.is_relative_to(UPSTREAM) and a.suffix == ".goose"
                          for a in args)
        base = UPSTREAM if in_upstream else REPO
        args = [a.relative_to(base).as_posix() if isinstance(a, Path) and a.is_relative_to(base)
                and a.suffix == ".goose" else str(a) for a in args]
        return run_capture([self.exe] + args, cwd=base, timeout=timeout, stdin_path=stdin_path)

    def gotool(self, *args):
        r = subprocess.run([self.go] + [str(a) for a in args], cwd=self.work, env=go_env(),
                           capture_output=True)
        return r.returncode, tc.decode(r.stdout), tc.decode(r.stderr)

    def find_gofmt(self):
        """gofmt, of the go tool's own toolchain (GOROOT/bin), else on PATH."""
        code, o, _ = self.gotool("env", "GOROOT")
        if code == 0 and o.strip():
            p = Path(o.strip()) / "bin" / f"gofmt{tc.EXE_SUFFIX}"
            if p.exists():
                return str(p)
        return shutil.which("gofmt")

    def gofmt_clean(self, gofile):
        """None where `gofmt -l` finds nothing to change in gofile (issue
        #20), else what `gofmt -d` would change, its first lines."""
        r = subprocess.run([self.gofmt, "-l", str(gofile)], capture_output=True)
        listed = tc.decode(r.stdout).strip()
        if r.returncode == 0 and not listed:
            return None
        d = subprocess.run([self.gofmt, "-d", str(gofile)], capture_output=True)
        text = tc.decode(d.stdout) + tc.decode(d.stderr) + tc.decode(r.stderr)
        return "\n".join(text.splitlines()[:40])

    # --- reporting -----------------------------------------------------------

    def report(self, lines):
        """One piece of work's lines, printed together."""
        with self.lock:
            for kind, what, detail in lines:
                if kind == "ok":
                    self.oks += 1
                    if self.verbose:
                        print(f"ok {what}")
                elif kind == "skip":
                    self.skips.append(what)
                    if self.verbose:
                        print(f"skip {what}")
                elif kind == "known":
                    self.known.append(what)
                    if self.verbose:
                        print(f"known difference: {what}")
                else:
                    self.failures.append(what)
                    print(f"FAIL {what}")
                    if detail:
                        print("    " + detail.rstrip().replace("\n", "\n    "))
            sys.stdout.flush()

    # --- the steps -----------------------------------------------------------

    def runtime_module(self, mod, module):
        """mod made a module that requires the Go runtime's package as this
        repository has it (runtime/go), where the Go --go-runtime=import
        writes builds: None, or why not."""
        mod.mkdir(parents=True, exist_ok=True)
        if (mod / "go.mod").exists():
            return None
        # The module's go line is the toolchain's own.
        for cmd in (["mod", "init", module],
                    ["mod", "edit", f"-require={RUNTIME_MODULE}@v0.0.0", f"-replace={RUNTIME_MODULE}={RUNTIME}"]):
            r = subprocess.run([self.go] + cmd, cwd=mod, env=go_env(), capture_output=True)
            if r.returncode != 0:
                return tc.decode(r.stdout) + tc.decode(r.stderr)
        return None

    def import_program(self, src, name, flags, out, unsupported=None):
        """src compiled to Go with --go-runtime=import (issue #19), as a
        package of a module that requires the runtime's, which must vet and
        be laid out as gofmt lays it out: the executable, built with Go's
        unsafe.Pointer checks (-d=checkptr), the runtime's package's
        included, or None after saying why."""
        mod = self.work / "import-mod"
        with self.lock:
            why = self.runtime_module(mod, "goosetest/imports")
        if why is not None:
            out.append(("fail", f"the module of --go-runtime=import's Go", why))
            return None
        pkg = mod / f"p-{name}"
        pkg.mkdir(parents=True, exist_ok=True)
        gofile = pkg / "main.go"
        gofile.unlink(missing_ok=True)
        code, o, e = self.goose(*flags, "--target=go", "--go-runtime=import", "-o", gofile, src)
        if code != 0:
            if unsupported and UNSUPPORTED in e:
                out.append(("skip", unsupported, None))
            else:
                out.append(("fail", f"goose --target=go --go-runtime=import {name}", o + e))
            return None
        r = subprocess.run([self.go, "vet", f"./p-{name}"], cwd=mod, env=go_env(), capture_output=True)
        text = tc.decode(r.stdout) + tc.decode(r.stderr)
        if r.returncode != 0 or text.strip():
            out.append(("fail", f"go vet {name} (--go-runtime=import)", text))
            return None
        diff = self.gofmt_clean(gofile)
        if diff is not None:
            out.append(("fail", f"gofmt -l {name} (--go-runtime=import)", diff))
            return None
        exe = mod / f"{name}-import{tc.EXE_SUFFIX}"
        r = subprocess.run([self.go, "build", "-gcflags=all=-d=checkptr", "-o", str(exe), f"./p-{name}"],
                           cwd=mod, env=go_env(), capture_output=True)
        if r.returncode != 0:
            out.append(("fail", f"go build {name} (--go-runtime=import)", tc.decode(r.stdout) + tc.decode(r.stderr)))
            return None
        return exe

    def go_program(self, src, name, flags, out, checkptr=False, unsupported=None):
        """Compiles src to Go, vets and builds it: the executable, or None
        after saying why in `out`. With checkptr, the executable built with
        Go's checks of every unsafe.Pointer conversion and arithmetic, which
        `go test -race` turns on as well (-d=checkptr), is returned too, as
        (exe, checked). A compile the Go backend refuses as not supported
        yet is counted as `unsupported` names it, where it does, and not
        failed."""
        gofile = self.work / f"{name}.go"
        exe = self.work / f"{name}-go{tc.EXE_SUFFIX}"
        gofile.unlink(missing_ok=True)
        code, o, e = self.goose(*flags, "--target=go", "-o", gofile, src)
        if code != 0:
            if unsupported and UNSUPPORTED in e:
                out.append(("skip", unsupported, None))
            else:
                out.append(("fail", f"goose --target=go {name}", o + e))
            return None
        code, o, e = self.gotool("vet", gofile.name)
        if code != 0 or (o + e).strip():
            out.append(("fail", f"go vet {name}", o + e))
            return None
        diff = self.gofmt_clean(gofile)
        if diff is not None:
            out.append(("fail", f"gofmt -l {name}", diff))
            return None
        code, o, e = self.gotool("build", "-o", exe.name, gofile.name)
        if code != 0:
            out.append(("fail", f"go build {name}", o + e))
            return None
        if not checkptr:
            return exe
        checked = self.work / f"{name}-go-checkptr{tc.EXE_SUFFIX}"
        code, o, e = self.gotool("build", "-gcflags=all=-d=checkptr", "-o", checked.name,
                                 gofile.name)
        if code != 0:
            out.append(("fail", f"go build -d=checkptr {name}", o + e))
            return None
        return exe, checked

    def c_run(self, src, name, flags, out, timeout=None, cwd=None, args=(), stdin=None,
              jit_in_cwd=False):
        """The C backend's run of src: (exit code, stdout, stderr), or None
        after saying why. The exit code is None where it ran past timeout.
        The program gets args and reads stdin (a file) where given. A
        compiled program runs in cwd where given; a TinyCC run in the
        compiler's process does from the repository's root, where the
        messages name the source as the Go's do, or in cwd as well with
        jit_in_cwd (a sample reading its data, whose source is then named
        by its whole path). Its stderr names a file of the standard library
        as the Go backend does, by its place in the library
        (go_file_names)."""
        if self.cc is None:
            # TinyCC in the compiler's process: what the compile itself
            # prints to stderr (its warnings) comes before the program's.
            if jit_in_cwd and cwd:
                # The standard library is the repository's, which a run from
                # its root finds there, whatever compiler it is.
                compiler = lambda *a, **kw: run_capture(
                    [self.exe, "--stdlib", UPSTREAM / "stdlib"] +
                    [x.resolve() if isinstance(x, Path) else x for x in a], cwd=cwd, **kw)
            else:
                compiler = self.goose
            run = ["--", *args] if args else []
            code, o, e = compiler(*flags, "--jit", src, *run, timeout=timeout, stdin_path=stdin)
            if code != 0 and "JIT mode does not support threads" in e:
                out.append(("skip", f"{name}: a program with workers, which a JIT run cannot build",
                            None))
                return None
            _, _, warnings = compiler(*flags, "--check", src)
            if warnings and e.startswith(warnings):
                e = e[len(warnings):]
            return code, o, go_file_names(e)
        exe = self.c_build(src, name, flags, out)
        if exe is None:
            return None
        code, o, e = run_capture([exe] + list(args), cwd=cwd, timeout=timeout, stdin_path=stdin)
        return code, o, go_file_names(e)

    def c_build(self, src, name, flags, out):
        """The C backend's executable for src, at the Goose level flags name
        (-O2 by default), or None after saying why."""
        cfile = self.work / f"{name}.c"
        exe = self.work / f"{name}-c{tc.EXE_SUFFIX}"
        code, o, e = self.goose(*flags, "--standalone", "-o", cfile, src)
        if code != 0:
            out.append(("fail", f"goose -o {name}.c", o + e))
            return None
        ok, log = self.cc.compile(cfile, exe, opt=1, log=self.work / f"{name}.cc.log")
        if not ok:
            out.append(("fail", f"cc {name}.c", "\n".join(log.splitlines()[:8])))
            return None
        return exe

    def stacks(self, src, name, flags, out, unsupported=False):
        """The data stack counts, from the analysis both backends share,
        which must agree at both levels; with unsupported, not at a level
        whose Go the backend refuses as not supported yet, which is counted
        instead. The warnings only the Go backend gives (GO_WARNING) are
        not part of what they must agree on."""
        for level in ("2", "0"):
            c = self.goose(*flags, f"-O{level}", "--check", "--stacks", src)
            g = self.goose(*flags, f"-O{level}", "--check", "--stacks", "--target=go", src)
            g = (g[0], g[1], GO_WARNING.sub("", g[2]))
            if unsupported and g[0] != 0 and UNSUPPORTED in g[2]:
                out.append(("skip", f"--stacks {name} -O{level}", None))
            elif c != g or c[0] != 0:
                out.append(("fail", f"--stacks {name} -O{level}",
                            f"--- go:\n{g[1]}{g[2]}\n--- c:\n{c[1]}{c[2]}"))
            else:
                out.append(("ok", f"--stacks {name} -O{level}", None))

    def program(self, src):
        name = src.stem
        flags = goose_flags(src)
        out = []
        # At -O2 and at -O0, where nothing is inlined, each against the C
        # backend's build at the same level: --debug checks the operations as
        # the optimizations leave them, so a tail loop's accumulator can
        # overflow where the recursion as written does not (optimize_tre.h).
        refs = {}
        for level in ("2", "0"):
            label = f"{name} -O{level}"
            lflags = flags + [f"-O{level}"]
            ref = refs[level] = self.c_run(src, f"{name}-O{level}", lflags, out)
            # At -O2, the build with Go's unsafe.Pointer checks runs too.
            exes = self.go_program(src, f"{name}-O{level}", lflags, out, checkptr=level == "2")
            if exes is None or ref is None:
                continue
            if not isinstance(exes, tuple):
                exes = (exes,)
            for exe, kind in zip(exes, ("", " -d=checkptr")):
                got = tc.run_capture([exe])
                same = True
                for what, g, c in (("exit code", got[0], ref[0]), ("stdout", got[1], ref[1]),
                                   ("stderr", got[2], ref[2])):
                    if g != c:
                        same = False
                        out.append(("fail", f"parity {label}{kind}: {what}",
                                    f"--- go:\n{g}\n--- c:\n{c}" if what != "exit code"
                                    else f"go {g}, c {c}"))
                if same:
                    out.append(("ok", f"run {label}{kind}", None))
        # The runtime imported (--go-runtime=import), at -O2, and every name
        # of the runtime's and of the program's unexported (--go-unexported),
        # at -O0, which change none of what the program does (issue #19).
        for level, how in (("2", "--go-runtime=import"), ("0", "--go-unexported")):
            ref = refs[level]
            if ref is None:
                continue
            lflags = flags + [f"-O{level}"]
            if how == "--go-runtime=import":
                exe = self.import_program(src, f"{name}-O{level}", lflags, out)
            else:
                exe = self.go_program(src, f"{name}-O{level}-unexported", lflags + [how], out)
            if exe is None:
                continue
            got = tc.run_capture([exe])
            bad = [what for what, g, c in (("exit code", got[0], ref[0]), ("stdout", got[1], ref[1]),
                                           ("stderr", got[2], ref[2])) if g != c]
            if bad:
                out.append(("fail", f"parity {name} -O{level} {how}: {', '.join(bad)}",
                            f"--- go (exit {got[0]}):\n{got[1]}{got[2]}\n--- c (exit {ref[0]}):\n{ref[1]}{ref[2]}"))
            else:
                out.append(("ok", f"run {name} -O{level} {how}", None))
        self.declared_warnings(src, flags, out)
        # What test/go/expected/ holds is the C run at -O2.
        ref = refs["2"]
        if ref is None:
            return out
        exp = HERE / "go" / "expected"
        files = {"out": ref[1], "stderr": ref[2], "exit": f"{ref[0]}\n" if ref[0] else ""}
        if self.bless:
            exp.mkdir(parents=True, exist_ok=True)
            for ext, text in files.items():
                path = exp / f"{name}.{ext}"
                if text or ext == "out":
                    tc.write_text(path, text)
                else:
                    path.unlink(missing_ok=True)
        else:
            for ext, text in files.items():
                path = exp / f"{name}.{ext}"
                want = tc.decode(path.read_bytes()) if path.exists() else ""
                if not path.exists() and ext == "out":
                    out.append(("fail", f"missing test/go/expected/{name}.out", None))
                elif want != text:
                    out.append(("fail", f"expected {name}.{ext}",
                                f"--- got:\n{text}\n--- want:\n{want}"))
        self.stacks(src, name, flags, out)
        return out

    def declared_warnings(self, src, flags, out):
        """A program that declares the warnings its compile prints, with
        `// warning:` lines as the C suite's fixtures declare theirs (issue
        #41): checked with `goose --check`, for C and with --target=go, it
        must print exactly those, one warning line each, in any order, as
        run_tests.py judges a fixture's. One that declares none is not
        judged here."""
        markers = warning_markers(src)
        if not markers:
            return
        for target in ([], ["--target=go"]):
            label = f"warnings {src.stem}{''.join(' ' + t for t in target)}"
            code, o, e = self.goose(*flags, "--check", *target, src)
            missing, extra = unmatched_warnings(markers, warning_lines(o + e))
            if code != 0:
                out.append(("fail", f"{label} (exit {code})", o + e))
            elif missing or extra:
                out.append(("fail", label,
                            "".join(f"no warning for marker: {m}\n" for m in missing) +
                            "".join(f"no marker for warning: {w}\n" for w in extra) + o + e))
            else:
                out.append(("ok", label, None))

    def build_only(self, src):
        """A program that never ends: its Go must vet and build at both
        levels and its C compile, neither of them run, and the two report
        the same data stacks."""
        name = src.stem
        flags = goose_flags(src)
        out = []
        for level in ("2", "0"):
            label = f"build-{name}-O{level}"
            if self.go_program(src, label, flags + [f"-O{level}"], out) is not None:
                out.append(("ok", f"build {name} -O{level}", None))
        if self.cc is None:
            # TinyCC would run it: the C backend's check is all there is.
            code, o, e = self.goose(*flags, "--check", src)
            if code != 0:
                out.append(("fail", f"build {name}: the C backend's check fails", o + e))
        elif self.c_build(src, f"build-{name}", flags, out) is not None:
            out.append(("ok", f"build {name} (C)", None))
        self.stacks(src, name, flags, out)
        return out

    def fuzz(self, seed):
        """A random program, which both backends must run alike, the Go
        built with its unsafe.Pointer checks as well."""
        out = []
        name = f"fuzz-{seed}"
        src = self.work / f"{name}.goose"
        tc.write_text(src, fuzz_program(seed))
        flags = ["--debug"] if seed % 2 else []
        exes = self.go_program(src, name, flags, out, checkptr=True)
        ref = self.c_run(src, name, flags, out)
        if exes is None or ref is None:
            return out
        for exe, kind in zip(exes, ("", " -d=checkptr")):
            got = tc.run_capture([exe])
            if got != ref:
                out.append(("fail", f"fuzz seed {seed}{' --debug' if flags else ''}{kind} ({src})",
                            f"--- go (exit {got[0]}):\n{got[1][-2000:]}{got[2]}\n"
                            f"--- c (exit {ref[0]}):\n{ref[1][-2000:]}{ref[2]}"))
            else:
                out.append(("ok", f"fuzz seed {seed}{kind}", None))
        return out

    def fuzz_flow(self, seed):
        """A random program of control flow (FlowProgram), which both
        backends must run alike at both levels, the Go built with its
        unsafe.Pointer checks as well at -O2, with the same stacks."""
        out = []
        name = f"flow-{seed}"
        src = self.work / f"{name}.goose"
        tc.write_text(src, flow_program(seed))
        for level in ("2", "0"):
            ref = self.c_run(src, f"{name}-O{level}", [f"-O{level}"], out, timeout=FLOW_TIMEOUT)
            if ref is None:
                continue
            if ref[0] is None:
                out.append(("fail", f"fuzz-flow seed {seed} -O{level}: the C run did not end in "
                            f"{FLOW_TIMEOUT}s ({src})", None))
                continue
            exes = self.go_program(src, f"{name}-O{level}", [f"-O{level}"], out,
                                   checkptr=level == "2")
            if exes is None:
                continue
            if not isinstance(exes, tuple):
                exes = (exes,)
            for exe, kind in zip(exes, ("", " -d=checkptr")):
                got = run_capture([exe], timeout=FLOW_TIMEOUT)
                if got != ref:
                    out.append(("fail", f"fuzz-flow seed {seed} -O{level}{kind} ({src})",
                                f"--- go (exit {got[0]}):\n{got[1][-2000:]}{got[2]}\n"
                                f"--- c (exit {ref[0]}):\n{ref[1][-2000:]}{ref[2]}"))
                else:
                    out.append(("ok", f"fuzz-flow seed {seed} -O{level}{kind}", None))
        self.stacks(src, name, [], out)
        return out

    def fuzz_values(self, seed):
        """A random program of fixed-size values (ValueProgram), which both
        backends must run alike at both levels, the Go built with its
        unsafe.Pointer checks as well, with the same stacks."""
        out = []
        name = f"values-{seed}"
        src = self.work / f"{name}.goose"
        tc.write_text(src, value_program(seed))
        code, o, e = self.goose("--check", src)
        if code != 0:
            out.append(("fail", f"fuzz-values seed {seed}: the program does not check ({src})",
                        o + e))
            return out
        # A random type can come out larger than this version holds in a
        # local (4096 bytes, which C keeps on a data stack): counted, as
        # --sweep counts what is not supported yet.
        code, o, e = self.goose("--target=go", "-o", self.work / f"{name}-probe.go", src)
        if code != 0 and UNSUPPORTED in e:
            out.append(("skip", f"fuzz-values seed {seed}", None))
            return out
        for level in ("2", "0"):
            ref = self.c_run(src, f"{name}-O{level}", [f"-O{level}"], out, timeout=FLOW_TIMEOUT)
            if ref is None:
                continue
            exes = self.go_program(src, f"{name}-O{level}", [f"-O{level}"], out, checkptr=True)
            if exes is None:
                continue
            for exe, kind in zip(exes, ("", " -d=checkptr")):
                got = run_capture([exe], timeout=FLOW_TIMEOUT)
                if got != ref:
                    out.append(("fail", f"fuzz-values seed {seed} -O{level}{kind} ({src})",
                                f"--- go (exit {got[0]}):\n{got[1][-2000:]}{got[2]}\n"
                                f"--- c (exit {ref[0]}):\n{ref[1][-2000:]}{ref[2]}"))
                else:
                    out.append(("ok", f"fuzz-values seed {seed} -O{level}{kind}", None))
        self.stacks(src, name, [], out)
        return out

    def overflow(self, name):
        """A print whose line may run past its stack's region (OVERFLOW_CASES):
        the C runtime's guard gap finds it, and the Go must report it alike,
        with the same text and status, and print what fits as C does, also
        built with its unsafe.Pointer checks."""
        out = []
        src = self.work / f"overflow-{name}.goose"
        tc.write_text(src, overflow_program(name))
        flags = ["--stack-reserve", "64K"]
        ref = self.c_run(src, f"overflow-{name}", flags, out)
        if ref is None:
            return out
        over = not name.endswith("fits")
        if (ref[0] != 0) != over or over != ("data stack overflow" in ref[2]):
            out.append(("fail", f"overflow {name}: the C run is not the case", f"exit {ref[0]}\n{ref[2]}"))
            return out
        exes = self.go_program(src, f"overflow-{name}", flags, out, checkptr=True)
        if exes is None:
            return out
        exe = self.import_program(src, f"overflow-{name}", flags, out)
        exes = exes + ((exe,) if exe else ())
        for exe, kind in zip(exes, ("", " -d=checkptr", " --go-runtime=import")):
            got = tc.run_capture([exe])
            if got != ref:
                out.append(("fail", f"overflow {name}{kind}", f"--- go (exit {got[0]}):\n{got[2]}\n"
                            f"--- c (exit {ref[0]}):\n{ref[2]}"))
            else:
                out.append(("ok", f"overflow {name}{kind}", None))
        return out

    def warnings(self, src):
        """A program the Go backend warns of where the C backend does not yet
        (test/go/warnings/, issue #27): compiled to Go at -O2 and at -O0,
        where nothing is inlined, and checked with --check --target=go, each
        compile must succeed and print exactly the warnings its `// warning:`
        lines declare, one warning line each, in any order, as run_tests.py
        judges a fixture's; the C backend must check it."""
        out = []
        markers = warning_markers(src)
        if not markers:
            out.append(("fail", f"warnings {src.name}: no // warning: marker", None))
            return out
        for what in ("-O2", "-O0", "--check"):
            label = f"warnings {src.name} {what}"
            if what == "--check":
                args = ["--check", "--target=go"]
            else:
                gofile = self.work / f"warnings-{src.stem}{what}.go"
                gofile.unlink(missing_ok=True)
                args = [what, "--target=go", "-o", gofile]
            code, o, e = self.goose(*args, src)
            missing, extra = unmatched_warnings(markers, warning_lines(o + e))
            if code != 0 or (what != "--check" and not gofile.exists()):
                out.append(("fail", f"{label} (exit {code})", o + e))
            elif missing or extra:
                out.append(("fail", label,
                            "".join(f"no warning for marker: {m}\n" for m in missing) +
                            "".join(f"no marker for warning: {w}\n" for w in extra) + o + e))
            else:
                out.append(("ok", label, None))
        code, o, e = self.goose("--check", src)
        if code != 0:
            out.append(("fail", f"warnings {src.name}: the C backend's check fails", o + e))
        return out

    def unsupported(self, src):
        """A program the Go backend refuses: with the messages its markers name,
        where the C backend compiles it."""
        out = []
        markers = error_markers(src)
        gofile = self.work / f"unsupported-{src.stem}.go"
        code, o, e = self.goose("--target=go", "-o", gofile, src)
        if not markers:
            out.append(("fail", f"unsupported {src.name}: no // error: marker", None))
        elif code != 1 or UNSUPPORTED not in e or any(m not in e for m in markers):
            out.append(("fail", f"unsupported {src.name} (exit {code})",
                        f"markers: {markers}\n{o}{e}"))
        else:
            out.append(("ok", f"unsupported {src.name}", None))
        code, o, e = self.goose("--check", src)
        if code != 0:
            out.append(("fail", f"unsupported {src.name}: the C backend's check fails", o + e))
        return out

    def fixture(self, src, sweep=False):
        """A fixture of the C suite's test/stdlib/ (issue #5) or test/threads/
        (issue #12), or with sweep any other category's (--sweep): compiled
        by both backends at -O2 and at -O0, and with --debug at -O2 where
        its first line asks run_tests.py for a GS_DEBUG run
        (`runtime-debug`), each with the -D its first line names
        (`runtime-define`), each run in a directory of its own, since the os
        fixtures write files where they run. The Go must vet, build (at -O2
        with -d=checkptr as well) and run as the C does, its -O2 run print
        what test/expected/ holds, as run_tests.py judges a C run, and the
        two report the same data stacks. One that differs by design
        (SWEEP_KNOWN) is counted, not run, and so is one of the sweep's the
        Go backend refuses as not supported yet."""
        out = []
        section = src.parent.name
        what = f"{'sweep ' if sweep else ''}{section}/{src.name}"
        known = SWEEP_KNOWN.get(f"{section}/{src.name}")
        if known:
            out.append(("known", f"{what} ({known})", None))
            return out
        name = f"{'sweep-' if sweep else ''}{section}-{src.stem}"
        flags = runtime_defines(src)
        if sweep:
            code, o, e = self.goose(*flags, "--target=go", "-o", self.work / f"{name}-probe.go", src)
            if code != 0:
                if UNSUPPORTED in e:
                    out.append(("skip", what, None))
                else:
                    out.append(("fail", f"{what}: goose --target=go", o + e))
                return out
        builds = [["-O2"], ["-O0"]]
        if "runtime-debug" in first_line(src):
            builds.append(["--debug", "-O2"])
        gotrun = None
        for b in builds:
            tag = "-".join(x.strip("-") for x in b)
            label = f"{what} {' '.join(b)}"
            rundir = self.work / f"{name}-{tag}-run"
            rundir.mkdir(parents=True, exist_ok=True)
            # A level of the sweep's the Go backend does not compile yet (a
            # value -O0 passes where -O2 inlines it away) is counted.
            exes = self.go_program(src, f"{name}-{tag}", flags + b, out, checkptr=b == ["-O2"],
                                   unsupported=label if sweep else None)
            if exes is None:
                continue
            ref = self.c_run(src, f"{name}-{tag}", flags + b, out, cwd=rundir)
            if ref is None:
                continue
            if not isinstance(exes, tuple):
                exes = (exes,)
            for exe, kind in zip(exes, ("", " -d=checkptr")):
                got = run_capture([exe], cwd=rundir)
                if b == ["-O2"] and not kind:
                    gotrun = got
                same = True
                for part, g, c in (("exit code", got[0], ref[0]), ("stdout", got[1], ref[1]),
                                   ("stderr", got[2], ref[2])):
                    if g != c:
                        same = False
                        out.append(("fail", f"parity {label}{kind}: {part}",
                                    f"--- go:\n{g}\n--- c:\n{c}" if part != "exit code"
                                    else f"go {g}, c {c}"))
                if same:
                    out.append(("ok", f"run {label}{kind}", None))
            # The runtime imported (--go-runtime=import), at -O2 (issue #19).
            if b == ["-O2"]:
                exe = self.import_program(src, f"{name}-{tag}", flags + b, out,
                                          unsupported=f"{label} --go-runtime=import" if sweep else None)
                if exe is not None:
                    got = run_capture([exe], cwd=rundir)
                    bad = [part for part, g, c in (("exit code", got[0], ref[0]), ("stdout", got[1], ref[1]),
                                                   ("stderr", got[2], ref[2])) if g != c]
                    if bad:
                        out.append(("fail", f"parity {label} --go-runtime=import: {', '.join(bad)}",
                                    f"--- go (exit {got[0]}):\n{got[1]}{got[2]}\n--- c (exit {ref[0]}):\n{ref[1]}{ref[2]}"))
                    else:
                        out.append(("ok", f"run {label} --go-runtime=import", None))
        if gotrun is not None:
            code, o, e = gotrun
            exp = UPSTREAM_TEST / "expected"
            bad = []
            if (exp / f"{src.stem}.aborts").exists() != (code != 0):
                bad.append(f"exit {code}")
            stderr_file = exp / f"{src.stem}.stderr"
            if stderr_file.exists():
                markers = [m.strip() for m in tc.decode(stderr_file.read_bytes()).splitlines()
                           if m.strip()]
                if any(m not in e for m in markers):
                    bad.append("stderr")
            outfile = exp / f"{src.stem}.out"
            if not outfile.exists():
                bad.append("no test/expected .out")
            elif o.rstrip("\n") != tc.decode(outfile.read_bytes()).rstrip("\n"):
                bad.append("stdout")
            if bad:
                out.append(("fail", f"expected {what} (Go -O2): {', '.join(bad)}",
                            f"--- stdout:\n{o}\n--- stderr:\n{e}"))
            else:
                out.append(("ok", f"expected {what}", None))
        self.stacks(src, name, flags, out, unsupported=sweep)
        return out

    def sample(self, src):
        """A sample of samples/ (parity_samples()): compiled by both backends
        at -O2 and at -O0, the C with the header beside it (<name>.h) and
        the Go with its Go port (<name>.go) spliced in by --include, and run
        as run_samples.py runs one, with the words of data/<name>.args as
        its arguments and data/<name>.stdin as its stdin, but each run in a
        directory of its own whose data/ is the samples' (a sample writes
        files where it runs); the Go built with -d=checkptr at -O2 as well.
        The runs must agree on stdout and the exit code, and on stderr once
        each number in it is masked: a sample's stderr is its timings, which
        no two runs share. The Go's stdout at -O2 must be what
        samples/expected/ holds, and the two report the same data stacks."""
        out = []
        name = f"sample-{src.stem}"
        bare = re.sub(r"^\d+_", "", src.stem)
        data = src.parent / "data"
        argfile, infile = data / f"{bare}.args", data / f"{bare}.stdin"
        args = argfile.read_text(encoding="utf-8").split() if argfile.exists() else []
        stdin = infile if infile.exists() else None
        cflags, goflags = [], []
        header = src.parent / f"{bare}.h"
        port = src.parent / f"{bare}.go"
        if not port.exists():
            port = HERE / "go" / "samples" / f"{bare}.go"
        if header.exists():
            if not port.exists():
                out.append(("fail", f"samples/{src.name}: {header.name} has no Go port, {port.name}",
                            None))
                return out
            cflags, goflags = ["--include", header], ["--include", port]
        for level in ("2", "0"):
            label = f"samples/{src.name} -O{level}"
            rundir = self.work / f"{name}-O{level}-run"
            shutil.rmtree(rundir, ignore_errors=True)
            rundir.mkdir(parents=True)
            (rundir / "data").symlink_to(data, target_is_directory=True)
            ref = self.c_run(src, f"{name}-O{level}", cflags + [f"-O{level}"], out, cwd=rundir,
                             timeout=SAMPLE_TIMEOUT, args=args, stdin=stdin, jit_in_cwd=True)
            exes = self.go_program(src, f"{name}-O{level}", goflags + [f"-O{level}"], out,
                                   checkptr=level == "2")
            if exes is None or ref is None:
                continue
            if ref[0] is None:
                out.append(("fail", f"{label}: the C run did not end in {SAMPLE_TIMEOUT}s", None))
                continue
            if not isinstance(exes, tuple):
                exes = (exes,)
            for exe, kind in zip(exes, ("", " -d=checkptr")):
                got = run_capture([exe] + args, cwd=rundir, timeout=SAMPLE_TIMEOUT, stdin_path=stdin)
                if got[0] is None:
                    out.append(("fail", f"{label}{kind}: the Go run did not end in {SAMPLE_TIMEOUT}s",
                                None))
                    continue
                bad = [what for what, g, c in (("exit code", got[0], ref[0]), ("stdout", got[1], ref[1]),
                                               ("stderr", masked(got[2]), masked(ref[2])))
                       if g != c]
                if bad:
                    out.append(("fail", f"parity {label}{kind}: {', '.join(bad)}",
                                f"--- go (exit {got[0]}):\n{got[1][-2000:]}{got[2]}\n"
                                f"--- c (exit {ref[0]}):\n{ref[1][-2000:]}{ref[2]}"))
                    continue
                out.append(("ok", f"run {label}{kind}", None))
                if level == "2" and not kind:
                    # The runtime imported (--go-runtime=import) runs it alike
                    # (issue #19).
                    exe = self.import_program(src, f"{name}-O{level}", goflags + [f"-O{level}"], out)
                    if exe is not None:
                        ig = run_capture([exe] + args, cwd=rundir, timeout=SAMPLE_TIMEOUT, stdin_path=stdin)
                        ibad = [what for what, g, c in (("exit code", ig[0], ref[0]), ("stdout", ig[1], ref[1]),
                                                        ("stderr", masked(ig[2]), masked(ref[2]))) if g != c]
                        if ibad:
                            out.append(("fail", f"parity {label} --go-runtime=import: {', '.join(ibad)}",
                                        f"--- go (exit {ig[0]}):\n{ig[1][-2000:]}{ig[2]}"))
                        else:
                            out.append(("ok", f"run {label} --go-runtime=import", None))
                    exp = src.parent / "expected" / f"{bare}.out"
                    want = tc.decode(exp.read_bytes()) if exp.exists() else None
                    if want is None or got[1].rstrip("\n") != want.rstrip("\n"):
                        out.append(("fail", f"expected samples/{src.name} (Go -O2)",
                                    f"--- got:\n{got[1]}\n--- want:\n{want}"))
                    else:
                        out.append(("ok", f"expected samples/{src.name}", None))
        self.stacks(src, name, [], out)
        return out

    def module_tests(self, mod, label, out, tags=None, race=False):
        """The tests of the module in mod: `go vet`, which must find nothing,
        `go test`, and `go test` built with -d=checkptr, Go's checks of every
        unsafe.Pointer conversion and of the arithmetic on one (what `go test
        -race` turns on); with race, `go test -race` too, where cgo can
        build it: whether all passed, each reported."""
        tag = [f"-tags={tags}"] if tags else []
        steps = [("go vet", ["vet"] + tag + ["./..."], go_env()),
                 ("go test", ["test", "-count=1"] + tag + ["./..."], go_env()),
                 ("go test -d=checkptr", ["test", "-count=1", "-gcflags=all=-d=checkptr"] + tag + ["./..."], go_env())]
        if race:
            steps.append(("go test -race", ["test", "-count=1", "-race"] + tag + ["./..."],
                          dict(go_env(), CGO_ENABLED="1")))
        for what, cmd, env in steps:
            r = subprocess.run([self.go] + cmd, cwd=mod, env=env, capture_output=True)
            text = tc.decode(r.stdout) + tc.decode(r.stderr)
            if what == "go test -race" and r.returncode != 0 and ("cgo" in text or "C compiler" in text or
                                                                  "-race is not supported" in text):
                # The race detector needs cgo, and a C compiler.
                out.append(("skip", f"{label}: {what} (no cgo here)", None))
                continue
            if r.returncode != 0 or (what == "go vet" and text.strip()):
                out.append(("fail", f"{label}: {what}", text))
                return False
            out.append(("ok", f"{label}: {what}", None))
        return True

    def export(self, d):
        """A library of export fns (test/go/export/<name>/, issue #5):
        <name>.goose compiled with --target=go --library at -O2 and at -O0,
        each other non-test .go file there spliced in with --include, into
        a module of its own with the directory's _test.go files, which must
        vet and pass `go test`, built with -d=checkptr as well, and for one
        of EXPORT_RACE under the race detector where cgo builds: their
        checks of what the Go entry points give, and testing.AllocsPerRun's
        that calling them in a loop allocates nothing on Go's heap. Then
        again with the runtime imported (--go-runtime=import, issue #19),
        the module requiring the runtime's package, runtime/go, with
        test/go/export/import_shim_test.go, which gives the tests the
        runtime's names they use, under -tags goose_import."""
        out = []
        name = d.name
        src = d / f"{name}.goose"
        includes = sorted(p for p in d.glob("*.go") if not p.name.endswith("_test.go"))
        tests = sorted(d.glob("*_test.go"))
        if not src.exists() or not tests:
            out.append(("fail", f"export {name}: no {name}.goose or no _test.go", None))
            return out
        for level, imp in (("2", False), ("0", False), ("2", True), ("0", True)):
            label = f"export {name} -O{level}" + (" --go-runtime=import" if imp else "")
            mod = self.work / f"export-{name}-O{level}{'-import' if imp else ''}"
            shutil.rmtree(mod, ignore_errors=True)
            mod.mkdir(parents=True)
            args = [f"-O{level}", "--target=go", "--library", "-o", mod / f"{name}.go"]
            if imp:
                args.append("--go-runtime=import")
            for inc in includes:
                args += ["--include", inc]
            code, o, e = self.goose(*args, src)
            if code != 0:
                out.append(("fail", f"{label}: goose --target=go", o + e))
                continue
            diff = self.gofmt_clean(mod / f"{name}.go")
            if diff is not None:
                out.append(("fail", f"{label}: gofmt -l", diff))
                continue
            out.append(("ok", f"{label}: gofmt -l", None))
            for t in tests + ([HERE / "go" / "export" / "import_shim_test.go"] if imp else []):
                shutil.copy(t, mod / t.name)
            if imp:
                why = self.runtime_module(mod, f"goosetest/{name}")
                if why is not None:
                    out.append(("fail", f"{label}: the module", why))
                    continue
            else:
                r = subprocess.run([self.go, "mod", "init", f"goosetest/{name}"], cwd=mod, env=go_env(),
                                   capture_output=True)
                if r.returncode != 0:
                    out.append(("fail", f"{label}: go mod init", tc.decode(r.stderr)))
                    continue
            self.module_tests(mod, label, out, "goose_import" if imp else None,
                              race=name in EXPORT_RACE and not imp)
        return out

    # --- the runtime (issue #19) ----------------------------------------------

    def runtime_package(self):
        """The runtime's package, runtime/go, which is the compiler's runtime:
        the files the compiler embeds are those of runtime/go as they are
        (`goose --emit-go-runtime` writes what it embedded, which must be
        them, every one but those the package keeps to itself), and the
        package's own tests pass, vetted, under the race detector where cgo
        can build it, and with -d=checkptr."""
        out = []
        emitted = self.work / "emit-go-runtime"
        shutil.rmtree(emitted, ignore_errors=True)
        code, o, e = self.goose("--emit-go-runtime", emitted)
        if code != 0:
            out.append(("fail", "goose --emit-go-runtime", o + e))
        else:
            files = sorted(p.name for p in RUNTIME.glob("*.go") if not p.name.endswith("_test.go"))
            want = [f for f in files if f not in RUNTIME_PACKAGE_ONLY]
            got = sorted(p.name for p in emitted.glob("*.go"))
            if got != want:
                out.append(("fail", "the compiler embeds runtime/go's files",
                            f"embedded {got}, runtime/go's {want} (cmake's GOOSE_GO_RUNTIME_FILES, "
                            "and RUNTIME_PACKAGE_ONLY here)"))
            else:
                stale = [f for f in want if (emitted / f).read_bytes() != (RUNTIME / f).read_bytes()]
                if stale:
                    out.append(("fail", "the compiler's runtime is runtime/go's",
                                f"{', '.join(stale)} changed since the compiler was built: rebuild it"))
                else:
                    out.append(("ok", "the compiler's runtime is runtime/go's", None))
        for what, cmd, env in (("go vet", ["vet", "./..."], go_env()),
                               ("go test", ["test", "-count=1", "./..."], go_env()),
                               ("go test -d=checkptr", ["test", "-count=1", "-gcflags=all=-d=checkptr", "./..."],
                                go_env()),
                               ("go test -race", ["test", "-count=1", "-race", "./..."],
                                dict(go_env(), CGO_ENABLED="1"))):
            r = subprocess.run([self.go] + cmd, cwd=RUNTIME, env=env, capture_output=True)
            text = tc.decode(r.stdout) + tc.decode(r.stderr)
            if what == "go test -race" and r.returncode != 0 and ("cgo" in text or "C compiler" in text or
                                                                  "-race is not supported" in text):
                # The race detector needs cgo, and a C compiler.
                out.append(("skip", f"runtime/go: {what} (no cgo here)", None))
            elif r.returncode != 0 or (what == "go vet" and text.strip()):
                out.append(("fail", f"runtime/go: {what}", text))
            else:
                out.append(("ok", f"runtime/go: {what}", None))
        return out

    def kernels(self):
        """A library of kernels (test/go/kernels/, issue #19), pure functions
        over slices: the file the compiler writes holds none of the
        runtime's data stacks, pools or text forms, nor, its entry points
        handing their arguments on as they are (issue #22), GsEscape or
        runtime.KeepAlive (KERNEL_ABSENT), and is short (KERNEL_MAX_LINES);
        with --go-no-withstack, not even the block type a NameWithStack
        entry point names. Its tests pass, vetted, and with -d=checkptr, at
        -O2 and at -O0."""
        out = []
        d = HERE / "go" / "kernels"
        for level, extra in (("2", []), ("0", []), ("2", ["--go-no-withstack"])):
            label = f"kernels -O{level}" + "".join(f" {x}" for x in extra)
            mod = self.work / f"kernels-O{level}{''.join(extra)}"
            shutil.rmtree(mod, ignore_errors=True)
            mod.mkdir(parents=True)
            gofile = mod / "kernels.go"
            code, o, e = self.goose(f"-O{level}", "--target=go", "--go-package", "kernels", *extra, "-o", gofile,
                                    d / "kernels.goose")
            if code != 0:
                out.append(("fail", f"{label}: goose --target=go", o + e))
                continue
            text = gofile.read_text(encoding="utf-8")
            absent = KERNEL_ABSENT + (["GsStackBlock", "WithStack"] if extra else [])
            found = [a for a in absent if a in text]
            lines = text.count("\n")
            if found or lines > KERNEL_MAX_LINES:
                out.append(("fail", f"{label}: the runtime it holds",
                            f"{lines} lines (at most {KERNEL_MAX_LINES}); holds {found}"))
            else:
                out.append(("ok", f"{label}: {lines} lines, no data stacks, pools or text forms", None))
            diff = self.gofmt_clean(gofile)
            if diff is not None:
                out.append(("fail", f"{label}: gofmt -l", diff))
                continue
            if extra:
                continue
            shutil.copy(d / "kernels_test.go", mod / "kernels_test.go")
            r = subprocess.run([self.go, "mod", "init", "goosetest/kernels"], cwd=mod, env=go_env(),
                               capture_output=True)
            if r.returncode != 0:
                out.append(("fail", f"{label}: go mod init", tc.decode(r.stderr)))
                continue
            self.module_tests(mod, label, out)
        return out

    def instances(self):
        """A library whose export fns' calls grow globals on data stacks of
        their own, and that spawns no workers (test/go/export/arena/, issue
        #25), whose calls each run on a program instance of their block's
        own: compiled at -O2 and at -O0, its file holds no declaration of the
        workers' and queues' runtime (THREADS_RUNTIME), nor the comment of
        its section, and its tests, which call the entry points from many
        goroutines at once, each on a block of its own and on the pool's,
        pass under the race detector, where cgo builds (Runner.export runs
        them as every export library's otherwise)."""
        out = []
        d = HERE / "go" / "export" / "arena"
        threads = go_declared(THREADS_RUNTIME.read_text(encoding="utf-8"))
        for level in ("2", "0"):
            label = f"arena -O{level}"
            mod = self.work / f"arena-race-O{level}"
            shutil.rmtree(mod, ignore_errors=True)
            mod.mkdir(parents=True)
            gofile = mod / "arena.go"
            code, o, e = self.goose(f"-O{level}", "--target=go", "--library", "-o", gofile, d / "arena.goose")
            if code != 0:
                out.append(("fail", f"{label}: goose --target=go", o + e))
                continue
            text = gofile.read_text(encoding="utf-8")
            found = sorted(go_declared(text) & threads) + ([THREADS_SECTION] if THREADS_SECTION in text else [])
            if found or "type gsGlobals struct" not in text:
                out.append(("fail", f"{label}: instances without the workers' runtime",
                            f"holds {found} of {THREADS_RUNTIME.name}" if found else "no program instances"))
            else:
                out.append(("ok", f"{label}: instances without the workers' runtime", None))
            for t in d.glob("*_test.go"):
                shutil.copy(t, mod / t.name)
            r = subprocess.run([self.go, "mod", "init", "goosetest/arena"], cwd=mod, env=go_env(),
                               capture_output=True)
            if r.returncode != 0:
                out.append(("fail", f"{label}: go mod init", tc.decode(r.stderr)))
                continue
            r = subprocess.run([self.go, "test", "-count=1", "-race", "./..."], cwd=mod,
                               env=dict(go_env(), CGO_ENABLED="1"), capture_output=True)
            text = tc.decode(r.stdout) + tc.decode(r.stderr)
            if r.returncode != 0 and ("cgo" in text or "C compiler" in text or "-race is not supported" in text):
                # The race detector needs cgo, and a C compiler.
                out.append(("skip", f"{label}: go test -race (no cgo here)", None))
            elif r.returncode != 0:
                out.append(("fail", f"{label}: go test -race", text))
            else:
                out.append(("ok", f"{label}: go test -race", None))
        return out

    def unexported_library(self):
        """A library compiled with --go-unexported (test/go/unexported/,
        issue #19): `go doc` lists its export fns' entry points, and for the
        NameWithStack ones StackBlock, AcquireStackBlock and
        ReleaseStackBlock, and nothing else; with --go-no-withstack too, the
        Name entry points alone, and the NameString and NameBytes of those
        taking a const u8[:] (issue #22). Its tests pass, vetted, and with
        -d=checkptr."""
        out = []
        d = HERE / "go" / "unexported"
        entries = {"Words", "Kept", "CallsSoFar", "Checked"}
        forms = {"WordsString", "WordsBytes", "KeptString", "KeptBytes"}
        for extra in ([], ["--go-no-withstack"]):
            label = "--go-unexported" + "".join(f" {x}" for x in extra)
            mod = self.work / f"unexported{''.join(extra)}"
            shutil.rmtree(mod, ignore_errors=True)
            mod.mkdir(parents=True)
            gofile = mod / "unexported.go"
            code, o, e = self.goose("--target=go", "--go-package", "unexported", "--go-unexported", *extra,
                                    "-o", gofile, d / "unexported.goose")
            if code != 0:
                out.append(("fail", f"{label}: goose --target=go", o + e))
                continue
            diff = self.gofmt_clean(gofile)
            if diff is not None:
                out.append(("fail", f"{label}: gofmt -l", diff))
                continue
            for t in d.glob("*_test.go"):
                shutil.copy(t, mod / t.name)
            r = subprocess.run([self.go, "mod", "init", "goosetest/unexported"], cwd=mod, env=go_env(),
                               capture_output=True)
            if r.returncode != 0:
                out.append(("fail", f"{label}: go mod init", tc.decode(r.stderr)))
                continue
            r = subprocess.run([self.go, "doc", "."], cwd=mod, env=go_env(), capture_output=True)
            doc = tc.decode(r.stdout) + tc.decode(r.stderr)
            listed = set()
            for line in doc.splitlines():
                m = re.match(r"\s*(?:func|type|var|const)\s+(?:\([^)]*\)\s*)?(\w+)", line)
                if m:
                    listed.add(m.group(1))
            want = entries | forms if extra else entries | forms | {f"{n}WithStack" for n in entries} | \
                {"StackBlock", "AcquireStackBlock", "ReleaseStackBlock"}
            if r.returncode != 0 or listed != want:
                out.append(("fail", f"{label}: go doc lists the entry points alone",
                            f"listed {sorted(listed)}, want {sorted(want)}\n{doc}"))
            else:
                out.append(("ok", f"{label}: go doc lists the entry points alone", None))
            self.module_tests(mod, label, out, "goose_nowithstack" if extra else None)
        return out

    def shared(self):
        """Two programs compiled into one Go package (test/go/shared/,
        issue #19), geom.goose and text.goose, with the runtime they share in
        a file of the package's (--go-runtime-file goose_runtime.go): their
        own names, many the same (Pt, norm, calls), carry their files' names,
        and their tests pass together, vetted and with -d=checkptr -- with
        the runtime the part they use of it, all of it, every name
        unexported, or imported. The runtime file is the same whichever
        program is compiled first, a program compiled again leaves it as it
        is, and a program compiled with other settings than those it records
        is refused."""
        out = []
        d = HERE / "go" / "shared"
        files = {}
        for mode, order in ((["--go-runtime=embed"], ("geom", "text")), (["--go-runtime=embed"], ("text", "geom")),
                            (["--go-unexported"], ("geom", "text")), (["--go-runtime=full"], ("geom", "text")),
                            (["--go-runtime=import"], ("geom", "text"))):
            label = f"shared {' '.join(mode)} ({', '.join(order)})"
            mod = self.work / f"shared-{mode[0].strip('-').replace('=', '-')}-{order[0]}"
            shutil.rmtree(mod, ignore_errors=True)
            mod.mkdir(parents=True)
            rtfile = mod / "goose_runtime.go"
            ok = True
            # Each program, then the first again, which must leave the
            # runtime file as the second left it: with what both use.
            texts = []
            for prog in order + (order[0],):
                code, o, e = self.goose("--target=go", "--library", "--go-package", "shared", *mode,
                                        "--go-runtime-file", rtfile, "-o", mod / f"{prog}.go", d / f"{prog}.goose")
                if code != 0:
                    out.append(("fail", f"{label}: goose --target=go {prog}", o + e))
                    ok = False
                    break
                texts.append(rtfile.read_text(encoding="utf-8"))
            if not ok:
                continue
            if texts[2] != texts[1]:
                out.append(("fail", f"{label}: a program compiled again changes the runtime file", None))
            for f in [rtfile] + [mod / f"{p}.go" for p in order]:
                diff = self.gofmt_clean(f)
                if diff is not None:
                    out.append(("fail", f"{label}: gofmt -l {f.name}", diff))
                    ok = False
            if not ok:
                continue
            for t in d.glob("*_test.go"):
                shutil.copy(t, mod / t.name)
            imp = mode == ["--go-runtime=import"]
            if imp:
                why = self.runtime_module(mod, "goosetest/shared")
            else:
                r = subprocess.run([self.go, "mod", "init", "goosetest/shared"], cwd=mod, env=go_env(),
                                   capture_output=True)
                why = None if r.returncode == 0 else tc.decode(r.stderr)
            if why is not None:
                out.append(("fail", f"{label}: the module", why))
                continue
            self.module_tests(mod, label, out, "goose_import" if imp else None)
            if mode == ["--go-runtime=embed"]:
                files[("order", order[0])] = texts[-1]
        if ("order", "geom") in files and ("order", "text") in files:
            same = files[("order", "geom")] == files[("order", "text")]
            out.append(("ok" if same else "fail", "shared: the runtime file is the same whichever program is first",
                        None if same else "geom then text, and text then geom, wrote other files"))
        # --go-build constrains a program's own file, never the runtime's,
        # which is built with whichever programs are: geom.go only with
        # -tags goose_geom, text.go always, and the package vets either way.
        mod = self.work / "shared-go-build"
        shutil.rmtree(mod, ignore_errors=True)
        mod.mkdir(parents=True)
        ok, detail = True, ""
        for prog, extra in (("geom", ["--go-build", "goose_geom"]), ("text", [])):
            code, o, e = self.goose("--target=go", "--library", "--go-package", "shared", *extra, "--go-runtime-file",
                                    mod / "goose_runtime.go", "-o", mod / f"{prog}.go", d / f"{prog}.goose")
            ok, detail = ok and code == 0, detail + o + e
        if ok:
            rt = (mod / "goose_runtime.go").read_text(encoding="utf-8")
            geom = (mod / "geom.go").read_text(encoding="utf-8")
            ok = "//go:build" not in rt and geom.startswith("//go:build goose_geom\n\n")
            detail = "the runtime file has a constraint, or geom.go has none"
            r = subprocess.run([self.go, "mod", "init", "goosetest/sharedbuild"], cwd=mod, env=go_env(),
                               capture_output=True)
            for tags in ([], ["-tags=goose_geom"]):
                if not ok:
                    break
                r = subprocess.run([self.go, "vet"] + tags + ["./..."], cwd=mod, env=go_env(), capture_output=True)
                text = tc.decode(r.stdout) + tc.decode(r.stderr)
                ok, detail = r.returncode == 0 and not text.strip(), f"go vet {' '.join(tags)}: {text}"
        out.append(("ok" if ok else "fail", "shared: --go-build constrains a program's file, not the runtime's",
                    None if ok else detail))
        # Settings of its own refused, and a package of its own, before any
        # file is written.
        mod = self.work / "shared-go-runtime-embed-geom"
        for k, (flags, why) in enumerate(((["--stack-reserve", "1G"], "holds the runtime of programs compiled with"),
                                          (["--go-unexported"], "holds the runtime of programs compiled with"),
                                          (["--go-package", "other"], "is the runtime of package shared"))):
            pkg = [] if "--go-package" in flags else ["--go-package", "shared"]
            third = mod / f"third{k}.go"
            before = (mod / "goose_runtime.go").read_bytes() if (mod / "goose_runtime.go").exists() else None
            code, o, e = self.goose("--target=go", "--library", *pkg, *flags, "--go-runtime-file",
                                    mod / "goose_runtime.go", "-o", third, d / "geom.goose")
            ok = code == 1 and why in e and not third.exists() and before is not None and \
                (mod / "goose_runtime.go").read_bytes() == before
            out.append(("ok" if ok else "fail", f"shared: --go-runtime-file refuses {' '.join(flags)}",
                        None if ok else f"exit {code}\n{o}{e}"))
        return out

    # --- reproducible files (issue #21) -----------------------------------------

    def reproducible(self):
        """What makes a generated file reproducible (issue #21). Its header
        keeps the line Go's tools know generated code by as it was
        (GO_GENERATED), and records the compiler's version, `goose
        version`'s, the revision of the runtime it embeds
        (go_runtime_revision) and the options its Go depends on, as one
        command line spells them whichever spelling the compile had, so
        that two compiles that say the same write the same file; a runtime
        file's header records the version. `--check` with `-o out.go` (and
        `goose check -o out.go`) writes nothing: it exits with 0 where out.go,
        and the --go-runtime-file, are what the compile would write, and
        with 1 where one is not, or is missing, with a short diff of each.
        And the same programs compiled in two temporary directories, each
        with a standard library of its own, are the same Go, byte for byte
        (Runner.same_go)."""
        out = []

        def check(what, ok, detail=""):
            out.append(("ok" if ok else "fail", f"reproducible: {what}", None if ok else detail))

        work = self.work / "reproducible"
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        hello = HERE / "go" / "programs" / "print.goose"
        code, o, e = self.goose("version")
        m = re.match(r"goose (\S+) \(runtime [0-9a-f]{12}\)\n$", o)
        check("goose version", code == 0 and m is not None, o + e)
        stamp = f"// goose-version: {m[1] if m else '?'} (runtime/go {go_runtime_revision()})\n"

        def compile_go(flags, src=hello, name="hello.go"):
            """The Go the compiler writes of src with flags, or ""."""
            gofile = work / name
            gofile.unlink(missing_ok=True)
            code, o, e = self.goose(*flags, "-o", gofile, src)
            if code != 0:
                check(f"goose {' '.join(map(str, flags))} -o {name}", False, f"exit {code}\n{o}{e}")
                return ""
            return gofile.read_text(encoding="utf-8")

        def header(text):
            """A generated file's comments before its package clause."""
            return text[:text.find("\npackage ")]

        # The header: Go's line as it was, then the version, the runtime's
        # revision and the options.
        text = compile_go([])
        first = text.split("\n", 1)[0]
        check("the header keeps Go's line, and records the version, the runtime's revision and the options",
              first == "// Code generated by the Goose compiler from test/go/programs/print.goose. DO NOT EDIT." and
              GO_GENERATED.match(first) is not None and
              header(text).startswith(f"{first}\n//\n{stamp}// goose-flags: --target=go -O2\n//\n// Names "),
              header(text))
        # The options the Go depends on, each spelled one way, in one order,
        # and only where it is not the default: whatever spelling the
        # compile had (-o out.go for --target=go, a -D for --debug or
        # --stack-reserve, a default given), and with -D the Go backend does
        # not read left out.
        helper = work / "helper.go"
        tc.write_text(helper, 'package main\n\nimport "strings"\n\n'
                              'func shout(gs *GsStackBlock, s GsSlice, out GsRref) {\n'
                              '\tGsBldAppendString(gs, out, strings.ToUpper(GsSliceToString(s))+"!")\n}\n')
        shout = work / "shout.goose"
        tc.write_text(shout, 'extern fn shout(s: const u8[:], out: u8[>..]&);\n\n'
                             'fn main() {\n    var b: u8[>..] = [];\n    shout("hi", b);\n    print(b);\n}\n')
        for flags, want, src in (
                ([], "--target=go -O2", hello),
                (["--target", "go", "-O2", "--go-package", "main", "--go-runtime=embed", "--stack-reserve", "2G",
                  "-DGS_STACK_GAP=0x100000", "-DGS_UNREAD=1"], "--target=go -O2", hello),
                (["--go-package", "geese"], "--target=go -O2 --go-package geese", hello),
                (["-O0", "--debug", "--no-bce", "--go-package=geese"], "--target=go -O0 --debug --no-bce --go-package geese",
                 hello),
                (["--library"], "--target=go -O2 --library", hello),
                (["--library", "--go-package", "goose", "--frame-reserve", "64M"], "--target=go -O2 --library", hello),
                (["--frame-reserve", "64M"], "--target=go -O2 --frame-reserve 64M", hello),
                (["--go-build", "linux&&amd64 ||!goose_x"], "--target=go -O2 --go-build '(linux && amd64) || !goose_x'",
                 hello),
                (["--go-runtime", "full", "--go-unexported", "--go-no-withstack"],
                 "--target=go -O2 --go-runtime=full --go-unexported --go-no-withstack", hello),
                (["--go-runtime=import"], "--target=go -O2 --go-runtime=import", hello),
                (["--stack-reserve", "64K", "-DGS_STACK_GAP=0x2000", "-DGS_STACK_BUDGET=(1 << 40)", "-DGS_MAX_STACKS=64",
                  "-DGS_STACK_STATS=1", "--frame-reserve", "2M"],
                 "--target=go -O2 --stack-reserve 64K -DGS_STACK_GAP=8192 -DGS_STACK_BUDGET=1099511627776 "
                 "-DGS_MAX_STACKS=64 -DGS_STACK_STATS=1 --frame-reserve 2M", hello),
                (["-DGS_DEBUG=1", "-DGS_STACK_RESERVE=0x10000"], "--target=go -O2 --debug --stack-reserve 64K", hello),
                (["-DGS_STACK_RESERVE=0x100"], "--target=go -O2 -DGS_STACK_RESERVE=256", hello),
                (["--include", helper], "--target=go -O2 --include helper.go", shout)):
            head = header(compile_go(flags, src))
            got = re.search(r"\n// goose-flags: (.*)\n", head)
            check(f"// goose-flags: {want}", got is not None and got[1] == want, head)
        heads = [header(compile_go(["--go-package", p], name=f"package-{p}.go")) for p in ("geese", "ducks")]
        check("--go-package changes the header", heads[0] != heads[1] and "--go-package geese\n" in heads[0] and
              "--go-package ducks\n" in heads[1], "\n".join(heads))
        same = [compile_go(["--go-package=geese", "-DGS_DEBUG=1", "-DGS_STACK_RESERVE=0x10000"], name="same-a.go"),
                compile_go(["--debug", "--stack-reserve", "64K", "--target=go", "-O2", "--go-package", "geese"],
                           name="same-b.go")]
        check("two compiles that say the same in other words write the same file", same[0] and same[0] == same[1],
              first_difference(*same))
        # A runtime file's header records the version; the programs' headers
        # the runtime file among their options.
        shared = work / "shared"
        shared.mkdir()
        sd = HERE / "go" / "shared"
        rt = shared / "goose_runtime.go"
        sargs = ["--target=go", "--library", "--go-package", "shared", "--go-runtime-file", rt]
        for prog in ("geom", "text"):
            code, o, e = self.goose(*sargs, "-o", shared / f"{prog}.go", sd / f"{prog}.goose")
            check(f"goose {prog}.goose --go-runtime-file", code == 0, f"exit {code}\n{o}{e}")
        rttext = rt.read_text(encoding="utf-8") if rt.exists() else ""
        geom = (shared / "geom.go").read_text(encoding="utf-8") if (shared / "geom.go").exists() else ""
        check("the runtime file's header records the version, and a program's header its runtime file",
              rttext.startswith(f"// Code generated by the Goose compiler. DO NOT EDIT.\n//\n{stamp}//\n") and
              "\n// goose-flags: --target=go -O2 --library --go-package shared --go-runtime-file goose_runtime.go\n"
              in header(geom), header(rttext) + "\n" + header(geom))
        # --check: the files the compile would write, compared with those
        # there, which it leaves as they are.
        src = work / "check.goose"
        shutil.copy(hello, src)
        gofile = work / "check.go"
        code, o, e = self.goose("--target=go", "--go-package", "geese", "-o", gofile, src)
        written = gofile.read_bytes() if code == 0 else b""
        check("goose -o check.go", code == 0, f"exit {code}\n{o}{e}")
        wlines = tc.decode(written).splitlines()

        def checked(*flags):
            """--check of check.go: the exit code, stdout and stderr, and
            whether the file is as it was."""
            before = gofile.read_bytes() if gofile.exists() else None
            code, o, e = self.goose(*flags, "-o", gofile, src)
            return code, o, e, (gofile.read_bytes() if gofile.exists() else None) == before

        for args in (["--target=go", "--check", "--go-package", "geese"],
                     ["check", "--target=go", "--go-package=geese"],
                     ["--check", "-O2", "--go-runtime=embed", "--go-package", "geese"]):
            code, o, e, kept = checked(*args)
            check(f"goose {' '.join(args)} -o check.go: the file is up to date",
                  code == 0 and not o and not e and kept, f"exit {code}\n{o}{e}")
        code, o, e, kept = checked("--target=go", "--check", "--stats", "--go-package", "geese")
        check("--check --stats: up to date", code == 0 and f"\n{gofile} is up to date ({len(written)} bytes)\n" in o
              and kept, f"exit {code}\n{o}{e}")
        # Other options: the header's options and the package clause differ.
        code, o, e, kept = checked("--target=go", "--check", "--go-package", "ducks")
        check("--check: a file compiled with other options is stale, with a diff of the lines that differ",
              code == 1 and not o and kept and e.startswith(f"{gofile} is stale: ") and
              "\n-// goose-flags: --target=go -O2 --go-package geese\n"
              "+// goose-flags: --target=go -O2 --go-package ducks\n" in e and
              "\n-package geese\n+package ducks\n" in e, f"exit {code}\n{o}{e}")
        # A file another compiler wrote: its version.
        tc.write_text(gofile, tc.decode(written).replace(stamp, "// goose-version: v0.1 (runtime/go 0123456789ab)\n"))
        code, o, e, kept = checked("--target=go", "--check", "--go-package", "geese")
        check("--check: a file another compiler wrote is stale",
              code == 1 and kept and f"\n@@ -3 +3 @@\n-// goose-version: v0.1 (runtime/go 0123456789ab)\n+{stamp}" in e,
              f"exit {code}\n{o}{e}")
        gofile.write_bytes(written)
        # The source changed: the one line of Go it changes.
        tc.write_text(src, hello.read_text(encoding="utf-8").replace('"hello, world"', '"hello, there"'))
        at = next((i for i, line in enumerate(wlines) if '"hello, world"' in line), 0)
        hunk = f"\n@@ -{at + 1} +{at + 1} @@\n-{wlines[at]}\n+{wlines[at].replace('hello, world', 'hello, there')}\n" \
            if wlines else "?"
        code, o, e, kept = checked("--target=go", "--check", "--go-package", "geese")
        check("--check: the file of a source that changed is stale, the line it changes its diff",
              code == 1 and kept and e.startswith(f"{gofile} is stale: ") and hunk in e and
              e.endswith(hunk[1:] if hunk.startswith("\n") else hunk), f"exit {code}\n{o}{e}\n--- want:{hunk}")
        shutil.copy(hello, src)
        # A diff of many lines is cut short.
        code, o, e, kept = checked("--target=go", "--check", "--go-package", "geese", "--go-runtime=full")
        check("--check: a long diff is cut short", code == 1 and kept and len(e.splitlines()) <= 30 and
              "more " in e.splitlines()[-1], f"exit {code}\n{o}{e}")
        # Other line breaks, which goose would write as \n.
        gofile.write_bytes(written.replace(b"\n", b"\r\n"))
        code, o, e, kept = checked("--target=go", "--check", "--go-package", "geese")
        check("--check: a file with \\r\\n line breaks is stale", code == 1 and kept and "line breaks" in e,
              f"exit {code}\n{o}{e}")
        # The program checked as the compile that writes the file checks it:
        # one with no fn main() and no export fn is refused, which a check
        # of the program alone lets pass.
        nomain = work / "nomain.goose"
        tc.write_text(nomain, "fn helper() -> i64 { 1 }\n")
        for args in (["--target=go"], ["--target=go", "--check"], ["check", "--target=go"]):
            code, o, e = self.goose(*args, "-o", work / "nomain.go", nomain)
            check(f"goose {' '.join(args)} -o nomain.go refuses a program with no fn main()",
                  code == 1 and "needs exactly one global fn main()" in e, f"exit {code}\n{o}{e}")
        code, o, e = self.goose("check", "--target=go", nomain)
        check("goose check --target=go, with no -o, checks a file with no fn main() on its own",
              code == 0 and not o and not e, f"exit {code}\n{o}{e}")
        # No file at all: stale, and still none.
        gofile.unlink()
        code, o, e, kept = checked("--target=go", "--check", "--go-package", "geese")
        check("--check: a missing file is stale, and is not written",
              code == 1 and kept and not gofile.exists() and f"cannot read {gofile}" in e, f"exit {code}\n{o}{e}")
        # A package's runtime file is checked too, as the compile would
        # write it again.
        for prog in ("geom", "text"):
            code, o, e = self.goose(*sargs, "--check", "-o", shared / f"{prog}.go", sd / f"{prog}.goose")
            check(f"--check {prog}.go --go-runtime-file: both files up to date", code == 0 and not o and not e,
                  f"exit {code}\n{o}{e}")
        rtbytes = rt.read_bytes() if rt.exists() else b""
        rt.write_bytes(rtbytes.replace(b"\n// goose-runtime: ", b"\n// An edit.\n// goose-runtime: ", 1))
        code, o, e = self.goose(*sargs, "--check", "-o", shared / "geom.go", sd / "geom.goose")
        check("--check: a runtime file that is not what the compile would write is stale",
              code == 1 and e.startswith(f"{rt} is stale: ") and "\n-// An edit.\n" in e and
              "geom.go is stale" not in e, f"exit {code}\n{o}{e}")
        rt.write_bytes(rtbytes)
        self.same_go(check, helper, shout)
        return out

    def same_go(self, check, helper, shout):
        """The same programs compiled in two temporary directories, each
        holding a copy of them and a standard library of its own, which the
        compiles are given by its whole path, as two machines would have
        them in places of their own (issue #21): every program of
        test/go/programs/, with the flags its first line names, at -O2 and
        at -O0; every fixture of the C suite's test/stdlib/, which import
        most of the library; and a program with the runtime whole,
        unexported or imported, a library, an --include (helper, by shout)
        and two programs sharing a runtime file. Each compile runs in its
        directory and names its files relative to it, and the Go must be
        the same, byte for byte, naming neither directory."""
        dirs = [Path(tempfile.mkdtemp(prefix="goose-same-")), Path(tempfile.mkdtemp(prefix="goose-same-other-"))]
        try:
            for t in dirs:
                shutil.copytree(HERE / "go" / "programs", t / "programs")
                shutil.copytree(UPSTREAM_TEST / "stdlib", t / "fixtures")
                shutil.copytree(UPSTREAM / "stdlib", t / "stdlib")
                shutil.copytree(HERE / "go" / "shared", t / "shared")
                shutil.copy(helper, t / "helper.go")
                shutil.copy(shout, t / "shout.goose")
            # Each case: its compiles, (flags, source, -o), in order, and the
            # files to compare once they ran.
            cases = []
            for p in sorted((HERE / "go" / "programs").glob("*.goose")):
                for level in ("-O2", "-O0"):
                    cases.append((f"programs/{p.name} {level}",
                                  [(goose_flags(p) + [level], f"programs/{p.name}", f"out/{p.stem}{level}.go")],
                                  [f"out/{p.stem}{level}.go"]))
            for p in sorted((UPSTREAM_TEST / "stdlib").glob("*.goose")):
                cases.append((f"test/stdlib/{p.name}", [(runtime_defines(p), f"fixtures/{p.name}", f"out/{p.stem}.go")],
                              [f"out/{p.stem}.go"]))
            for k, flags in enumerate((["--go-runtime=full"], ["--go-unexported"], ["--go-runtime=import"],
                                       ["--library", "--go-package", "geese", "--debug"])):
                cases.append((f"programs/print.goose {' '.join(flags)}",
                              [(flags, "programs/print.goose", f"out/print-{k}.go")], [f"out/print-{k}.go"]))
            cases.append(("shout.goose --include helper.go", [(["--include", "helper.go"], "shout.goose", "shout.go")],
                          ["shout.go"]))
            sargs = ["--library", "--go-package", "shared", "--go-runtime-file", "shared/goose_runtime.go"]
            cases.append(("shared/geom.goose and shared/text.goose --go-runtime-file",
                          [(sargs, "shared/geom.goose", "shared/geom.go"), (sargs, "shared/text.goose", "shared/text.go")],
                          ["shared/geom.go", "shared/text.go", "shared/goose_runtime.go"]))
            differ, compared = [], 0
            for label, runs, files in cases:
                got = []
                for t in dirs:
                    ok, log = True, ""
                    for flags, src, gofile in runs:
                        code, o, e = run_capture([self.exe, "--stdlib", t / "stdlib", *flags, "--target=go", "-o",
                                                  gofile, src], cwd=t)
                        ok, log = ok and code == 0, log + o + e
                    got.append((ok, log, [tc.decode((t / f).read_bytes()) if ok else "" for f in files]))
                if not got[0][0] or not got[1][0]:
                    where = "neither directory" if not got[0][0] and not got[1][0] else "one directory only"
                    differ.append(f"{label}: compiled in {where}:\n{got[0][1]}{got[1][1]}")
                    continue
                compared += 1
                for f, a, b in zip(files, got[0][2], got[1][2]):
                    if a != b:
                        differ.append(f"{label}: {f} differs, first at {first_difference(a, b)}")
                    elif any(str(t) in a or t.name in a for t in dirs):
                        differ.append(f"{label}: {f} names the directory it was compiled in")
            check(f"{compared} compiles in two temporary directories write the same Go, byte for byte",
                  compared == len(cases) and not differ,
                  f"{compared} of {len(cases)} compared\n" + "\n".join(differ[:12]))
        finally:
            for t in dirs:
                shutil.rmtree(t, ignore_errors=True)

    def release(self):
        """Prebuilt compilers (issue #29). scripts/release.py packages the
        compiler under test as a release holds it: one directory,
        goose-<version>-<os>-<arch>/, of the compiler, the standard library
        git tracks, the license and TinyCC's, in an archive that is the
        same bytes when it is packaged again, beside SHA256SUMS, which lists
        its checksum; a version the compiler does not report is refused.
        cmd/goose, the wrapper `go run` runs, passes go vet, gofmt and its
        own tests. Served from a module proxy of the suite's, as `go run
        github.com/robtandy/goose.go/cmd/goose@<version>` fetches it, and run
        with nothing but Go on PATH and a home of its own, the wrapper
        downloads the release of its version (WRAPPER_TEST_VERSION, the
        compiler under test) from a local server, once, checks it against
        SHA256SUMS, keeps it in the user's cache directory and runs it:
        `go generate` writes the library the compiler under test writes,
        byte for byte, which vets, builds and passes its test, and `--check`
        through the wrapper finds it up to date; installed, the wrapper
        compiles every sample to the Go the compiler under test does.
        GOOSE_RELEASE runs another release; one whose archive is not what
        its SHA256SUMS says is refused and not kept; and one there is none
        of is named, with where it was looked for. With --release-dist, the
        release the wrapper runs is the one there, as scripts/release.py
        build made it for this platform, which must be the compiler under
        test's version: the Go it writes must be the compiler under test's,
        byte for byte (issue #29's acceptance criterion)."""
        out = []

        def check(what, ok, detail=""):
            out.append(("ok" if ok else "fail", f"release: {what}", None if ok else detail))

        _, goos, _ = self.gotool("env", "GOOS")
        _, goarch, _ = self.gotool("env", "GOARCH")
        goos, goarch = goos.strip(), goarch.strip()
        if (goos, goarch) not in RELEASE_PLATFORMS:
            return [("skip", f"release: there are no prebuilt compilers for {goos}/{goarch}", None)]
        plat = f"{goos}-{goarch}"
        work = self.work / "release"
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        code, o, e = self.goose("version")
        m = re.match(r"goose (\S+) \(runtime [0-9a-f]{12}\)\n$", o)
        if not m:
            check("goose version", False, f"exit {code}\n{o}{e}")
            return out
        version, says = m[1], o

        def package(*args):
            r = subprocess.run([sys.executable, RELEASE_SCRIPT, "package", "--exe", self.exe, *args],
                               capture_output=True)
            return r.returncode, tc.decode(r.stdout) + tc.decode(r.stderr)

        # The script's command line, on the compiler under test: the
        # archive of the version it reports, and SHA256SUMS.
        dist = work / "dist"
        code, log = package("--dist", dist)
        name = f"goose-{version}-{plat}.tar.gz"
        archive, sums = dist / version / name, dist / version / "SHA256SUMS"
        check(f"release.py package writes {version}/{name} and {version}/SHA256SUMS",
              code == 0 and archive.is_file() and sums.is_file(), f"exit {code}\n{log}")
        if not archive.is_file() or not sums.is_file():
            return out
        digest = hashlib.sha256(archive.read_bytes()).hexdigest()
        check("SHA256SUMS lists the archive's checksum, as sha256sum writes it",
              sums.read_bytes() == f"{digest}  {name}\n".encode(), sums.read_text(encoding="utf-8"))
        top = name[:-len(".tar.gz")]
        want = {"goose": Path(self.exe).read_bytes(), "LICENSE": (REPO / "LICENSE").read_bytes()}
        listed = subprocess.run(["git", "-C", UPSTREAM, "ls-files", "-z", "--", "stdlib"], capture_output=True)
        for p in tc.decode(listed.stdout).split("\0"):
            if p:
                want[p] = (UPSTREAM / p).read_bytes()
        for p, src in RELEASE_LICENSES.items():
            want[p] = src.read_bytes()
        try:
            members = tar_contents(archive)
        except (OSError, tarfile.TarError) as ex:
            check("the archive is a gzipped tar", False, str(ex))
            return out
        got = {n[len(top) + 1:]: data for n, (_, data) in members.items() if n.startswith(f"{top}/") and data is not None}
        files = [n for n, (_, data) in members.items() if data is not None]
        check(f"the archive is {top}/: the compiler, the standard library git tracks, the license and TinyCC's",
              "stdlib/std.goose" in want and len(got) == len(files) and got == want,
              f"in one and not the other: {sorted(set(got) ^ set(want))}; differing: "
              f"{sorted(k for k in set(got) & set(want) if got[k] != want[k])}; outside {top}/: "
              f"{[n for n in files if not n.startswith(top + '/')]}")
        modes = {n: (info.mode, info.uid, info.gid, info.isdir()) for n, (info, _) in members.items()}
        check("its files are root's, the compiler executable, the rest read-only to others, and its directories listed",
              all(uid == 0 and gid == 0 and mode == (0o755 if isdir or n == f"{top}/goose" else 0o644)
                  for n, (mode, uid, gid, isdir) in modes.items()) and
              modes.get(top, (0, 0, 0, False))[3] and modes.get(f"{top}/stdlib", (0, 0, 0, False))[3],
              "\n".join(f"{n}: {oct(mode)} {uid}:{gid}{' dir' if isdir else ''}"
                        for n, (mode, uid, gid, isdir) in sorted(modes.items())))
        again = work / "dist-again"
        code, log = package("--dist", again)
        check("packaged again, the archive and SHA256SUMS are the same bytes",
              code == 0 and (again / version / name).read_bytes() == archive.read_bytes() and
              (again / version / "SHA256SUMS").read_bytes() == sums.read_bytes(), f"exit {code}\n{log}")
        wrong = work / "dist-wrong"
        code, log = package("--version", "v9.9.9", "--dist", wrong)
        check("release.py package refuses a version the compiler does not report",
              code != 0 and version in log and not wrong.exists(), f"exit {code}\n{log}")
        # The wrapper's own tests.
        for what, cmd in (("go vet", ["vet", "./..."]), ("go test", ["test", "-count=1", "./..."])):
            r = subprocess.run([self.go, *cmd], cwd=WRAPPER, env=go_env(), capture_output=True)
            text = tc.decode(r.stdout) + tc.decode(r.stderr)
            check(f"cmd/goose: {what}", r.returncode == 0 and not (what == "go vet" and text.strip()), text)
        r = subprocess.run([self.gofmt, "-l", WRAPPER], capture_output=True)
        check("cmd/goose: gofmt -l", r.returncode == 0 and not r.stdout.strip(),
              tc.decode(r.stdout) + tc.decode(r.stderr))
        try:
            rel = release_script()
        except Exception as ex:
            check("scripts/release.py loads as a module", False, repr(ex))
            return out
        check("release.py reads the compiler's platform from its header",
              rel.binary_platform(self.exe) == f"{goos}/{goarch}", str(rel.binary_platform(self.exe)))
        # The wrapper's module, at WRAPPER_TEST_VERSION in a proxy of the
        # suite's, as the go command fetches a module; the compiler under
        # test as the release of that name -- or, with --release-dist, the
        # compiler of the release scripts/release.py built there, of the
        # compiler under test's version, which is served as that version's
        # too; and a release whose archive is not what its SHA256SUMS says,
        # which no compiler need be in.
        v, tampered = WRAPPER_TEST_VERSION, "v0.0.0-tampered"
        proxy = work / "proxy"
        at = proxy / WRAPPER_MODULE / "@v"
        at.mkdir(parents=True)
        tc.write_text(at / "list", f"{v}\n")
        tc.write_text(at / f"{v}.info", json.dumps({"Version": v, "Time": "2026-01-01T00:00:00Z"}) + "\n")
        shutil.copy(WRAPPER / "go.mod", at / f"{v}.mod")
        with zipfile.ZipFile(at / f"{v}.zip", "w") as z:
            for p in sorted(WRAPPER.rglob("*")):
                if p.is_file():
                    z.write(p, f"{WRAPPER_MODULE}@{v}/{p.relative_to(WRAPPER).as_posix()}")
        if self.release_dist:
            given, gsums = self.release_dist / name, self.release_dist / "SHA256SUMS"
            listed = given.is_file() and gsums.is_file() and \
                f"{hashlib.sha256(given.read_bytes()).hexdigest()}  {name}" in gsums.read_text().splitlines()
            check(f"--release-dist {self.release_dist} holds {name}, which its SHA256SUMS lists", listed,
                  "\n".join(p.name for p in self.release_dist.iterdir()) if self.release_dist.is_dir() else "no such directory")
            if not listed:
                return out
            shutil.rmtree(dist / version)
            shutil.copytree(self.release_dist, dist / version)
            (dist / v).mkdir(parents=True)
            shutil.copy(given, dist / v / f"goose-{v}-{plat}.tar.gz")
            rel.write_sums(dist / v)
        else:
            rel.package(self.exe, f"{goos}/{goarch}", v, dist)
        rel.package(RELEASE_SCRIPT, f"{goos}/{goarch}", tampered, dist)
        bad = dist / tampered / f"goose-{tampered}-{plat}.tar.gz"
        data = bytearray(bad.read_bytes())
        data[len(data) // 2] ^= 0xFF
        bad.write_bytes(bytes(data))
        served, lock = [], threading.Lock()

        class Releases(http.server.SimpleHTTPRequestHandler):
            def log_request(self, code="-", size="-"):
                with lock:
                    served.append((self.path, int(code) if str(code).isdigit() else code))

            def log_message(self, *args):
                pass

        server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), functools.partial(Releases, directory=str(dist)))
        threading.Thread(target=server.serve_forever, daemon=True).start()
        url = f"http://127.0.0.1:{server.server_address[1]}"
        # Nothing but Go: its directory alone on PATH, and a home of the
        # wrapper's own, whose cache directory it keeps the release in.
        _, goroot, _ = self.gotool("env", "GOROOT")
        _, gocache, _ = self.gotool("env", "GOCACHE")
        gobin = Path(goroot.strip()) / "bin"
        go = str(gobin / f"go{tc.EXE_SUFFIX}")
        home = work / "home"
        home.mkdir()
        env = {"PATH": str(gobin), "HOME": str(home), "GOPROXY": proxy.as_uri(), "GOSUMDB": "off",
               "GOMODCACHE": str(work / "modcache"), "GOFLAGS": "-modcacherw", "GOTOOLCHAIN": "local",
               "CGO_ENABLED": "0", "GOCACHE": gocache.strip(), "GOOSE_RELEASE_URL": url}
        if os.environ.get("TMPDIR"):
            env["TMPDIR"] = os.environ["TMPDIR"]
        cache = home / ("Library/Caches" if goos == "darwin" else ".cache") / "goose.go"

        def run(argv, cwd, **extra):
            r = subprocess.run([str(a) for a in argv], cwd=cwd, env=dict(env, **extra), capture_output=True)
            return r.returncode, tc.decode(r.stdout), tc.decode(r.stderr)

        def downloads(release, archive_name):
            return [(f"/{release}/SHA256SUMS", 200), (f"/{release}/{archive_name}", 200)]

        try:
            # go generate in a module of its own, beside the same files
            # compiled by the compiler under test.
            gen, local = work / "gen", work / "local"
            for d in (gen, local):
                d.mkdir()
                tc.write_text(d / "stats.goose", STATS_GOOSE)
            args = ["--target=go", "--library", "--go-package", "stats", "-o", "stats.go", "stats.goose"]
            tc.write_text(gen / "go.mod", "module example.com/gen\n\ngo 1.21\n")
            tc.write_text(gen / "gen.go", "// Package stats is stats.goose, compiled by a release of the compiler.\n"
                                          f"package stats\n\n//go:generate go run {WRAPPER_MODULE}@{v} {' '.join(args)}\n")
            tc.write_text(gen / "stats_test.go", STATS_TEST)
            code, o, e = run([go, "generate", "./..."], gen)
            check("go generate runs the release with nothing but Go on PATH",
                  code == 0 and (gen / "stats.go").is_file(), f"exit {code}\n{o}{e}")
            lcode, lo, le = run_capture([self.exe, *args], cwd=local)
            a = (gen / "stats.go").read_bytes() if (gen / "stats.go").is_file() else b""
            b = (local / "stats.go").read_bytes() if lcode == 0 else b""
            check("go generate writes the file the compiler under test writes, byte for byte",
                  a and a == b, f"first at {first_difference(tc.decode(a), tc.decode(b))}\n{lo}{le}")
            check(f"the release is downloaded once, SHA256SUMS and then the archive, from {url}",
                  served == downloads(v, f"goose-{v}-{plat}.tar.gz"), str(served))
            kept = cache / v / plat
            check("and kept in the user's cache directory",
                  (kept / "goose").is_file() and (kept / "stdlib" / "std.goose").is_file(),
                  "\n".join(str(p) for p in sorted(cache.rglob("*"))[:20]) if cache.exists() else f"no {cache}")
            for what, cmd in (("vetted", ["vet", "./..."]), ("tested", ["test", "-count=1", "./..."])):
                code, o, e = run([go, *cmd], gen)
                check(f"the generated library is {what}", code == 0 and not (what == "vetted" and (o + e).strip()),
                      f"exit {code}\n{o}{e}")
            with lock:
                served.clear()
            code, o, e = run([go, "generate", "./..."], gen)
            check("go generate again downloads nothing, and writes the same file",
                  code == 0 and not served and (gen / "stats.go").read_bytes() == a, f"exit {code}\n{o}{e}\n{served}")
            code, o, e = run([go, "run", f"{WRAPPER_MODULE}@{v}", "--check", *args], gen)
            check("--check through the wrapper finds the file up to date", code == 0 and not o and not e,
                  f"exit {code}\n{o}{e}")
            # The wrapper installed, on every sample, beside the compiler
            # under test.
            bindir = work / "bin"
            code, o, e = run([go, "install", f"{WRAPPER_MODULE}@{v}"], work, GOBIN=str(bindir))
            wrapper = bindir / f"goose{tc.EXE_SUFFIX}"
            check("go install of the wrapper", code == 0 and wrapper.is_file(), f"exit {code}\n{o}{e}")
            samples = parity_samples()
            dirs = [work / "samples-wrapper", work / "samples-local"]
            for d in dirs:
                d.mkdir()
                for s in (UPSTREAM / "samples").glob("*.goose"):
                    shutil.copy(s, d / s.name)
            differ = []
            for s in samples:
                sargs = ["--target=go", "-o", f"{s.stem}.go", s.name]
                ra = run([wrapper, *sargs], dirs[0])
                rb = run_capture([self.exe, *sargs], cwd=dirs[1])
                ga, gb = [(d / f"{s.stem}.go").read_bytes() if (d / f"{s.stem}.go").is_file() else None
                          for d in dirs]
                if ra != rb or ga != gb or ga is None:
                    differ.append(f"{s.name}: exit {ra[0]} and {rb[0]}\n{ra[1]}{ra[2]}{rb[1]}{rb[2]}" +
                                  (f"first at {first_difference(tc.decode(ga), tc.decode(gb))}" if ga and gb else ""))
            check(f"the installed wrapper compiles {len(samples)} samples to the Go the compiler under test does, "
                  "downloading nothing", samples and not differ and not served, "\n".join(differ[:6]) + f"\n{served}")
            # Another release, named; one that is not what its SHA256SUMS
            # says; and one there is none of.
            code, o, e = run([wrapper, "version"], work, GOOSE_RELEASE=version)
            check(f"GOOSE_RELEASE={version} runs that release, which says what the compiler under test says",
                  code == 0 and o == says and served == downloads(version, name), f"exit {code}\n{o}{e}\n{served}")
            code, o, e = run([wrapper, "version"], work, GOOSE_RELEASE=tampered)
            check("an archive that is not what SHA256SUMS says is refused, and not kept",
                  code != 0 and "SHA256SUMS" in e and not o and
                  (not (cache / tampered).exists() or not any((cache / tampered).iterdir())), f"exit {code}\n{o}{e}")
            code, o, e = run([wrapper, "version"], work, GOOSE_RELEASE="v9.9.9")
            check("a release there is none of is named, with where it was looked for",
                  code != 0 and f"{url}/v9.9.9/SHA256SUMS" in e and not o, f"exit {code}\n{o}{e}")
        finally:
            server.shutdown()
            server.server_close()
        return out

    def library_asm(self, src, tag, out):
        """The code Go makes of the library src: compiled with --target=go
        --library at -O2 and at -O0 into a module of its own, and by Go for
        each of WIDE_ARCHES (`go build -gcflags=-S`, which prints the code of
        every function, and cross-compiles with nothing but the toolchain).
        Yields (level, arch, that code) for each build, after a failure in
        out for each step that failed."""
        for level in ("2", "0"):
            mod = self.work / f"{tag}-asm-O{level}"
            shutil.rmtree(mod, ignore_errors=True)
            mod.mkdir(parents=True)
            code, o, e = self.goose(f"-O{level}", "--target=go", "--library", "-o",
                                    mod / f"{src.stem}.go", src)
            if code != 0:
                out.append(("fail", f"{tag} asm -O{level}: goose --target=go", o + e))
                continue
            r = subprocess.run([self.go, "mod", "init", f"goosetest/{tag}asm"], cwd=mod, env=go_env(),
                               capture_output=True)
            if r.returncode != 0:
                out.append(("fail", f"{tag} asm -O{level}: go mod init", tc.decode(r.stderr)))
                continue
            for arch in WIDE_ARCHES:
                env = go_env()
                env["GOARCH"] = arch
                r = subprocess.run([self.go, "build", "-gcflags=-S", "."], cwd=mod, env=env,
                                   capture_output=True)
                asm = tc.decode(r.stdout) + tc.decode(r.stderr)
                if r.returncode != 0:
                    out.append(("fail", f"{tag} asm -O{level} {arch}: go build -gcflags=-S", asm[-2000:]))
                    continue
                yield level, arch, asm

    def wide_asm(self):
        """The code Go makes of the wide loads and stores of
        test/go/export/wide/wide.goose (issue #23), at -O2 and at -O0, for
        each of WIDE_ARCHES (Runner.library_asm): each export fn an `// asm:`
        line annotates must reach memory through a pointer as it says: `load
        n`, once, with one n-byte load; `store n`, once, with one n-byte
        store; `none`, never, as a local array on Go's own stack does (one on
        the fixed-frame stack would not)."""
        out = []
        src = HERE / "go" / "export" / "wide" / "wide.goose"
        want = wide_annotations(src)
        if not want:
            out.append(("fail", f"wide asm: no // asm: line in {src.name}", None))
            return out
        for level, arch, asm in self.library_asm(src, "wide", out):
            for fn, (kind, n) in sorted(want.items()):
                label = f"wide asm {fn} -O{level} {arch}: {kind}{f' {n}' if n else ''}"
                got = wide_accesses(arch, asm, f"{fn}_g")
                if got is None:
                    out.append(("fail", label, f"no {fn}_g in Go's code for {arch}"))
                    continue
                ok = not got if kind == "none" else [(k, b) for k, b, _ in got] == [(kind, n)]
                out.append(("ok" if ok else "fail", label,
                            None if ok else "\n".join(t for _, _, t in got) or "no access"))
        return out

    def select_asm(self):
        """The code Go makes of the selects of
        test/go/export/selects/selects.goose (issue #26), at -O2 and at -O0,
        for each of WIDE_ARCHES (Runner.library_asm): each export fn an `//
        asm: select, branches m` line annotates must have at least one
        conditional select and exactly m conditional branches, those of its
        loop and its guard, so that none of the choices it makes is a
        branch."""
        out = []
        src = HERE / "go" / "export" / "selects" / "selects.goose"
        want = select_annotations(src)
        if not want:
            out.append(("fail", f"select asm: no // asm: line in {src.name}", None))
            return out
        for level, arch, asm in self.library_asm(src, "selects", out):
            for fn, branches in sorted(want.items()):
                label = f"select asm {fn} -O{level} {arch}: select, branches {branches}"
                got = select_counts(arch, asm, f"{fn}_g")
                if got is None:
                    out.append(("fail", label, f"no {fn}_g in Go's code for {arch}"))
                    continue
                sels, brs = got
                ok = sels and len(brs) == branches
                out.append(("ok" if ok else "fail", label, None if ok else
                            f"{len(sels)} select(s), {len(brs)} branch(es):\n" + "\n".join(sels + brs)))
        return out

    def spill_asm(self):
        """The code Go makes of the bounds checks of
        test/go/export/bounds/bounds.goose (issue #40), at -O2 and at -O0, for
        each of WIDE_ARCHES (Runner.library_asm): each export fn an `// asm:
        no spills` line annotates must move no value between a register and
        its frame (spill_moves), which a check whose failure Go took to
        return made it do around the check, and each one an `// asm: no
        index checks` line must make none of Go's own checks (index_panics),
        which Go proves pass past GsIdx; and every call of GsIdx and GsSub
        in the file must be one Go inlines (`go build -gcflags=-m`)."""
        out = []
        src = HERE / "go" / "export" / "bounds" / "bounds.goose"
        want = spill_annotations(src)
        if not want:
            out.append(("fail", f"spill asm: no // asm: line in {src.name}", None))
            return out
        levels = set()
        for level, arch, asm in self.library_asm(src, "bounds", out):
            if level not in levels:
                levels.add(level)
                mod = self.work / f"bounds-asm-O{level}"
                r = subprocess.run([self.go, "build", "-gcflags=-m", "."], cwd=mod, env=go_env(),
                                   capture_output=True)
                mout = tc.decode(r.stdout) + tc.decode(r.stderr)
                gosrc = (mod / f"{src.stem}.go").read_text(encoding="utf-8")
                absent = [n for n in ("GsIdx", "GsSub") if not re.search(rf"^(?!func ).*\b{n}\(", gosrc, re.M)]
                missed = not_inlined(gosrc, mout, {"GsIdx", "GsSub"}) if r.returncode == 0 else [mout[-2000:]]
                ok = not absent and not missed
                out.append(("ok" if ok else "fail", f"spill asm -O{level}: GsIdx and GsSub inlined",
                            None if ok else "\n".join(missed + [f"no call of {n}" for n in absent])))
            for fn, what in sorted(want.items()):
                label = f"spill asm {fn} -O{level} {arch}: no {what}"
                got = (spill_moves if what == "spills" else index_panics)(asm, f"{fn}_g")
                if got is None:
                    out.append(("fail", label, f"no {fn}_g in Go's code for {arch}"))
                    continue
                out.append(("ok" if not got else "fail", label, None if not got else "\n".join(got)))
        return out

    def bce_gaps(self, src):
        """A reproducer of a gap in the front end's bounds-check elimination
        (test/go/bce_gaps/, issue #26): at -O1 and -O2, where calls are
        inlined, each of its `// bce:keep` lines must keep every check on it,
        and each `// bce:elide` line elide every one (goose --check
        --bce-test). Once the front end closes the gap, a `bce:keep` line
        fails, and the write-up is out of date."""
        out = []
        for level in ("1", "2"):
            label = f"bce gap {src.name} -O{level}"
            code, o, e = self.goose(f"-O{level}", "--check", "--bce-test", src)
            out.append(("ok", label, None) if code == 0 else ("fail", label, o + e))
        return out

    def frames(self):
        """The frames of the export fns of
        test/go/export/inline_refs/inline_refs.goose (issue #24): compiled
        with --target=go --library at -O2, where its helpers are inlined,
        each export fn a `// frame:` line annotates must take as many slots
        on the fixed-frame stack as it says, `none` none at all: a local an
        inlined body takes by reference stays a Go local unless its address
        outlives the body."""
        out = []
        src = HERE / "go" / "export" / "inline_refs" / "inline_refs.goose"
        want = frame_annotations(src)
        if not want:
            out.append(("fail", f"frames: no // frame: line in {src.name}", None))
            return out
        gofile = self.work / "frames-O2.go"
        gofile.unlink(missing_ok=True)
        code, o, e = self.goose("-O2", "--target=go", "--library", "-o", gofile, src)
        if code != 0:
            out.append(("fail", "frames -O2: goose --target=go", o + e))
            return out
        gosrc = gofile.read_text(encoding="utf-8")
        for fn, n in sorted(want.items()):
            label = f"frames {fn} -O2: {n or 'none'}"
            got = frame_slots(gosrc, f"{fn}_g")
            if got is None:
                out.append(("fail", label, f"no func {fn}_g in {gofile.name}"))
                continue
            text, slots = got
            out.append(("ok" if slots == n else "fail", label,
                        None if slots == n else f"{slots} slot(s):\n" + "\n".join(text.splitlines()[:40])))
        return out

    def cycles(self):
        """Helpers inlined into the functions of recursive cycles (issue #42):
        each source of CYCLE_SOURCES compiled with --target=go at -O2, the
        Go function of each function an `// inlined:` line annotates must
        call none of the Go functions of the helpers it names, which take no
        data stack and are inlined into it as into any other function, and
        must call each one a `// calls:` line names, which takes one and
        stays a call, as the optimizer leaves every call in a cycle."""
        out = []
        for rel, flags in CYCLE_SOURCES:
            src = HERE / "go" / rel
            want = cycle_annotations(src)
            if not want:
                out.append(("fail", f"cycles: no // inlined: line in {src.name}", None))
                continue
            gofile = self.work / f"cycles-{src.stem}-O2.go"
            gofile.unlink(missing_ok=True)
            code, o, e = self.goose("-O2", "--target=go", *flags, "-o", gofile, src)
            if code != 0:
                out.append(("fail", f"cycles {src.stem} -O2: goose --target=go", o + e))
                continue
            gosrc = gofile.read_text(encoding="utf-8")
            for fn, (inlined, calls) in sorted(want.items()):
                text = go_function(gosrc, fn)
                if text is None:
                    out.append(("fail", f"cycles {src.stem} {fn} -O2", f"no Go function of {fn} in {gofile.name}"))
                    continue
                for h in sorted(inlined):
                    ok = not go_calls(text, h)
                    out.append(("ok" if ok else "fail", f"cycles {src.stem} {fn} -O2: {h} inlined",
                                None if ok else "\n".join(text.splitlines()[:60])))
                for h in sorted(calls):
                    ok = go_calls(text, h)
                    out.append(("ok" if ok else "fail", f"cycles {src.stem} {fn} -O2: {h} a call",
                                None if ok else "\n".join(text.splitlines()[:60])))
        return out

    def entry_points(self):
        """The entry points of the export fns of
        test/go/export/inlining/inlining.goose (issue #22), compiled with
        --target=go --library at -O2 and at -O0, the directory's other .go
        files --include'd, into a module with its tests. Of each export fn
        an `// entry:` line annotates, as `go test -c -gcflags=-m=2` reports:
        `inline`, Go can inline every entry point (and every instantiation
        by shape of a generic one that the tests make); `no escape`, no parameter of
        them that views a caller's memory escapes (`does not escape`, or it
        leaks to the result alone); `escape`, each one does (`leaking
        param`). Go reports no escapes of a generic function's own, whose
        callers' arrays inlining_test.go follows instead (AllocsPerRun).
        Then, for each of WIDE_ARCHES (`go test -c -gcflags=-S`),
        each callX function of the tests must be the same instructions as
        directX: a call of an entry point is the call of its function that
        a caller could write by hand."""
        out = []
        d = HERE / "go" / "export" / "inlining"
        src = d / "inlining.goose"
        want = entry_annotations(src)
        tests = sorted(d.glob("*_test.go"))
        includes = sorted(p for p in d.glob("*.go") if not p.name.endswith("_test.go"))
        pairs = call_pairs("".join(t.read_text(encoding="utf-8") for t in tests))
        if not want or not pairs:
            out.append(("fail", f"entry points: no // entry: line in {src.name}, or no callX and directX in "
                                "its tests", None))
            return out
        for name, props in sorted(want.items()):
            if not props <= ENTRY_PROPS or {"escape", "no escape"} <= props:
                out.append(("fail", f"entry points {name}: // entry: {', '.join(sorted(props))}",
                            f"an // entry: line lists some of {', '.join(sorted(ENTRY_PROPS))}, not both escapes"))
        for level in ("2", "0"):
            mod = self.work / f"entries-O{level}"
            shutil.rmtree(mod, ignore_errors=True)
            mod.mkdir(parents=True)
            gofile = mod / f"{src.stem}.go"
            args = [f"-O{level}", "--target=go", "--library", "-o", gofile]
            for inc in includes:
                args += ["--include", inc]
            code, o, e = self.goose(*args, src)
            if code != 0:
                out.append(("fail", f"entry points -O{level}: goose --target=go", o + e))
                continue
            for t in tests:
                shutil.copy(t, mod / t.name)
            r = subprocess.run([self.go, "mod", "init", "goosetest/entries"], cwd=mod, env=go_env(),
                               capture_output=True)
            if r.returncode != 0:
                out.append(("fail", f"entry points -O{level}: go mod init", tc.decode(r.stderr)))
                continue
            r = subprocess.run([self.go, "test", "-c", "-o", os.devnull, "-gcflags=-m=2", "."], cwd=mod,
                               env=go_env(), capture_output=True)
            report = tc.decode(r.stdout) + tc.decode(r.stderr)
            if r.returncode != 0:
                out.append(("fail", f"entry points -O{level}: go test -c -gcflags=-m=2", report[-2000:]))
                continue
            gosrc = gofile.read_text(encoding="utf-8")
            for name, props in sorted(want.items()):
                eps = go_entry_points(gosrc, name)
                if not eps:
                    out.append(("fail", f"entry points {name} -O{level}", f"no func {name} in {gofile.name}"))
                    continue
                for ep, line, views, generic in eps:
                    at = f"./{gofile.name}:{line}:"
                    said = [x[len(at):].partition(": ")[2] for x in report.splitlines() if x.startswith(at)]
                    if "inline" in props:
                        # A generic one's calls are of its instantiations by
                        # shape, which its dictionary's wrappers (ep[string])
                        # are not.
                        costs = [x for x in said if re.match(rf"(?:can|cannot) inline {ep}" +
                                                             (r"\[go\.shape\." if generic else r"\b"), x)]
                        ok = costs and all(x.startswith("can ") for x in costs)
                        out.append(("ok" if ok else "fail", f"entry points {ep} -O{level}: Go inlines it",
                                    None if ok else "\n".join(costs) or "no cost reported"))
                    for p in [] if generic else views:
                        about = [x for x in said if re.fullmatch(rf"{p} does not escape|leaking param(?: content)?: "
                                                                 rf"{p}(?: to result .*)?", x)]
                        heap = [x for x in about if re.fullmatch(rf"leaking param(?: content)?: {p}", x)]
                        for prop, ok in (("no escape", about and not heap), ("escape", heap)):
                            if prop in props:
                                out.append(("ok" if ok else "fail", f"entry points {ep} -O{level}: {p}: {prop}",
                                            None if ok else "\n".join(about) or "nothing reported"))
            # The calls, as Go compiles them for each target.
            for arch in WIDE_ARCHES:
                env = go_env()
                env["GOARCH"] = arch
                r = subprocess.run([self.go, "test", "-c", "-o", os.devnull, "-gcflags=-S", "."], cwd=mod, env=env,
                                   capture_output=True)
                asm = tc.decode(r.stdout) + tc.decode(r.stderr)
                if r.returncode != 0:
                    out.append(("fail", f"entry points -O{level} {arch}: go test -c -gcflags=-S", asm[-2000:]))
                    continue
                for x in pairs:
                    label = f"entry points -O{level} {arch}: call{x} is direct{x}"
                    a, b = asm_instructions(asm, f"call{x}"), asm_instructions(asm, f"direct{x}")
                    ok = a is not None and a == b
                    detail = None if ok else "\n".join(difflib.unified_diff(a or [], b or [], f"call{x}", f"direct{x}",
                                                                            lineterm=""))
                    out.append(("ok" if ok else "fail", label, detail))
        return out

    # --- the command line ----------------------------------------------------

    def cli(self):
        out = []
        progs = HERE / "go" / "programs"
        hello = progs / "print.goose"
        exitprog = progs / "exit_code.goose"

        def check(what, ok, detail=""):
            out.append(("ok" if ok else "fail", f"cli: {what}", None if ok else detail))

        # -o out.go picks the Go backend.
        gofile = self.work / "cli-inferred.go"
        code, o, e = self.goose("-o", gofile, hello)
        text = gofile.read_text(encoding="utf-8") if gofile.exists() else ""
        check("-o out.go writes Go", code == 0 and text.startswith("// Code generated by the Goose "
                                                                  "compiler") and
              "\npackage main\n" in text, o + e)
        # --go-package and --library.
        code, o, e = self.goose("--target=go", "--go-package", "geese", "-o", gofile, hello)
        text = gofile.read_text(encoding="utf-8") if gofile.exists() else ""
        check("--go-package", code == 0 and "\npackage geese\n" in text, o + e)
        code, o, e = self.goose("--target=go", "--library", "-o", gofile, hello)
        text = gofile.read_text(encoding="utf-8") if gofile.exists() else ""
        # A library has no init: the first call of an entry point starts it
        # (issue #18), where it has anything to start (issue #19): this one,
        # with no globals, has not.
        check("--library", code == 0 and "\npackage goose\n" in text and
              "\nfunc main()" not in text and "\nfunc init()" not in text and
              "\nfunc gsLibStart() {" not in text and "gsInitGlobals" not in text, o + e)
        if code == 0:
            vc, vo, ve = self.gotool("vet", gofile.name)
            check("--library vets", vc == 0 and not (vo + ve).strip(), vo + ve)
            diff = self.gofmt_clean(gofile)
            check("--library is laid out as gofmt lays it out", diff is None, diff or "")
        # A Go program that imports a library: importing it runs nothing, the
        # first call of an entry point initializes the globals, once, the
        # library's fn main() never runs, and its exit() ends no process but
        # panics with a *GsError the host recovers (issue #18).
        host = self.work / "cli-host"
        shutil.rmtree(host, ignore_errors=True)
        (host / "goose").mkdir(parents=True)
        libsrc = self.work / "cli-library.goose"
        tc.write_text(libsrc, 'fn noisy() -> i64 {\n    print("library global initialized");\n    7\n}\n'
                              'let answer = noisy();\n'
                              'export fn get_answer() -> i64 { answer }\n'
                              'export fn quit(code: i64) { exit(code); }\n'
                              'fn main() {\n    print("library main ran: ", answer);\n    exit(3);\n}\n')
        tc.write_text(host / "main.go", 'package main\n\nimport (\n\t"fmt"\n\n\t"gooshost/goose"\n)\n\n'
                                        'func main() {\n\tfmt.Println("host main ran")\n'
                                        '\tfmt.Println(goose.GetAnswer(), goose.GetAnswer())\n'
                                        '\tdefer func() { fmt.Println("recovered", recover()) }()\n'
                                        '\tgoose.Quit(3)\n}\n')
        code, o, e = self.goose("--target=go", "--library", "-o", host / "goose" / "goose.go", libsrc)
        text = (host / "goose" / "goose.go").read_text(encoding="utf-8") if code == 0 else ""
        check("a --library package with globals starts at its first call, a library that never exits",
              code == 0 and "\nfunc gsLibStart() {" in text and
              "\nvar gsProg = gsProgram{StackReserve: gsStackReserve, StackGap: gsStackGap, "
              "FrameReserve: gsFrameReserve, Library: true}\n" in text, o + e)
        detail = o + e
        ok = False
        if code == 0:
            # The module's go line is the toolchain's own.
            r = subprocess.run([self.go, "mod", "init", "gooshost"], cwd=host, env=go_env(),
                               capture_output=True)
            if r.returncode == 0:
                r = subprocess.run([self.go, "run", "."], cwd=host, env=go_env(), capture_output=True)
            detail = f"exit {r.returncode}\n{tc.decode(r.stdout)}{tc.decode(r.stderr)}"
            ok = r.returncode == 0 and tc.decode(r.stdout) == \
                "host main ran\nlibrary global initialized\n7 7\nrecovered goose runtime error: exit(3)\n"
        check("a Go program imports a --library package, which starts at its first call and runs no fn main",
              ok, detail)
        # Nor does a worker's abort: no host can recover a panic on the
        # worker's goroutine, which the runtime started, so the process ends
        # with Go's report of the abort's panic, as for any goroutine's.
        whost = self.work / "cli-worker-host"
        shutil.rmtree(whost, ignore_errors=True)
        (whost / "goose").mkdir(parents=True)
        libsrc = self.work / "cli-worker-library.goose"
        tc.write_text(libsrc, 'thread_fn fail(x: i64) { assert(x < 0); }\n'
                              'export fn run() -> i64 {\n    thread_wait(thread_spawn(fail, 1));\n    1\n}\n')
        tc.write_text(whost / "main.go", 'package main\n\nimport "goosworker/goose"\n\nfunc main() { goose.Run() }\n')
        code, o, e = self.goose("--target=go", "--library", "-o", whost / "goose" / "goose.go", libsrc)
        detail = o + e
        ok = False
        if code == 0:
            r = subprocess.run([self.go, "mod", "init", "goosworker"], cwd=whost, env=go_env(),
                               capture_output=True)
            if r.returncode == 0:
                r = subprocess.run([self.go, "build", "-o", "host" + tc.EXE_SUFFIX, "."], cwd=whost, env=go_env(),
                                   capture_output=True)
            if r.returncode == 0:
                r = subprocess.run([str(whost / ("host" + tc.EXE_SUFFIX))], cwd=whost, capture_output=True)
            err = tc.decode(r.stderr)
            detail = f"exit {r.returncode}\n{tc.decode(r.stdout)}{err}"
            ok = r.returncode == 2 and not r.stdout and \
                err.startswith("panic: goose runtime error: assert failed (") and "\ngoroutine " in err
        check("a worker's abort in a --library package is a panic, not an exit", ok, detail)
        # The fixed-frame stack's reservation (issue #18): a program's 1 GB, a
        # library's 64 MB, or --frame-reserve's, which Go alone has.
        for flags, want in (([], 1 << 30), (["--library"], 64 << 20), (["--frame-reserve", "2M"], 2 << 20),
                            (["--library", "--frame-reserve", "1G"], 1 << 30)):
            code, o, e = self.goose("--target=go", *flags, "-o", gofile, hello)
            text = gofile.read_text(encoding="utf-8") if gofile.exists() else ""
            check(f"--target=go {' '.join(flags)} reserves {want} bytes of fixed-frame stack".replace("  ", " "),
                  code == 0 and f"\n\tgsFrameReserve = {want}\n" in text, o + e)
        for flags, why in ((["--target=go", "--frame-reserve", "32K"], "--frame-reserve takes a size"),
                           (["--target=go", "--frame-reserve", "lots"], "--frame-reserve takes a size"),
                           (["--frame-reserve", "1M"], "are for --target=go")):
            code, o, e = self.goose(*flags, "-o", self.work / "cli-frame.c", hello)
            check(f"{' '.join(flags)} is refused", code == 1 and why in e, f"exit {code}\n{o}{e}")
        # Frames that outgrow it are the overflow C reports of its native
        # stack: a recursion holding 6 KB in each frame, at 64 KB.
        code, o, e = self.goose("run", "--target=go", "--frame-reserve", "64K", progs / "large_cycle_locals.goose")
        check("--frame-reserve 64K: frames past it are a native call stack overflow",
              code == 1 and not o and e == "goose runtime error: native call stack overflow\n",
              f"exit {code}\n{o}{e}")
        # The runtime's configuration in the forms C reads a -D in: the Go is
        # sized by the value itself, and both reports give it. One the
        # compiler cannot read stops a Go compile, which no C compiler reads.
        code, o, e = self.goose("--target=go", "-DGS_STACK_RESERVE=0x10000", "-o", gofile, hello)
        text = gofile.read_text(encoding="utf-8") if gofile.exists() else ""
        check("-DGS_STACK_RESERVE=0x10000 sizes the Go's stacks",
              code == 0 and "\n\tgsStackReserve = 65536\n" in text, o + e)
        for target in ([], ["--target=go"]):
            code, o, e = self.goose(*target, "--check", "--stacks", "-DGS_STACK_RESERVE=(1ull << 16)",
                                    hello)
            check(f"--stacks {' '.join(target + ['-DGS_STACK_RESERVE=(1ull << 16)'])}",
                  code == 0 and (o + e).startswith("data stacks: 64 KB reserved per stack"), o + e)
        # Less than a page, on every platform: the runtime starts no program.
        for target in ([], ["--target=go"]):
            code, o, e = self.goose(*target, "-DGS_STACK_RESERVE=0x100", hello)
            check(f"{' '.join(target + ['-DGS_STACK_RESERVE=0x100'])} stops the program",
                  code == 1 and not o and e == "goose runtime error: the data stack reservation "
                  "(GS_STACK_RESERVE) is less than a page\n", f"exit {code}\n{o}{e}")
        code, o, e = self.goose("--target=go", "-DGS_STACK_RESERVE=lots", hello)
        check("--target=go refuses a -DGS_STACK_RESERVE it cannot read",
              code == 1 and "reads GS_STACK_RESERVE as an integer constant" in e, f"exit {code}\n{o}{e}")
        code, o, e = self.goose("--target=rust", hello)
        check("--target=rust is refused", code == 1 and "--target takes c or go" in e, o + e)
        # export fns and no main make a library, with each export's Go entry
        # points (issue #5).
        libsrc = self.work / "cli-exports.goose"
        # A doc comment of lines go/doc/comment would lay out otherwise -- an
        # indented one, which it makes code, a blank one, a directive -- is
        # its text, as gofmt leaves it (issue #20).
        tc.write_text(libsrc, '// Twice x.\n//\n//     an indented line\n//no space after the slashes\n'
                              '//go:noinline\nexport fn twice(x: i64) -> i64 { x * 2 }\n'
                              'export "parse_count" fn count(s: const u8[:]) -> i64 { s.len }\n')
        code, o, e = self.goose("--target=go", "-o", gofile, libsrc)
        text = gofile.read_text(encoding="utf-8") if gofile.exists() else ""
        check("export fns without main make a library with their Go entry points",
              code == 0 and "\npackage goose\n" in text and "\nfunc main()" not in text and
              "\nfunc Twice(x int64) int64 {" in text and
              "\nfunc TwiceWithStack(gs *GsStackBlock, x int64) int64 {" in text and
              "\nfunc ParseCount[T0 ~string | ~[]byte](s T0) int64 {" in text and
              "\nfunc ParseCountString(s string) int64 {" in text and
              "\nfunc ParseCountBytes(s []byte) int64 {" in text, o + e)
        if code == 0:
            vc, vo, ve = self.gotool("vet", gofile.name)
            check("the export library vets", vc == 0 and not (vo + ve).strip(), vo + ve)
            diff = self.gofmt_clean(gofile)
            check("the export library is laid out as gofmt lays it out", diff is None, diff or "")
        # A NameString or NameBytes form whose name another export fn's entry
        # point has is left out (issue #22): KeptBytes is kept_bytes's.
        clashsrc = self.work / "cli-forms.goose"
        tc.write_text(clashsrc, 'export fn kept(s: const u8[:], t: const u8[:]) -> i64 { s.len + t.len }\n'
                                'export fn kept_bytes(n: i64) -> i64 { n }\n')
        code, o, e = self.goose("--target=go", "-o", gofile, clashsrc)
        text = gofile.read_text(encoding="utf-8") if code == 0 else ""
        check("an export fn's NameBytes is left out where another export fn's entry point has its name",
              code == 0 and "\nfunc KeptString(s string, t string) int64 {" in text and
              "\nfunc KeptBytes(n int64) int64 {" in text and "func KeptBytes(s" not in text and
              self.gotool("vet", gofile.name)[0] == 0, o + e)
        # A Go []string crosses as a const (const u8[:])[:] (issue #42), to Go
        # alone: its entry points take one. The checker refuses every slice
        # of slices at an export's boundary, which is C's, so --target=go
        # checks an export fn taking a slice of u8 slices itself, as the
        # checker checks one but for that parameter: another constness of it
        # is refused, and so is all the checker refuses, with its messages.
        strsrc = self.work / "cli-strings.goose"
        tc.write_text(strsrc, "export fn count(tags: const (const u8[:])[:], tag: const u8[:]) -> i64 {\n"
                              "    var n = 0;\n    for t in tags {\n        if t == tag { n += 1; }\n    }\n    n\n}\n")
        code, o, e = self.goose("--target=go", "-o", gofile, strsrc)
        text = gofile.read_text(encoding="utf-8") if code == 0 else ""
        check("an export fn takes a const (const u8[:])[:] as a []string",
              code == 0 and "\nfunc Count[T0 ~string | ~[]byte](tags []string, tag T0) int64 {" in text and
              "\nfunc CountWithStack[T0 ~string | ~[]byte](gs *GsStackBlock, tags []string, tag T0) int64 {" in text and
              "\nfunc CountString(tags []string, tag string) int64 {" in text and
              self.gotool("vet", gofile.name)[0] == 0, o + e)
        code, o, e = self.goose("--check", strsrc)
        check("the C backend refuses a const (const u8[:])[:] parameter of an export fn",
              code == 1 and "export fn count: parameter tags of type const (const u8[:])[:] cannot cross to C: "
              "nested references and slices do not cross" in e, f"exit {code}\n{o}{e}")
        for decl, why in (
                ("export fn f(tags: (const u8[:])[:]) -> i64 { tags.len }",
                 "export fn f: parameter tags of type (const u8[:])[:] cannot cross to Go: a Go []string crosses as "
                 "a const (const u8[:])[:], whose strings, as Go's, nothing writes"),
                ("export fn f(tags: const (u8[:])[:]) -> i64 { tags.len }",
                 "export fn f: parameter tags of type const (u8[:])[:] cannot cross to Go: a Go []string crosses"),
                ("struct Held { xs: const i64[:] }\n"
                 "export fn f(tags: const (const u8[:])[:], h: Held) -> i64 { tags.len + h.xs.len }",
                 "export fn f: parameter h of type Held cannot cross to C: values holding references do not cross"),
                ("export fn f(tags: const (const u8[:])[:], x: i64?) -> i64 { tags.len }",
                 "export fn f: parameter x of type i64? cannot cross to C: only plain references cross"),
                ("export fn f(tags: const (const u8[:])[:]) -> const (const u8[:])[:] { tags }",
                 "export fn f: cannot return const (const u8[:])[:] to C: nested references and slices do not cross"),
                ("export fn f(tags: const (const u8[:])[:]) -> i64 { tags.len }\n"
                 'export "f" fn g(x: i64) -> i64 { x }',
                 "export fn f: C symbol f is exported more than once"),
                ("export fn f<T>(tags: const (const u8[:])[:], x: T) -> i64 { tags.len }",
                 "export fn f: cannot be generic")):
            tc.write_text(strsrc, decl + "\n")
            code, o, e = self.goose("--target=go", "-o", gofile, strsrc)
            check(f"--target=go refuses {why.partition(': ')[2]}", code == 1 and why in e, f"exit {code}\n{o}{e}")
        # --include with --target=go is Go: a file whose extern fn the
        # program calls, its imports merged, its errors named in its own
        # lines (//line).
        incdir = self.work / "cli-include"
        shutil.rmtree(incdir, ignore_errors=True)
        incdir.mkdir(parents=True)
        incsrc = incdir / "shout.goose"
        tc.write_text(incsrc, 'extern fn shout(s: const u8[:], out: u8[>..]&);\n'
                              'extern fn half(x: i64) -> i64 = "goHalf";\n'
                              'fn main() {\n    var b: u8[>..] = [];\n    shout("hi", b);\n'
                              '    print(b, " ", half(84));\n}\n')
        helper = incdir / "helper.go"
        tc.write_text(helper, '//go:build ignore\n\npackage main\n\nimport (\n\t"strings"\n\t"os"\n)\n\n'
                              'func shout(gs *GsStackBlock, s GsSlice, out GsRref) {\n'
                              '\tGsBldAppendString(gs, out, strings.ToUpper(GsSliceToString(s))+"!")\n}\n\n'
                              'func goHalf(x int64) int64 { _ = os.Args; return x / 2 }\n')
        code, o, e = self.goose("run", "--target=go", "--include", helper, incsrc)
        check("goose run --target=go --include helper.go", code == 0 and o == "HI! 42\n",
              f"exit {code}\n{o}{e}")
        # The file names the runtime as runtime/go declares it (GsSlice,
        # GsBldAppendString), which the compiler renames in it as in the
        # program's Go (issue #19).
        for flags in (["--go-unexported"], ["--go-runtime=full", "--go-unexported"]):
            code, o, e = self.goose("run", "--target=go", *flags, "--include", helper, incsrc)
            check(f"goose run --target=go {' '.join(flags)} --include helper.go", code == 0 and o == "HI! 42\n",
                  f"exit {code}\n{o}{e}")
        code, o, e = self.goose("--target=go", "--include", helper, "-o", incdir / "out.go", incsrc)
        text = (incdir / "out.go").read_text(encoding="utf-8") if (incdir / "out.go").exists() else ""
        # Each run of the file's lines under a //line directive naming it and
        # the run's line (issue #20), its imports among the file's own.
        check("--include's //line directives name the file and each run's line",
              code == 0 and "\n//line helper.go:10\nfunc shout(" in text and
              "\n//line helper.go:14\nfunc goHalf(" in text and "//go:build" not in text and
              '\n\t"os"\n\t"runtime"\n' in text and '\n\t"strconv"\n\t"strings"\n\t"sync"\n' in text, o + e)
        if code == 0:
            diff = self.gofmt_clean(incdir / "out.go")
            check("a file with an --include is laid out as gofmt lays it out", diff is None, diff or "")
        bad = incdir / "bad.go"
        tc.write_text(bad, 'package main\n\n// shout is wrong, on line 7, past a blank line.\n'
                           'func shout(gs *GsStackBlock, s GsSlice, out GsRref) {\n\t_ = gs\n\n'
                           '\tvar n int = "not an int"\n\t_ = n\n}\n\nfunc goHalf(x int64) int64 { return x }\n')
        code, o, e = self.goose("run", "--target=go", "--include", bad, incsrc)
        check("an --include file's Go error names its line", code != 0 and "bad.go:7:" in o + e,
              f"exit {code}\n{o}{e}")
        # What runtime.Caller reports in an --include file's Go, and a panic's
        # frames there, name the file's own lines, past the blank lines gofmt
        # collapses and the comments it lays out.
        wsrc = incdir / "where.goose"
        tc.write_text(wsrc, 'extern fn where_am_i() -> i64 = "goWhere";\n'
                            'extern fn boom(x: i64) -> i64 = "goBoom";\n\n'
                            'fn main() {\n    print(where_am_i());\n    print(boom(1));\n}\n')
        where = incdir / "where.go"
        tc.write_text(where, '//go:build ignore\n\n'
                             '// The --include file of a check of positions: runtime.Caller and a panic\n'
                             '// name its own lines.\n'
                             'package main\n\nimport (\n\t"runtime"\n\t"strings"\n)\n\n\n\n'
                             '// goWhere reports the line it asks runtime.Caller about, 19.\n'
                             'func goWhere() int64 {\n\tn := int64(0)\n\n\n'
                             '\t_, file, line, _ := runtime.Caller(0)\n'
                             '\tif !strings.HasSuffix(file, "where.go") {\n\t\treturn n - 1\n\t}\n'
                             '\treturn n + int64(line)\n}\n\n'
                             '/* goBoom panics on line 30. */\n'
                             'func goBoom(x int64) int64 {\n\tif x > 0 {\n\n'
                             '\t\tpanic("boom from the include")\n\t}\n\treturn x\n}\n')
        code, o, e = self.goose("run", "--target=go", "--include", where, wsrc)
        check("runtime.Caller and a panic in an --include file name its lines",
              code == 2 and o == "19\n" and "panic: boom from the include" in e and
              re.search(r"\bwhere\.go:30\b", e) is not None,
              f"exit {code}\n{o}{e}")
        wout = incdir / "where-out.go"
        code, o, e = self.goose("--target=go", "--include", where, "-o", wout, wsrc)
        if code == 0:
            diff = self.gofmt_clean(wout)
            check("an --include file's doc comments and runs are laid out as gofmt lays them out",
                  diff is None, diff or "")
            # And they hold after gofmt -w, which collapsed the blank lines
            # that kept them before.
            subprocess.run([self.gofmt, "-w", str(wout)], capture_output=True)
            wexe = incdir / f"where-out{tc.EXE_SUFFIX}"
            bc, bo, be = self.gotool("build", "-o", wexe, wout)
            ok, detail = bc == 0, bo + be
            if ok:
                rc, ro, re_ = run_capture([wexe])
                ok = rc == 2 and ro == "19\n" and re.search(r"\bwhere\.go:30\b", re_) is not None
                detail = f"exit {rc}\n{ro}{re_}"
            check("an --include file's positions hold after gofmt -w", ok, detail)
        else:
            check("goose --target=go --include where.go", False, o + e)
        code, o, e = self.goose("--target=go", "--include", incdir / "shout.h", incsrc)
        check("--target=go refuses an --include that is not Go",
              code == 1 and "an --include file is Go source" in e, f"exit {code}\n{o}{e}")
        code, o, e = self.goose("--go-package", "geese", "-o", self.work / "cli.c", hello)
        check("--go-package needs --target=go", code == 1, o + e)
        # --go-build: the file starts with the build constraint, which go
        # build, building the file's package, honors (issue #20).
        bdir = self.work / "cli-gobuild"
        shutil.rmtree(bdir, ignore_errors=True)
        bdir.mkdir(parents=True)
        bfile = bdir / "main.go"
        code, o, e = self.goose("--target=go", "--go-build", "goose_x", "-o", bfile, hello)
        text = bfile.read_text(encoding="utf-8") if bfile.exists() else ""
        check("--go-build goose_x starts the file with //go:build goose_x",
              code == 0 and text.startswith("//go:build goose_x\n\n// Code generated by the Goose compiler"),
              o + e)
        if code == 0:
            diff = self.gofmt_clean(bfile)
            check("--go-build's file is laid out as gofmt lays it out", diff is None, diff or "")
            r = subprocess.run([self.go, "mod", "init", "goosebuild"], cwd=bdir, env=go_env(),
                               capture_output=True)
            check("go mod init for --go-build", r.returncode == 0, tc.decode(r.stderr))
            r = subprocess.run([self.go, "build", "-o", "untagged", "."], cwd=bdir, env=go_env(),
                               capture_output=True)
            check("go build skips a --go-build goose_x file without -tags goose_x",
                  r.returncode != 0 and "build constraints exclude all Go files" in tc.decode(r.stderr),
                  f"exit {r.returncode}\n{tc.decode(r.stdout)}{tc.decode(r.stderr)}")
            r = subprocess.run([self.go, "build", "-tags", "goose_x", "-o", "tagged", "."], cwd=bdir,
                               env=go_env(), capture_output=True)
            detail = f"exit {r.returncode}\n{tc.decode(r.stdout)}{tc.decode(r.stderr)}"
            ok = r.returncode == 0
            if ok:
                rc, ro, re_ = tc.run_capture([bdir / f"tagged{tc.EXE_SUFFIX}"])
                ok = rc == 0 and "hello, world" in ro
                detail = f"exit {rc}\n{ro}{re_}"
            check("go build -tags goose_x builds a --go-build goose_x file", ok, detail)
        # The expression as gofmt writes a //go:build line; one that is none
        # is refused, and so is --go-build where no Go file of its own is
        # written.
        code, o, e = self.goose("--target=go", "--go-build", "  linux&&amd64 ||!goose_x", "-o", bfile, hello)
        text = bfile.read_text(encoding="utf-8") if bfile.exists() else ""
        check("--go-build writes its expression as gofmt does",
              code == 0 and text.startswith("//go:build (linux && amd64) || !goose_x\n\n") and
              self.gofmt_clean(bfile) is None, o + e)
        for bad_expr, why in (("goose_x &&", "unexpected end of expression"), ("!!goose_x", "double negation"),
                              ("goose_x\npackage", "invalid syntax"), ("(a || b", "missing close paren")):
            code, o, e = self.goose("--target=go", "--go-build", bad_expr, "-o", bfile, hello)
            check(f"--go-build refuses {bad_expr!r}",
                  code == 1 and "--go-build takes a build constraint" in e and why in e, f"exit {code}\n{o}{e}")
        code, o, e = self.goose("--go-build", "goose_x", "-o", self.work / "cli.c", hello)
        check("--go-build needs --target=go", code == 1 and "are for --target=go" in e, o + e)
        code, o, e = self.goose("run", "--target=go", "--go-build", "goose_x", hello)
        check("goose run refuses --go-build", code == 1 and "--go-build constrains the Go that -o writes" in e,
              f"exit {code}\n{o}{e}")
        # goose run --target=go: the program's streams and exit code.
        code, o, e = self.goose("run", "--target=go", exitprog)
        check("goose run --target=go", code == 7 and o == "start\nexiting with 7\n" and not e,
              f"exit {code}\n{o}{e}")
        code, o, e = self.goose("--target=go", progs / "abort_msg.goose")
        check("goose --target=go file runs it", code == 1 and o == "3 42\n" and
              "goose runtime error: division by zero requested" in e, f"exit {code}\n{o}{e}")
        # The Go that run and build write is main.go, whatever the program is
        # called: the go tool takes *_test.go for a test and does not read a
        # name that starts with . or _, and the executable run builds beside
        # it is main, which main.go.goose's name would otherwise give the Go.
        names = self.work / "cli-names"
        shutil.rmtree(names, ignore_errors=True)
        names.mkdir(parents=True)
        for stem in ("scalar_test", "_under", ".dot", "main.go"):
            src = names / f"{stem}.goose"
            tc.write_text(src, 'fn main() { print("hello"); }\n')
            code, o, e = self.goose("run", "--target=go", src)
            check(f"goose run --target=go {stem}.goose", code == 0 and o == "hello\n",
                  f"exit {code}\n{o}{e}")
            exe = names / f"{stem}-built{tc.EXE_SUFFIX}"
            code, o, e = self.goose("build", "--target=go", "-o", exe, src)
            ok = code == 0 and exe.exists()
            if ok:
                rc, ro, re_ = tc.run_capture([exe])
                ok = rc == 0 and ro == "hello\n"
            check(f"goose build --target=go {stem}.goose", ok, o + e)
        # goose build --target=go -o exe.
        exe = self.work / f"cli-built{tc.EXE_SUFFIX}"
        exe.unlink(missing_ok=True)
        code, o, e = self.goose("build", "--target=go", "-o", exe, hello)
        ok = code == 0 and exe.exists()
        if ok:
            rc, ro, re_ = tc.run_capture([exe])
            ok = rc == 0 and ro.startswith("\n\nhello, world\n")
        check("goose build --target=go", ok, o + e)
        # Where the runtime comes from (issue #19): --go-runtime=full writes
        # every one of its declarations, which, every name unexported too,
        # builds and runs as the part a program uses does.
        for flags in (["--go-runtime=full"], ["--go-runtime=full", "--go-unexported"]):
            gofile = self.work / f"cli-{'-'.join(f.strip('-').replace('=', '-') for f in flags)}.go"
            code, o, e = self.goose("--target=go", *flags, "-o", gofile, hello)
            text = gofile.read_text(encoding="utf-8") if code == 0 else ""
            unexp = "--go-unexported" in flags
            names = ["gsSpansAlloc", "gsOsReadFile", "gsQueue", "newStackBlock"] if unexp else \
                ["GsSpansAlloc", "GsOsReadFile", "GsQueue", "NewStackBlock", "gsEscape", "gsProgram"]
            check(f"{' '.join(flags)} writes the whole runtime", code == 0 and all(
                re.search(rf"\n(func|type) (\([^)]*\) )?{n}\b", text) for n in names), o + e)
            if code == 0:
                vc, vo, ve = self.gotool("vet", gofile.name)
                check(f"{' '.join(flags)} vets", vc == 0 and not (vo + ve).strip(), vo + ve)
                diff = self.gofmt_clean(gofile)
                check(f"{' '.join(flags)} is laid out as gofmt lays it out", diff is None, diff or "")
                exe = self.work / f"{gofile.stem}{tc.EXE_SUFFIX}"
                bc, bo, be = self.gotool("build", "-o", exe.name, gofile.name)
                rc, ro, _ = tc.run_capture([exe]) if bc == 0 else (bc, bo + be, "")
                check(f"{' '.join(flags)} runs", rc == 0 and ro.startswith("\n\nhello, world\n"), ro)
        # A file with every runtime name unexported has none Go exports but
        # the entry points, and the block type they name (whose functions
        # leave no block to make).
        code, o, e = self.goose("--target=go", "--go-runtime=full", "--go-unexported", "--library",
                                "-o", self.work / "cli-unexported-lib.go", self.work / "cli-exports.goose")
        text = (self.work / "cli-unexported-lib.go").read_text(encoding="utf-8") if code == 0 else ""
        exported = sorted(set(re.findall(r"\n(?:func|type|var|const) ([A-Z]\w*)", text)))
        check("--go-unexported leaves the entry points the file's only exported names",
              code == 0 and exported == ["ParseCount", "ParseCountBytes", "ParseCountString", "ParseCountWithStack",
                                         "StackBlock", "Twice", "TwiceWithStack"],
              f"{exported}\n{o}{e}")
        # The options' checks.
        for args, why in ((["--target=go", "--go-runtime=static", hello], "--go-runtime takes embed, import or full"),
                          (["--go-runtime=import", "-o", self.work / "cli-c.c", hello], "are for --target=go"),
                          (["--go-unexported", "-o", self.work / "cli-c.c", hello], "are for --target=go"),
                          (["run", "--target=go", "--go-runtime=import", hello], "--go-runtime=import and"),
                          (["build", "--target=go", "--go-runtime-file", self.work / "rt.go", hello],
                           "--go-runtime-file are for the Go that -o writes"),
                          (["--target=go", "--go-runtime-file", self.work / "rt.c", "-o", self.work / "p.go", hello],
                           "--go-runtime-file takes a Go source file"),
                          (["--target=go", "--go-runtime-file", self.work / "elsewhere" / "rt.go", "-o",
                            self.work / "p.go", hello], "is in another directory than"),
                          (["--target=go", "--go-runtime-file", self.work / "cli-same.go", "-o",
                            self.work / "cli-same.go", hello], "names the program's own file")):
            code, o, e = self.goose(*args)
            check(f"{' '.join(str(a) for a in args if not isinstance(a, Path))} is refused",
                  code == 1 and why in e, f"exit {code}\n{o}{e}")
        # A foreign function cannot be named as the runtime's names are in a
        # file with --go-unexported.
        src = self.work / "cli-runtime-name.goose"
        tc.write_text(src, 'extern fn mk() -> i64 = "newStackBlock";\nfn main() { print(mk()); }\n')
        code, o, e = self.goose("--target=go", "-o", self.work / "cli-runtime-name.go", src)
        check("an extern fn may not call a Go function the runtime names",
              code == 1 and "the Go name newStackBlock is the Go runtime's" in e, f"exit {code}\n{o}{e}")
        return out


def main():
    tc.setup_console()
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--exe", help="the goose compiler (default: under build/)")
    ap.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--sweep", action="store_true",
                    help="also run the C suite's programs the Go backend compiles")
    ap.add_argument("--fuzz", type=int, default=0, metavar="N",
                    help="also run N random programs through both backends")
    ap.add_argument("--fuzz-flow", type=int, default=0, metavar="N",
                    help="also run N random programs of control flow through both backends")
    ap.add_argument("--fuzz-values", type=int, default=0, metavar="N",
                    help="also run N random programs of fixed-size values through both backends")
    ap.add_argument("--fuzz-seed", type=int, default=0,
                    help="the first --fuzz, --fuzz-flow and --fuzz-values program's seed")
    ap.add_argument("--bless", action="store_true",
                    help="write test/go/expected/ from the C backend's runs")
    ap.add_argument("--jit-reference", action="store_true",
                    help="run the C reference in the compiler's process (TinyCC)")
    ap.add_argument("--workdir", help="where the generated files go (default build/go-tests)")
    ap.add_argument("--release-dist", metavar="DIR",
                    help="the release section runs the compiler of the release in DIR, dist/<version>/ as "
                         "scripts/release.py build writes it, of the compiler under test's version, in place "
                         "of the compiler under test packaged")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("names", nargs="*",
                    help="run only these programs of test/go/programs and test/go/build_only")
    args = ap.parse_args()
    r = Runner(args)
    if not r.go:
        print("FAIL: no Go toolchain: put `go` on PATH, or set GOOSE_GO")
        return 2
    r.gofmt = r.find_gofmt()
    if not r.gofmt:
        print("FAIL: no gofmt beside the Go toolchain, which checks the layout of every file")
        return 2
    _, gover, _ = r.gotool("version")
    print(f"goose: {r.exe}")
    print(f"go: {gover.strip()}")
    print(f"C reference: {r.cc.desc if r.cc else 'TinyCC (goose --jit)'}")
    programs = sorted((HERE / "go" / "programs").glob("*.goose"))
    build_only = sorted((HERE / "go" / "build_only").glob("*.goose"))
    # The C suite's fixtures of the stdlib and of the threads (issue #12),
    # sections of their own (Runner.fixture).
    stdlib = sorted((UPSTREAM_TEST / "stdlib").glob("*.goose")) + sorted((UPSTREAM_TEST / "threads").glob("*.goose"))
    samples = parity_samples()
    exports = sorted(p for p in (HERE / "go" / "export").iterdir() if p.is_dir())
    bcegaps = sorted((HERE / "go" / "bce_gaps").glob("*.goose"))
    warnings = sorted((HERE / "go" / "warnings").glob("*.goose"))
    # The runtime's sections (issue #19): its package, a library of kernels,
    # one compiled with --go-unexported, and two programs in one package;
    # what makes a generated file reproducible (issue #21); and the
    # prebuilt compilers that `go run` runs (issue #29).
    runtime_sections = {"runtime": r.runtime_package, "kernels": r.kernels, "unexported": r.unexported_library,
                        "shared": r.shared, "reproducible": r.reproducible, "release": r.release}
    if args.names:
        runtime_sections = {k: v for k, v in runtime_sections.items() if k in args.names}
        programs = [p for p in programs if p.stem in args.names]
        build_only = [p for p in build_only if p.stem in args.names]
        stdlib = [p for p in stdlib if p.stem in args.names]
        exports = [p for p in exports if p.name in args.names]
        samples = [p for p in samples if p.stem in args.names]
        bcegaps = [p for p in bcegaps if p.stem in args.names or "bce_gaps" in args.names]
        warnings = [p for p in warnings if p.stem in args.names or "warnings" in args.names]
        if not programs and not build_only and not stdlib and not exports and not samples and \
                not bcegaps and not warnings and not runtime_sections:
            ap.error(f"no program named {' '.join(args.names)} in test/go/programs, "
                     "test/go/build_only, test/stdlib, test/threads, test/go/export, "
                     "test/go/bce_gaps, test/go/warnings or samples, and no section runtime, kernels, "
                     "unexported, shared, reproducible or release")
    if args.bless:
        # Their expected output is the C suite's (test/expected/), a
        # sample's its own, an export library's its Go tests, and a BCE
        # gap's and a warnings program's their annotations.
        stdlib, exports, samples, bcegaps, warnings = [], [], [], [], []
        runtime_sections = {}
    unsupported = [] if args.names else sorted((HERE / "go" / "unsupported").glob("*.goose"))
    sweep = []
    if args.sweep:
        # test/stdlib/ and test/threads/ have sections of their own
        # (Runner.fixture).
        skipdirs = {"errors", "errors_tc", "goose_in_goose", "lsp", "go", "packages", "gfx",
                    "physics", "ui", "audio", "stdlib", "threads"}
        sweep = [f for f in sorted(UPSTREAM_TEST.glob("*/*.goose"))
                 if f.parent.name not in skipdirs and f.name != "lexer_tokens.goose" and
                 not error_markers(f) and "parse-only" not in first_line(f)]
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        futures = [pool.submit(r.program, p) for p in programs]
        futures += [pool.submit(r.build_only, p) for p in build_only]
        futures += [pool.submit(r.unsupported, p) for p in unsupported]
        futures += [pool.submit(r.warnings, p) for p in warnings]
        futures += [pool.submit(r.fixture, p) for p in stdlib]
        futures += [pool.submit(r.sample, p) for p in samples]
        futures += [pool.submit(r.export, p) for p in exports]
        futures += [pool.submit(f) for f in runtime_sections.values()]
        # The code Go makes of the wide loads and stores (issue #23) and of
        # the selects (issue #26), beside their export libraries' own tests.
        if not args.bless and (not args.names or "wide" in args.names):
            futures.append(pool.submit(r.wide_asm))
        if not args.bless and (not args.names or "selects" in args.names):
            futures.append(pool.submit(r.select_asm))
        # The code Go makes of bounds checks (issue #40), beside their export
        # library's own tests.
        if not args.bless and (not args.names or "bounds" in args.names):
            futures.append(pool.submit(r.spill_asm))
        # The frames of locals handed to inlined bodies by reference (issue
        # #24), beside their export library's own tests.
        if not args.bless and (not args.names or "inline_refs" in args.names):
            futures.append(pool.submit(r.frames))
        # The helpers inlined into recursive cycles (issue #42), beside
        # their program's and their export library's own tests.
        if not args.bless and (not args.names or {"cycle_inlining", "string_slices"} & set(args.names)):
            futures.append(pool.submit(r.cycles))
        # What Go makes of the entry points of pure kernels (issue #22),
        # beside their export library's own tests.
        if not args.bless and (not args.names or "inlining" in args.names):
            futures.append(pool.submit(r.entry_points))
        # A library whose globals are in program instances without workers
        # (issue #25), beside its export library's own tests: no workers'
        # runtime, and race-clean.
        if not args.bless and (not args.names or "arena" in args.names):
            futures.append(pool.submit(r.instances))
        futures += [pool.submit(r.bce_gaps, p) for p in bcegaps]
        futures += [pool.submit(r.overflow, name) for name in ([] if args.names else OVERFLOW_CASES)]
        futures += [pool.submit(r.fixture, p, True) for p in sweep]
        futures += [pool.submit(r.fuzz, seed)
                    for seed in range(args.fuzz_seed, args.fuzz_seed + args.fuzz)]
        futures += [pool.submit(r.fuzz_flow, seed)
                    for seed in range(args.fuzz_seed, args.fuzz_seed + args.fuzz_flow)]
        futures += [pool.submit(r.fuzz_values, seed)
                    for seed in range(args.fuzz_seed, args.fuzz_seed + args.fuzz_values)]
        for f in futures:
            r.report(f.result())
    # The command line.
    if not args.names and not args.bless:
        r.report(r.cli())
    print(f"{r.oks} ok, {len(r.failures)} failed" +
          (f", {len(r.skips)} not supported by --target=go yet" if r.skips else "") +
          (f", {len(r.known)} known differences" if r.known else ""))
    if r.failures:
        print("failures:")
        for f in r.failures:
            print(f"  {f}")
    return 1 if r.failures else 0


if __name__ == "__main__":
    sys.exit(main())
