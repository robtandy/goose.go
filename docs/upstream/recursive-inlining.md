# Proposal: inline into a recursive cycle what takes no data stack

Status: a draft for upstream Goose ([aardappel/goose](https://github.com/aardappel/goose)),
**not filed**: goose.go's maintainer files it. It comes from goose.go's issue #42,
and is written against upstream at `cec434a5` (v2026.3-140), the `vendor/goose`
this repository builds with. Until upstream has it, goose.go's `--target=go`
makes the change for itself (§5).

## 1. The problem

The optimizer inlines nothing into a function of a recursive cycle: a
`recursive fn`, or a member of its cycle. `Optimizer::TryInline`
(`src/optimize.h`):

```cpp
    // Never into a recursive cycle: the inlined body's locals would become
    // the cycle function's own, upsetting the §7.8 stack-assignment rule.
    if (curspec && (curspec->incycle || curspec->sf->isrec)) return nullptr;
```

So every call a recursive kernel makes stays a call: a leaf helper's -- a hash
function, a comparator, `min` -- and an `inline fn`'s too, which "passes the
size rules wherever inlining is on" everywhere else. The call costs what a
call costs at every step of the recursion, and the helper's body is optimized
and bounds-checked apart from what its caller knows:

```goose
inline fn fnv(s: const u8[:]) -> u64 {
    var h: u64 = 14695981039346656037;
    for c in s { h = (h ^ c) * 1099511628211; }
    h
}

fn combine(a: u64, b: u64) -> u64 { (a ^ (b >> 7)) * 1099511628211 + b }

// A string table's digest: each string's hash at the leaves.
recursive fn digest(text: const u8[:], ends: const i64[:], lo: i64, hi: i64) -> u64 {
    if hi - lo <= 1 {
        if hi == lo { return 0; }
        let from = if lo == 0 { 0 } else { ends[lo - 1] };
        return fnv(text[from..ends[lo]]);
    }
    let mid = lo + (hi - lo) / 2;
    combine(digest(text, ends, lo, mid), digest(text, ends, mid, hi))
}
```

`goose --check --specs` shows `fnv(...)` and `combine(...)` as calls in
`digest`, and the inline fn as a function of its own. The way out today is to
move the body into an ordinary function and keep the `recursive fn` as a thin
wrapper that calls it, which is what goose.go's issue #42 measured: a
string-table kernel ran 1.1--1.5x faster so.

## 2. What the rule protects, and what it needs

§7.8 lets no function of a cycle call into the cycle while a nonfixed local
is in scope, so that every activation starts its stacks at the same index and
a program's stack count is static (§10.3). A callee that is in no cycle
cannot call into one it is called from: a call reaching the cycle would have
joined it to it (`TypeCheck::JoinCycle`). So its locals, inlined, are in
scope at no call into the cycle either, and the checker's verdict on the
program stands. What inlining one could still change is where values live,
and so what each activation of the cycle holds:

* a nonfixed local or temporary of the callee takes a data stack, at the
  cycle function's watermark: the same index as through the call, but the
  stack is now the cycle function's own (`stackown`), and the callee's own
  line in `--stacks` goes where it goes dead;
* a fixed value of more than `NATIVE_VALUE_LIMIT` bytes is on a data stack in
  the callee but a native local in a cycle (§10.3: `LargeFixedOnStack`,
  `FixedLocal`), so the count itself changes, and so does the native frame of
  every activation.

A callee that takes no data stack at all -- whose code, and that of every
function it calls, holds no value but fixed-size ones of at most
`NATIVE_VALUE_LIMIT` bytes, builds no slice literal's array as large, adapts
no variable-mode ADT to the fixed-mode one (a call's result or a reference's
pointee, built as what it arrives as first), dispatches on no variable-mode
ADT to a case function taking its variant by value, and calls no builtin
that takes a stack (`print`, `str`, `format`, `to_bytes`, `from_bytes`, the
spawns, waits and queues) -- changes none of it. These are the places a
function takes `gs_sp` for (`CodeGen::CollectSpecs`; goose.go's
`CodeGenGo::CollectSpecs` follows the same rules), so every activation holds
the stacks it held, at the same indices, and the report is the same. Those
are exactly the helpers of §1.

## 3. The change

Classify each specialization once more in `Optimizer::Scan`, beside
`noinline`: `InlineInfo::nostack`, whether nothing a call of it runs takes a
data stack. Then:

```cpp
    // Into a recursive cycle only what takes no data stack: the inlined
    // body's locals become the cycle function's own, and a data stack among
    // them would upset the §7.8 stack-assignment rule.
    if (curspec && (curspec->incycle || curspec->sf->isrec) && !info.nostack) return nullptr;
```

A cycle member is never inlined anyway (`Scan` makes it `noinline`), so the
change admits the callees outside every cycle alone. Two notes:

* `BaseCaseInliner::BaseOK` asserts that a recursive body holds no
  `InlineBlock`, "nothing is ever inlined into one". The assertion still
  holds -- `Setup` reads the body before `OptBody` optimizes it -- but the
  comment needs the reason changed. The base case's copies at the self-calls
  are optimized as the call sites they stand at (`TryBaseCase`), so their
  helpers are inlined there too.
* `nostack` is the question the backend answers as it emits (`NeedsStack`,
  `AllocStk` and their callers); the optimizer has the types' size classes
  (`StructInst::sclass`, `EnumInst::varclass`), and the sizes as a bound of
  their parts: a scalar's, 16 for a slice or a reference, a fixed array's,
  struct's or ADT's from its fields and pads, which needs no layout. A bound
  that is too high only keeps a call. goose.go's `GoTakesNoStack` and
  `GoCycleValueBound` (`src/codegen_go_cycles.h`) are such a classification,
  and the builtins' list (`GoBuiltinTakesNoStack`) is the C backend's as much
  as Go's.

Nothing in the specification changes: §7.8 and §10.3 describe the program's
stacks, which stay as they are; the native frame of a cycle's function grows
by what its inlined callees hold, as any function's does.

## 4. Tests

A fixture of `test/optimizer/` that runs: recursive fns calling an inline fn,
a leaf with an early return, a used-once helper with a loop, a helper taking
a local by reference, a base case calling a helper, an accumulator the tail
pass makes a loop of, a cycle of two functions, a `recursive fn` that does
not recurse, and a nested recursive fn reading its parent's local; beside
them helpers that must stay calls -- one that prints, one that builds a
`str`, one with a 4800-byte local, one handing a 4800-byte slice literal
on, one reading a variable-mode ADT through a reference as the fixed-mode
one, one calling into another cycle. Its output must not change, nor its
`--stacks` report, since nothing inlined into a cycle takes a stack;
`--specs` shows which calls are left. goose.go's
`test/go/programs/cycle_inlining.goose` is that fixture, with its expected
output in `test/go/expected/`; its `// inlined:` and `// calls:` lines say
which calls the Go must have left (`run_go_tests.py`'s `Runner.cycles`).

## 5. goose.go's interim

goose.go cannot change `vendor/goose`, so its `--target=go` inlines into the
cycles after the optimizer is done: `InlineIntoCycles`
(`src/codegen_go_cycles.h`), which `src/main.cpp` calls right after the
optimizer, before BCE. It walks each function of a cycle with the
optimizer's own walk again (`OptBlock`), the rule lifted for that function
alone, every callee that may take a data stack marked `noinline` for the
walk and the base-case template set aside (it was applied already), then
recomputes liveness as the optimizer's driver does. The C backend keeps
upstream's rule, and both report the same stacks for every program, which
`run_go_tests.py` checks (`Runner.stacks`) over the suite and the C suite's
fixtures (`--sweep`).

Once upstream has the change, goose.go deletes `src/codegen_go_cycles.h`
and its call.
