# Proposal: mark std's choices of scalars `checkfree`

Status: a draft for upstream Goose ([aardappel/goose](https://github.com/aardappel/goose)),
**not filed**: goose.go's maintainer files it. It comes from goose.go's issue #62,
and is written against upstream at `2ed1a63d`, the `vendor/goose` this
repository builds with. Until upstream has it, goose.go's `--target=go` makes
the change for itself (§4).

## 1. The problem

A `checkfree fn` may call only functions that are `checkfree` themselves,
builtins and `extern fn`s (§7.14, "What it calls"), and the checker refuses
anything else (`TypeCheck::CheckfreeCallee`, `src/typecheck_calls.h`). std's
`min`, `max`, `clamp`, `abs` and `sign` are not marked, so a kernel that
states its preconditions with `assert`s and keeps its loops free of bounds
checks cannot take a maximum:

```goose
import std;

checkfree fn best(xs: const i64[:]) -> i64 {
    var m = 0;
    for x in xs { m = max(m, x); }
    m
}
```

```
error: checkfree fn best calls max, which is not checkfree: the bounds checks
left in it would run inside best (§7.14)
```

The error is right by the rule, and says nothing true of `max`: it has no
bounds check to run, in any specialization. Writing the choice out by hand
(`if x > m { m = x; }`) is the way round it, in every such kernel.

## 2. Why the marks are safe

Each of these functions is comparisons, returns and literals, over values of
one type:

```goose
fn min<T>(a: T, b: T) -> T { if b < a { return b; } return a; }
fn max<T>(a: T, b: T) -> T { if a < b { return b; } return a; }
fn clamp<T>(x: T, lo: T, hi: T) -> T { ... }
fn abs<T>(x: T) -> T { if x < 0 { return -x; } return x; }   // and f32, f64
fn sign<T>(x: T) -> T { ... }                                // and f32, f64
```

Ordering exists on the numeric types only (§4.5), so every specialization of
the generic ones is of a number, and none indexes, slices or converts into an
index type: §7.14 counts nothing else, and no bounds check can be left in any
specialization or in any copy inlined into a caller. An `abs` of an integer
still fails at its type's least value in a debug build, which §7.14 allows
("the other aborts of §9.3 are still allowed"). `checkfree` "changes nothing
about what the program computes or how it is compiled": marking them changes
which programs compile, and nothing else. The array overloads (`min<T>(xs:
T[:])` and the rest), which index, stay as they are.

## 3. The change

In `stdlib/std.goose`, `checkfree` before each scalar overload:

```goose
checkfree fn min<T>(a: T, b: T) -> T { if b < a { return b; } return a; }
checkfree fn max<T>(a: T, b: T) -> T { if a < b { return b; } return a; }
checkfree fn clamp<T>(x: T, lo: T, hi: T) -> T { ... }
checkfree fn abs<T>(x: T) -> T { ... }
checkfree fn abs(x: f32) -> f32 { ... }
checkfree fn abs(x: f64) -> f64 { ... }
checkfree fn sign<T>(x: T) -> T { ... }
checkfree fn sign(x: f32) -> f32 { ... }
checkfree fn sign(x: f64) -> f64 { ... }
```

BCE then verifies each as it verifies any `checkfree fn`, which costs a
moment and finds nothing. A fixture of `test/optimizer/`: a `checkfree fn`
calling each of the five, inlined and at `-O0`, whose output and `--stacks`
report do not change; and `docs/stdlib.md` saying that a `checkfree fn` may
call them.

## 4. goose.go's interim

goose.go cannot change `vendor/goose`, so its `--target=go` marks the five for
itself: `GoCheckfreeChoices` (`src/codegen_go_select.h`), which `src/main.cpp`
calls before the program is checked, sets `SFunction::checkfree` on each
function of the standard library of those names whose parameters are numbers
or type parameters and whose body is comparisons, arithmetic, returns, ifs,
literals and names and nothing else -- the shape above, which a change to std
that put anything else in one would undo the mark of. BCE verifies them as
any `checkfree fn`. The C backend keeps upstream's rule, and refuses the
program of §1. `test/go/export/selects/` holds a `checkfree fn` calling `max`
and `min`, whose choices are still selects (`Runner.select_asm`).

Once upstream has the change, goose.go deletes `GoCheckfreeChoices` and its
call.
