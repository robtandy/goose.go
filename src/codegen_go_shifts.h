// Goose compiler — the shifts --target=go warns of (issue #27). A shift
// computes at its left operand's type, whose width masks its count
// (goose_spec.md §6.1, §6.2): with a u8 b, `b << 16` is `b << 0`, so
// `let v: u32 = b << 16;` stores b itself where `(b as u32) << 16` was
// meant, and nothing says so. A count that is a constant outside
// 0..width-1 is never one a program needs, since the masked count could be
// written instead; the checker lets every such shift pass. Until the shared
// front-end diagnoses them (docs/upstream/shift-diagnostic.md), the Go
// backend warns of them on the checked program it receives, before the
// optimizer folds or inlines anything: what it warns of is what the source
// says, at every -O level, once per shift as the source has it.
#pragma once

namespace goose {

// A count the checker takes for an integer constant (Val::ck == CK_INT): a
// literal, a named constant (VarDef::constlit), and `-` and the integer
// operators of constants, folded as TypeCheck::FoldInt folds them, at i64,
// or at u64 where an operand is above i64.max. A cast, a `let` that is no
// named constant, or anything else is no constant to the checker, nor here.
struct ShiftCount {
    int64_t val = 0;
    bool uns = false;   // val's bits are a u64 above i64.max (IntLit::uns).
};

inline bool ConstShiftCount(const Node *n, ShiftCount &c) {
    if (auto i = Is<IntLit>(n)) {
        if (i->fixedtype) return false;
        c = { i->val, i->uns };
        return true;
    }
    if (auto id = Is<Ident>(n)) {
        auto vd = id->vdef;
        // A named `~c` adapts to its width (Val::notconst): no constant yet.
        if (!vd || !vd->constlit || vd->constnot || !vd->type || !IsIntT(vd->type)) return false;
        c = { vd->constval, vd->constuns };
        return true;
    }
    if (auto u = Is<Unary>(n); u && u->op == T_MINUS) {
        ShiftCount v;
        // -(2^63) is i64.min; -(i64.min) is no constant (NumericUnary).
        if (!ConstShiftCount(u->child, v) || (v.uns ? v.val != INT64_MIN : v.val == INT64_MIN))
            return false;
        c = { v.uns ? INT64_MIN : -v.val, false };
        return true;
    }
    if (auto b = Is<Binary>(n)) {
        ShiftCount l, r;
        if (!ConstShiftCount(b->left, l) || !ConstShiftCount(b->right, r)) return false;
        auto shift = b->op == T_SHL || b->op == T_SHR;
        auto s = (shift ? l.uns : l.uns || r.uns) ? IS_U64 : IS_I64;
        // Nor is a comparison, a signed result past i64 or a quotient by
        // zero (FoldIntOp).
        int64_t out;
        if (!FoldIntOp(b->op, l.val, r.val, s, out)) return false;
        c = { out, s == IS_U64 && out < 0 };
        return true;
    }
    return false;
}

// The integer type a shift computes at, of its left operand's (or compound
// assignment's target's) type t: t, a plain reference's pointee, and i64
// for a varint's value (CodeGenGo::GenBinary); null for any other.
inline TypeExpr *ShiftedType(Ast &ast, TypeExpr *t) {
    if (t && IsPlainRef(t)) t = t->ref->sub;
    if (!t || t->kind != TY_INT) return nullptr;
    return t->intstorage == IS_VARINT ? ast.inttypes[IS_I64] : t;
}

// The node that prints for n as the source writes it (Written): past a
// conversion or an `&` the checker inserted.
inline const Node *WrittenNode(const Node *n) {
    dumpwritten = true;
    auto w = Written(n);
    dumpwritten = false;
    return w;
}

// The expression n as the source writes it, as TypeCheck::ExprStr gives it;
// where `grouped`, in parentheses unless it is a primary expression or one
// its dump parenthesizes (a binary operation, a cast), so that no `as` or
// operator after it binds to a part of it (`-x as u32` is `-(x as u32)`).
inline string WrittenExpr(const Node *n, bool grouped = false) {
    string s;
    auto w = WrittenNode(n);
    dumpwritten = true;
    w->Dump(s, 0);
    dumpwritten = false;
    if (grouped && !Is<Ident>(w) && !Is<IntLit>(w) && !Is<Index>(w) && !Is<Dot>(w) &&
        !Is<Call>(w) && !Is<Binary>(w) && !Is<AsCast>(w))
        s = cat("(", s, ")");
    return s;
}

// What --target=go warns of a shift at t by a constant count outside its
// width (nothing where the count is no constant or in range): what the
// shift does instead and, for
// `x << c` (not `x <<= c`), the shift that keeps x's bits, its operand cast
// first to the type the value is widened to at once (the shift's own type,
// once the checker has fitted it to a wider destination or operand, as in
// `let v: u32 = b << 16;`), or else to the narrowest integer type of its
// signedness wide enough for the count.
inline string ShiftCountWarning(const Node *at, TypeExpr *t, Node *left, Node *count,
                                TypeExpr *widened) {
    ShiftCount c;
    if (!ConstShiftCount(count, c)) return {};
    auto s = t->intstorage;
    auto bits = IntBits(s);
    if (!c.uns && c.val >= 0 && c.val < bits) return {};
    auto by = c.uns ? to_string((uint64_t)c.val) : to_string(c.val);
    auto msg = cat("shift of ", IntStorageName(s), " by ", by, " is a shift by ",
                   (uint64_t)c.val & (uint64_t)(bits - 1), ": at ", IntStorageName(s),
                   " the count is masked to 0..", bits - 1, " (§6.2)");
    auto b = Is<Binary>(at);
    if (!b || b->op != T_SHL || c.uns || c.val < 0 || c.val >= 64) return msg;
    auto wide = -1;
    if (widened && IsIntT(widened) && c.val < IntBits(widened->intstorage))
        wide = widened->intstorage;
    for (auto w = (int)s + 1; wide < 0 && w <= (IsUnsigned(s) ? (int)IS_U64 : (int)IS_I64); w++)
        if (c.val < IntBits((IntStorage)w)) wide = w;
    if (wide < 0) return msg;
    // An operand widened by a cast of its own, `(c[2] as u16) << 16`, is
    // cast to the wider type instead.
    auto operand = (const Node *)left;
    if (auto ac = Is<AsCast>(WrittenNode(left)); ac && ac->totype && IsIntT(ac->totype)) {
        auto ct = ac->child->exprtype;
        if (ct && IsPlainRef(ct)) ct = ct->ref->sub;
        if (ct && IsIntT(ct) && IntBits(ct->intstorage) < IntBits(ac->totype->intstorage))
            operand = ac->child;
    }
    return cat(msg, "; cast the operand first: `(", WrittenExpr(operand, true), " as ",
               IntStorageName(wide), ") << ", WrittenExpr(count, true), "`");
}

// Warns of every shift (`<<`, `>>`, `<<=`, `>>=`) of the checked program by
// a constant count outside its type's width: in every specialization's body
// and every global's initializer, the code that runs (RunChildren), once per
// shift as the source has it, whichever specializations share it (a generic
// function's, say, at each type it is warned of). In the order of the
// source, after the checker's own warnings.
inline void WarnShiftCounts(Ast &ast) {
    struct Found {
        DiagLoc where;
        string msg;
    };
    vector<Found> found;
    set<pair<const Node *, string>> seen;
    auto note = [&](const Node *at, string msg) {
        if (!msg.empty() && seen.insert({ at->Origin(), msg }).second)
            found.push_back({ LocOf(at), std::move(msg) });
    };
    function<void(Node *)> walk = [&](Node *n) {
        // A function value is checked where it is called, as a body of its
        // own (Call::fvbody); its template is no code that runs.
        if (!n || Is<FunVal>(n)) return;
        if (auto b = Is<Binary>(n); b && (b->op == T_SHL || b->op == T_SHR)) {
            if (auto t = ShiftedType(ast, b->left->exprtype))
                note(b, ShiftCountWarning(b, t, b->left, b->right, b->exprtype));
        } else if (auto a = Is<Assign>(n); a && (a->op == T_SHLEQ || a->op == T_SHREQ)) {
            if (auto t = ShiftedType(ast, a->lval->exprtype))
                note(a, ShiftCountWarning(a, t, a->lval, a->rhs, nullptr));
        }
        RunChildren(n, walk);
    };
    for (auto sp : ast.fnspecs)
        if (sp->body) walk(sp->body);
    ast.ForEachRootTree([&](Node *&n) { walk(n); });
    stable_sort(found.begin(), found.end(), [](const Found &x, const Found &y) {
        return tie(x.where.fileidx, x.where.line, x.where.col) <
               tie(y.where.fileidx, y.where.line, y.where.col);
    });
    for (auto &f : found) ast.Warn(f.where, f.msg);
}

}  // namespace goose
