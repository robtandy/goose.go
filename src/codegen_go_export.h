// Goose compiler — the Go backend's foreign boundary (definitions of
// CodeGenGo members, codegen_go.h; DESIGN.md §3.5): the calls of extern fns
// (§7.10), the Go entry points of export fns, and the --include files a
// program's extern fns call into.
//
// * An extern fn calls the Go its C symbol names. The C runtime's functions
//   std, math and os bind (gs_sqrt, gs_find_any, gs_os_read_file, ...) and
//   the C library's that Go's math package has (floor, sinf, ...) are the Go
//   runtime's and Go's own; any other symbol is a Go function of the
//   generated file's package, which an --include file (or a file beside the
//   generated one) defines, called directly: scalars as Go's own types, a
//   fixed-size value as its Go type, a plain reference as a pointer to it, a
//   slice as a GsSlice, and a u8[>..]& builder as a GsRref, after the data
//   stack block, which such a function is handed first to append with
//   (GsBldAppend). Its result is a scalar's or a fixed-size value's.
// * An export fn has two Go entry points: NameWithStack(gs, ...), which runs
//   it on the data stacks of a block the caller holds, and Name(...), which
//   runs it on a block from its program's pool (gsProg.AcquireStackBlock),
//   rewound and given back as it returns. Both take Go's own
//   values, which they view in place: a u8[:] as a []byte, a const u8[:] as
//   a string or a []byte, a const (const u8[:])[:] as a []string (issue
//   #42), any other T[:] as a []T, a T& as a *T, a scalar or a fixed-size
//   value as its Go type, and keep each argument viewing
//   Go's memory off Go's moving stacks (GsEscape) and alive until the call
//   returns (runtime.KeepAlive), which the Goose code may hold only where
//   the collector does not look -- but where it holds them in Go's own
//   variables alone, a kernel's, which they hand on as they are, for Go to
//   inline (ArgsStayInGo, issue #22). Where a const u8[:] makes Name
//   generic, NameString(...) and NameBytes(...) take each one as a string
//   and as a []byte. A const u8[:] result is a []byte viewing what the
//   slice does. In a library, each starts the package at its first call
//   (gsLibStart). A call on a block turns panic on fault on for itself
//   only, on its caller's goroutine.
// * An --include file is Go source: its package clause and build
//   constraints are dropped, its imports join the generated file's, and the
//   rest follows the program, each run of lines between blank ones under a
//   //line directive naming the file and the run's line (GoIncludeLines,
//   codegen_go_emit.h), which gofmt leaves be.
#pragma once

namespace goose {

// Go's keywords and predeclared identifiers, which no name a program gives
// a Go function can be.
static bool GoReservedWord(string_view n) {
    static const unordered_set<string_view> words = {
        "break", "case", "chan", "const", "continue", "default", "defer", "else", "fallthrough",
        "for", "func", "go", "goto", "if", "import", "interface", "map", "package", "range",
        "return", "select", "struct", "switch", "type", "var",
        "any", "bool", "byte", "comparable", "complex64", "complex128", "error", "float32",
        "float64", "int", "int8", "int16", "int32", "int64", "rune", "string", "uint", "uint8",
        "uint16", "uint32", "uint64", "uintptr", "true", "false", "iota", "nil", "append", "cap",
        "clear", "close", "complex", "copy", "delete", "imag", "len", "make", "max", "min", "new",
        "panic", "print", "println", "real", "recover", "init", "main", "_",
    };
    return words.count(n) != 0;
}

// Whether a Go name is free for the program's foreign functions: a Go
// identifier, no keyword or predeclared name, none of the runtime's in any
// of the forms a file gives them (which start with gs or Gs, or are
// NewStackBlock and the pool's two, exported or not, or the StackBlock of
// --go-unexported), and none the generated code gives the program's own
// (which end in _g and digits).
static bool GoForeignNameOk(string_view n, string &why) {
    auto ident = !n.empty() && (isalpha((unsigned char)n[0]) || n[0] == '_') &&
                 all_of(n.begin(), n.end(), [](char c) { return isalnum((unsigned char)c) || c == '_'; });
    if (!ident) {
        why = cat("the Go name ", n, " is no Go identifier");
        return false;
    }
    if (GoReservedWord(n)) {
        why = cat("the Go name ", n, " is Go's own");
        return false;
    }
    if (n.size() >= 2 && (n.substr(0, 2) == "gs" || n.substr(0, 2) == "Gs")) {
        why = cat("the Go name ", n, " starts as the Go runtime's names do (gs, Gs)");
        return false;
    }
    if (GoRuntimeName(n)) {
        why = cat("the Go name ", n, " is the Go runtime's");
        return false;
    }
    auto g = n.rfind("_g");
    if (g != string_view::npos &&
        all_of(n.begin() + g + 2, n.end(), [](char c) { return isdigit((unsigned char)c) || c == '_'; })) {
        why = cat("the Go name ", n, " ends as the names of the program's own do (_g)");
        return false;
    }
    return true;
}

// The exported Go name of an export fn's C symbol: its words, between the
// underscores, each capitalized, parse_json being ParseJson.
static string GoExportName(string_view cname) {
    string s;
    auto up = true;
    for (auto c : cname) {
        if (c == '_') {
            up = true;
            continue;
        }
        s += up ? (char)toupper((unsigned char)c) : c;
        up = false;
    }
    return s;
}

// The C library's functions of floats that Go's math package has, and
// math's under the runtime's names (stdlib/math.goose), by C symbol: the Go
// function, how many operands, and whether it is the f32 form, which Go
// computes at f64 and rounds (a correctly rounded root stays one, and
// floor, ceil, round, trunc, fabs and copysign of an f32 are f32s).
// GsMulAddF32 takes f32s itself.
struct GoLibmFn {
    const char *c, *go;
    int n;
    bool f32;
};
static const GoLibmFn go_libm[] = {
    { "gs_sqrt", "math.Sqrt", 1, false },   { "gs_sqrtf", "math.Sqrt", 1, true },
    { "gs_sin", "math.Sin", 1, false },     { "gs_sinf", "math.Sin", 1, true },
    { "gs_cos", "math.Cos", 1, false },     { "gs_cosf", "math.Cos", 1, true },
    { "gs_tan", "math.Tan", 1, false },     { "gs_tanf", "math.Tan", 1, true },
    { "gs_asin", "math.Asin", 1, false },   { "gs_asinf", "math.Asin", 1, true },
    { "gs_acos", "math.Acos", 1, false },   { "gs_acosf", "math.Acos", 1, true },
    { "gs_atan", "math.Atan", 1, false },   { "gs_atanf", "math.Atan", 1, true },
    { "gs_atan2", "math.Atan2", 2, false }, { "gs_atan2f", "math.Atan2", 2, true },
    { "gs_exp", "math.Exp", 1, false },     { "gs_expf", "math.Exp", 1, true },
    { "gs_log", "math.Log", 1, false },     { "gs_logf", "math.Log", 1, true },
    { "gs_log2", "math.Log2", 1, false },   { "gs_log2f", "math.Log2", 1, true },
    { "gs_log10", "math.Log10", 1, false }, { "gs_log10f", "math.Log10", 1, true },
    { "gs_pow", "math.Pow", 2, false },     { "gs_powf", "math.Pow", 2, true },
    // x * y + z rounded once, as C's is on arm64 (and x86-64 built for FMA):
    // Go has FMA everywhere (§7.12 leaves it to the implementation).
    { "gs_mul_add", "math.FMA", 3, false }, { "gs_mul_addf", "GsMulAddF32", 3, true },
    { "sqrt", "math.Sqrt", 1, false },      { "sqrtf", "math.Sqrt", 1, true },
    { "sin", "math.Sin", 1, false },        { "sinf", "math.Sin", 1, true },
    { "cos", "math.Cos", 1, false },        { "cosf", "math.Cos", 1, true },
    { "tan", "math.Tan", 1, false },        { "tanf", "math.Tan", 1, true },
    { "asin", "math.Asin", 1, false },      { "asinf", "math.Asin", 1, true },
    { "acos", "math.Acos", 1, false },      { "acosf", "math.Acos", 1, true },
    { "atan", "math.Atan", 1, false },      { "atanf", "math.Atan", 1, true },
    { "atan2", "math.Atan2", 2, false },    { "atan2f", "math.Atan2", 2, true },
    { "sinh", "math.Sinh", 1, false },      { "sinhf", "math.Sinh", 1, true },
    { "cosh", "math.Cosh", 1, false },      { "coshf", "math.Cosh", 1, true },
    { "tanh", "math.Tanh", 1, false },      { "tanhf", "math.Tanh", 1, true },
    { "exp", "math.Exp", 1, false },        { "expf", "math.Exp", 1, true },
    { "exp2", "math.Exp2", 1, false },      { "exp2f", "math.Exp2", 1, true },
    { "log", "math.Log", 1, false },        { "logf", "math.Log", 1, true },
    { "log2", "math.Log2", 1, false },      { "log2f", "math.Log2", 1, true },
    { "log10", "math.Log10", 1, false },    { "log10f", "math.Log10", 1, true },
    { "pow", "math.Pow", 2, false },        { "powf", "math.Pow", 2, true },
    { "hypot", "math.Hypot", 2, false },    { "hypotf", "math.Hypot", 2, true },
    { "cbrt", "math.Cbrt", 1, false },      { "cbrtf", "math.Cbrt", 1, true },
    { "fmod", "math.Mod", 2, false },       { "fmodf", "math.Mod", 2, true },
    { "fabs", "math.Abs", 1, false },       { "fabsf", "math.Abs", 1, true },
    { "copysign", "math.Copysign", 2, false }, { "copysignf", "math.Copysign", 2, true },
    { "floor", "math.Floor", 1, false },    { "floorf", "math.Floor", 1, true },
    { "ceil", "math.Ceil", 1, false },      { "ceilf", "math.Ceil", 1, true },
    // C's round is half away from zero, as Go's is.
    { "round", "math.Round", 1, false },    { "roundf", "math.Round", 1, true },
    { "trunc", "math.Trunc", 1, false },    { "truncf", "math.Trunc", 1, true },
    { "fma", "math.FMA", 3, false },        { "fmaf", "GsMulAddF32", 3, true },
};

// The OS primitives of stdlib/os.goose the Go runtime has (go_runtime_os),
// by C symbol; each is the Go function its symbol names in GoExportName's
// way, gs_os_read_file being GsOsReadFile.
static bool GoOsFn(string_view c) {
    static const unordered_set<string_view> fns = {
        "gs_os_read_file", "gs_os_write_file", "gs_os_write_file_atomic", "gs_os_append_file",
        "gs_os_file_exists", "gs_os_remove_file", "gs_os_rename_file", "gs_os_make_dir",
        "gs_os_is_dir", "gs_os_remove_dir", "gs_os_list_dir", "gs_os_read_line",
        "gs_os_read_stdin", "gs_os_write_stdout", "gs_os_write_stderr", "gs_os_flush_stdout",
        "gs_os_arg_count", "gs_os_arg", "gs_os_getenv", "gs_os_resource_dir", "gs_os_time_ns",
        "gs_os_clock_ns", "gs_os_sleep_ms", "gs_os_random_u64", "gs_trim_memory",
    };
    return fns.count(c) != 0;
}

// The export fn whose C symbol an extern fn binds, if any: a call of it is
// a call of the export's C wrapper in C, and of the export's function in Go,
// as its entry points make it (EmitExports).
inline FnSpec *CodeGenGo::ExportOfExtern(FnSpec *sp) {
    if (!sp || !sp->sf->isextern || !sp->sf->dylib.empty()) return nullptr;
    for (auto sf : ast.functions)
        if (sf->isexport && sf->cname == sp->sf->cname && sf->specs.size() == 1 && sinfo.count(sf->specs[0]))
            return sf->specs[0];
    return nullptr;
}

// Whether a call of the extern fn hands it the data stack block first: one
// taking a u8[>..] builder appends to it through the block (GsBldAppend),
// and os.trim_memory trims the block's stacks.
inline bool CodeGenGo::ExternTakesGs(FnSpec *sp) {
    if (sp->sf->cname == "gs_trim_memory") return true;
    for (auto pt : sp->argtypes)
        if (IsFatRef(pt)) return true;
    return false;
}

// The Go of a call of one of the C runtime's or C library's functions the Go
// runtime and Go's own library have, into x, its operands evaluated as a;
// false for any other.
inline bool CodeGenGo::GoLibExtern(Call *c, FnSpec *sp, const vector<string> &a, string &x) {
    auto &name = sp->sf->cname;
    auto isf = [&](TypeExpr *t, bool f32) {
        return t && t->kind == TY_FLT && (t->fltstorage == FS_F32) == f32;
    };
    for (auto &m : go_libm) {
        if (name != m.c) continue;
        auto fits = (int)sp->argtypes.size() == m.n && sp->rets.size() == 1 && isf(sp->rets[0], m.f32);
        for (auto pt : sp->argtypes) fits = fits && isf(pt, m.f32);
        // Bound with a signature of its own: a Go function of the name.
        if (!fits) return false;
        string list;
        for (auto &v : a) Append(list, list.empty() ? "" : ", ", m.f32 && m.go[0] != 'G' ? cat("float64(", v, ")") : v);
        x = cat(m.go, "(", list, ")");
        if (m.f32 && m.go[0] != 'G') x = cat("float32(", x, ")");
        return true;
    }
    // popcount, clz and ctz count 64 for zero, as Go's math/bits does.
    if (name == "gs_popcount" || name == "gs_clz" || name == "gs_ctz") {
        auto fn = name == "gs_popcount" ? "OnesCount64" : name == "gs_clz" ? "LeadingZeros64" : "TrailingZeros64";
        x = cat("int64(bits.", fn, "(", a[0], "))");
        return true;
    }
    if (name == "gs_find_any") {
        x = cat("GsFindAny(", a[0], ", ", AddrOfRef(sp->argtypes[1], a[1]), ")");
        return true;
    }
    if (name == "gs_find_pair") {
        x = cat("GsFindPair(", a[0], ", ", AddrOfRef(sp->argtypes[1], a[1]), ", ", a[2], ", ",
                AddrOfRef(sp->argtypes[3], a[3]), ")");
        return true;
    }
    if (name == "frexp" && a.size() == 2) {
        x = cat("GsFrexp(", a[0], ", ", a[1], ")");
        return true;
    }
    if (name == "ldexp" && a.size() == 2) {
        x = cat("math.Ldexp(", a[0], ", int(", a[1], "))");
        return true;
    }
    if (GoOsFn(name)) {
        usesos = true;
        auto list = ExternTakesGs(sp) ? string("gs") : string();
        // os.resource_dir's answer where it is not the executable's
        // directory, which goose run gives (gsJitResourceDir).
        if (name == "gs_os_resource_dir") list += ", gsJitResourceDir";
        for (auto &v : a) Append(list, list.empty() ? "" : ", ", v);
        x = cat(GoExportName(name), "(", list, ")");
        return true;
    }
    (void)c;
    return false;
}

// A call of an extern fn (§7.10), its arguments evaluated in order as for
// any call (CodeGen::EmitExternCall): the Go runtime's or Go's own function
// where it is one of theirs, a Go function of the package otherwise, which
// is handed the block first where it appends to a builder.
inline string CodeGenGo::ExternCallX(Call *c, FnSpec *sp) {
    auto &name = sp->sf->cname;
    auto ln = c->line;
    if (!sp->sf->dylib.empty())
        Unsupported(ln, cat("a call to ", sp->sf->name, ", a function of the shared library \"", sp->sf->dylib,
                            "\" (an extern block)"));
    auto an = CallArgNodes(c, sp->argtypes.size());
    vector<bool> later(an.size() + 1, false);
    for (size_t i = an.size(); i-- > 0;) later[i] = later[i + 1] || HasStmts(an[i]);
    vector<string> a;
    for (size_t i = 0; i < an.size(); i++) {
        auto pt = sp->argtypes[i];
        auto x = Coerce(GenX(an[i]), an[i]->exprtype, pt, an[i]->line);
        if (later[i + 1] && !IsGoConst(x) && x != "nil") x = Snapshot(pt, x);
        a.push_back(x);
    }
    // The program's own export, called back through its symbol: its
    // function, on this block, its stacks from 0, as its C wrapper's gs_sp
    // is (no call of the program's own hands it a stack, so none counts).
    if (auto xsp = ExportOfExtern(sp)) {
        auto &ki = sinfo[xsp];
        if (xsp->argtypes.size() != a.size())
            Fail(ln, cat("extern fn ", sp->sf->name, " binds export fn ", xsp->sf->name, " with another count of "
                         "parameters"));
        auto list = ki.needssp ? string("gs, 0") : ki.needsgs ? string("gs") : GsArgPh(xsp);
        for (size_t i = 0; i < a.size(); i++)
            Append(list, list.empty() ? "" : ", ", Coerce(a[i], sp->argtypes[i], xsp->argtypes[i], ln));
        if (ki.hasrf) Fail(ln, cat("internal: export fn ", xsp->sf->name, " with a long-distance return"));
        return cat(ki.name, "(", list, ")");
    }
    string x;
    if (GoLibExtern(c, sp, a, x)) return x;
    if (name.rfind("gs_", 0) == 0)
        Unsupported(ln, cat("a call to the extern fn ", sp->sf->name, " (C's ", name, ", which the Go runtime has no "
                            "form of)"));
    string why;
    if (!GoForeignNameOk(name, why))
        Fail(ln, cat("extern fn ", sp->sf->name, " calls the Go function its symbol names, and ", why,
                     ": name another with = \"symbol\" (§7.10)"));
    auto list = ExternTakesGs(sp) ? string("gs") : string();
    for (auto &v : a) Append(list, list.empty() ? "" : ", ", v);
    return cat(name, "(", list, ")");
}

// ------------------------------------------------------------------
// export fn.

// A Go []string (issue #42): a Go string is its data and its length, the two
// words of a const u8[:], so a []string is laid out as a slice of those,
// const at both depths, since nothing writes a Go string's bytes or the
// strings of a []string an entry point is handed. An export fn's entry
// points take a parameter of that type as a []string, which no C host has.
inline bool GoStringsT(const TypeExpr *t) {
    return t && t->kind == TY_SLICE && t->cq && t->sub->kind == TY_SLICE && t->sub->cq && IsU8(t->sub->sub);
}

// A slice of u8 slices, of any constness: what a parameter meant as a Go
// []string is.
inline bool GoStringsLikeT(const TypeExpr *t) {
    return t && t->kind == TY_SLICE && t->sub->kind == TY_SLICE && IsU8(t->sub->sub);
}

// The checks of an export fn's boundary that TypeCheck::CheckExport makes
// (vendor/goose/src/typecheck_calls.h), for --target=go's export fns taking
// a Go []string, which the checker's own check, C's boundary, refuses
// ("nested references and slices do not cross"): the driver hides them from
// it (HideGoExports, while the program is checked, as the functions nothing
// calls are checked) and checks them here, as it would, but for a
// const (const u8[:])[:] parameter, which crosses. Their messages are its
// own. docs/upstream/go-string-slices.md proposes the option to upstream's
// check, which would make these go.
struct GoExportCheck {
    Ast &ast;

    static string TypeStr(const TypeExpr *t) {
        string s;
        t->Dump(s);
        return s;
    }

    // TypeCheck::Reaches: whether t, or a type a value of it reaches through
    // references, slices, arrays and fields, satisfies pred.
    static bool Reaches(TypeExpr *t, const function<bool(TypeExpr *)> &pred) {
        set<pair<const void *, const void *>> seen;
        function<bool(TypeExpr *)> walk = [&](TypeExpr *t) -> bool {
            if (!t) return false;
            if (pred(t)) return true;
            switch (t->kind) {
                case TY_REF: return walk(t->ref->sub);
                case TY_SLICE: return walk(t->sub);
                case TY_ARRAY: return walk(t->arr->sub);
                case TY_STRUCT: case TY_ENUM: case TY_VARIANT: {
                    // Each entered once, as a recursive type reaches itself again;
                    // a variant apart from its ADT, its payload alone.
                    auto adt = t->kind == TY_VARIANT ? t->var->adt : t;
                    const void *inst = adt->kind == TY_STRUCT ? (const void *)adt->struc->inst
                                       : adt->kind == TY_ENUM ? (const void *)adt->enu->inst
                                                              : (const void *)adt;
                    auto key = pair<const void *, const void *>(inst, t->kind == TY_VARIANT ? t->var->variant : nullptr);
                    if (!seen.insert(key).second) return false;
                    for (auto &run : Runs(t))
                        for (auto ft : *run.ftypes)
                            if (ft && walk(ft)) return true;
                    return false;
                }
                default: return false;
            }
        };
        return walk(t);
    }

    // The fields of a struct's, an ADT's or a variant's values, as the
    // checker instantiated them (TypeCheck::FieldRuns).
    static span<const FieldRun> Runs(TypeExpr *t) {
        switch (t->kind) {
            case TY_STRUCT: return t->struc->inst ? RunsOf(t->struc->inst) : span<const FieldRun>();
            case TY_ENUM: return t->enu->inst ? RunsOf(t->enu->inst) : span<const FieldRun>();
            case TY_VARIANT: {
                auto adt = t->var->adt;
                if (adt->kind != TY_ENUM || !adt->enu->inst) return {};
                auto ei = adt->enu->inst;
                return RunsOf(ei, ei->en->VariantIndex(t->var->variant));
            }
            default: return {};
        }
    }

    // TypeCheck::ClassOf.
    static SizeClass ClassOf(TypeExpr *t) {
        switch (t->kind) {
            case TY_INT: return t->intstorage == IS_VARINT ? SC_VARIABLE : SC_FIXED;
            case TY_REF: return t->ref->lenstorage == IS_VARINT ? SC_VARIABLE : SC_FIXED;
            case TY_STRUCT: return t->struc->inst ? t->struc->inst->sclass : SC_VARIABLE;
            case TY_ENUM:
                if (!t->enu->varmode) return SC_FIXED;
                return t->enu->inst ? t->enu->inst->varclass : SC_VARIABLE;
            case TY_ARRAY:
                switch (t->arr->akind) {
                    case A_FIXED: return SC_FIXED;
                    case A_VAR: return SC_VARIABLE;
                    case A_LIMITED: return t->arr->size >= 0 ? SC_FIXED : SC_VARIABLE;
                    default: return SC_RESIZABLE;
                }
            case TY_VARIANT: {
                auto c = SC_FIXED;
                for (auto &run : Runs(t))
                    for (auto ft : *run.ftypes)
                        if (ft) c = std::max(c, ClassOf(ft));
                return c;
            }
            default: return SC_FIXED;
        }
    }

    // TypeCheck::IsFlat: no references, slices or relative references at any
    // depth (§1.1).
    static bool IsFlat(TypeExpr *t) {
        switch (t->kind) {
            case TY_REF: case TY_SLICE: return false;
            case TY_STRUCT: return t->struc->inst && t->struc->inst->flat;
            case TY_ENUM: return t->enu->inst && t->enu->inst->flat;
            case TY_ARRAY: return IsFlat(t->arr->sub);
            case TY_VARIANT:
                for (auto &run : Runs(t))
                    for (auto ft : *run.ftypes)
                        if (ft && !IsFlat(ft)) return false;
                return true;
            default: return true;
        }
    }

    // TypeCheck::ExternValueOk: a fixed-size value C takes by value (§7.10).
    static bool ValueOk(TypeExpr *t, string &why) {
        if (Reaches(t, [](TypeExpr *t) { return t->kind == TY_INT && t->ix; })) {
            why = "C code is not checked against the array an index type names (§3.11)";
            return false;
        }
        switch (t->kind) {
            case TY_INT:
                if (t->intstorage == IS_VARINT) {
                    why = "varints have no C form";
                    return false;
                }
                return true;
            case TY_FLT: case TY_BOOL: return true;
            case TY_REF: case TY_SLICE: why = "nested references and slices do not cross"; return false;
            default: break;
        }
        if (ClassOf(t) != SC_FIXED) {
            why = "only fixed-size values cross by value";
            return false;
        }
        if (!IsFlat(t)) {
            why = "values holding references do not cross";
            return false;
        }
        return true;
    }

    // TypeCheck::ExternParamOk.
    static bool ParamOk(TypeExpr *t, string &why) {
        if (t->kind == TY_REF) {
            if (t->ref->optional || t->ref->lenstorage >= 0) {
                why = "only plain references cross";
                return false;
            }
            auto s = t->ref->sub;
            if (s->kind == TY_ARRAY && s->arr->akind == A_GROW && IsU8(s->arr->sub)) return true;
            return ValueOk(s, why);
        }
        if (t->kind == TY_SLICE) return ValueOk(t->sub, why);
        return ValueOk(t, why);
    }

    // TypeCheck::ScanForeignFrom: whether the body holds `return ... from f`
    // for an f not declared within.
    static void ScanForeignFrom(Node *n, set<string_view> &names, bool &foreign) {
        if (!n || foreign) return;
        if (auto r = Is<Return>(n)) {
            if (!r->from.empty() && !names.count(r->from)) foreign = true;
        } else if (auto fd = Is<FnDecl>(n)) {
            names.insert(fd->sf->name);
            if (fd->sf->body) ScanForeignFrom(fd->sf->body, names, foreign);
            return;
        }
        n->Children([&](Node *c) { ScanForeignFrom(c, names, foreign); });
    }

    // CheckExport's checks of sf, in its order: the first that fails is its
    // error, why, as it is there.
    bool Check(SFunction *sf, string &why) {
        auto fail = [&](string w) {
            why = std::move(w);
            return false;
        };
        if (sf->isnested || sf->isthread || sf->isextern) return fail("must be a top-level Goose function");
        if (!sf->generics.empty()) return fail("cannot be generic");
        string_view n = sf->cname;
        if (n.empty()) return fail("needs a C symbol");
        if (n == "main" || n == "goose_init") return fail(cat("C symbol ", n, " is reserved"));
        if (n.rfind("gs_", 0) == 0) return fail("C symbols beginning with gs_ are reserved for Goose internals");
        if (n[0] == '_' || !isalpha((unsigned char)n[0]) ||
            !all_of(n.begin(), n.end(), [](char c) { return isalnum((unsigned char)c) || c == '_'; }))
            return fail("C symbol must be a C identifier");
        if (IsCKeyword(n)) return fail("C symbol cannot be a C keyword");
        for (auto other : ast.functions)
            if (other != sf && other->isexport && other->cname == sf->cname)
                return fail(cat("C symbol ", n, " is exported more than once"));
        for (auto &p : sf->params) {
            if (!p.type) return fail(cat("parameter ", p.name, " needs a type"));
            if (p.defaultval) return fail(cat("parameter ", p.name, " cannot have a default"));
        }
        set<string_view> names = { sf->name };
        auto foreign = false;
        if (sf->body) ScanForeignFrom(sf->body, names, foreign);
        if (foreign) return fail("cannot use return ... from across the C boundary");
        if (sf->specs.size() != 1) return fail("could not resolve one concrete function signature");
        auto spec = sf->specs[0];
        // Its check failed, which was reported: nothing more can be said.
        if (spec->failed) return true;
        if (spec->params.size() != sf->params.size()) return fail("could not resolve one concrete function signature");
        for (size_t i = 0; i < spec->argtypes.size(); i++) {
            auto t = spec->argtypes[i];
            auto pn = sf->params[i].name;
            if (GoStringsT(t)) continue;
            if (GoStringsLikeT(t))
                return fail(cat("parameter ", pn, " of type ", TypeStr(t), " cannot cross to Go: a Go []string crosses "
                                "as a const (const u8[:])[:], whose strings, as Go's, nothing writes"));
            string w;
            if (!ParamOk(t, w)) return fail(cat("parameter ", pn, " of type ", TypeStr(t), " cannot cross to C: ", w));
            if (CarriesPool(t)) return fail(cat("parameter ", pn, " uses a reusable pool and has no public C representation"));
            if (t->kind == TY_REF && t->ref->sub->kind == TY_ARRAY && t->ref->sub->arr->akind == A_GROW &&
                IsU8(t->ref->sub->arr->sub))
                return fail(cat("parameter ", pn, " is a Goose string builder and cannot be passed from C"));
        }
        if (spec->rets.size() > 1) return fail("returns at most one value");
        if (!sf->has_rets && !spec->rets.empty()) return fail("a returned value needs an explicit return type");
        for (auto t : spec->rets) {
            if (t->kind == TY_SLICE && IsU8(t->sub)) {
                if (!t->cq) return fail("a returned u8 slice must be read-only (const u8[:])");
                continue;
            }
            string w;
            if (!ValueOk(t, w)) return fail(cat("cannot return ", TypeStr(t), " to C: ", w));
        }
        return true;
    }
};

// The export fns the checker's check of an export's boundary would refuse
// for a parameter that is a slice of u8 slices, which --target=go checks
// itself (GoExportCheck): while the program is checked they are no exports,
// and checked as a function nothing calls is, which is how an export is.
inline vector<SFunction *> HideGoExports(Ast &ast) {
    vector<SFunction *> hidden;
    for (auto sf : ast.functions) {
        if (!sf->isexport) continue;
        auto strings = false;
        for (auto &p : sf->params) strings = strings || GoStringsLikeT(p.type);
        if (!strings) continue;
        sf->isexport = false;
        hidden.push_back(sf);
    }
    return hidden;
}

// The hidden export fns again, each checked as the checker checks one but for
// a []string, each error reported as the checker reports one, at the fn.
inline void CheckGoExports(Ast &ast, const vector<SFunction *> &hidden) {
    for (auto sf : hidden) sf->isexport = true;
    GoExportCheck ck { ast };
    for (auto sf : hidden) {
        // A broken signature was reported where it was found.
        if (sf->brokensig) continue;
        string why;
        if (ck.Check(sf, why)) continue;
        Diagnostic d;
        d.where = LocOf(sf->line);
        d.message = cat("export fn ", sf->name, ": ", why);
        ast.Report(d);
    }
}

// A parameter's Go name in an entry point: its Goose name where Go takes
// that, else with the _g the program's names have; none of them gs, which
// the block is, or gs_r, the result.
static string GoExportParam(string_view name, set<string> &taken) {
    string s(name);
    for (auto &c : s) if (!isalnum((unsigned char)c)) c = '_';
    if (s.empty() || isdigit((unsigned char)s[0])) s = "p" + s;
    if (GoReservedWord(s) || s.rfind("gs", 0) == 0 || s.rfind("Gs", 0) == 0) s += "_g";
    auto base = s;
    for (auto n = 2; taken.count(s); n++) s = cat(base, "_", n);
    taken.insert(s);
    return s;
}

// Whether a value of type t holds an address in its own bytes: it is a
// reference or a slice, or has a field or an element at any depth that is
// one (whatever what it points at holds).
inline bool CodeGenGo::HoldsAddr(TypeExpr *t) {
    set<const void *> open;
    function<bool(TypeExpr *)> rec = [&](TypeExpr *x) -> bool {
        switch (x->kind) {
            case TY_REF: case TY_SLICE: return true;
            case TY_ARRAY: return rec(x->arr->sub);
            case TY_STRUCT: case TY_ENUM: case TY_VARIANT:
                for (auto &run : FieldRuns(x)) {
                    if (!open.insert(run.ftypes).second) continue;
                    for (auto ft : *run.ftypes) if (ft && rec(ft)) return true;
                }
                return false;
            default: return false;
        }
    };
    return rec(t);
}

// Whether every address an export fn's entry points hand its function stays
// in Go's own variables while the call runs (issue #22): its parameters and
// locals of slice and pointer types, and those of the functions it calls,
// whose flows Go's escape analysis follows, whose pointers its collector
// keeps alive and which it moves with a goroutine's stack. The entry points
// then hand Go's slices and pointers on as they are: no GsEscape, which
// would keep a caller's array off its stack for nothing, and no
// runtime.KeepAlive, which would keep alive what Go's own copies do; and Go
// can inline each into its caller as the call of the function. So it is
// where nothing the export's calls reach, its own body included, can put
// an address anywhere else, or hand it to code that may:
// * none takes the block: there is no data stack, fixed-frame stack or
//   program instance, which the block holds, Go's collector does not scan
//   and Go does not move;
// * none has a value of a type holding an address but a plain or optional
//   reference or a slice of what holds none: no struct, ADT or array
//   holding one (a struct's Go value is then its bytes, which hide it), no
//   reference to one;
// * none names a global var that can hold one;
// * none hands one to an extern fn, but to the runtime's own that only
//   read it (std's find_any and its pair, frexp, os's files): the Go of
//   another may keep it anywhere;
// * none takes free variables, a caller's own locals, or returns from
//   another function (§7.9), whose values wait in globals.
// Anything else is taken to put it where Go does not see it, and the entry
// points move the arguments off a caller's stack and keep them alive, as
// they always have.
inline bool CodeGenGo::ArgsStayInGo(FnSpec *sp) {
    auto ok = [&](TypeExpr *t) {
        if (!t || t->kind == TY_VOID || t->kind == TY_FN || t->kind == TY_GENERIC || !HoldsAddr(t)) return true;
        // A slice of what holds no address, or of const slices of it, as a
        // Go []string is (issue #42): the slices it holds are where Go's
        // collector sees them, in the memory Go laid out as strings.
        if (t->kind == TY_SLICE)
            return !HoldsAddr(t->sub) || (t->cq && t->sub->kind == TY_SLICE && t->sub->cq && !HoldsAddr(t->sub->sub));
        return t->kind == TY_REF && !IsRelRef(t) && !IsFatRef(t) && !CarriesPool(t) && !HoldsAddr(t->ref->sub);
    };
    set<FnSpec *> seen;
    vector<FnSpec *> work = { sp };
    while (!work.empty()) {
        auto f = work.back();
        work.pop_back();
        if (!seen.insert(f).second) continue;
        if (!sinfo.count(f) || !f->body) return false;
        auto &fi = sinfo[f];
        auto takesgs = fi.needssp || fi.needsgs || !specnum.count(f) || gsneeds.size() != numspec.size() ||
                       gsneeds[specnum[f]];
        if (takesgs || !fi.freevars.empty() || fi.hasrf || fromids.count(f)) return false;
        for (auto t : f->argtypes) if (!ok(t)) return false;
        for (auto t : f->rets) if (!ok(t)) return false;
        auto stays = true;
        function<void(Node *)> walk = [&](Node *n) {
            if (!n || !stays) return;
            stays = ok(n->exprtype);
            auto var = [&](VarDef *v) {
                if (v && (!ok(v->type) || (v->isglobal && v->isvar && HoldsAddr(v->type)))) stays = false;
            };
            if (auto id = Is<Ident>(n)) var(id->vdef);
            if (auto d = Is<VarDecl>(n)) for (auto v : d->defs) var(v);
            if (auto fl = Is<ForLoop>(n)) {
                var(fl->vdef);
                var(fl->idxdef);
            }
            if (auto m = Is<MatchExpr>(n)) for (auto &arm : m->arms) var(arm.binder);
            if (auto c = Is<Call>(n)) {
                for (auto t : c->rettypes) stays = stays && ok(t);
                for (auto v : c->fvparams) var(v);
                if (c->builtin < 0 && c->spec && c->spec->sf->isextern && !ExportOfExtern(c->spec)) {
                    auto &name = c->spec->sf->cname;
                    auto reads = name == "gs_find_any" || name == "gs_find_pair" || name == "frexp" || GoOsFn(name);
                    for (auto t : c->spec->argtypes) stays = stays && (reads || !HoldsAddr(t));
                }
            }
            if (stays) RunChildren(n, walk);
        };
        walk(f->body);
        if (!stays) return false;
        for (auto k : calleesof[f]) work.push_back(k);
    }
    return true;
}

// The names of the types the Go type gt names (its identifiers): a host
// names them too.
static void GoTypeNames(const string &gt, set<string> &names) {
    for (size_t i = 0; i < gt.size();) {
        if (!(isalpha((unsigned char)gt[i]) || gt[i] == '_')) {
            i++;
            continue;
        }
        auto j = i;
        while (j < gt.size() && (isalnum((unsigned char)gt[j]) || gt[j] == '_')) j++;
        names.insert(gt.substr(i, j - i));
        i = j;
    }
}

// The Go entry points of the program's export fns (DESIGN.md §3.5), into
// xcode, once the program's data stack counts are known and which
// functions take the block: the export fn's own count is what its
// entry points make sure the block has. An export's calls may not reach a
// global's own data stack, which the program's block holds (its index is
// one there), where an entry point runs on another.
inline void CodeGenGo::EmitExports() {
    set<string> names;
    // Every export fn's Name and NameWithStack, which no NameString or
    // NameBytes of another's may take.
    set<string> primary;
    for (auto sf : ast.functions) {
        if (!sf->isexport) continue;
        primary.insert(GoExportName(sf->cname));
        primary.insert(cat(GoExportName(sf->cname), "WithStack"));
    }
    for (auto sf : ast.functions) {
        if (!sf->isexport) continue;
        if (sf->specs.size() != 1 || !sinfo.count(sf->specs[0]))
            Fail(sf->line, cat("internal: export fn ", sf->name, " has no emitted specialization"));
        auto sp = sf->specs[0];
        auto &si = sinfo[sp];
        auto ln = sf->line;
        if (!si.freevars.empty()) Fail(ln, cat("internal: export fn ", sf->name, " has free variables"));
        auto name = GoExportName(sf->cname);
        string why;
        if (!GoForeignNameOk(name, why))
            Fail(ln, cat("export fn ", sf->name, ": its Go entry point is ", name, ", and ", why,
                         ": name another with export \"symbol\" fn"));
        auto with = cat(name, "WithStack");
        for (auto &n : { name, with })
            if (!names.insert(n).second)
                Fail(ln, cat("export fn ", sf->name, ": another export fn's Go entry point is ", n, " too"));
        entrynames.insert(name);
        // --go-no-withstack writes no NameWithStack: Name runs on a block
        // through a function of the program's own, gsWithStack_Name.
        if (opts.nowithstack) with = cat("gsWithStack_", name);
        else entrynames.insert(with);
        // What its calls reach, its own body included. In a program with
        // workers a call runs on an instance of its block's own, whose
        // globals' stacks are the block's (codegen_go_threads.h).
        set<FnSpec *> seen;
        vector<FnSpec *> work = { sp };
        while (!work.empty() && !workers) {
            auto f = work.back();
            work.pop_back();
            if (!seen.insert(f).second) continue;
            if (gstkusers.count(f))
                Unsupported(ln, cat("export fn ", sf->name, ", whose calls reach the data stack of a global that "
                                    "lives on one of its own (growing it, or handing on a reference to it), which "
                                    "the program's block holds,"));
            for (auto k : calleesof[f]) work.push_back(k);
        }
        auto stacks = ProgramStacksOf(*this, { sp });
        auto takesgs = si.needssp || si.needsgs || (specnum.count(sp) && gsneeds[specnum[sp]]);
        // Where every address the function is handed stays in Go's own
        // variables, the entry points hand the arguments on as they are
        // (ArgsStayInGo, issue #22). Elsewhere they keep what the arguments
        // view off Go's moving goroutine stacks (GsEscape: a caller's array
        // is on the heap, or static) and alive until the call returns.
        auto stay = !takesgs && ArgsStayInGo(sp);
        // The parameters: the Go types the entry points take, and the Goose
        // values they hand the function; and those that view Go's memory,
        // which the call keeps alive (kept). NameString and NameBytes take
        // each const u8[:] as a string (sparams, sargs) and as a []byte
        // (bparams, bargs), where Name takes either (a type parameter).
        set<string> taken;
        vector<string> tparams, params, fwd, args, kept, sparams, sargs, bparams, bargs, bytesnames;
        for (size_t i = 0; i < sp->argtypes.size(); i++) {
            auto pt = sp->argtypes[i];
            auto pn = GoExportParam(i < sf->params.size() ? sf->params[i].name : string_view(), taken);
            // The slice of what a Go value views, from its data's address.
            auto view = [&](const string &data) { return cat("GsSlice{Data: ", data, ", Len: int64(len(", pn, "))}"); };
            string gt, arg, sarg, barg;
            if (pt->kind == TY_SLICE && IsU8(pt->sub) && pt->cq) {
                auto tp = cat("T", tparams.size());
                tparams.push_back(cat(tp, " ~string | ~[]byte"));
                gt = tp;
                // A string's header begins as a slice's does, with its data:
                // a type parameter's is read through its address.
                arg = stay ? view(cat("*(*unsafe.Pointer)(unsafe.Pointer(&", pn, "))")) : cat("GsBytesOf(", pn, ")");
                sarg = stay ? view(cat("unsafe.Pointer(unsafe.StringData(", pn, "))")) : cat("GsSliceFromString(", pn, ")");
                barg = stay ? view(cat("unsafe.Pointer(unsafe.SliceData(", pn, "))")) : cat("GsSliceFromBytes(", pn, ")");
                bytesnames.push_back(pn);
                kept.push_back(pn);
            } else if (GoStringsT(pt)) {
                // A []string's strings are its elements, each the data and
                // the length of a const u8[:] (issue #42).
                gt = "[]string";
                arg = stay ? view(cat("unsafe.Pointer(unsafe.SliceData(", pn, "))")) : cat("GsSliceOf(", pn, ")");
                kept.push_back(pn);
            } else if (pt->kind == TY_SLICE) {
                gt = IsU8(pt->sub) ? string("[]byte") : cat("[]", GoT(pt->sub, ln));
                arg = stay ? view(cat("unsafe.Pointer(unsafe.SliceData(", pn, "))")) : cat("GsSliceOf(", pn, ")");
                kept.push_back(pn);
            } else if (IsPlainRef(pt) && !pt->ref->optional && IsFix(pt->ref->sub)) {
                auto st = GoT(pt->ref->sub, ln);
                gt = cat("*", st);
                arg = stay && !RefIsAddr(pt) ? pn
                      : RefFromAddr(pt, stay ? cat("unsafe.Pointer(", pn, ")") : cat("GsEscape(unsafe.Pointer(", pn, "))"));
                kept.push_back(pn);
            } else if (IsFix(pt) && pt->kind != TY_REF) {
                gt = GoT(pt, ln);
                arg = pn;
            } else {
                Fail(ln, cat("internal: export fn ", sf->name, " takes ", TypeText(pt)));
            }
            params.push_back(cat(pn, " ", gt));
            sparams.push_back(cat(pn, " ", sarg.empty() ? gt : string("string")));
            bparams.push_back(cat(pn, " ", barg.empty() ? gt : string("[]byte")));
            GoTypeNames(gt, sigtypes);
            fwd.push_back(pn);
            args.push_back(arg);
            sargs.push_back(sarg.empty() ? arg : sarg);
            bargs.push_back(barg.empty() ? arg : barg);
        }
        // What stays in Go's variables Go keeps alive.
        if (stay) kept.clear();
        // The result, and what makes the function's the Go one.
        string rt, conv;
        if (sp->rets.size() > 1) Fail(ln, cat("internal: export fn ", sf->name, " returns more than one value"));
        if (!sp->rets.empty()) {
            auto t = sp->rets[0];
            if (t->kind == TY_SLICE && IsU8(t->sub)) {
                rt = "[]byte";
                conv = "GsSliceToBytes";
            } else if (t->kind == TY_SLICE) {
                rt = cat("[]", GoT(t->sub, ln));
                conv = cat("GsSliceToSlice[", GoT(t->sub, ln), "]");
            } else if (IsFix(t) && t->kind != TY_REF) {
                rt = GoT(t, ln);
            } else {
                Fail(ln, cat("internal: export fn ", sf->name, " returns ", TypeText(t)));
            }
            GoTypeNames(rt, sigtypes);
        }
        auto ret = [&](const string &x) {
            if (rt.empty()) return x;
            return cat("return ", conv.empty() ? x : cat(conv, "(", x, ")"));
        };
        // The call x of the function, as the entry point's last statements.
        // Go keeps a value alive only up to its last use, and a caller's last
        // use of an argument may be the call itself, where the Goose code may
        // keep what the argument views only where the collector does not
        // look (a data stack, the fixed-frame stack, the bytes of a value):
        // every argument viewing Go's memory is kept alive until the call
        // returns (issue #30).
        auto finish = [&](const string &x) {
            if (kept.empty()) return cat("\t", ret(x), "\n");
            auto s = rt.empty() ? cat("\t", x, "\n") : cat("\tgs_r := ", conv.empty() ? x : cat(conv, "(", x, ")"), "\n");
            for (auto &k : kept) Append(s, "\truntime.KeepAlive(", k, ")\n");
            if (!rt.empty()) s += "\treturn gs_r\n";
            return s;
        };
        // In a program with workers, a call that takes the block runs on its
        // program instance (§11.2): one of the block's own, which a block a
        // caller keeps keeps across its calls, and one copied for the call
        // from the package's where the block comes from the pool.
        auto inst = workers && takesgs;
        // The functions filling the instance, emitted before the entry points.
        auto enterall = inst ? EnsureInstanceFn(nullptr) : string();
        auto enter = inst ? EnsureInstanceFn(sp) : string();
        string call = cat(si.name, "(", si.needssp ? "gs, 0" : takesgs ? "gs" : "");
        for (size_t i = 0; i < args.size(); i++) Append(call, i || takesgs ? ", " : "", args[i]);
        call += ")";
        auto join = [](const vector<string> &v, const string &first) {
            string s = first;
            for (auto &x : v) Append(s, s.empty() ? "" : ", ", x);
            return s;
        };
        string tps;
        if (!tparams.empty()) tps = cat("[", join(tparams, ""), "]");
        auto retsig = rt.empty() ? string() : cat(" ", rt);
        auto where = cat("export fn ", sf->name, " (", ast.sources[ln.fileidx].first, ":", ln.line, ")");
        for (auto &c : where) if ((uint8_t)c < 32 || (uint8_t)c >= 127) c = '?';
        // The Goose documentation, as Go's: a paragraph of the text of its
        // lines, which go/doc/comment lays out as it is (gofmt lays a doc
        // comment out as go/doc/comment does, which would make an indented
        // line code, say).
        string doc;
        for (size_t p = 0; p < sf->doc.size();) {
            auto e = sf->doc.find('\n', p);
            if (e == string_view::npos) e = sf->doc.size();
            auto line = sf->doc.substr(p, e - p);
            p = e + 1;
            auto trim = [&] {
                auto b = line.find_first_not_of(" \t\r");
                line = b == string_view::npos ? string_view() : line.substr(b);
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t'))
                    line.remove_suffix(1);
            };
            trim();
            if (line.substr(0, 2) == "//") {
                line.remove_prefix(2);
                trim();
            }
            if (line.empty()) continue;
            string l(line);
            for (auto &c : l) if ((uint8_t)c < 32 && c != '\t') c = ' ';
            Append(doc, "// ", l, "\n");
        }
        if (!doc.empty()) doc = cat("//\n", doc);
        // A library starts at the first call of either entry point
        // (gsLibStart, EmitProgram), the pool's before it takes a block. A
        // call on a block turns panic on fault on for itself only, so that a
        // fault in a guard gap is a panic on the caller's goroutine, and gives
        // the caller's setting back as it returns or panics (issue #18).
        auto start = libstart ? string("\tgsLibStart()\n") : string();
        auto faults = takesgs ? string("\tdefer debug.SetPanicOnFault(debug.SetPanicOnFault(true))\n") : string();
        auto stackstext = cat(stacks, stacks == 1 ? " data stack" : " data stacks");
        if (!opts.nowithstack) {
            Append(xcode, "// ", with, " runs ", where, "\n",
                   !takesgs ? string("// with no data stacks: it leaves gs alone.\n")
                   : stacks ? cat("// on the ", stackstext, " of gs it takes, which it makes sure gs has, and gives\n"
                                  "// ", stacks == 1 ? "it" : "them", " back as it found ", stacks == 1 ? "it" : "them", ".\n")
                            : string("// on gs, whose fixed-frame stack it gives back as it found it.\n"),
                   inst ? "// It runs on gs's program instance, whose globals stay gs's across calls: a copy of\n"
                          "// the package's, made where gs has none (gsEnter).\n" : "",
                   doc, "func ", with, tps, "(", join(params, cat("gs *", BlockTypeName())), ")", retsig, " {\n",
                   start, faults);
            hostblocks = hostblocks || takesgs;
        } else if (takesgs) {
            // Name's body on its block, with no start of its own: Name has made it.
            Append(xcode, "// ", with, " runs ", where, "\n// on gs, for ", name, ", which --go-no-withstack writes alone.\n",
                   "func ", with, tps, "(", join(params, "gs *GsStackBlock"), ")", retsig, " {\n", faults);
        }
        if (!opts.nowithstack || takesgs) {
            if (stacks) Append(xcode, "\tgs.EnsureStacks(", stacks, ")\n");
            else if (!takesgs) xcode += "\t_ = gs\n";
            if (inst) Append(xcode, "\tif gs.Gl == nil {\n\t\t", enterall, "(gs)\n\t}\n");
            Append(xcode, finish(call), "}\n\n");
        }
        Append(xcode, "// ", name, " runs ", where, "\n",
               !takesgs ? string("// with no data stacks.\n")
                        : string("// on a block of data stacks from its program's pool, which it gives back as it\n"
                                 "// returns.\n"),
               inst ? "// It runs on a program instance of its own, copied from the package's for the call.\n" : "",
               doc, "func ", name, tps, "(", join(params, ""), ")", retsig, " {\n", start);
        if (!takesgs) {
            // Nothing of the block's is used: the function is called itself.
            Append(xcode, finish(cat(si.name, "(", join(args, ""), ")")), "}\n\n");
        } else {
            Append(xcode, "\tgs := gsProg.AcquireStackBlock(", stacks, ")\n");
            if (inst) Append(xcode, "\t", enter, "(gs)\n");
            auto fcall = cat(with, "(", join(fwd, "gs"), ")");
            if (rt.empty()) {
                Append(xcode, "\t", fcall, "\n\tgsProg.ReleaseStackBlock(gs)\n}\n\n");
            } else {
                // A slice of the call's instance's globals is gone with it: it
                // crosses as a copy.
                auto keep = inst && rt[0] == '[';
                Append(xcode, "\tgs_r := ", fcall, "\n", keep ? "\tgs_r = GsKeep(gs, gs_r)\n" : "",
                       "\tgsProg.ReleaseStackBlock(gs)\n\treturn gs_r\n}\n\n");
            }
        }
        // Where a const u8[:] makes Name generic, NameString and NameBytes
        // are Name taking each one as a string and as a []byte (issue #22).
        // Go makes of a call of either, where the function takes no block,
        // the call of the function itself, which it cannot of Name's: Name
        // reads its type parameter's data through its address, which takes a
        // frame. One taking a block is Name's own call. A form whose name
        // another export fn's Name or NameWithStack has is left out.
        if (!bytesnames.empty()) {
            string list;
            for (size_t i = 0; i < bytesnames.size(); i++)
                Append(list, i == 0 ? "" : i + 1 == bytesnames.size() ? " and " : ", ", bytesnames[i]);
            auto plural = bytesnames.size() > 1;
            for (auto bytes : { false, true }) {
                auto vname = cat(name, bytes ? "Bytes" : "String");
                if (primary.count(vname) || !names.insert(vname).second) continue;
                entrynames.insert(vname);
                Append(xcode, "// ", vname, " runs ", where, "\n// as ", name, " does, taking ", list, " as ",
                       bytes ? (plural ? "[]bytes" : "a []byte") : (plural ? "strings" : "a string"), ".\n",
                       doc, "func ", vname, "(", join(bytes ? bparams : sparams, ""), ")", retsig, " {\n");
                if (!takesgs)
                    Append(xcode, start, finish(cat(si.name, "(", join(bytes ? bargs : sargs, ""), ")")), "}\n\n");
                else
                    Append(xcode, "\t", rt.empty() ? "" : "return ", name, "(", join(fwd, ""), ")\n}\n\n");
            }
        }
    }
}

// ------------------------------------------------------------------
// --include files.

// An --include file's Go (text) for the generated file: its imports, each
// as Go writes one (`"fmt"`, `m "math"`), into imports, and the rest of it
// into body, with its package clause and build constraints, and the imports,
// blanked so that every line keeps its number (GoIncludeLines numbers them),
// and the comments left among them starting their lines, as gofmt starts a
// comment at the package level. False, with why, where it is not Go of that
// shape: a package clause, then imports, then the rest.
static bool GoSplitInclude(const string &text, vector<pair<string, string>> &imports, string &body,
                           string &why) {
    body = text;
    size_t p = 0;
    if (body.compare(0, 3, "\xEF\xBB\xBF") == 0) {
        body.replace(0, 3, "   ");
        p = 3;
    }
    auto blank = [&](size_t from, size_t to) {
        for (auto i = from; i < to && i < body.size(); i++)
            if (body[i] != '\n') body[i] = ' ';
    };
    // Past spaces and comments; build constraints, which belong before a
    // package clause, are blanked with it.
    auto skip = [&](bool constraints) {
        for (;;) {
            while (p < body.size() && isspace((unsigned char)body[p])) p++;
            if (body.compare(p, 2, "//") == 0) {
                auto e = body.find('\n', p);
                if (e == string::npos) e = body.size();
                if (constraints && (body.compare(p, 10, "//go:build") == 0 || body.compare(p, 9, "// +build") == 0))
                    blank(p, e);
                p = e;
            } else if (body.compare(p, 2, "/*") == 0) {
                auto e = body.find("*/", p + 2);
                p = e == string::npos ? body.size() : e + 2;
            } else {
                return;
            }
        }
    };
    auto isid = [](char c) { return isalnum((unsigned char)c) || c == '_'; };
    auto word = [&]() {
        auto b = p;
        while (p < body.size() && isid(body[p])) p++;
        return body.substr(b, p - b);
    };
    skip(true);
    auto at = p;
    if (word() != "package") {
        why = "it does not start with a package clause";
        return false;
    }
    skip(false);
    if (word().empty()) {
        why = "its package clause names no package";
        return false;
    }
    blank(at, p);
    // An import spec: an optional name, then the path's literal.
    auto spec = [&]() -> bool {
        string name;
        if (p < body.size() && (body[p] == '.' || body[p] == '_' || isalpha((unsigned char)body[p]))) {
            if (body[p] == '.') {
                name = ".";
                p++;
            } else {
                name = word();
            }
            skip(false);
        }
        if (p >= body.size() || (body[p] != '"' && body[p] != '`')) {
            why = "an import names no path";
            return false;
        }
        auto q = body[p];
        auto b = p++;
        while (p < body.size() && body[p] != q && body[p] != '\n') p += body[p] == '\\' && q == '"' ? 2 : 1;
        if (p >= body.size() || body[p] != q) {
            why = "an import's path is not a string";
            return false;
        }
        p++;
        auto path = body.substr(b + 1, p - b - 2);
        if (path == "C") {
            why = "it imports \"C\": the Go a --target=go compile writes builds without cgo";
            return false;
        }
        imports.push_back({ name, path });
        return true;
    };
    for (;;) {
        skip(false);
        at = p;
        auto save = p;
        if (word() != "import") {
            p = save;
            break;
        }
        skip(false);
        if (p < body.size() && body[p] == '(') {
            p++;
            for (;;) {
                skip(false);
                if (p < body.size() && body[p] == ')') {
                    p++;
                    break;
                }
                if (p >= body.size()) {
                    why = "an import declaration does not end";
                    return false;
                }
                if (!spec()) return false;
                skip(false);
                if (p < body.size() && body[p] == ';') p++;
            }
        } else if (!spec()) {
            return false;
        }
        while (p < body.size() && (body[p] == ' ' || body[p] == '\t' || body[p] == ';')) p++;
        blank(at, p);
    }
    // The lines up to the rest's first, without the blanks that start them.
    auto end = std::min(body.find('\n', p), body.size());
    string head;
    for (size_t pos = 0; pos < end;) {
        auto nl = std::min(body.find('\n', pos), end);
        auto b = std::min(body.find_first_not_of(" \t", pos), nl);
        Append(head, string_view(body).substr(b, nl - b), nl < end ? "\n" : "");
        pos = nl + 1;
    }
    body = head + body.substr(end);
    return true;
}

}  // namespace goose
