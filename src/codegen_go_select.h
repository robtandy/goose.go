// Goose compiler — the Go backend's selects (definitions of CodeGenGo
// members, codegen_go.h; DESIGN.md §3.9, issue #26). A body that does
// nothing but choose among values it computes nothing for -- std's min,
// max, clamp, abs and sign, inlined or not, a function of the program's own
// written so, an if choosing between values -- is written as Go's builtin
// min or max of two integers, or as a temporary assigned the value no
// condition picks and then, condition by condition, the others, the first
// condition last, so that it wins:
//
//     t := x
//     if hi < x {
//         t = hi
//     }
//     if x < lo {
//         t = lo
//     }
//
// Go makes a conditional select of each such `if` (CSEL on arm64, CMOV on
// amd64), and of its min and max, where the value is an integer of two
// bytes or more; a byte's choice is made at four, since amd64 has no
// conditional move of one byte. The labeled `switch { default: }` an
// inlined body is otherwise, each of its exits assigning the destination,
// is a select only where the destination is a local and the body has one
// condition: a choice stored into an element is a store on each side, and
// clamp's three exits are branches.
//
// Every condition and value runs whichever value is chosen, so each must be
// computed by nothing that runs a statement or can fail (SelPure): scalar
// variables and constants, comparisons of them, `!`, and a negation where no
// debug build checks it. A choice of x or y by a comparison of the two
// integers is their least or greatest however the comparison breaks ties,
// equal integers being the same value. Floats are not so -- -0.0 and 0.0 are
// equal and differ, and a NaN is in no order -- and Go's builtins do not
// choose for them as std does (Go's max(1, NaN) is NaN, std's 1), so a
// float's choice is the assignments, which Go leaves a branch: it makes no
// select of a float.
//
// The builtins are no new dependence: the runtime every file holds calls
// them too (codegen_go_runtime.h), and the generated code's own names end
// in _g or start with gs or Gs, so nothing of the file's declares one.
#pragma once

namespace goose {

// Whether n is a scalar a select may compute whichever value it chooses:
// nothing in it runs a statement or can fail.
inline bool CodeGenGo::SelPure(Node *n) {
    if (!n || !IsScalarT(n->exprtype) || fillvalues.count(n)) return false;
    switch (n->nkind) {
        case NK_IntLit: case NK_FltLit: case NK_BoolLit: return true;
        case NK_Ident: {
            auto id = (Ident *)n;
            return id->vdef && IsScalarT(id->vdef->type);
        }
        case NK_Unary: {
            auto u = (Unary *)n;
            if (u->op == T_NOT) return SelPure(u->child);
            // A debug build checks an integer's negation, which fails at its
            // type's least value.
            if (u->op == T_MINUS)
                return SelPure(u->child) && (!debug || u->child->exprtype->kind == TY_FLT);
            return false;
        }
        case NK_Binary: {
            auto b = (Binary *)n;
            switch (b->op) {
                case T_LT: case T_GT: case T_LTEQ: case T_GTEQ: case T_EQ: case T_NEQ:
                    return SelPure(b->left) && SelPure(b->right);
                default: return false;
            }
        }
        default: return false;
    }
}

// The value an exit of a chain returns to target: a return of one value,
// alone in its block.
static Node *SelExit(Node *n, SFunction *target) {
    if (!target) return nullptr;
    auto r = Is<Return>(n);
    if (auto b = Is<Block>(n)) {
        if (b->stmts.size() == 1 && !b->tail) r = Is<Return>(b->stmts[0]);
        else if (b->stmts.empty()) r = Is<Return>(b->tail);
    }
    return r && r->target == target && r->vals.size() == 1 ? r->vals[0] : nullptr;
}

// The value an arm of an if picks: an exit returning one to target, or the
// value alone in its block, or alone where the optimizer unwrapped the
// block (Block::Opt; an else may be any expression).
static Node *SelArmValue(Node *n, SFunction *target) {
    if (auto v = SelExit(n, target)) return v;
    auto b = Is<Block>(n);
    if (!b) return Is<Return>(n) ? nullptr : n;
    return b->stmts.empty() && b->tail && !Is<Return>(b->tail) ? b->tail : nullptr;
}

// The choices the statements of b from first on and its tail make, each
// condition with the value it picks, in the order they are tested, and last
// the value none picks: any number of `if c { return x; }`, then the end of
// the chain (SelEnd), the last statement or the tail. target is the
// function a return leaves (the inlined one, or the one emitted); nullptr
// where no return may (an if's value). valued: the tail's value is the
// chain's.
//
// The end is a guard, whose then-block goes on with the chain; an if whose
// arms are values (or returns) and whose else may be an if of its own; a
// return; or, valued, the value itself.
inline bool CodeGenGo::SelChain(Block *b, size_t first, SFunction *target, bool valued,
                                vector<SelArm> &arms) {
    for (auto i = first; i < b->stmts.size(); i++) {
        auto fi = Is<IfExpr>(b->stmts[i]);
        if (fi && !fi->flat && !fi->elseb) {
            auto v = SelExit(fi->thenb, target);
            if (!v) return false;
            arms.push_back({ fi->cond, false, v });
            continue;
        }
        return i + 1 == b->stmts.size() && !b->tail && SelEnd(b->stmts[i], target, false, arms);
    }
    return b->tail && SelEnd(b->tail, target, valued, arms);
}

inline bool CodeGenGo::SelEnd(Node *n, SFunction *target, bool valued, vector<SelArm> &arms) {
    if (auto fi = Is<IfExpr>(n)) {
        if (fi->flat) {
            // A guard: its else exits where the condition does not hold.
            auto v = SelExit(fi->elseb, target);
            if (!v) return false;
            arms.push_back({ fi->cond, true, v });
            return SelChain(fi->thenb, 0, target, valued, arms);
        }
        if (!fi->elseb) return false;
        auto pick = [&](Node *arm) { return valued ? SelArmValue(arm, target) : SelExit(arm, target); };
        auto v = pick(fi->thenb);
        if (!v) return false;
        arms.push_back({ fi->cond, false, v });
        if (auto ei = Is<IfExpr>(fi->elseb)) return SelEnd(ei, target, valued, arms);
        auto e = pick(fi->elseb);
        if (!e) return false;
        arms.push_back({ nullptr, false, e });
        return true;
    }
    if (auto r = Is<Return>(n)) {
        if (!target || r->target != target || r->vals.size() != 1) return false;
        arms.push_back({ nullptr, false, r->vals[0] });
        return true;
    }
    if (!valued || !IsScalarT(n->exprtype)) return false;
    arms.push_back({ nullptr, false, n });
    return true;
}

// Whether a chain of one condition and the value none picks chooses the
// least or the greatest of two integers of type t: `if x < y { return p; }
// return q;`, {p, q} being {x, y}, under any order and either way round.
// Equal integers are the same value, so the comparison's ties do not
// matter. "min", "max" or "".
inline string CodeGenGo::SelMinMax(const vector<SelArm> &arms, TypeExpr *t) {
    if (arms.size() != 2 || !t || t->kind != TY_INT || IsVarintT(t)) return "";
    auto b = Is<Binary>(arms[0].cond);
    if (!b || !SameGoT(b->left->exprtype, t) || !SameGoT(b->right->exprtype, t)) return "";
    // Whether the condition holds where its left operand is the lesser; a
    // guard picks where its condition does not hold, which for integers
    // is the converse order.
    bool lesser;
    switch (b->op) {
        case T_LT: case T_LTEQ: lesser = true; break;
        case T_GT: case T_GTEQ: lesser = false; break;
        default: return "";
    }
    if (arms[0].neg) lesser = !lesser;
    auto picked = arms[0].val, other = arms[1].val;
    if (SamePure(picked, b->left) && SamePure(other, b->right)) return lesser ? "min" : "max";
    if (SamePure(picked, b->right) && SamePure(other, b->left)) return lesser ? "max" : "min";
    return "";
}

// The value a chain chooses, of type t, into x: Go's min or max of two
// integers, or a temporary its assignments leave it in. False, with
// nothing emitted, where the values are not all of t's Go type or a part
// would emit a statement (a constant's negation is held in a variable,
// say).
inline bool CodeGenGo::GenSelect(const vector<SelArm> &arms, TypeExpr *t, Line ln, string &x) {
    if (arms.size() < 2) return false;
    for (size_t i = 0; i < arms.size(); i++) {
        auto &a = arms[i];
        if (!a.cond != (i + 1 == arms.size()) || !SelPure(a.val) || (a.cond && !SelPure(a.cond)) ||
            !SameGoT(a.val->exprtype, t))
            return false;
    }
    vector<string> conds(arms.size()), vals(arms.size());
    for (size_t i = 0; i < arms.size(); i++) {
        if (arms[i].cond && !TryPure([&] { return GenTruth(arms[i].cond); }, conds[i])) return false;
        if (!TryPure([&] { return GenX(arms[i].val); }, vals[i])) return false;
    }
    auto gt = GoT(t, ln);
    // A byte's choice is made at four bytes, where amd64 has a conditional
    // move; extending a byte keeps its order.
    auto wt = t->kind == TY_INT && IntBits(t->intstorage) == 8
                  ? string(IsUnsigned(t->intstorage) ? "uint32" : "int32") : string();
    auto wide = [&](const string &v) { return wt.empty() ? v : cat(wt, "(", v, ")"); };
    auto narrow = [&](const string &v) { return wt.empty() ? v : cat(gt, "(", v, ")"); };
    if (auto mm = SelMinMax(arms, t); !mm.empty()) {
        // The value none picks first: std's min(a, b) is min(a, b).
        x = narrow(cat(mm, "(", wide(vals[1]), ", ", wide(vals[0]), ")"));
        return true;
    }
    auto tv = T();
    Decl(tv, wt.empty() ? gt : wt, wide(vals.back()));
    for (auto i = arms.size() - 1; i-- > 0;) {
        L("if ", arms[i].neg ? cat("!", conds[i]) : CondText(conds[i]), " {");
        ind++;
        L(tv, " = ", wide(vals[i]));
        ind--;
        LS("}");
    }
    x = narrow(tv);
    return true;
}

// The value of an inlined body that only chooses (EmitIbBody), into d, its
// arguments bound already from stmts[first] on: true where it is one.
inline bool CodeGenGo::GenSelectIb(InlineBlock *ib, const GoDst &d, size_t first) {
    // A condition's truth takes the body's returns as branches, as C's does.
    if (d.k == GD_STACK || d.pool || d.truth || !IsScalarT(ib->exprtype)) return false;
    if (d.k == GD_LVALUE && d.t && !IsScalarT(d.t)) return false;
    vector<SelArm> arms;
    if (!SelChain(ib->body, first, ib->sf, true, arms)) return false;
    // Nothing in it has an effect.
    if (d.k == GD_DISCARD) {
        for (auto &a : arms)
            if (!SelPure(a.val) || (a.cond && !SelPure(a.cond))) return false;
        return true;
    }
    string x;
    if (!GenSelect(arms, ib->exprtype, ib->line, x)) return false;
    x = Coerce(x, ib->exprtype, d.t ? d.t : ib->exprtype, ib->line);
    if (x != d.s) L(d.s, " = ", x);
    return true;
}

// An if that chooses between values (GenIf), into the lvalue d: true where
// it is one.
inline bool CodeGenGo::GenSelectIf(IfExpr *fi, const GoDst &d) {
    if (d.k != GD_LVALUE || d.pool || d.truth || fi->flat || !fi->elseb || !IsScalarT(fi->exprtype) ||
        (d.t && !IsScalarT(d.t)))
        return false;
    vector<SelArm> arms;
    string x;
    if (!SelEnd(fi, nullptr, true, arms) || !GenSelect(arms, fi->exprtype, fi->line, x)) return false;
    x = Coerce(x, fi->exprtype, d.t ? d.t : fi->exprtype, fi->line);
    if (x != d.s) L(d.s, " = ", x);
    return true;
}

// A function's body that only chooses among its values (EmitSpec), as std's
// min and max are where nothing is inlined (-O0): one return of the choice.
inline bool CodeGenGo::GenSelectReturn(FnSpec *sp) {
    if (emiter || fromids.count(sp) || sp->rets.size() != 1 || !IsScalarT(sp->rets[0])) return false;
    vector<SelArm> arms;
    string x;
    if (!SelChain(sp->body, 0, sp->sf, true, arms) || !GenSelect(arms, sp->rets[0], sp->sf->line, x))
        return false;
    EmitExitRestores(0);
    L("return ", x);
    dead = true;
    return true;
}

}  // namespace goose
