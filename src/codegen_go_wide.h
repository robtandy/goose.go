// Goose compiler — the Go backend's wide loads and stores (definitions of
// CodeGenGo members, codegen_go.h): an integer assembled from the bytes of
// consecutive elements of a u8 array or slice, or taken apart into them, as
// one load or one store of 2, 4 or 8 bytes, little- or big-endian (issue #23).
//
//     c[0] | (c[1] as u64) << 8 | ... | (c[7] as u64) << 56      // a load
//     (c[0] as u32) << 24 | (c[1] as u32) << 16 | ... | c[3]     // big-endian
//     for k in 8 { o[j + k] = (v >> (8 * k)) as! u8; }           // a store
//     o[j] = v as! u8; o[j + 1] = (v >> 8) as! u8; ...           // a run of them
//
// A C compiler makes one load or store of each of these. Go's makes one only
// where every byte's address is a constant away from one base it knows is no
// nil pointer, and a byte read through the slice's own pointer, at offset 0,
// is a nil check that keeps the rest apart; nor does it see through a loop.
// So the backend writes them as encoding/binary does, the bytes viewed as an
// array, `binary.LittleEndian.Uint64((*[8]byte)(p)[:])`, which Go compiles to
// one load (and a byte swap for the other order) on every target that has
// unaligned ones, and to the bytes elsewhere: the value is the same on every
// target, its byte order included.
//
// Only where every byte's bounds check is gone already -- BCE proved it
// (Index::nobc), or the index is a constant within a fixed array -- so that
// no check in between could abort, and only for the elements of one array
// (the same path, with nothing between that could move it) at indices that
// are one expression plus consecutive constants, or the loop's counter: the
// expression is evaluated once, where the program evaluates its first byte's,
// and the bytes are read or written in one go. An index's arithmetic must be
// 64-bit, where a constant added to an index cannot wrap from one in range to
// another; that of a narrower unsigned type could (255 + 1 as a u8 is 0).
// Every type a byte's value passes through on its way into the result must
// hold it whole, as a zero extension does: an unsigned one wide enough, or
// the result's own. A store's value and index must be scalar variables and
// constants, which the bytes it writes cannot change, and its conversion to
// u8 one that truncates (`as!`, `as` in a release build, or a value masked
// to 255 first), as a single store does.
#pragma once

namespace goose {

// Whether two expressions are the same, and evaluating either again gives
// the same: variables, constants, fields, properties, elements, and the
// operators and conversions of those, with no call anywhere, nor anything
// else that runs statements (HasStmts). An expression is SamePure with
// itself where it is pure so.
inline bool CodeGenGo::SamePure(Node *a, Node *b) {
    if (!a || !b || a->nkind != b->nkind || !a->exprtype || !b->exprtype ||
        !SameGoT(a->exprtype, b->exprtype))
        return false;
    switch (a->nkind) {
        case NK_Ident: {
            auto x = (Ident *)a, y = (Ident *)b;
            return x->vdef && x->vdef == y->vdef;
        }
        case NK_IntLit: {
            auto x = (IntLit *)a, y = (IntLit *)b;
            return x->val == y->val && x->uns == y->uns;
        }
        case NK_Unary: {
            auto x = (Unary *)a, y = (Unary *)b;
            return x->op == y->op && x->op != T_BITAND && SamePure(x->child, y->child);
        }
        case NK_Binary: {
            auto x = (Binary *)a, y = (Binary *)b;
            return x->op == y->op && x->op != T_DOTEQ && x->op != T_DOTNEQ && x->nonneg == y->nonneg &&
                   SamePure(x->left, y->left) && SamePure(x->right, y->right);
        }
        case NK_AsCast: {
            auto x = (AsCast *)a, y = (AsCast *)b;
            return x->totype && y->totype && SameGoT(x->totype, y->totype) && x->unchecked == y->unchecked &&
                   x->nobc == y->nobc && SamePure(x->child, y->child);
        }
        case NK_Dot: {
            auto x = (Dot *)a, y = (Dot *)b;
            return !x->variantconst && !y->variantconst && x->fieldidx == y->fieldidx &&
                   x->member == y->member && (x->member < 0 || x->member == B_LEN) &&
                   SamePure(x->obj, y->obj);
        }
        case NK_Index: {
            auto x = (Index *)a, y = (Index *)b;
            return SamePure(x->obj, y->obj) && SamePure(x->idx, y->idx);
        }
        default: return false;
    }
}

// Whether n is a scalar computed from scalar variables and constants alone:
// what no store into an array's elements can change.
static bool WideScalar(Node *n) {
    if (!n || !n->exprtype || !IsIntT(n->exprtype)) return false;
    if (Is<IntLit>(n)) return true;
    if (auto id = Is<Ident>(n)) return id->vdef && id->vdef->type && IsIntT(id->vdef->type);
    if (auto u = Is<Unary>(n)) return (u->op == T_MINUS || u->op == T_BITNOT) && WideScalar(u->child);
    if (auto b = Is<Binary>(n)) {
        switch (b->op) {
            case T_PLUS: case T_MINUS: case T_MUL: case T_DIV: case T_MOD: case T_BITAND:
            case T_BITOR: case T_XOR: case T_SHL: case T_SHR:
                return WideScalar(b->left) && WideScalar(b->right);
            default: return false;
        }
    }
    if (auto c = Is<AsCast>(n)) return WideScalar(c->child);
    return false;
}

// Whether n reads the variable vd.
static bool WideUses(Node *n, const VarDef *vd) {
    if (!n) return false;
    if (auto id = Is<Ident>(n)) return id->vdef == vd;
    auto found = false;
    RunChildren(n, [&](Node *c) { found = found || WideUses(c, vd); });
    return found;
}

// A 64-bit integer type, whose arithmetic on an index cannot wrap from one
// in range to another by a small constant: a signed one wraps to a negative
// index, a u64 to one as large as a negative one is (WideIndex).
static bool Wide64(TypeExpr *t) {
    return t && t->kind == TY_INT && (t->intstorage == IS_I64 || t->intstorage == IS_U64);
}

// An index as the expression it adds a constant to, and the constant: `e +
// c`, `c + e` and `e - c`, through any number of them, in 64-bit arithmetic;
// base is null for a constant index, and the index itself where it adds no
// constant.
inline bool CodeGenGo::WideIndex(Node *idx, Node *&base, int64_t &off) {
    if (!Wide64(idx->exprtype)) return false;
    if (auto il = Is<IntLit>(idx)) {
        if (il->uns) return false;
        base = nullptr;
        off = il->val;
        return true;
    }
    auto b = Is<Binary>(idx);
    auto small = [](Node *n) {
        auto il = Is<IntLit>(n);
        return il && !il->uns && il->val > -(1 << 20) && il->val < (1 << 20) ? il : nullptr;
    };
    // GenBinary computes `l op r` at the type of l.
    if (b && (b->op == T_PLUS || b->op == T_MINUS) && Wide64(b->left->exprtype)) {
        Node *inner = nullptr;
        int64_t c = 0;
        if (auto r = small(b->right)) {
            inner = b->left;
            c = b->op == T_PLUS ? r->val : -r->val;
        } else if (auto l = small(b->left); l && b->op == T_PLUS) {
            inner = b->right;
            c = l->val;
        }
        if (inner && WideIndex(inner, base, off)) {
            off += c;
            return true;
        }
    }
    base = idx;
    off = 0;
    return true;
}

// Whether ix is an element of a u8 array or slice (through any references)
// whose bounds check is gone: BCE proved it, or the index is a constant
// within a fixed array, which IndexLoc never checks.
inline bool CodeGenGo::WideElem(Index *ix) {
    auto t = ix->obj->exprtype;
    while (t && t->kind == TY_REF) t = t->ref->sub;
    if (!t) return false;
    TypeExpr *elem = t->kind == TY_SLICE ? t->sub : t->kind == TY_ARRAY ? t->arr->sub : nullptr;
    if (!elem || !IsU8(elem)) return false;
    if (ix->nobc) return true;
    auto il = Is<IntLit>(ix->idx);
    return t->kind == TY_ARRAY && t->arr->akind == A_FIXED && il && !il->uns && il->val >= 0 &&
           il->val < ArrSize(t->arr);
}

// A leaf of an OR at type ot that is a byte shifted left by a constant:
// `a[i] as T << c`, `a[i] << c` with the element read at a wider type, or
// either without the shift. Each type its value takes on the way to ot
// holds it whole (an unsigned one of enough bits, or ot itself): before the
// shift, the element's (as the Index reads it), the conversion's and the
// cast's own; from the shift on, the shift's, the leaf's and ot.
inline bool CodeGenGo::WideLeaf(Node *n, TypeExpr *ot, WideByte &w) {
    auto holds = [&](TypeExpr *t, int64_t bits) {
        return t && IsIntT(t) && !t->ix && IntBits(t->intstorage) >= bits &&
               (IsUnsigned(t->intstorage) || SameGoT(t, ot));
    };
    int64_t c = 0;
    auto x = n;
    if (auto s = Is<Binary>(x); s && s->op == T_SHL) {
        auto cl = Is<IntLit>(s->right);
        if (!cl || cl->uns || cl->val < 0 || cl->val > 56) return false;
        c = cl->val;
        // Go's shift and the C runtime's both mask the count to the width,
        // which a count within it leaves as it is.
        if (!holds(s->left->exprtype, c + 8)) return false;
        x = s->left;
    }
    if (!holds(ot, c + 8) || !holds(n->exprtype, c + 8) || !holds(x->exprtype, 8)) return false;
    if (auto cast = Is<AsCast>(x)) {
        if (!holds(cast->totype, 8)) return false;
        x = cast->child;
        if (!holds(x->exprtype, 8)) return false;
    }
    auto ix = Is<Index>(x);
    if (!ix || !WideElem(ix)) return false;
    w.ix = ix;
    w.shift = c;
    return true;
}

// The n bytes from element `at` of the u8 view v, as the []byte encoding/binary
// takes: a Go local's own array sliced, which takes no address of it
// (GoLoc::golocal), anything else viewed at its address as a [n]byte, which
// Go converts with no check but for nil (unsafe.Slice would check that the
// address does not overflow too).
inline string CodeGenGo::WideBytes(const GoView &v, const string &at, int64_t n) {
    int64_t k;
    bool uns;
    auto isconst = GoConstInt(at, k, uns) && !uns;
    if (v.local) {
        if (isconst) return cat(v.arr, "[", k, ":", k + n, "]");
        return cat(v.arr, "[", at, ":", at, "+", n, "]");
    }
    auto p = isconst ? AddOff(v.data, k) : cat("unsafe.Add(", v.data, ", ", at, ")");
    return cat("(*[", n, "]byte)(", p, ")[:]");
}

// The index `idx`, an int64 IndexValue gave, moved by a constant.
static string WideAt(const string &idx, int64_t delta) {
    int64_t k;
    bool uns;
    if (CodeGenGo::GoConstInt(idx, k, uns) && !uns) return cat(k + delta);
    if (!delta) return idx;
    return cat("(", idx, delta > 0 ? " + " : " - ", delta > 0 ? delta : -delta, ")");
}

static const char *WideOrder(bool le) { return le ? "LittleEndian" : "BigEndian"; }

// An OR of 2, 4 or 8 bytes of one array at consecutive indices, each shifted
// to its place in a little- or a big-endian order (WideLeaf), as one load:
// its Go expression, of the type of b, or "" where b is no such OR, before
// anything is emitted for it.
inline string CodeGenGo::GenWideLoad(Binary *b) {
    // GenBinary computes an OR at the type of its left operand.
    auto ot = b->left->exprtype;
    if (!ot || !IsIntT(ot) || ot->ix || IntBits(ot->intstorage) < 16) return "";
    // The tree's leaves, left to right, through the ORs at that type.
    vector<Node *> leaves;
    function<bool(Node *)> flat = [&](Node *n) -> bool {
        if (auto o = Is<Binary>(n); o && o->op == T_BITOR && SameGoT(o->left->exprtype, ot) &&
                                    SameGoT(o->exprtype, ot))
            return flat(o->left) && flat(o->right);
        leaves.push_back(n);
        return leaves.size() <= 8;
    };
    if (!flat(b->left) || !flat(b->right)) return "";
    auto n = (int64_t)leaves.size();
    if ((n != 2 && n != 4 && n != 8) || 8 * n > IntBits(ot->intstorage)) return "";
    vector<WideByte> w((size_t)n);
    Node *base0 = nullptr;
    for (size_t i = 0; i < w.size(); i++) {
        Node *base = nullptr;
        if (!WideLeaf(leaves[i], ot, w[i]) || !WideIndex(w[i].ix->idx, base, w[i].off)) return "";
        if (!i) {
            base0 = base;
            continue;
        }
        if (!SamePure(w[i].ix->obj, w[0].ix->obj) || (base0 == nullptr) != (base == nullptr) ||
            (base && !SamePure(base, base0)))
            return "";
    }
    // Each offset from the least one on once, each byte at its place.
    auto k0 = w[0].off;
    for (auto &x : w) k0 = std::min(k0, x.off);
    vector<int64_t> shiftat((size_t)n, -1);
    for (auto &x : w) {
        auto d = x.off - k0;
        if (d >= n || shiftat[(size_t)d] >= 0) return "";
        shiftat[(size_t)d] = x.shift;
    }
    auto le = true, be = true;
    for (int64_t d = 0; d < n; d++) {
        le = le && shiftat[(size_t)d] == 8 * d;
        be = be && shiftat[(size_t)d] == 8 * (n - 1 - d);
    }
    if (!le && !be) return "";
    // The array and the index the leftmost byte names, which the program
    // evaluates first.
    auto &first = w[0];
    auto l = GenLoc(first.ix->obj);
    while (l.t->kind == TY_REF) DerefLoc(l);
    auto v = ArrayView(l, first.ix->line);
    auto at = WideAt(IndexValue(first.ix->idx), k0 - first.off);
    usesbinary = usesbinary || !dry;
    auto x = cat("binary.", WideOrder(le), ".Uint", 8 * n, "(", WideBytes(v, at, n), ")");
    auto ut = ast.inttypes[n == 2 ? IS_U16 : n == 4 ? IS_U32 : IS_U64];
    return Coerce(Coerce(x, ut, ot, b->line), ot, b->exprtype, b->line);
}

// The byte a store writes, `(v >> c) as u8`, as v and c: a truncating
// conversion of a shift (or of v itself, c a null), or of either masked to
// 255 first, with v a scalar of at least `bits` bits that no store into an
// array's elements can change (WideScalar).
inline bool CodeGenGo::WideStoreByte(Node *rhs, int64_t bits, Node *&v, Node *&count) {
    auto cast = Is<AsCast>(rhs);
    if (!cast || !cast->totype || !IsU8(cast->totype)) return false;
    auto y = cast->child;
    auto masked = false;
    if (auto m = Is<Binary>(y); m && m->op == T_BITAND) {
        auto mask = Is<IntLit>(m->right);
        if (!mask || mask->uns || mask->val != 255) return false;
        masked = true;
        y = m->left;
    }
    // `as` checks the value's range in a debug build, which only a value
    // masked to a byte passes whatever the bytes are.
    if (debug && !cast->unchecked && !masked) return false;
    count = nullptr;
    if (auto s = Is<Binary>(y); s && s->op == T_SHR) {
        count = s->right;
        y = s->left;
    }
    if (!WideScalar(y) || y->exprtype->ix || IntBits(y->exprtype->intstorage) < bits) return false;
    v = y;
    return true;
}

// a * k + b, for an expression of the variable k (null: of none) built of
// constants, k, `+`, `-`, `*` and `<<` by a constant, with small values.
static bool WideAffine(Node *n, const VarDef *k, int64_t &a, int64_t &b) {
    if (auto il = Is<IntLit>(n)) {
        if (il->uns || il->val < -1024 || il->val > 1024) return false;
        a = 0;
        b = il->val;
        return true;
    }
    if (auto id = Is<Ident>(n)) {
        if (!k || id->vdef != k) return false;
        a = 1;
        b = 0;
        return true;
    }
    if (auto c = Is<AsCast>(n)) {
        if (!c->totype || !IsIntT(c->totype) || IntBits(c->totype->intstorage) < 16) return false;
        return WideAffine(c->child, k, a, b);
    }
    auto bn = Is<Binary>(n);
    int64_t a1, b1, a2, b2;
    if (!bn || !WideAffine(bn->left, k, a1, b1) || !WideAffine(bn->right, k, a2, b2)) return false;
    switch (bn->op) {
        case T_PLUS: a = a1 + a2; b = b1 + b2; break;
        case T_MINUS: a = a1 - a2; b = b1 - b2; break;
        case T_MUL:
            if (a1 && a2) return false;
            a = a1 * b2 + a2 * b1;
            b = b1 * b2;
            break;
        case T_SHL:
            if (a2 || b2 < 0 || b2 > 6) return false;
            a = a1 << b2;
            b = b1 << b2;
            break;
        default: return false;
    }
    return a >= -1024 && a <= 1024 && b >= -1024 && b <= 1024;
}

// k's coefficient in idx, where idx is k plus or minus scalars that do not
// read it (WideScalar), in 64-bit arithmetic; false where it is anything
// else.
static bool WideCoef(Node *idx, const VarDef *k, int64_t &coef) {
    if (!Wide64(idx->exprtype)) return false;
    if (!WideUses(idx, k)) {
        coef = 0;
        return WideScalar(idx);
    }
    if (auto id = Is<Ident>(idx)) {
        coef = 1;
        return id->vdef == k;
    }
    auto b = Is<Binary>(idx);
    int64_t l, r;
    if (!b || (b->op != T_PLUS && b->op != T_MINUS) || !Wide64(b->left->exprtype) ||
        !WideCoef(b->left, k, l) || !WideCoef(b->right, k, r))
        return false;
    coef = b->op == T_PLUS ? l + r : l - r;
    return true;
}

// `for k in n { o[e + k] = (v >> (8 * k)) as! u8; }`, n 2, 4 or 8, and the
// big-endian `(v >> (8 * (n - 1 - k)))`, as one store: emitted, where f is
// such a loop, with the index at k = 0, as the program would compute it.
inline bool CodeGenGo::GenWideStoreLoop(ForLoop *f) {
    if ((f->iterkind != IK_RANGE && f->iterkind != IK_COUNT) || f->idxdef || !f->vdef) return false;
    Node *count = f->iter;
    if (auto r = Is<RangeExpr>(f->iter)) {
        auto lo = Is<IntLit>(r->lo);
        if (!lo || lo->uns || lo->val != 0) return false;
        count = r->hi;
    }
    auto cl = Is<IntLit>(count);
    if (!cl || cl->uns || (cl->val != 2 && cl->val != 4 && cl->val != 8)) return false;
    auto n = cl->val;
    auto body = f->body;
    Node *st = body->stmts.size() == 1 && !body->tail ? body->stmts[0]
             : body->stmts.empty() ? body->tail : nullptr;
    auto a = Is<Assign>(st);
    if (!a || a->op != T_ASSIGN || a->pointee) return false;
    auto ix = Is<Index>(a->lval);
    int64_t coef = 0;
    if (!ix || !ix->nobc || !WideElem(ix) || !Is<Ident>(ix->obj) || !WideCoef(ix->idx, f->vdef, coef) ||
        coef != 1)
        return false;
    Node *v, *sc;
    int64_t sa, sb;
    if (!WideStoreByte(a->rhs, 8 * n, v, sc) || !sc || WideUses(v, f->vdef) ||
        !WideAffine(sc, f->vdef, sa, sb))
        return false;
    auto le = sa == 8 && sb == 0, be = sa == -8 && sb == 8 * (n - 1);
    if (!le && !be) return false;
    auto l = GenLoc(ix->obj);
    while (l.t->kind == TY_REF) DerefLoc(l);
    auto view = ArrayView(l, ix->line);
    // The counter at 0, where the first store would be.
    vnames[f->vdef] = GoIntLit(0, false, f->vdef->type, f->line);
    auto at = IndexValue(ix->idx);
    vnames.erase(f->vdef);
    auto ut = ast.inttypes[n == 2 ? IS_U16 : n == 4 ? IS_U32 : IS_U64];
    auto x = Coerce(GenX(v), v->exprtype, ut, a->line);
    usesbinary = usesbinary || !dry;
    L("binary.", WideOrder(le), ".PutUint", 8 * n, "(", WideBytes(view, at, n), ", ", x, ")");
    return true;
}

// The run of 8, 4 or 2 statements from stmts[i] that store the bytes of one
// value into consecutive elements of one array, each `o[e + c] = (v >> s)
// as! u8` with the same o, e and v, in either order (GenWideStoreLoop), as
// one store: how many it takes, 0 where they are no such run, before
// anything is emitted.
inline size_t CodeGenGo::GenWideStoreRun(const vector<Node *> &stmts, size_t i) {
    for (int64_t n : { 8, 4, 2 }) {
        if (i + (size_t)n > stmts.size()) continue;
        vector<WideByte> w;
        Node *base0 = nullptr, *v0 = nullptr;
        for (int64_t j = 0; j < n; j++) {
            auto a = Is<Assign>(stmts[i + (size_t)j]);
            auto ix = a && a->op == T_ASSIGN && !a->pointee ? Is<Index>(a->lval) : nullptr;
            Node *base = nullptr, *v = nullptr, *sc = nullptr;
            WideByte b;
            int64_t sa = 0;
            if (!ix || !WideElem(ix) || !Is<Ident>(ix->obj) || !WideIndex(ix->idx, base, b.off) ||
                (base && !WideScalar(base)) || !WideStoreByte(a->rhs, 8 * n, v, sc) ||
                (sc && (!WideAffine(sc, nullptr, sa, b.shift) || b.shift < 0)))
                break;
            b.ix = ix;
            if (j > 0 && (!SamePure(ix->obj, w[0].ix->obj) || (base0 == nullptr) != (base == nullptr) ||
                          (base && !SamePure(base, base0)) || !SamePure(v, v0)))
                break;
            if (!j) {
                base0 = base;
                v0 = v;
            }
            w.push_back(b);
        }
        if ((int64_t)w.size() != n) continue;
        auto k0 = w[0].off;
        for (auto &x : w) k0 = std::min(k0, x.off);
        vector<int64_t> shiftat((size_t)n, -1);
        auto ok = true;
        for (auto &x : w) {
            auto d = x.off - k0;
            if (d >= n || shiftat[(size_t)d] >= 0) {
                ok = false;
                break;
            }
            shiftat[(size_t)d] = x.shift;
        }
        if (!ok) continue;
        auto le = true, be = true;
        for (int64_t d = 0; d < n; d++) {
            le = le && shiftat[(size_t)d] == 8 * d;
            be = be && shiftat[(size_t)d] == 8 * (n - 1 - d);
        }
        if (!le && !be) continue;
        auto first = Is<Assign>(stmts[i]);
        if (first->line.line > 0) curline = first->line;
        PushSc(SC_STMT);
        auto l = GenLoc(w[0].ix->obj);
        while (l.t->kind == TY_REF) DerefLoc(l);
        auto view = ArrayView(l, w[0].ix->line);
        auto at = WideAt(IndexValue(w[0].ix->idx), k0 - w[0].off);
        auto ut = ast.inttypes[n == 2 ? IS_U16 : n == 4 ? IS_U32 : IS_U64];
        auto x = Coerce(GenX(v0), v0->exprtype, ut, first->line);
        usesbinary = usesbinary || !dry;
        L("binary.", WideOrder(le), ".PutUint", 8 * n, "(", WideBytes(view, at, n), ", ", x, ")");
        PopSc();
        return (size_t)n;
    }
    return 0;
}

// A block's statements from `first` on, each run of byte stores that is
// one wide store (GenWideStoreRun) emitted as one.
inline void CodeGenGo::GenStmts(const vector<Node *> &stmts, size_t first) {
    for (auto i = first; i < stmts.size();) {
        auto n = Unreached() ? 0 : GenWideStoreRun(stmts, i);
        if (n) i += n;
        else GenStmt(stmts[i++]);
    }
}

}  // namespace goose
