# Proposal: let a backend's export fns take a slice of `const u8[:]`

Status: a draft for upstream Goose ([aardappel/goose](https://github.com/aardappel/goose)),
**not filed**: goose.go's maintainer files it. It comes from goose.go's issue #42,
and is written against upstream at `cec434a5` (v2026.3-140), the `vendor/goose`
this repository builds with. Until upstream has it, goose.go's `--target=go`
checks such export fns itself (§4).

## 1. The problem

An export fn's parameters must cross to C (§7.10, `TypeCheck::CheckExport`
and `ExternParamOk` in `src/typecheck_calls.h`): a flat value, or a plain
reference or a slice of one. A slice of slices is refused:

```
strs.goose:2: error: export fn count_tag: parameter tags of type const (const u8[:])[:] cannot cross to C: nested references and slices do not cross
```

For C that is right: no C host has a value of that layout to pass. For Go it
is not. A Go `string` is its data and its length, the two words of a Goose
`const u8[:]`, so a Go `[]string`'s elements are laid out as a
`const (const u8[:])[:]`'s, and goose.go's entry points take one in place,
as they take a `string` for a `const u8[:]`. Kernels over a list of strings
from Go -- a tag filter, a string table, a set lookup -- had to be reached
some other way: marked `recursive fn`, which the optimizer never inlines, and
kept alive by a call from a dummy export fn, so that the host could call the
generated function itself -- no entry point, no stable name, and no facts
from an entry for bounds-check elimination, which knows nothing of the
arguments of a function called from outside.

## 2. The change

A `TypeCheck` option, which the driver sets for a backend whose host can pass
one -- goose.go's for `--target=go` -- under which `CheckExport` takes a
parameter of the type `const (const u8[:])[:]`, `const` at both depths, as
crossing. In `CheckExport`'s loop over the parameters, before
`ExternParamOk`:

```cpp
        if (stringslices && IsConstStringSlice(t)) continue;   // a Go []string
```

where `IsConstStringSlice(t)` is `t->kind == TY_SLICE && t->cq &&
t->sub->kind == TY_SLICE && t->sub->cq && IsU8(t->sub->sub)`, and
`TypeCheckProgram` takes the option on to the `TypeCheck` it builds. Both
depths must be `const`: a Go string's bytes are never written (a literal's
are in read-only memory), and a `[]string` an entry point is lent is the
caller's, whose strings no store may re-point at Goose's own memory. A slice
of `u8` slices of another constness can keep today's message, or name the
type that crosses: goose.go's says

```
export fn f: parameter tags of type (const u8[:])[:] cannot cross to Go: a Go []string crosses as a const (const u8[:])[:], whose strings, as Go's, nothing writes
```

Nothing else changes: the body is checked as every export's is, as a
function nothing calls (`CheckUnreached`), with each slice parameter rooted
at a global of its own, and every other check of the boundary applies to the
export's other parameters and its result. C's own boundary is as it is.

## 3. Tests

A fixture under the option's flag with export fns taking
`tags: const (const u8[:])[:]` -- alone, beside a `const u8[:]`, two of them,
one returning a `const u8[:]` view of a tag -- and fixtures that must fail
with it: each other constness, and a second parameter or a result the
boundary refuses, with their messages. goose.go's are
`test/go/export/string_slices/` and the `cli` checks of `run_go_tests.py`.

## 4. goose.go's interim

goose.go cannot change `vendor/goose`, so for `--target=go` its driver hides
the export fns that take a slice of `u8` slices from the checker's check of
the boundary while the program is checked (`HideGoExports`,
`src/codegen_go_export.h`, called from `src/main.cpp`): they are checked as
functions nothing calls, which is how an export is checked anyway. Then it
checks them itself (`CheckGoExports`, `GoExportCheck`): `CheckExport`'s
checks, in its order and with its messages, but for a
`const (const u8[:])[:]` parameter, which crosses, and another constness of
one, which is refused with the message above. Once upstream has the option,
goose.go deletes those three and sets the option instead.
