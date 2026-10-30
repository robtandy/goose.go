# Proposal: diagnose shifts by a constant count outside the shifted type's width

Status: a draft for upstream Goose ([aardappel/goose](https://github.com/aardappel/goose)),
**not filed**: goose.go's maintainer files it. It comes from goose.go's issue #27,
and is written against upstream at `cec434a5` (v2026.3-140), the `vendor/goose`
this repository builds with. Until upstream has it, goose.go's `--target=go`
gives the warning itself (§6).

## 1. The problem

A shift takes its left operand's type, and its count is masked to that type's
width (`goose_spec.md` §6.1, §6.2: "Shift counts are masked to `0..width-1`
(defined)"). So with a `u8` value `b`, `b << 16` is a `u8` shift by
`16 & 7 == 0`: it is `b`. Code that assembles an integer from bytes writes
exactly that, the cast it needed left out, and nothing says so:

```goose
// shift_repro.goose: assembling a u32 from bytes, the shifts at u8.
fn le32(b: u8[:]) -> u32 {
    b[0] | b[1] << 8 | b[2] << 16 | b[3] << 24
}

fn main() {
    var bytes: u8[4] = [0x78, 0x56, 0x34, 0x12];
    let b = bytes[2];
    let v: u32 = b << 16;           // a u8 shift by 0: v is b, 0x34
    let w: u32 = (b as u32) << 16;  // what was meant: 0x340000
    print(v, " ", w, " ", le32(bytes));
}
```

`goose --check shift_repro.goose` prints nothing, and the program prints
`52 3407872 126` where `52 3407872 305419896` was meant: `le32` ORs the four
bytes unshifted, `0x78 | 0x56 | 0x34 | 0x12`, and widens the `u8` it gets.
It happens in practice: tests caught one such slip, where nothing in the
language would have.

The program is well defined -- both backends mask the count alike -- but a
constant count outside `0..width-1` is never one a program needs: it means
the same as the masked count, which the source could write instead. Such a
shift is a cast left out (or a type that is narrower than the writer thought).

## 2. The rule

A shift -- `<<`, `>>`, `<<=` or `>>=` -- whose count is an integer constant
(`Val::ck == CK_INT`: a literal, a named constant, and arithmetic on them,
`8 * 2`; a cast or a `let` local is no constant) outside `0..width-1` of the
type the shift computes at gets a diagnostic. That type is the left
operand's (§6.1), a constant left operand's `i64` (or `u64`), the target's
for `<<=` and `>>=`. A negative constant is outside too: it is masked as
well (`x >> -1` of an `i64` is `x >> 63`).

A count that is no constant is never judged: `b << k` is left alone, whatever
`k` turns out to be, and so is a count the optimizer proves constant only
after inlining. What compiles without a diagnostic then depends on what the
source says, never on how much the optimizer proved, as for §6.1's
comparisons with `u64`.

## 3. A warning, not an error

The recommendation is a warning:

- The program means what §6.2 defines, at every count; a warning changes no
  program's meaning and needs no change to the specification, where an
  error would turn §6.2's "masked (defined)" into "a constant count must lie
  in `0..width-1`".
- It fits the checker's warning machinery as it is (`TypeCheck::Warn`,
  `PrintWarning`): held back while a loop's pass or a join may be repeated,
  and printed once per message for the node as the source has it, so a
  generic function shifting by 8 is warned of once at `T = u8` and not at
  all at `T = u16`. The redundant-cast warnings skip generic code (`InGenericCode`)
  because a cast one specialization needs may be redundant in another; here
  the shift is wrong in the specialization it is warned of, which the
  message names by its type.
- The C suite has fixtures that shift by such counts on purpose, to test the
  masking (§5). With a warning they declare it; with an error they would
  have to compute their counts.

An error is defensible too -- no correct program needs such a shift -- and
the same check would call `Error` instead of `Warn`.

## 4. The message

```
shift_repro.goose:9: warning: shift of u8 by 16 is a shift by 0: at u8 the count is masked to 0..7 (§6.2); cast the operand first: `(b as u32) << 16`
```

That is, for a shift at `T`, of width `w`, by `c`, and for `x << c` with
`0 <= c < 64` the second line's cast:

```
shift of T by c is a shift by c & (w - 1): at T the count is masked to 0..w-1 (§6.2)
shift of T by c is a shift by c & (w - 1): at T the count is masked to 0..w-1 (§6.2); cast the operand first: `(x as W) << c`
```

with `x` and `c` as the source writes them (`TypeCheck::ExprStr`), grouped
where `as` would bind to a part of them.

The issue's draft was "shift of u8 by 16 discards every bit; cast the
operand first". Discarding every bit is what a `u8` shift by 16 would do if
its count were not masked (and C, which promotes the `u8` to `int` first,
would keep every bit, at bits 16 to 23, as the writer expected). In Goose
the count is masked, so the shift keeps every bit and moves none of them
where the source says -- `b << 16` is `b` -- and the message says what it
does instead.

**The type to cast to.** Where the shift's value is widened at once -- to a
destination, an operand of a wider operation, a parameter, a field --
the checker has fitted the shift node to that type (its `exprtype` is the
wider type, its left operand's the narrow one), and that type is the one the
writer meant: `let v: u32 = b << 16;` suggests `(b as u32) << 16`. Otherwise
it is the narrowest integer type of the operand's signedness wide enough
for the count (`b << 8` of a `u8` suggests `u16`). An operand that a cast of
its own widens too little is cast wider instead: `(c[2] as u16) << 16`
suggests `(c[2] as u32) << 16`. A right shift, a compound assignment, a
negative count and a count of 64 or more suggest nothing: no cast of the
operand makes them mean anything else.

**A second warning, maybe: widened at once.** The issue asks for a warning
"ideally also when the result is immediately widened, which makes the
intent obvious": a left shift whose value is widened at once loses the bits
the wider type would have held, whatever the count, as in
`let v: u32 = b << 4;` or `v |= b[i] << (8 * i);` (with `v: u32`). That is
a broader rule, with false positives: over the corpus of §5 it fires once,
on `(s[i] as u16) << 8 | (s[i + 1] as u64)` in goose.go's own tests, which
loses nothing, since the operand holds 8 bits of its 16. A precise version
would count the operand's significant bits (a `u8` cast up to `u16` has 8,
a value masked to 255 has 8) and warn where those and the count exceed the
width; it is left out of this proposal's first step, and goose.go does not
give it.

## 5. Where it goes, and its tests

The checker sees each shift with its operands' values:

1. `TypeCheck::NumericBinary` (`src/typecheck_exprs.h`), `case T_SHL: case
   T_SHR:`, once the left operand's type is the shift's (`ct = v.type = ...`):
   where `!trial && rv.ck == CK_INT` and `v.type` is an integer type (not a
   varint), and `rv` lies outside `0..IntBits(v.type->intstorage) - 1`
   (`rv.uns` is always outside), `Warn(b, ...)`.
2. `TypeCheck::CompoundAssign` (`src/typecheck_flow.h`), the `isshift`
   branch, which drops the count's value today: `auto cv = CheckIntAny(a->rhs);`,
   then the same test against the target's type `st`, and `Warn(a, ...)`.
3. Optionally, `TypeCheck::ConstIntValue` (`src/typecheck_exprs.h`), whose
   shifts are those of array sizes, fill counts and match patterns
   (`let b: i64[K << 8] = [1; K << 8];`, with `let K: i8 = 65;`).

Two things the checker knows only later: the type a shift's value is widened
to, which its consumer gives it (`RetypeOperand`), and the type a shift of
`Val::flexint` left operand ends up at (`(1 << k) << 8` fitted to a `u8`,
`RetypeFlexInt`). Checking at (1) suggests the narrowest type wide enough
and misses the flexint case. A pass over the checked program after the check,
as goose.go's interim one (§6), sees both, and gives the same warnings at
every `-O` level, since nothing is folded or inlined yet. Either is a small
change; (1) and (2) keep the diagnostic with the rest of §6.1's, and are what
the language server shows as it checks, through the same `Ast::Warn`.

**Tests.** A fixture with `// warning:` markers, as `run_tests.py` reads them
(one warning line each, in any order; a positive fixture without markers
prints none): goose.go's `test/go/warnings/shift_counts.goose` is written so,
and could be copied to `test/typing/` (it needs a `test/expected/` output).
It covers `u8 << 8`, `u16 << 16` and `u32 << 32`, each beside the same shift
with its operand cast first, which gives no warning, and the other forms
above.

**What else it fires on.** Over every program of upstream's `test/` (but the
`errors/`, `errors_tc/` and `lsp/` fixtures) and `samples/`, the standard
library as they use it, and goose.go's own tests -- about 800 programs, as
goose.go's interim check judges them (§6) -- the rule fires on
four lines, all fixtures testing the masking on purpose, which would need a
`// warning:` marker each (or a count that is no constant):

| Fixture | Line | Shift | Warning |
|---|---|---|---|
| `test/codegen/codegen_exec.goose` | 456 | `print(1 << 65);` | shift of i64 by 65 is a shift by 1 |
| `test/codegen/codegen_exec.goose` | 641 | `print((1 as u8) << 9);` | shift of u8 by 9 is a shift by 1 |
| `test/typing/constant_widths.goose` | 35 | `print(v << 8);` (`v: i8`) | shift of i8 by 8 is a shift by 0 |
| `test/optimizer/bce_halving_index.goose` | 12 | `s += xs[i >> 65];` | shift of i64 by 65 is a shift by 1 |

With (3), `test/typing/constant_widths.goose:33` too (`K << 8` at `i8`, in a
size and a fill count). Nothing in `stdlib/` or `samples/` is warned of.

## 6. goose.go's interim warning

goose.go cannot change `vendor/goose`, so its `--target=go` warns on the
checked program it receives: `WarnShiftCounts` (`src/codegen_go_shifts.h`),
called by `src/main.cpp` right after `TypeCheckProgram`, walks every
specialization's body and every global's initializer (`RunChildren`), with
the rule of §2 and the message of §4, once per shift as the source has it and
message, in the order of the source. `test/go/warnings/shift_counts.goose` is
its test (`run_go_tests.py`'s `Runner.warnings`), and the suite leaves those
warnings out of the two backends' `--check --stacks` reports, which must agree
on the rest (`GO_WARNING`).

Once upstream gives the diagnostic, goose.go deletes `src/codegen_go_shifts.h`,
its call and `GO_WARNING`; until it does, a vendor bump that brings it shows
as each warning printed twice in `test/go/warnings/`.
