# Upstream note: a cast that widens a shift's left operand, and which cast a redundant-cast warning means

Status: a draft for upstream Goose ([aardappel/goose](https://github.com/aardappel/goose)),
**not filed**. goose.go's maintainer files it. It comes from goose.go's issue #41,
was written against upstream at `cec434a5` (v2026.3-140), the `vendor/goose`
this repository builds with, and was checked against `0357b1c2` (v2026.3-245)
too, whose cast checks are the same. Its companion is
[`shift-diagnostic.md`](shift-diagnostic.md) (issue #27).

## 1. The report

A shift computes at its left operand's type, and its count is masked to that
type's width (`goose_spec.md` §6.1, §6.2). So with a `u32` x, widening x
before the shift is required:

```goose
fn main() {
    let x: u32 = 0xFFFFFFFF;
    let a: u64 = (x << 32) | 1;          // 4294967295: u32 shift by 0, then widened
    let b: u64 = ((x as u64) << 32) | 1; // 18446744069414584321: what was meant
    print(a, " ", b);
}
```

The issue reported that the compiler warns that the `as u64` in
`(x as u64) << 32` is a redundant cast. It isn't: removing it changes the
result. The issue asked that such a cast never be called redundant.

## 2. What the checker does: such a cast never warns

`goose --check` of the program above prints nothing, at `cec434a5` and at
`0357b1c2`. The checker never calls a cast that widens a shift's left operand
redundant, at any count and wherever the shift's value goes. Here is why, in
`src/typecheck_exprs.h`:

1. `NoteCast` follows an explicit cast whose operand converts to its type
   implicitly (`Converts`), outside generic code: `x as u64` of a `u32` x.
2. The cast's consumer is the shift. `CheckBinaryResult` calls
   `JudgeBinaryCasts`, which computes the shift without the cast with
   `NumericBinary` in trial mode. `NumericBinary`'s `case T_SHL: case T_SHR:`
   gives the shift its left operand's type (`ct = v.type = ... : lt`), the
   rules of §6.1, so without the cast `x << 32` is a `u32` shift. With the
   cast it is a `u64` shift.
3. The two values are not the same (`SameNumVal`), and their types differ
   (`!SameNum`), so the verdict is "needed" (`CastVerdict(a, "")`).
   `FollowCast` gives that verdict and follows the cast no further.
   `JudgeBinaryCasts` gives it itself where the count is such a cast too.
4. `ReportRedundantCasts` warns about a cast only where every verdict on it
   found it redundant. One "needed" verdict keeps it silent (`Tally::needed`).

This is §6.3's rule: "A cast that decides anything stays silent: ... an
operation's width or signedness". A left operand's cast decides the shift's
width. On a shift the checker can warn about only these casts, all of them
really redundant:

- an identity cast, `x as u32` of a `u32` x (`x is already u32`);
- a cast of a constant to the type its shift computes at anyway, `i64` (or
  `u64` above `i64.max`): `(2 as i64) << 6` (`2 fits i64 here`);
- a cast of the count, which may be of any integer type: `x << (k as u64)`.

How this was checked, with goose.go's build of `cec434a5`:

- **The issue's two shifts in many settings.** `((x as u64) << 32)` for a
  `u32` x and `((y as u32) << 16)` for a `u8` y were tried at a typed
  destination, ORed with 1, in an inferred `let`, as an argument and as a
  return, in compound assignments and comparisons, with `>>`, and with
  constant and non-constant counts. None of them warns. goose.go's
  `test/go/programs/shift_operand_casts.goose` is that program (§4.3).
- **Random programs.** goose.go generated 520 random programs of integer
  arithmetic, casts and shifts. The values came from parameters,
  references, slice and array elements, fields, calls, globals and named
  constants. They went into typed and untyped `let`s, `if` branches,
  arguments (overloaded too), returns, `push`, `block`s, comparisons and
  compound assignments. Each of the 10,327 casts the warning called redundant was
  deleted, one at a time, and the program was run again.
  - Of the 1,227 casts written as a shift's left operand, the checker warned
    about 323: 302 identity casts and 21 constants cast to `i64` or `u64`.
    It warned about no widening cast.
  - Every deletion left the output as it was, except two. Those two differ
    only at `-O1` and `-O2`, because of a miscompile that has nothing to do
    with shifts (§5).

## 3. What looks like it: a warning about another cast on the same line

The likely source of the report is a correct warning. It is about another
cast on the same line, and its text doesn't say which cast it means. When
bytes or words are assembled into a wider integer, the operand that isn't
shifted converts to the wider type implicitly, so its cast is redundant:

```goose
fn join(hi: u32, lo: u32) -> u64 {
    (hi as u64) << 32 | (lo as u64)
}

fn main() { print(join(1, 2)); }
```

```
join.goose:2: warning: redundant `as u64`: a u32 converts to u64 implicitly here
```

The warning is about `lo as u64`, at column 26. The terminal's text has no
column, and only `--diagnostics json` gives one. The reason names neither
`lo` nor anything else that tells the two casts apart. The cast a reader
notices is the shifted one, and deleting that one gives `hi << 32 | (lo as u64)`,
a `u32` shift by 0: `join(1, 2)` becomes 3 instead of 4294967298. Deleting
`lo`'s cast, the one the warning means, changes nothing.

Upstream has hit this too. Commit `2d720546` ("Delete the casts the
redundant-cast warning finds") turned `samples/08_bignum.goose`'s
`let cur = rem << 32 | n[i] as u64;` into `rem << 32 | n[i]`. The warning
there was about `n[i] as u64` too, on a line whose eye-catching part is a
shift.

Of `CastReason`'s reasons, this is the one that names nothing: every other
reason names the operand or its value (`x is already T`, `17 fits u32 here`,
`SZ is 15, which fits u8 here`, `(n * 0.5) is an f64 here`, `the cast to
i64 around it converts b as well`), except `its constants 1 to 2 fit i64
here`.

## 4. Proposed changes

### 4.1 Name the operand in the warning (recommended)

Name the operand in the last reason, as the others do (`src/typecheck_exprs.h`,
`TypeCheck::CastReason`, against `0357b1c2`):

```diff
@@ -2489,7 +2489,7 @@ inline string TypeCheck::CastReason(const CastAlt &a, TypeExpr *reach, bool byca
     if (cv.ck != CK_NONE || cv.litfloat) return cat(what, " is ", an(reach), " here");
     if (cv.litint)
         return cat("its constants ", cv.litlo, " to ", cv.lithi, " fit ", TypeStr(reach), " here");
-    return cat(an(st), " converts to ", TypeStr(reach), " implicitly here");
+    return cat(what, " is ", an(st), ", which converts to ", TypeStr(reach), " implicitly here");
 }
```

```
join.goose:2: warning: redundant `as u64`: lo is a u32, which converts to u64 implicitly here
```

The fixtures' markers, which are substrings of the message, change with it.
These are the `// warning:` lines that contain `converts to ... implicitly
here`, each with the text it becomes before `implicitly here`:

- `test/typing/redundant_casts.goose`: line 2, `s is an i16, which converts
  to i64`; lines 4, 10 and 13, `a is an i32, which converts to f32`; line 5,
  `((0.25) as f32) is an f32, which converts to f64`; line 12, `n is an i64,
  which converts to f32`;
- `test/typing/redundant_call_casts.goose`: line 2, `y is an i32, which
  converts to f64`;
- `test/typing/generic_float_arguments.goose`: line 2, `rows is an i32,
  which converts to f32`;
- the examples' text in `docs/tutorial.md` (line 278) and `docs/reference.md`
  (line 501): `b is a u8, which converts to i64`.

This was checked by building `0357b1c2` with the patch. With the new
markers, `run_tests.py`'s `unmatched_warnings` pairs every warning of the
three fixtures. None of the other 396 fixtures with casts (outside
`errors*/` and `lsp/`) checks differently. `test/diagnostics_json.py` only tests that a
message starts with ``redundant `as i64` ``. The `its constants lo to hi` reason could name its
construct the same way, but such a construct is rarely one of several casts
on a line.

A column in the text (`join.goose:2:26: warning: ...`) would tell the casts
apart too. It would change every diagnostic's text and every test that
reads one, so it isn't proposed here.

### 4.2 Say it in §6.3

§6.3's list of casts that decide something could name the shift, whose
width the left operand's type is, so that nobody has to work it out from
§6.1:

> A cast that decides anything stays silent: a type nothing else gives
> (`let k = 5 as u32;`, `print(b as u8)`), an operation's width or signedness
> (`(a as i64) * b` with `i32` operands, `(x as u64) << 32` with a `u32` x,
> whose left operand's type is the shift's), ...

### 4.3 A regression fixture

goose.go's `test/go/programs/shift_operand_casts.goose` can be copied to
`test/typing/` as is, with its output (goose.go's
`test/go/expected/shift_operand_casts.out`) as
`test/expected/shift_operand_casts.out`: at `0357b1c2` it parses, round-trips,
checks with just its two warnings and prints that output at `-O2` and
`-O0`. It shifts a `u32` and a `u8`, cast
first, in each setting of §2, beside the same shifts without the cast, by
counts that are no constants, and prints the values. Its two `// warning:`
markers are the casts of §3's form, an operand that isn't shifted ORed with
a shifted one. Since `run_tests.py` pairs each marker with one warning line,
a warning about any shifted cast in it would be a line no marker declares,
and would fail. The markers change with §4.1.

## 5. A miscompile the deletions found (a separate bug)

Two of the deletions of §2 changed what a program printed at `-O1` and
`-O2`, and not at `-O0`. The warnings were right, and the cast-free
programs are miscompiled. The bug: a call returning a narrower integer
is the left operand of a binary operator whose other operand is wider, and
the optimizer reduces the call to a constant or to a global's value. The
operation is then computed at the call's type:

```goose
fn three() -> u32 { 3 }

fn main() {
    var w: u64 = 0x100000000;
    print(three() * w);   // 12884901888 at -O0; 0 at -O1 and -O2
    w += 1;
}
```

Both of goose.go's backends print 0 at `-O2` (`cec434a5`), and so does
upstream's C backend at `0357b1c2`. The C it writes is
`gs_mul_u32((gs_u32)(3), w_g)`. `Binary::CgX` (`src/codegen_nodes.h`)
computes an operator at its left operand's type
(`cg.OperandT(left->exprtype)`). The checker widened that to `u64` on the
call node, but the value that replaces the call keeps the callee's `u32`.

- **Also wrong:** `+`, `|` and `<` (`three() < w` is false), a call that
  returns a `u32` global (`fn get() -> u32 { G }`), a `let` the optimizer
  propagates, and the same at `i8` against an `i64`.
- **Right:** `w * three()`; `id(x) * w`, whose inlined value is a variable;
  every one of these at `-O0`.

The checker's advice to delete a cast such as `(three() as u64) * w` then
changes the program. This deserves an issue of its own.

## 6. What goose.go does

There is no false warning to suppress, so goose.go's driver is unchanged. A
filter for warnings on shifted casts couldn't be exercised by any program,
and after a `vendor/goose` bump that brought the bug in, it would hide the
very warning the test is there to catch. Instead,
`test/go/programs/shift_operand_casts.goose` runs through both backends at
`-O2` and `-O0` against `test/go/expected/`, and `run_go_tests.py` checks
that `goose --check` prints exactly the two warnings it declares, for C and
for `--target=go` (`Runner.declared_warnings`). If §4.1 is adopted, its two
markers change with the vendor bump that brings it in.
