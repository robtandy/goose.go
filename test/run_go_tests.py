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

Data stack overflow is checked on programs generated for it: a print whose
line, at --stack-reserve 64K, runs past its stack by a literal or inside a
value's text, or just fits, must run in Go as it does in C.

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
must run alike. --fuzz-flow N generates N programs of random control flow,
whose returns, breaks, continues and guards end it in every position Go's
flow analysis follows (conditions, arguments, loop bounds, values), with
code after them that is never reached: their Go must vet and build, and run
as their C does at both levels, with the same --stacks report. --bless writes test/go/expected/ from the C backend's runs
instead of comparing with it.

  python3 test/run_go_tests.py [--exe build/goose] [-j N] [--sweep] [--fuzz N]
                               [--fuzz-flow N] [--bless] [-v] [name...]
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
FLOW_TIMEOUT = 60   # Seconds a --fuzz-flow program may run.


def run_capture(argv, cwd=None, timeout=None):
    """tc.run_capture, or with a limit on the time it takes: what runs on
    past it is stopped, and the result is (None, "", "")."""
    if timeout is None:
        return tc.run_capture(argv, cwd=cwd)
    try:
        r = subprocess.run([str(a) for a in argv], cwd=cwd, capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None, "", ""
    return r.returncode, tc.decode(r.stdout), tc.decode(r.stderr)


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

    def goose(self, *args, timeout=None):
        """The compiler, run from the repository's root, where a source named
        relative to it is named as the messages of what it compiles say: the
        same on every machine."""
        args = [a.relative_to(REPO).as_posix() if isinstance(a, Path) and a.is_relative_to(REPO)
                and a.suffix == ".goose" else str(a) for a in args]
        return run_capture([self.exe] + args, cwd=REPO, timeout=timeout)

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

    def c_run(self, src, name, flags, out, timeout=None):
        """The C backend's run of src: (exit code, stdout, stderr), or None
        after saying why. The exit code is None where it ran past timeout."""
        if self.cc is None:
            # TinyCC in the compiler's process: what the compile itself
            # prints to stderr (its warnings) comes before the program's.
            code, o, e = self.goose(*flags, "--jit", src, timeout=timeout)
            _, _, warnings = self.goose(*flags, "--check", src)
            if warnings and e.startswith(warnings):
                e = e[len(warnings):]
            return code, o, e
        exe = self.c_build(src, name, flags, out)
        return None if exe is None else run_capture([exe], timeout=timeout)

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

    def fuzz_flow(self, seed):
        """A random program of control flow (FlowProgram), which both
        backends must run alike at both levels, with the same stacks."""
        out = []
        name = f"flow-{seed}"
        src = self.work / f"{name}.goose"
        tc.write_text(src, flow_program(seed))
        ref = self.c_run(src, name, [], out, timeout=FLOW_TIMEOUT)
        if ref is None:
            return out
        if ref[0] is None:
            out.append(("fail", f"fuzz-flow seed {seed}: the C run did not end in {FLOW_TIMEOUT}s "
                        f"({src})", None))
            return out
        for level in ("2", "0"):
            exe = self.go_program(src, f"{name}-O{level}", [f"-O{level}"], out)
            if exe is None:
                continue
            got = run_capture([exe], timeout=FLOW_TIMEOUT)
            if got != ref:
                out.append(("fail", f"fuzz-flow seed {seed} -O{level} ({src})",
                            f"--- go (exit {got[0]}):\n{got[1][-2000:]}{got[2]}\n"
                            f"--- c (exit {ref[0]}):\n{ref[1][-2000:]}{ref[2]}"))
            else:
                out.append(("ok", f"fuzz-flow seed {seed} -O{level}", None))
        self.stacks(src, name, [], out)
        return out

    def overflow(self, name):
        """A print whose line may run past its stack's region (OVERFLOW_CASES):
        the C runtime's guard gap finds it, and the Go must report it alike,
        with the same text and status, and print what fits as C does."""
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
        exe = self.go_program(src, f"overflow-{name}", flags, out)
        if exe is None:
            return out
        got = tc.run_capture([exe])
        if got != ref:
            out.append(("fail", f"overflow {name}", f"--- go (exit {got[0]}):\n{got[2]}\n"
                        f"--- c (exit {ref[0]}):\n{ref[2]}"))
        else:
            out.append(("ok", f"overflow {name}", None))
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
        # A Go program that imports a library: the package's init
        # initializes the globals, and the library's fn main() never runs.
        host = self.work / "cli-host"
        shutil.rmtree(host, ignore_errors=True)
        (host / "goose").mkdir(parents=True)
        libsrc = self.work / "cli-library.goose"
        tc.write_text(libsrc, 'fn noisy() -> i64 {\n    print("library global initialized");\n    7\n}\n'
                              'let answer = noisy();\n'
                              'fn main() {\n    print("library main ran: ", answer);\n    exit(3);\n}\n')
        tc.write_text(host / "main.go", 'package main\n\nimport (\n\t"fmt"\n\n\t_ "gooshost/goose"\n)\n\n'
                                        'func main() { fmt.Println("host main ran") }\n')
        code, o, e = self.goose("--target=go", "--library", "-o", host / "goose" / "goose.go", libsrc)
        detail = o + e
        ok = False
        if code == 0:
            # The module's go line is the toolchain's own.
            r = subprocess.run([self.go, "mod", "init", "gooshost"], cwd=host, env=go_env(),
                               capture_output=True)
            if r.returncode == 0:
                r = subprocess.run([self.go, "run", "."], cwd=host, env=go_env(), capture_output=True)
            detail = f"exit {r.returncode}\n{tc.decode(r.stdout)}{tc.decode(r.stderr)}"
            ok = r.returncode == 0 and \
                tc.decode(r.stdout) == "library global initialized\nhost main ran\n"
        check("a Go program imports a --library package, whose init runs no fn main", ok, detail)
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
    ap.add_argument("--fuzz-flow", type=int, default=0, metavar="N",
                    help="also run N random programs of control flow through both backends")
    ap.add_argument("--fuzz-seed", type=int, default=0,
                    help="the first --fuzz and --fuzz-flow program's seed")
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
        futures += [pool.submit(r.overflow, name) for name in ([] if args.names else OVERFLOW_CASES)]
        futures += [pool.submit(r.sweep, p) for p in sweep]
        futures += [pool.submit(r.fuzz, seed)
                    for seed in range(args.fuzz_seed, args.fuzz_seed + args.fuzz)]
        futures += [pool.submit(r.fuzz_flow, seed)
                    for seed in range(args.fuzz_seed, args.fuzz_seed + args.fuzz_flow)]
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
