// Goose compiler — the Go backend's names, types and values (definitions of
// CodeGenGo members, codegen_go.h): literals, variables, operators and
// conversions (§6.1–§6.3), and the routing of a value to its destination.
#pragma once

namespace goose {

// One string constant per source file, which an abort names with the line
// (gs_file<N> in the C backend).
inline string CodeGenGo::FileRef(Line l) {
    auto it = filerefs.find(l.fileidx);
    if (it == filerefs.end()) {
        auto f = l.fileidx >= 0 && l.fileidx < (int)ast.sources.size()
                     ? ast.sources[l.fileidx].first : string("?");
        for (auto &c : f) if (c == '\\') c = '/';
        auto name = Unique(cat("gsFile", filerefs.size()));
        Append(filedecls, "const ", name, " = ", GoStr(f), "\n");
        it = filerefs.emplace(l.fileidx, name).first;
    }
    return it->second;
}

// ------------------------------------------------------------------
// Names: the C backend's (CodeGen::Sanitize), whose _g suffix no Go keyword,
// predeclared name or runtime name has.

inline string CodeGenGo::Sanitize(string_view name) {
    string s(name);
    if (s.empty()) s = "_";
    for (auto &c : s) if (c == '#') c = '_';
    return s + "_g";
}

inline string CodeGenGo::Sanitize(string_view ns, string_view name) {
    if (ns.empty()) return Sanitize(name);
    return cat(ns, "_", name, "_g", ns.size());
}

inline string CodeGenGo::Unique(string base) {
    if (!used.count(base)) { used.insert(base); return base; }
    for (auto n = 2;; n++) {
        auto s = cat(base, "_", n);
        if (!used.count(s)) { used.insert(s); return s; }
    }
}

// ------------------------------------------------------------------
// Types.

inline const char *CodeGenGo::IntGoT(IntStorage s) {
    switch (s) {
        case IS_I8:  return "int8";
        case IS_I16: return "int16";
        case IS_I32: return "int32";
        case IS_I64: return "int64";
        case IS_U8:  return "uint8";
        case IS_U16: return "uint16";
        case IS_U32: return "uint32";
        case IS_U64: return "uint64";
        default:     return "int64";
    }
}

// A type as an error message names it.
inline string CodeGenGo::TypeText(TypeExpr *t) {
    if (!t) return "a value without a type";
    string s;
    t->Dump(s, 200);
    if (s.size() > 200) s.resize(200);
    return cat("`", s, "`");
}

inline bool CodeGenGo::SameGoT(TypeExpr *a, TypeExpr *b) {
    if (!a || !b) return a == b;
    if (!IsScalarT(a) || !IsScalarT(b)) return a == b || (a->kind == b->kind && TEq(a, b));
    if (a->kind != b->kind) return false;
    if (a->kind == TY_INT) return a->intstorage == b->intstorage;
    if (a->kind == TY_FLT) return a->fltstorage == b->fltstorage;
    return true;
}

// ------------------------------------------------------------------
// Expression text.

// Whether evaluating n can run statements: a call, or a construct with
// statements of its own (CodeGen::HasStmts). A value read before such an
// operand is taken into a temporary first, since Go leaves the order of a
// variable's read and a call's effects in one expression open.
inline bool CodeGenGo::HasStmts(Node *n) {
    if (!n) return false;
    if (Is<Call>(n) || IsCtl(n)) return true;
    auto found = false;
    RunChildren(n, [&](Node *c) { found = found || HasStmts(c); });
    return found;
}

// x = gen(), where generating it emits no statement: a value that can sit in
// a `for`'s condition or on the right of a `&&`, which runs only where the
// left side lets it. Where it would emit statements, nothing of it is kept
// and the caller takes the form with statements. HasStmts says which nodes
// call or run constructs; some others (a struct's ==, a literal) still emit
// statements of their own, which this finds.
inline bool CodeGenGo::TryPure(const function<string()> &gen, string &x) {
    auto mark = body.size();
    auto savetmp = tmpn;
    // What the attempt takes in the open scopes -- the locals it declares,
    // the stacks it claims and the tops it saves -- goes with its code.
    auto savenext = stknext, savemax = stkmax;
    auto saveopen = openat;
    vector<size_t> nsaves, ndefers;
    for (auto &s : scopes) {
        nsaves.push_back(s.saves.size());
        ndefers.push_back(s.defers.size());
    }
    x = gen();
    if (body.size() == mark) return true;
    body.resize(mark);
    tmpn = savetmp;
    stknext = savenext;
    stkmax = savemax;
    openat = std::move(saveopen);
    for (size_t i = 0; i < nsaves.size() && i < scopes.size(); i++) {
        scopes[i].saves.resize(nsaves[i]);
        scopes[i].defers.resize(ndefers[i]);
    }
    return false;
}

inline bool CodeGenGo::IsGoName(const string &x) {
    return !x.empty() && !isdigit((unsigned char)x[0]) &&
           all_of(x.begin(), x.end(), [](char c) { return isalnum((unsigned char)c) || c == '_'; });
}

// An integer constant as GoIntLit spells it, `T(v)` or a bare decimal, and
// its value; uns: v holds a u64 above i64.max as its bits.
inline bool CodeGenGo::GoConstInt(const string &x, int64_t &v, bool &uns) {
    string s = x;
    auto open = s.find('(');
    if (open != string::npos) {
        if (s.back() != ')' || !IsGoName(s.substr(0, open))) return false;
        s = s.substr(open + 1, s.size() - open - 2);
    }
    if (s.empty()) return false;
    auto neg = s[0] == '-';
    auto digits = s.substr(neg ? 1 : 0);
    if (digits.empty() || digits.find_first_not_of("0123456789") != string::npos) return false;
    errno = 0;
    if (neg) {
        v = strtoll(s.c_str(), nullptr, 10);
        uns = false;
    } else {
        auto u = strtoull(s.c_str(), nullptr, 10);
        v = (int64_t)u;
        uns = u > (uint64_t)INT64_MAX;
    }
    return errno == 0;
}

// Whether x is a Go constant, which Go evaluates at compile time and
// rejects where the result does not fit its type, or divides by zero: what
// GoIntLit and GoFltLit spell, and a bool literal. Those are the only
// constants the backend writes.
inline bool CodeGenGo::IsGoConst(const string &x) {
    if (x == "true" || x == "false") return true;
    int64_t v;
    bool uns;
    if (GoConstInt(x, v, uns)) return true;
    for (auto ft : { "float32(", "float64(" }) {
        auto n = strlen(ft);
        if (x.compare(0, n, ft) != 0 || x.back() != ')') continue;
        auto in = x.substr(n, x.size() - n - 1);
        return !in.empty() && in.find_first_not_of("0123456789.eE+-") == string::npos;
    }
    return false;
}

// x without the parentheses around the whole of it, as gofmt writes an if's
// or a for's condition.
inline string CodeGenGo::CondText(const string &x) {
    if (x.size() < 2 || x.front() != '(' || x.back() != ')') return x;
    int depth = 0;
    for (size_t i = 0; i < x.size(); i++) {
        auto c = x[i];
        if (c == '"') {
            for (i++; i < x.size() && x[i] != '"'; i++)
                if (x[i] == '\\') i++;
        } else if (c == '(') {
            depth++;
        } else if (c == ')' && --depth == 0 && i + 1 < x.size()) {
            return x;   // The first one closes before the end: `(a) && (b)`.
        }
    }
    return x.substr(1, x.size() - 2);
}

// x without any of the parentheses around the whole of it.
inline string CodeGenGo::GoUnparen(string x) {
    for (;;) {
        auto y = CondText(x);
        if (y == x) return x;
        x = std::move(y);
    }
}

// The parts of x that op joins outside every bracket and string literal:
// x alone where it joins none.
inline vector<string> CodeGenGo::GoSplitTop(const string &x, const string &op) {
    vector<string> parts;
    int depth = 0;
    size_t from = 0;
    for (size_t i = 0; i < x.size(); i++) {
        auto c = x[i];
        if (c == '"') {
            for (i++; i < x.size() && x[i] != '"'; i++)
                if (x[i] == '\\') i++;
        } else if (c == '(' || c == '[' || c == '{') {
            depth++;
        } else if (c == ')' || c == ']' || c == '}') {
            depth--;
        } else if (depth == 0 && x.compare(i, op.size(), op) == 0) {
            parts.push_back(x.substr(from, i - from));
            from = i + op.size();
            i = from - 1;
        }
    }
    parts.push_back(x.substr(from));
    return parts;
}

// The operands Go's vet takes a chain of `&&`s (or of `||`s) to have: the
// ones the operator joins, through parentheses, and theirs in turn. One
// whose top level `||` joins is a single operand of a chain of `&&`s, which
// bind more tightly.
inline void CodeGenGo::GoChainOperands(const string &x, bool andand, vector<string> &out) {
    auto s = GoUnparen(x);
    auto parts = GoSplitTop(s, andand ? " && " : " || ");
    if (parts.size() == 1 || (andand && GoSplitTop(s, " || ").size() > 1)) {
        out.push_back(s);
        return;
    }
    for (auto &p : parts) GoChainOperands(p, andand, out);
}

// Whether `l && r` (or `l || r`) has what Go's vet reports as a mistake,
// and Goose takes as written: an operand of the chain twice (`c && c`), or
// two comparisons of one value with different constants (`n != 1 || n !=
// 2`, `n == 1 && n == 2`). Any side two such comparisons share is taken for
// that value, constant or not. l and r have neither of their own.
inline bool CodeGenGo::VetBoolClash(const string &l, const string &r, bool andand) {
    vector<string> ops;
    GoChainOperands(l, andand, ops);
    auto nl = ops.size();
    GoChainOperands(r, andand, ops);
    auto key = [](const string &s) {
        string k;
        for (auto c : GoUnparen(s)) if (c != ' ') k += c;
        return k;
    };
    auto cmp = andand ? " == " : " != ";
    for (size_t i = 0; i < nl; i++) {
        auto a = GoSplitTop(ops[i], cmp);
        for (auto j = nl; j < ops.size(); j++) {
            if (key(ops[i]) == key(ops[j])) return true;
            auto b = GoSplitTop(ops[j], cmp);
            if (a.size() != 2 || b.size() != 2) continue;
            for (auto &x : a)
                for (auto &y : b)
                    if (key(x) == key(y)) return true;
        }
    }
    return false;
}

// A constant held in a variable, so that an operation on it is computed as
// the program computes it, wrapping and rounding at run time, rather than
// exactly by the Go compiler, which rejects a result its type cannot hold
// (`int8(100) + int8(100)`). The Go compiler still folds the value.
inline string CodeGenGo::Materialize(TypeExpr *t, const string &x) {
    if (!IsGoConst(x)) return x;
    auto v = T();
    Decl(v, LocalT(t, Line {}), x);
    return v;
}

// x, a value of type `from`, as a value of type `to`: Go converts nothing
// implicitly, where C converts a scalar to the type of the slot it lands in.
inline string CodeGenGo::Coerce(const string &x, TypeExpr *from, TypeExpr *to, Line ln) {
    // A varint as a Go value is the i64 it encodes (§3.6).
    if (to && IsVarintT(to)) to = ast.inttypes[IS_I64];
    if (from && IsVarintT(from)) from = ast.inttypes[IS_I64];
    if (!to || !from || SameGoT(from, to)) return x;
    if (!IsScalarT(from) || !IsScalarT(to)) return CoerceVal(x, from, to, ln);
    auto gt = GoT(to, ln);
    if (to->kind == TY_BOOL || from->kind == TY_BOOL)
        Fail(ln, cat("internal: no conversion from ", TypeText(from), " to ", TypeText(to)));
    // A constant converted is checked to fit by the Go compiler, where the
    // program truncates it: an integer constant is converted here, as C
    // converts it (to a float it rounds the same either way), and any other
    // one is held in a variable first.
    int64_t v;
    bool uns;
    if (from->kind == TY_INT && GoConstInt(x, v, uns)) {
        if (to->kind == TY_INT) return GoIntLit(WrapStorage(v, to->intstorage), false, to, ln);
        return GoIntLit(v, uns || (from->intstorage == IS_U64 && v < 0), to, ln);
    }
    return cat(gt, "(", Materialize(from, x), ")");
}

// Materialize a read now, before a later operand can change what it reads.
inline string CodeGenGo::Snapshot(TypeExpr *t, const string &x) {
    auto v = T();
    Decl(v, LocalT(t, Line {}), x);
    return v;
}

inline string CodeGenGo::GenPure(Node *n) {
    auto x = GenX(n);
    if (IsGoConst(x)) return x;
    return Snapshot(n->exprtype, x);
}

// An operand whose value must not change before it is used: when `later`,
// the code that runs before its use has statements of its own, which may
// write what it reads (CodeGen::GenOperandVal).
inline string CodeGenGo::GenOperandVal(Node *n, bool later) {
    auto x = GenX(n);
    if (IsGoConst(x) || (IsGoName(x) && !later)) return x;
    if (later || HasStmts(n)) return Snapshot(n->exprtype, x);
    return x;
}

// An integer constant of type t: `T(v)`, a u64 above i64.max from its bits.
inline string CodeGenGo::GoIntLit(int64_t v, bool uns, TypeExpr *t, Line ln) {
    auto gt = GoT(t, ln);
    if (t->kind == TY_FLT) return cat(gt, "(", uns ? cat((uint64_t)v) : cat(v), ")");
    if (t->kind != TY_INT) Fail(ln, cat("internal: an integer literal of type ", TypeText(t)));
    if (uns || (v < 0 && t->intstorage == IS_U64)) return cat(gt, "(", (uint64_t)v, ")");
    return cat(gt, "(", v, ")");
}

// A float constant of type t, spelled as the C backend spells it (CatOne:
// the shortest digits that read back), from which Go rounds to the type as
// a C compiler does. Go constants have no infinities, NaNs or negative zero,
// and math gives those; an f32 constant from halfway past its largest value
// up is an infinity (CodeGen::FltStr), and a negative one too small for an
// f32 is its negative zero, where Go's would round to zero.
inline string CodeGenGo::GoFltLit(double v, TypeExpr *t, Line ln) {
    auto gt = GoT(t, ln);
    auto f32 = t->kind == TY_FLT && t->fltstorage == FS_F32;
    if (t->kind != TY_FLT) Fail(ln, cat("internal: a float literal of type ", TypeText(t)));
    auto wrap = [&](const string &x) { return f32 ? cat("float32(", x, ")") : x; };
    if (v != v) return wrap("math.NaN()");
    if (std::isinf(v) || (f32 && std::fabs(v) >= std::ldexp(2.0 - std::ldexp(1.0, -24), 127)))
        return wrap(v > 0 ? "math.Inf(1)" : "math.Inf(-1)");
    if ((f32 ? (float)v == 0 : v == 0) && std::signbit(v)) return wrap("math.Copysign(0, -1)");
    string s;
    CatOne(s, v);
    return cat(gt, "(", s, ")");
}

// A Go string literal of arbitrary bytes: printable ASCII as itself, the rest
// as \x escapes, so that no byte is read as UTF-8.
inline string CodeGenGo::GoStr(string_view v) {
    string s = "\"";
    for (auto c : v) {
        auto u = (uint8_t)c;
        if (c == '"' || c == '\\') { s += '\\'; s += c; }
        else if (u >= 32 && u < 127) s += c;
        else {
            char buf[8];
            snprintf(buf, sizeof buf, "\\x%02x", u);
            s += buf;
        }
    }
    s += '"';
    return s;
}

// A variable as Go names it: a local, a global, or a free variable through
// the pointer it arrived as.
inline string CodeGenGo::VarRef(const VarDef *vd, Line ln) {
    if (auto it = vnames.find(vd); it != vnames.end())
        return fvptr.count(vd) ? cat("(*", it->second, ")") : it->second;
    if (auto it = gnames.find(vd); it != gnames.end()) return it->second;
    Fail(ln, cat("internal: no Go name for ", vd->name));
}

// ------------------------------------------------------------------
// Arithmetic (§6.2), the one table binary operators, compound assignments,
// `++` and `--` share: `l op r` at type t, an integer or float type, with l
// and r already of t's Go type (a shift's count of any integer type).
// `nonneg`: a signed `/`, `%` or `>>` BCE found to be of nonnegative
// operands, computed unsigned (Binary::nonneg).
inline string CodeGenGo::ArithX(TType op, TypeExpr *t, string l, string r, Line ln,
                                bool nonneg) {
    switch (op) {
        case T_PLUSEQ:  op = T_PLUS;   break;
        case T_MINUSEQ: op = T_MINUS;  break;
        case T_MULEQ:   op = T_MUL;    break;
        case T_DIVEQ:   op = T_DIV;    break;
        case T_MODEQ:   op = T_MOD;    break;
        case T_ANDEQ:   op = T_BITAND; break;
        case T_OREQ:    op = T_BITOR;  break;
        case T_XOREQ:   op = T_XOR;    break;
        case T_SHLEQ:   op = T_SHL;    break;
        case T_SHREQ:   op = T_SHR;    break;
        default: break;
    }
    auto gt = GoT(t, ln);
    if (op == T_SHL || op == T_SHR) {
        // A constant count makes a constant of a constant shifted.
        if (IsGoConst(r)) l = Materialize(t, l);
        return ShiftX(op, t, l, r);
    }
    // Two constants would make a constant expression of the operation.
    if (IsGoConst(l) && IsGoConst(r)) l = Materialize(t, l);
    if (t->kind == TY_FLT) {
        switch (op) {
            case T_PLUS:  return cat("(", l, " + ", r, ")");
            case T_MINUS: return cat("(", l, " - ", r, ")");
            // Rounded on its own: Go may otherwise fuse it with an add.
            case T_MUL:   return cat(gt, "(", l, " * ", r, ")");
            // Go rejects a float division by a constant zero only where the
            // dividend is a constant too, which the materialization above
            // rules out: any other gives the infinity or NaN it gives in C.
            case T_DIV:   return cat("(", l, " / ", r, ")");
            case T_MOD:
                if (gt == "float32")
                    return cat("float32(math.Mod(float64(", l, "), float64(", r, ")))");
                return cat("math.Mod(", l, ", ", r, ")");
            default: Fail(ln, "internal: a float operator Go has no form for");
        }
    }
    if (t->kind != TY_INT || t->intstorage == IS_VARINT)
        Fail(ln, cat("internal: arithmetic on ", TypeText(t)));
    auto s = t->intstorage;
    auto checked = debug && !IsUnsigned(s);
    auto sfx = string(1, (char)toupper(IntStorageName(s)[0])) + (IntStorageName(s) + 1);
    switch (op) {
        case T_PLUS:
            return checked ? cat("GsAdd", sfx, "(", l, ", ", r, ", ", LocArgs(ln), ")")
                           : cat("(", l, " + ", r, ")");
        case T_MINUS:
            return checked ? cat("GsSub", sfx, "(", l, ", ", r, ", ", LocArgs(ln), ")")
                           : cat("(", l, " - ", r, ")");
        case T_MUL:
            return checked ? cat("GsMul", sfx, "(", l, ", ", r, ", ", LocArgs(ln), ")")
                           : cat("(", l, " * ", r, ")");
        case T_DIV: case T_MOD:
            if (nonneg) {
                // Both nonnegative and the divisor positive (BCE): no check
                // to make, and the unsigned operation is the signed one's.
                // A negative constant would not convert to uint64 at all.
                int64_t rv;
                bool runs;
                if (GoConstInt(r, rv, runs) && rv < 0 && !runs) r = Materialize(t, r);
                return cat(gt, "(uint64(", l, ")", op == T_DIV ? " / " : " % ", "uint64(", r, "))");
            }
            return DivX(op == T_DIV, t, l, r, ln);
        case T_BITAND: return cat("(", l, " & ", r, ")");
        case T_BITOR:  return cat("(", l, " | ", r, ")");
        case T_XOR:    return cat("(", l, " ^ ", r, ")");
        default: Fail(ln, "internal: an integer operator Go has no form for");
    }
}

// `/` and `%` at an integer type: a zero divisor aborts in every build, and
// i64.min / -1 too; a debug build's narrow signed quotient that does not fit
// aborts, a release build's wraps. `%` is Euclidean (§6.2).
inline string CodeGenGo::DivX(bool div, TypeExpr *t, const string &l, const string &r, Line ln) {
    auto s = t->intstorage;
    auto name = string(IntStorageName(s));
    name[0] = (char)toupper(name[0]);
    auto narrowsigned = !IsUnsigned(s) && s != IS_I64;
    auto fn = cat(div ? "GsDiv" : "GsMod", name, div && debug && narrowsigned ? "Debug" : "");
    return cat(fn, "(", l, ", ", r, ", ", LocArgs(ln), ")");
}

// A shift by a count masked to the type's width, as the C runtime's gs_shl_
// and gs_shr_ mask it; a constant count is masked here.
inline string CodeGenGo::ShiftX(TType op, TypeExpr *t, const string &l, const string &r) {
    auto bits = IntBits(t->intstorage);
    auto o = op == T_SHL ? " << " : " >> ";
    int64_t v;
    bool uns;
    if (GoConstInt(r, v, uns)) return cat("(", l, o, (uint64_t)v & (uint64_t)(bits - 1), ")");
    return cat("(", l, o, "(uint64(", r, ") & ", bits - 1, "))");
}

// ------------------------------------------------------------------
// Expression values.

inline string CodeGenGo::GenX(Node *n) {
    // Past a statement that ends the flow nothing is emitted, and the value
    // is never used.
    if (Unreached()) {
        GenUnreached([&] { GenX(n); });
        return "0";
    }
    if (auto it = fillvalues.find(n); it != fillvalues.end())
        return LoadLoc(it->second, n->exprtype, n->line);
    if (n->line.line > 0) curline = n->line;
    switch (n->nkind) {
        case NK_IntLit: {
            auto i = (IntLit *)n;
            return GoIntLit(i->val, i->uns, n->exprtype, n->line);
        }
        case NK_FltLit: return GoFltLit(((FltLit *)n)->val, n->exprtype, n->line);
        case NK_BoolLit: {
            if (n->exprtype && n->exprtype->kind != TY_BOOL) GoT(n->exprtype, n->line);
            return ((BoolLit *)n)->val ? "true" : "false";
        }
        case NK_Ident: {
            auto id = (Ident *)n;
            if (!id->vdef) Unsupported(n->line, "a named function used as a value outside a call");
            GoT(id->vdef->type, n->line);
            if (!IsScalarT(id->vdef->type))
                return LoadLoc(VarLoc(id->vdef, n->line), n->exprtype, n->line);
            // A named constant or a literal parameter (§7.7) is read at the
            // type its use adapted it to.
            return Coerce(VarRef(id->vdef, n->line), id->vdef->type, n->exprtype, n->line);
        }
        case NK_Unary: return GenUnary((Unary *)n);
        case NK_Binary: return GenBinary((Binary *)n);
        case NK_AsCast: return GenCast((AsCast *)n);
        case NK_Call: return GenCall((Call *)n);
        case NK_StrLit: return GenStrLit((StrLit *)n);
        case NK_ArrayLit: case NK_StructLit: return GenLitVal(n);
        case NK_NullLit:
            GoT(n->exprtype, n->line);
            if (n->exprtype->kind != TY_REF) Fail(n->line, "internal: null of no reference type");
            return IsFatRef(n->exprtype) ? "GsRref{}" : "nil";
        case NK_SelfRef: Fail(n->line, "internal: self outside a literal's field");
        case NK_Dot: return GenDot((Dot *)n);
        case NK_Index: return LoadLoc(GenLoc(n), n->exprtype, n->line);
        case NK_SliceExpr: return GenSlice((SliceExpr *)n);
        case NK_Block: case NK_IfExpr: case NK_MatchExpr: case NK_EarlyBlock: case NK_LoopExpr:
        case NK_InlineBlock:
            return CtlValX(n);
        default:
            Fail(n->line, "internal: a statement in value position");
    }
}

inline string CodeGenGo::GenUnary(Unary *u) {
    auto t = u->exprtype;
    if (u->op == T_BITAND) {
        // Where the checker decayed the reference, `&x` reads as its
        // pointee (§3.8), or a whole array as a slice of it (§3.10).
        if (t && t->kind != TY_REF) {
            if (IsScalarT(t) && IsScalarT(u->child->exprtype))
                return Coerce(GenX(u->child), u->child->exprtype, t, u->line);
            return LoadLoc(GenLoc(u->child), t, u->line);
        }
        GoT(t, u->line);
        return GenRefVal(u->child, u->line);
    }
    if (u->op == T_MINUS && t && (t->kind == TY_STRUCT || t->kind == TY_ARRAY)) {
        auto x = Snapshot(t, CoerceVal(GenX(u->child), u->child->exprtype, t, u->line));
        auto v = T();
        Decl(v, LocalT(t, u->line));
        ElemwiseNegInto(t, u->line, x, AtVar(v, t));
        return v;
    }
    auto ct = u->child->exprtype;
    if (u->op == T_NOT && ct && ct->kind == TY_REF) {
        // The null test of an optional (§3.8).
        GoT(ct, u->line);
        return cat("(", NullTestOperand(u->child), " == nil)");
    }
    auto x = GenX(u->child);
    switch (u->op) {
        case T_MINUS: {
            GoT(ct, u->line);
            x = Materialize(ct, x);
            if (ct->kind == TY_INT && debug && !IsUnsigned(ct->intstorage)) {
                auto name = string(IntStorageName(ct->intstorage));
                name[0] = (char)toupper(name[0]);
                return Coerce(cat("GsNeg", name, "(", x, ", ", LocArgs(u->line), ")"), ct, t,
                              u->line);
            }
            return Coerce(cat("(-", x, ")"), ct, t, u->line);
        }
        case T_NOT:
            if (ct->kind != TY_BOOL) Unsupported(u->line, cat("`!` of ", TypeText(ct)));
            return cat("(!", x, ")");
        case T_BITNOT: {
            // At the operand's type (§6.1); the slot it lands in can be wider.
            x = Materialize(ct, x);
            return Coerce(cat("(^", x, ")"), ct, t, u->line);
        }
        default: Fail(u->line, "internal: unary operator");
    }
}

inline string CodeGenGo::GenBinary(Binary *b) {
    auto op = b->op;
    if (op == T_DOTEQ || op == T_DOTNEQ) {
        // Reference identity (§3.8): the addresses, null the null one. A
        // reference variable reads as the reference it holds.
        auto side = [&](Node *n) -> string {
            if (Is<NullLit>(n)) return "nil";
            if (auto id = Is<Ident>(n); id && id->vdef && id->vdef->type->kind == TY_REF) {
                GoT(id->vdef->type, n->line);
                auto l = VarLoc(id->vdef, n->line);
                return cat("unsafe.Pointer(", LoadLoc(l, l.t, n->line), ")");
            }
            return cat("unsafe.Pointer(", GenX(n), ")");
        };
        auto l = T();
        Decl(l, "unsafe.Pointer", side(b->left));
        auto r = side(b->right);
        return cat("(", l, op == T_DOTEQ ? " == " : " != ", r, ")");
    }
    if (op == T_ANDAND || op == T_OROR) {
        auto l = GenTruth(b->left);
        string r;
        if (!HasStmts(b->right) && TryPure([&] { return GenTruth(b->right); }, r)) {
            // A call is an operand vet compares with none (GsBool).
            if (VetBoolClash(l, r, op == T_ANDAND)) r = cat("GsBool(", r, ")");
            return cat("(", l, op == T_ANDAND ? " && " : " || ", r, ")");
        }
        // Short circuit: the right operand's statements run only where the
        // left one leaves the value open.
        auto t = T();
        Decl(t, "bool", l);
        if (Unreached()) {
            GenUnreached([&] { GenTruth(b->right); });
            return t;
        }
        L("if ", op == T_ANDAND ? t : cat("!", t), " {");
        ind++;
        PushSc(SC_PLAIN);
        r = GenTruth(b->right);
        L(t, " = ", r);
        PopSc();
        ind--;
        LS("}");
        dead = false;
        return t;
    }
    if ((op == T_EQ || op == T_NEQ) && (Is<NullLit>(b->left) || Is<NullLit>(b->right))) {
        // The null test of an optional (§3.8), which reads the reference.
        auto lnull = Is<NullLit>(b->left) != nullptr, rnull = Is<NullLit>(b->right) != nullptr;
        if (lnull && rnull) return op == T_EQ ? "true" : "false";
        auto x = NullTestOperand(lnull ? b->right : b->left);
        return cat("(", x, op == T_EQ ? " == nil)" : " != nil)");
    }
    auto lt = b->left->exprtype;
    if (lt && IsPlainRef(lt)) lt = lt->ref->sub;
    if (b->exprtype && (b->exprtype->kind == TY_STRUCT || b->exprtype->kind == TY_ARRAY)) {
        // Elementwise arithmetic (§6.1).
        string l, r;
        bool ls, rs;
        ElemwiseOperands(b, l, ls, r, rs);
        auto v = T();
        Decl(v, LocalT(b->exprtype, b->line));
        ElemwiseInto(b->exprtype, op, b->line, l, ls, r, rs, AtVar(v, b->exprtype));
        return v;
    }
    // A varint operand or result is the i64 it encodes (§3.6).
    if (lt && IsVarintT(lt)) lt = ast.inttypes[IS_I64];
    if ((op == T_EQ || op == T_NEQ) && lt && !IsScalarT(lt)) return GenEquality(b);
    if (!IsScalarT(lt) || (b->exprtype && !IsScalarT(b->exprtype) && !IsVarintT(b->exprtype)))
        Unsupported(b->line, cat("the operator on ", TypeText(lt)));
    // Left to right (§2): the left operand is read into a temporary first
    // where the right one runs statements.
    auto l = Coerce(GenOperandVal(b->left, HasStmts(b->right)), b->left->exprtype, lt, b->line);
    auto r = GenX(b->right);
    switch (op) {
        case T_SHL: case T_SHR:
            return Coerce(ArithX(op, lt, l, r, b->line, b->nonneg), lt, b->exprtype, b->line);
        default: break;
    }
    r = Coerce(r, b->right->exprtype, lt, b->line);
    switch (op) {
        case T_PLUS: case T_MINUS: case T_MUL: case T_DIV: case T_MOD:
        case T_BITAND: case T_BITOR: case T_XOR:
            return Coerce(ArithX(op, lt, l, r, b->line, b->nonneg), lt, b->exprtype, b->line);
        case T_LT: case T_GT: case T_LTEQ: case T_GTEQ: case T_EQ: case T_NEQ: {
            const char *o = op == T_LT ? " < " : op == T_GT ? " > " : op == T_LTEQ ? " <= "
                          : op == T_GTEQ ? " >= " : op == T_EQ ? " == " : " != ";
            return cat("(", l, o, r, ")");
        }
        default: Fail(b->line, "internal: binary operator");
    }
}

// `as` checks the range in a debug build, naming the value, the target type
// and the cast's location; `as!` always wraps or truncates, and so does `as`
// in a release build (§6.3). A float converts to an integer through
// GsF2IWrap, which truncates toward zero and wraps modulo 2^64 the same on
// every platform; a conversion to a float rounds and is never checked.
inline string CodeGenGo::GenCast(AsCast *c) {
    auto st = c->child->exprtype, tt = c->totype;
    if (!tt) Fail(c->line, "internal: unchecked cast");
    if (tt->ix) Unsupported(c->line, cat("a conversion to the index type ", TypeText(tt)));
    auto tgt = GoT(tt, c->line);
    GoT(st, c->line);
    // A constant converted would be checked by the Go compiler to fit.
    auto x = Materialize(st, GenX(c->child));
    string out;
    if (tt->kind == TY_INT) {
        auto is = tt->intstorage;
        auto named = [&] { return cat("\"", IntStorageName(is), "\", ", LocArgs(c->line)); };
        if (st->kind == TY_FLT) {
            auto f32 = IsF32(st) ? "true" : "false";
            if (c->unchecked || !debug)
                out = cat(tgt, "(GsF2IWrap(float64(", x, ")))");
            else if (is == IS_U64)
                out = cat("GsF2U(float64(", x, "), ", f32, ", ", LocArgs(c->line), ")");
            else {
                auto [lo, hi] = IntRange(is);
                out = cat(tgt, "(GsF2I(float64(", x, "), ", f32, ", ", lo, ", ", hi, ", ", named(),
                          "))");
            }
        } else if (c->unchecked || !debug || SameGoT(st, tt)) {
            out = SameGoT(st, tt) ? x : cat(tgt, "(", x, ")");
        } else if (st->intstorage == IS_U64) {
            // From u64: value-preserving iff it does not exceed the target's
            // maximum.
            if (is == IS_U64) out = x;
            else out = cat(tgt, "(GsRangeU(", x, ", ", (uint64_t)IntRange(is).second, ", ", named(),
                           "))");
        } else {
            auto [slo, shi] = IntRange(st->intstorage);
            auto [lo, hi] = IntRange(is);
            if (slo >= lo && shi <= hi) out = cat(tgt, "(", x, ")");
            else out = cat(tgt, "(GsRangeI(int64(", x, "), ", lo, ", ", hi, ", ", named(), "))");
        }
    } else if (tt->kind == TY_FLT) {
        out = SameGoT(st, tt) ? x : cat(tgt, "(", x, ")");
    } else {
        Fail(c->line, cat("internal: a cast to ", TypeText(tt)));
    }
    // The slot the result lands in can be wider than the cast's own type.
    return Coerce(out, tt, c->exprtype, c->line);
}

// The reference a null test reads: a narrowed optional variable's own, not
// the pointee its use may have decayed to.
inline string CodeGenGo::NullTestOperand(Node *n) {
    if (auto id = Is<Ident>(n); id && id->vdef && IsOptional(id->vdef->type)) {
        GoT(id->vdef->type, n->line);
        auto l = VarLoc(id->vdef, n->line);
        auto x = LoadLoc(l, l.t, n->line);
        // A fat reference is null where its header is.
        return IsFatRef(l.t) ? cat(x, ".Hdr") : x;
    }
    auto x = GenX(n);
    return n->exprtype && IsFatRef(n->exprtype) ? cat(x, ".Hdr") : x;
}

// A condition's Go bool: a reference's null test where it is one (§3.8).
inline string CodeGenGo::GenTruth(Node *n) {
    auto t = n->exprtype;
    if (t && t->kind == TY_REF) return cat("(", NullTestOperand(n), " != nil)");
    return GenX(n);
}

// A control construct as a value: built into a temporary of its type. One
// typed as a relative reference (a branch's value for such a slot) is the
// plain reference it encodes.
inline string CodeGenGo::CtlValX(Node *n) {
    auto vt = n->exprtype;
    if (vt && IsRelRef(vt)) vt = ast.RefTo(vt->ref->sub, n->line, vt->ref->optional);
    auto t = T();
    Decl(t, LocalT(vt, n->line));
    GenAny(n, GoDst { GD_LVALUE, t, vt });
    return t;
}

inline void CodeGenGo::GenAny(Node *n, const GoDst &d) {
    if (Unreached()) return GenUnreached([&] { GenAny(n, d); });
    // A fill's value, evaluated once already (FreezeFill).
    if (fillvalues.count(n)) return LeafAny(n, d);
    switch (n->nkind) {
        case NK_Block: GenBlock((Block *)n, d); return;
        case NK_IfExpr: GenIf((IfExpr *)n, d); return;
        case NK_MatchExpr: GenMatch((MatchExpr *)n, d); return;
        case NK_EarlyBlock: GenEarlyBlock((EarlyBlock *)n, d); return;
        case NK_LoopExpr: GenLoop((LoopExpr *)n, d); return;
        case NK_InlineBlock: GenInlineBlock((InlineBlock *)n, d); return;
        case NK_Call: GenCallAny((Call *)n, d); return;
        case NK_While: case NK_ForLoop: case NK_Return: case NK_Break: case NK_Continue:
        case NK_Defer: case NK_VarDecl: case NK_Assign: case NK_IncDec: case NK_FnDecl:
            GenStmt2(n);
            return;
        case NK_StructDecl: case NK_EnumDecl: case NK_AliasDecl: return;
        case NK_FunVal: Fail(n->line, "internal: function value emitted");
        case NK_ConstFor: Fail(n->line, "internal: unexpanded const for");
        default: LeafAny(n, d); return;
    }
}

// A non-control node's value routed to a destination: assigned, or, for
// effect, given to Go's blank identifier, which is how Go evaluates an
// expression that is no call.
inline void CodeGenGo::LeafAny(Node *n, const GoDst &d) {
    if (d.k == GD_STACK) {
        GenConstruct(n, d.s, d.t, d.lenlv);
        return;
    }
    if (d.k == GD_LVALUE && d.pool) {
        L(d.s, " = ", GenPrefVal(n));
        return;
    }
    if (d.k == GD_LVALUE) {
        if (d.t && IsRelRef(d.t)) {
            GenRelStore(AtVar(d.s, d.t), n, false);
            return;
        }
        // A literal holding relative references is built where it stays:
        // its offsets are measured from there (§3.9).
        if (d.t && (Is<StructLit>(n) || Is<ArrayLit>(n)) && HasRelRef(d.t) &&
            n->exprtype->kind != TY_SLICE) {
            LitInto(n, AtVar(d.s, d.t), false);
            return;
        }
        // A slice or an array of another kind into a limited array goes
        // straight into its slots.
        if (d.t && IsStaticLimited(d.t) && n->exprtype && !TEq(n->exprtype, d.t) &&
            (n->exprtype->kind == TY_SLICE || n->exprtype->kind == TY_ARRAY ||
             (IsPlainRef(n->exprtype) && n->exprtype->ref->sub->kind == TY_ARRAY))) {
            auto l = GenLoc(n);
            while (l.t->kind == TY_REF) DerefLoc(l);
            if (l.t->kind == TY_SLICE || !TEq(l.t, d.t)) {
                CopyIntoLimited(l, d.t, n->line, AtVar(d.s, d.t));
                return;
            }
            L(d.s, " = ", Ld(l));
            return;
        }
        auto x = d.t ? GenXD(n, d.t) : GenX(n);
        // Go's vet reports an assignment of a variable to itself.
        if (x != d.s) L(d.s, " = ", x);
        return;
    }
    if (IsVoidT(n->exprtype)) Fail(n->line, "internal: valueless leaf");
    // A resizable or bytes value is reached through its location, whose
    // path's checks still run: a path's is where it is stored, anything
    // else is built on a temporary stack the enclosing scope releases.
    if (IsBytesT(n->exprtype)) {
        // Its address read, for the checks on the path (C's `(void)(loc)`).
        auto l = GenLoc(n);
        auto x = !l.addr.empty() ? l.addr : !l.hdr.empty() ? cat("&", l.hdr) : l.lv;
        if (!x.empty() && !IsGoName(x)) L("_ = ", x);
        return;
    }
    // A constant or a variable read has no effect to keep.
    if (Is<IntLit>(n) || Is<FltLit>(n) || Is<BoolLit>(n) || Is<StrLit>(n) || Is<NullLit>(n))
        return;
    auto x = GenX(n);
    if (IsGoConst(x) || x == "nil") return;
    L("_ = ", x);
}

}  // namespace goose
