#!/usr/bin/env python3
"""Go backend test runner (--target=go).

Every program of the suite, test/go/programs/*.goose (the modules they
import are in programs/modules/), is

1. compiled to Go with `goose --target=go -o <name>.go`, with the flags its
   first line names (`// goose-flags: --debug`), at -O2 and at -O0, where
   nothing is inlined;
2. checked with `go vet`, which must find nothing;
3. built with `go build` and run;
4. compiled with the C backend as well, with the same flags (`goose -o
   <name>.c --standalone` and the C compiler scripts/toolchain.py finds, or
   TinyCC in the compiler's own process where there is none), and run;
5. judged: the two runs must agree on stdout, stderr and the exit code, and
   match what test/go/expected/ holds for the program (<name>.out, and
   <name>.stderr and <name>.exit where it writes to stderr or does not exit
   with 0);
6. reported on with `goose --check --stacks` for both targets, at both
   levels, which must agree: both backends count data stacks with the same
   analysis (BoundStacks, src/codegen_stacks.h).

The programs of test/go/build_only/ never end (a loop that only stopping
them leaves): each is compiled to Go at both levels, vetted and built, and
with the C backend, as above, and reported on with --stacks, but neither is
run.

The programs of test/go/unsupported/ use what this version of the Go backend
does not compile yet. Each must stop the compile with every `// error:`
message its source names, where the C backend compiles it.

A few checks of the command line follow: the target -o out.go implies, `goose
run --target=go` and `goose build --target=go`, --go-package and --library.

--sweep also runs the C suite's own programs (test/<category>/*.goose)
through the Go backend: the ones it compiles must vet, build, and print what
test/expected/ holds for them, as run_tests.py judges a C run; one it
refuses as not supported yet is counted, not failed. --fuzz N generates N
programs of random scalar arithmetic, conversions and comparisons (from
--fuzz-seed on, every other one built with --debug), which both backends
must run alike. --bless writes test/go/expected/ from the C backend's runs
instead of comparing with it.

  python3 test/run_go_tests.py [--exe build/goose] [-j N] [--sweep] [--fuzz N] [--bless] [-v]
                               [name...]
"""

import argparse
import concurrent.futures
import os
import random
import re
import shutil
import subprocess
import sys
import threading
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(REPO / "scripts"))
import toolchain as tc

UNSUPPORTED = "is not supported by --target=go yet"


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


def go_env():
    """The Go tool's environment: the module and build flags of whoever runs
    the suite play no part in building one generated file."""
    env = dict(os.environ)
    env["GOFLAGS"] = ""
    env["GO111MODULE"] = "auto"
    return env


class Runner:
    def __init__(self, args):
        self.exe = tc.find_goose(args.exe)
        self.go = os.environ.get("GOOSE_GO") or shutil.which("go")
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

    # --- processes -----------------------------------------------------------

    def goose(self, *args):
        """The compiler, run from the repository's root, where a source named
        relative to it is named as the messages of what it compiles say: the
        same on every machine."""
        args = [a.relative_to(REPO).as_posix() if isinstance(a, Path) and a.is_relative_to(REPO)
                and a.suffix == ".goose" else str(a) for a in args]
        return tc.run_capture([self.exe] + args, cwd=REPO)

    def gotool(self, *args):
        r = subprocess.run([self.go] + [str(a) for a in args], cwd=self.work, env=go_env(),
                           capture_output=True)
        return r.returncode, tc.decode(r.stdout), tc.decode(r.stderr)

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
                else:
                    self.failures.append(what)
                    print(f"FAIL {what}")
                    if detail:
                        print("    " + detail.rstrip().replace("\n", "\n    "))
            sys.stdout.flush()

    # --- the steps -----------------------------------------------------------

    def go_program(self, src, name, flags, out):
        """Compiles src to Go, vets and builds it: the executable, or None
        after saying why in `out`."""
        gofile = self.work / f"{name}.go"
        exe = self.work / f"{name}-go{tc.EXE_SUFFIX}"
        gofile.unlink(missing_ok=True)
        code, o, e = self.goose(*flags, "--target=go", "-o", gofile, src)
        if code != 0:
            out.append(("fail", f"goose --target=go {name}", o + e))
            return None
        code, o, e = self.gotool("vet", gofile.name)
        if code != 0 or (o + e).strip():
            out.append(("fail", f"go vet {name}", o + e))
            return None
        code, o, e = self.gotool("build", "-o", exe.name, gofile.name)
        if code != 0:
            out.append(("fail", f"go build {name}", o + e))
            return None
        return exe

    def c_run(self, src, name, flags, out):
        """The C backend's run of src: (exit code, stdout, stderr), or None
        after saying why."""
        if self.cc is None:
            # TinyCC in the compiler's process: what the compile itself
            # prints to stderr (its warnings) comes before the program's.
            code, o, e = self.goose(*flags, "--jit", src)
            _, _, warnings = self.goose(*flags, "--check", src)
            if warnings and e.startswith(warnings):
                e = e[len(warnings):]
            return code, o, e
        exe = self.c_build(src, name, flags, out)
        return None if exe is None else tc.run_capture([exe])

    def c_build(self, src, name, flags, out):
        """The C backend's executable for src, built with the C compiler at
        -O2, or None after saying why."""
        cfile = self.work / f"{name}.c"
        exe = self.work / f"{name}-c{tc.EXE_SUFFIX}"
        code, o, e = self.goose(*flags, "-O2", "--standalone", "-o", cfile, src)
        if code != 0:
            out.append(("fail", f"goose -o {name}.c", o + e))
            return None
        ok, log = self.cc.compile(cfile, exe, opt=1, log=self.work / f"{name}.cc.log")
        if not ok:
            out.append(("fail", f"cc {name}.c", "\n".join(log.splitlines()[:8])))
            return None
        return exe

    def stacks(self, src, name, flags, out):
        """The data stack counts, from the analysis both backends share,
        which must agree at both levels."""
        for level in ("2", "0"):
            c = self.goose(*flags, f"-O{level}", "--check", "--stacks", src)
            g = self.goose(*flags, f"-O{level}", "--check", "--stacks", "--target=go", src)
            if c != g or c[0] != 0:
                out.append(("fail", f"--stacks {name} -O{level}",
                            f"--- go:\n{g[1]}{g[2]}\n--- c:\n{c[1]}{c[2]}"))
            else:
                out.append(("ok", f"--stacks {name} -O{level}", None))

    def program(self, src):
        name = src.stem
        flags = goose_flags(src)
        out = []
        ref = self.c_run(src, name, flags, out)
        # At -O2 as the C reference is, and at -O0, where nothing is inlined.
        for level in ("2", "0"):
            label = f"{name} -O{level}"
            exe = self.go_program(src, f"{name}-O{level}", flags + [f"-O{level}"], out)
            if exe is None or ref is None:
                continue
            got = tc.run_capture([exe])
            same = True
            for what, g, c in (("exit code", got[0], ref[0]), ("stdout", got[1], ref[1]),
                               ("stderr", got[2], ref[2])):
                if g != c:
                    same = False
                    out.append(("fail", f"parity {label}: {what}",
                                f"--- go:\n{g}\n--- c:\n{c}" if what != "exit code"
                                else f"go {g}, c {c}"))
            if same:
                out.append(("ok", f"run {label}", None))
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
        """A random program, which both backends must run alike."""
        out = []
        name = f"fuzz-{seed}"
        src = self.work / f"{name}.goose"
        tc.write_text(src, fuzz_program(seed))
        flags = ["--debug"] if seed % 2 else []
        exe = self.go_program(src, name, flags, out)
        ref = self.c_run(src, name, flags, out)
        if exe is None or ref is None:
            return out
        got = tc.run_capture([exe])
        if got != ref:
            out.append(("fail", f"fuzz seed {seed}{' --debug' if flags else ''} ({src})",
                        f"--- go (exit {got[0]}):\n{got[1][-2000:]}{got[2]}\n"
                        f"--- c (exit {ref[0]}):\n{ref[1][-2000:]}{ref[2]}"))
        else:
            out.append(("ok", f"fuzz seed {seed}", None))
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

    def sweep(self, src):
        """A C suite fixture: skipped where the Go backend refuses it, judged
        by its blessed output otherwise."""
        out = []
        name = f"sweep-{src.parent.name}-{src.stem}"
        flags = runtime_defines(src)
        gofile = self.work / f"{name}.go"
        code, o, e = self.goose(*flags, "--target=go", "-o", gofile, src)
        if code != 0:
            if UNSUPPORTED in e:
                out.append(("skip", f"sweep {src.parent.name}/{src.name}", None))
            else:
                out.append(("fail", f"sweep {src.parent.name}/{src.name}: compile", o + e))
            return out
        exe = self.go_program(src, name, flags, out)
        if exe is None:
            return out
        code, o, e = tc.run_capture([exe])
        exp = HERE / "expected"
        aborts = (exp / f"{src.stem}.aborts").exists()
        bad = []
        if aborts != (code != 0):
            bad.append(f"exit {code}")
        stderr_file = exp / f"{src.stem}.stderr"
        if stderr_file.exists():
            markers = [m.strip() for m in tc.decode(stderr_file.read_bytes()).splitlines()
                       if m.strip()]
            if any(m not in e for m in markers):
                bad.append("stderr")
        outfile = exp / f"{src.stem}.out"
        if outfile.exists() and o.rstrip("\n") != tc.decode(outfile.read_bytes()).rstrip("\n"):
            bad.append("stdout")
        if bad:
            out.append(("fail", f"sweep {src.parent.name}/{src.name}: {', '.join(bad)}",
                        f"--- stdout:\n{o}\n--- stderr:\n{e}"))
        else:
            out.append(("ok", f"sweep {src.parent.name}/{src.name}", None))
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
        check("--library", code == 0 and "\npackage goose\n" in text and
              "\nfunc main()" not in text and "\nfunc init()" in text, o + e)
        if code == 0:
            vc, vo, ve = self.gotool("vet", gofile.name)
            check("--library vets", vc == 0 and not (vo + ve).strip(), vo + ve)
        code, o, e = self.goose("--target=rust", hello)
        check("--target=rust is refused", code == 1 and "--target takes c or go" in e, o + e)
        code, o, e = self.goose("--go-package", "geese", "-o", self.work / "cli.c", hello)
        check("--go-package needs --target=go", code == 1, o + e)
        # goose run --target=go: the program's streams and exit code.
        code, o, e = self.goose("run", "--target=go", exitprog)
        check("goose run --target=go", code == 7 and o == "start\nexiting with 7\n" and not e,
              f"exit {code}\n{o}{e}")
        code, o, e = self.goose("--target=go", progs / "abort_msg.goose")
        check("goose --target=go file runs it", code == 1 and o == "3 42\n" and
              "goose runtime error: division by zero requested" in e, f"exit {code}\n{o}{e}")
        # goose build --target=go -o exe.
        exe = self.work / f"cli-built{tc.EXE_SUFFIX}"
        exe.unlink(missing_ok=True)
        code, o, e = self.goose("build", "--target=go", "-o", exe, hello)
        ok = code == 0 and exe.exists()
        if ok:
            rc, ro, re_ = tc.run_capture([exe])
            ok = rc == 0 and ro.startswith("\n\nhello, world\n")
        check("goose build --target=go", ok, o + e)
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
    ap.add_argument("--fuzz-seed", type=int, default=0, help="the first --fuzz program's seed")
    ap.add_argument("--bless", action="store_true",
                    help="write test/go/expected/ from the C backend's runs")
    ap.add_argument("--jit-reference", action="store_true",
                    help="run the C reference in the compiler's process (TinyCC)")
    ap.add_argument("--workdir", help="where the generated files go (default build/go-tests)")
    ap.add_argument("-v", "--verbose", action="store_true")
    ap.add_argument("names", nargs="*",
                    help="run only these programs of test/go/programs and test/go/build_only")
    args = ap.parse_args()
    r = Runner(args)
    if not r.go:
        print("FAIL: no Go toolchain: put `go` on PATH, or set GOOSE_GO")
        return 2
    _, gover, _ = r.gotool("version")
    print(f"goose: {r.exe}")
    print(f"go: {gover.strip()}")
    print(f"C reference: {r.cc.desc if r.cc else 'TinyCC (goose --jit)'}")
    programs = sorted((HERE / "go" / "programs").glob("*.goose"))
    build_only = sorted((HERE / "go" / "build_only").glob("*.goose"))
    if args.names:
        programs = [p for p in programs if p.stem in args.names]
        build_only = [p for p in build_only if p.stem in args.names]
        if not programs and not build_only:
            ap.error(f"no program named {' '.join(args.names)} in test/go/programs or "
                     "test/go/build_only")
    unsupported = [] if args.names else sorted((HERE / "go" / "unsupported").glob("*.goose"))
    sweep = []
    if args.sweep:
        skipdirs = {"errors", "errors_tc", "goose_in_goose", "lsp", "go", "packages", "gfx",
                    "physics", "ui", "audio"}
        sweep = [f for f in sorted(HERE.glob("*/*.goose"))
                 if f.parent.name not in skipdirs and f.name != "lexer_tokens.goose" and
                 not error_markers(f) and "parse-only" not in first_line(f)]
    with concurrent.futures.ThreadPoolExecutor(max(1, args.jobs)) as pool:
        futures = [pool.submit(r.program, p) for p in programs]
        futures += [pool.submit(r.build_only, p) for p in build_only]
        futures += [pool.submit(r.unsupported, p) for p in unsupported]
        futures += [pool.submit(r.sweep, p) for p in sweep]
        futures += [pool.submit(r.fuzz, seed)
                    for seed in range(args.fuzz_seed, args.fuzz_seed + args.fuzz)]
        for f in futures:
            r.report(f.result())
    # The command line.
    if not args.names and not args.bless:
        r.report(r.cli())
    print(f"{r.oks} ok, {len(r.failures)} failed" +
          (f", {len(r.skips)} not supported by --target=go yet" if r.skips else ""))
    if r.failures:
        print("failures:")
        for f in r.failures:
            print(f"  {f}")
    return 1 if r.failures else 0


if __name__ == "__main__":
    sys.exit(main())
