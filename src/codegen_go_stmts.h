// Goose compiler — the Go backend's statements (definitions of CodeGenGo
// members, codegen_go.h): scopes and their exits, `defer` (§6.6), blocks,
// conditionals, loops with their break and continue, declarations,
// assignment, and returns.
#pragma once

namespace goose {

// ------------------------------------------------------------------
// Scopes.

inline void CodeGenGo::PushSc(int kind) {
    GoScope s;
    s.kind = kind;
    s.stkbase = stknext;
    scopes.push_back(s);
}

// Leaves the innermost scope at its end, which runs its restores (the
// generated ones only, where control does not get there).
inline void CodeGenGo::PopSc() {
    if (Unreached()) GenUnreached([&] { EmitRestores(scopes.back()); });
    else EmitRestores(scopes.back());
    stknext = scopes.back().stkbase;
    scopes.pop_back();
}

// Code no path reaches, which the program has where Go's flow has ended:
// none of it is emitted, since Go's vet reports such code, but it is
// generated all the same, into text that is dropped. The C backend emits
// it, so what it would take of the data stacks (AllocStk, NoteStackCall)
// is counted as the C backend counts it, and both report and reserve the
// same stacks (--stacks). Nothing else it does reaches the code that is
// kept: not its text, nor the labels it names or the defers it registers.
// The names it gives its locals stay theirs, for the unreached code after
// it in their scope, which is generated the same way; they are unique, and
// no code that is kept is in that scope.
inline void CodeGenGo::GenUnreached(const function<void()> &gen) {
    struct Mark { bool usedlabel, usedbrk; size_t defers; };
    vector<Mark> marks;
    for (auto &s : scopes) marks.push_back({ s.usedlabel, s.usedbrk, s.defers.size() });
    auto nscopes = scopes.size();
    auto text = std::move(body);
    body.clear();
    auto saveind = ind, savenext = stknext;
    dry++;
    dead = false;
    gen();
    dry--;
    if (scopes.size() != nscopes) Fail(Line {}, "internal: unbalanced scopes in unreached code");
    for (size_t i = 0; i < nscopes; i++) {
        scopes[i].usedlabel = marks[i].usedlabel;
        scopes[i].usedbrk = marks[i].usedbrk;
        scopes[i].defers.resize(marks[i].defers);
    }
    body = std::move(text);
    ind = saveind;
    stknext = savenext;
    dead = true;
}

// A data stack index for the innermost scope, free again once it ends.
inline string CodeGenGo::AllocStk() {
    auto k = stknext++;
    stkmax = std::max(stkmax, stknext);
    // Spaced as gofmt spaces an index.
    auto idx = SpIdx(k);
    idx.erase(std::remove(idx.begin(), idx.end(), ' '), idx.end());
    return cat("gs.Stks[", idx, "]");
}

// What an exit through scope s runs: its `defer` bodies, latest first. A
// body opens scopes of its own, which may move the scope vector, so the
// list is copied first.
inline void CodeGenGo::EmitRestores(const GoScope &s) {
    auto defers = s.defers;
    for (auto i = (int)defers.size() - 1; i >= 0; i--) EmitDeferBody(defers[i]);
}

// What an exit runs on its way out down to (and including) scope `to`.
inline void CodeGenGo::EmitExitRestores(int to) {
    for (auto i = (int)scopes.size() - 1; i >= to; i--) EmitRestores(scopes[i]);
}

// A `defer` statement (§6.6): its body joins the exits of the block it is
// in, the innermost scope that is not a statement's.
inline void CodeGenGo::RegisterDefer(Defer *d) {
    for (auto i = (int)scopes.size() - 1; i >= 0; i--) {
        if (scopes[i].kind == SC_STMT) continue;
        scopes[i].defers.push_back(d);
        return;
    }
    Fail(d->line, "internal: defer outside every scope");
}

// One run of a deferred body at an exit of its block, in a Go block of its
// own. Every exit emits the body again, so what it declares is named afresh
// each time (CodeGen::NameScope). The body cannot leave (§6.6).
inline void CodeGenGo::EmitDeferBody(Defer *d) {
    if (Unreached()) return GenUnreached([&] { EmitDeferBody(d); });
    auto names = vnames;
    PushSc(SC_PLAIN);
    L("{");
    ind++;
    for (auto st : d->body->stmts) GenStmt(st);
    if (d->body->tail) GenStmt(d->body->tail);
    PopSc();
    ind--;
    LS("}");
    vnames = std::move(names);
}

// A scope whose Go statement a break, continue or return can name: a loop's
// `for`, a block's or an inlined body's `switch`. Its label is written in
// front of the statement (LabelHere) once something names it (UseLabel).
inline void CodeGenGo::OpenLabeled(int kind) {
    PushSc(kind);
    scopes.back().label = Lbl();
}

inline void CodeGenGo::LabelHere() {
    scopes.back().labelat = body.size();
    scopes.back().labelind = ind;
}

inline string CodeGenGo::UseLabel(int si, bool brk) {
    scopes[si].usedlabel = true;
    if (brk) scopes[si].usedbrk = true;
    return scopes[si].label;
}

// Leaves the labeled scope at its end; its restores already ran where they
// had to (the caller emptied them otherwise). Returns it, for PlaceLabel.
inline CodeGenGo::GoScope CodeGenGo::PopLabeled() {
    if (Unreached()) GenUnreached([&] { EmitRestores(scopes.back()); });
    else EmitRestores(scopes.back());
    auto s = scopes.back();
    stknext = s.stkbase;
    scopes.pop_back();
    return s;
}

// The label in front of its statement, where something named it. Every
// scope opened after it has closed, so the offsets of those still open lie
// before it and stay where they are.
inline void CodeGenGo::PlaceLabel(const GoScope &s) {
    if (!s.usedlabel) return;
    string line((size_t)std::max(s.labelind - 1, 0), '\t');
    Append(line, s.label, ":\n");
    body.insert(s.labelat, line);
}

// ------------------------------------------------------------------
// Blocks.

// A Block's contents, without its scope or braces (the caller's). Its tail
// is its value; an inlined body's tail is a return too (exitscope).
inline void CodeGenGo::GenBlockInner(Block *b, const GoDst &d, size_t first, int exitscope) {
    for (auto i = first; i < b->stmts.size(); ++i) GenStmt(b->stmts[i]);
    if (!b->tail) return;
    if (Unreached()) return GenUnreached([&] { GenBlockInner(b, d, b->stmts.size(), exitscope); });
    if (!IsVoidT(b->tail->exprtype) && d.k != GD_DISCARD) {
        if (exitscope >= 0) GenExitValue(b->tail, exitscope);
        else GenAny(b->tail, d);
    } else {
        GenAny(b->tail, GoDst {});
    }
}

// The argument bindings an inlined call's block starts with (`inline_arg`),
// made in the current scope, as a call makes its arguments in the caller's
// (CodeGen::GenInlineArgs). Returns how many there are.
inline size_t CodeGenGo::GenInlineArgs(Block *b) {
    size_t n = 0;
    while (n < b->stmts.size()) {
        auto arg = Is<VarDecl>(b->stmts[n]);
        if (!arg || !arg->inline_arg) break;
        GenStmt2(arg);
        n++;
    }
    return n;
}

inline void CodeGenGo::GenStmt(Node *n) {
    if (Unreached()) return GenUnreached([&] { GenStmt(n); });
    PushSc(SC_STMT);
    GenStmt2(n);
    PopSc();
}

inline void CodeGenGo::GenStmt2(Node *n) {
    if (Unreached()) return GenUnreached([&] { GenStmt2(n); });
    switch (n->nkind) {
        case NK_VarDecl: GenVarDecl((VarDecl *)n); return;
        case NK_Assign: GenAssign((Assign *)n); return;
        case NK_IncDec: GenIncDec((IncDec *)n); return;
        case NK_While: GenWhile((While *)n); return;
        case NK_ForLoop: GenFor((ForLoop *)n); return;
        case NK_Return: GenReturn((Return *)n); return;
        case NK_Break: GenBreak((Break *)n); return;
        case NK_Continue: GenContinue((Continue *)n); return;
        case NK_Defer: RegisterDefer((Defer *)n); return;
        // A nested function is a specialization of its own.
        case NK_FnDecl: case NK_StructDecl: case NK_EnumDecl: case NK_AliasDecl: return;
        default: GenAny(n, GoDst {}); return;
    }
}

inline void CodeGenGo::GenBlock(Block *b, const GoDst &d) {
    // A self-call's arguments, bound in front of the base case inlined in
    // its place (optimize_basecase.h).
    auto first = GenInlineArgs(b);
    if (Unreached()) return GenUnreached([&] { GenBlock(b, d); });
    PushSc(SC_PLAIN);
    L("{");
    ind++;
    GenBlockInner(b, d, first);
    PopSc();
    ind--;
    LS("}");
}

// ------------------------------------------------------------------
// Conditionals.

inline void CodeGenGo::GenIf(IfExpr *fi, const GoDst &d) {
    auto c = GenTruth(fi->cond);
    if (Unreached()) return GenUnreached([&] { GenIf(fi, d); });
    if (fi->flat) {
        // The else cannot complete normally, so the then-block's contents
        // follow it in this block (IfExpr::flat): a run of guards nests no
        // Go blocks, however long.
        L("if !", c, " {");
        ind++;
        PushSc(SC_PLAIN);
        if (auto b = Is<Block>(fi->elseb)) GenBlockInner(b, d);
        else GenAny(fi->elseb, d);
        scopes.back().defers.clear();   // Its end is never reached.
        PopSc();
        ind--;
        LS("}");
        dead = false;
        PushSc(SC_PLAIN);
        GenBlockInner(fi->thenb, d);
        PopSc();
        return;
    }
    L("if ", CondText(c), " {");
    ind++;
    PushSc(SC_PLAIN);
    GenBlockInner(fi->thenb, d);
    PopSc();
    ind--;
    auto thendead = dead;
    dead = false;
    if (!fi->elseb) {
        LS("}");
        return;
    }
    LS("} else {");
    ind++;
    PushSc(SC_PLAIN);
    if (auto b = Is<Block>(fi->elseb)) GenBlockInner(b, d);
    else GenAny(fi->elseb, d);
    PopSc();
    ind--;
    LS("}");
    dead = thendead && dead;
}

// A match over an integer: a chain of ifs in arm order, comparing at the
// scrutinee's type, whose constants carry that type's value bits.
inline void CodeGenGo::GenMatch(MatchExpr *m, const GoDst &d) {
    auto st = m->scrutinee->exprtype;
    if (!st || st->kind != TY_INT) {
        if (st && (st->kind == TY_ENUM || (st->kind == TY_REF && st->ref->sub->kind == TY_ENUM)))
            Unsupported(m->line, "a match over an enum");
        Unsupported(m->line, cat("a match over ", TypeText(st)));
    }
    GoT(st, m->line);
    auto x = GenPure(m->scrutinee);
    if (Unreached()) return GenUnreached([&] { GenMatch(m, d); });
    auto first = true, alldead = true, wild = false;
    for (auto &arm : m->arms) {
        string open;
        if (arm.pat.kind == P_WILDCARD) {
            open = first ? "{" : "} else {";
            wild = true;
        } else {
            string c;
            auto several = arm.ranges.size() > 1;
            for (auto &r : arm.ranges) {
                if (!c.empty()) c += " || ";
                auto lo = GoIntLit(r.lo, false, st, m->line);
                if (r.hi == r.lo) Append(c, x, " == ", lo);
                else Append(c, several ? "(" : "", x, " >= ", lo, " && ", x, " <= ",
                            GoIntLit(r.hi, false, st, m->line), several ? ")" : "");
            }
            open = cat(first ? "" : "} else ", "if ", c, " {");
        }
        if (first) L(open);
        else LS(open);
        dead = false;
        first = false;
        ind++;
        PushSc(SC_PLAIN);
        GenAny(arm.body, d);
        PopSc();
        ind--;
        alldead = alldead && dead;
        if (wild) break;   // Nothing after a wildcard is reached.
    }
    if (first) return;
    LS("}");
    dead = alldead && wild;
}

// `block { ... }` (§6.4): a switch with only a default, which a break
// leaves, that is a block of its own otherwise.
inline void CodeGenGo::GenEarlyBlock(EarlyBlock *eb, const GoDst &d) {
    if (Unreached()) return GenUnreached([&] { GenEarlyBlock(eb, d); });
    OpenLabeled(SC_BLOCK);
    scopes.back().dst = d;
    LabelHere();
    L("switch {");
    L("default:");
    ind++;
    GenBlockInner(eb->body, d);
    auto s = PopLabeled();
    ind--;
    LS("}");
    PlaceLabel(s);
    dead = dead && !s.usedbrk;
}

// An inlined call's body: a `return` to the callee leaves it with its value,
// as a break leaves a block.
inline void CodeGenGo::GenInlineBlock(InlineBlock *ib, const GoDst &d) {
    auto first = GenInlineArgs(ib->body);
    if (Unreached()) return GenUnreached([&] { GenInlineBlock(ib, d); });
    OpenLabeled(SC_IB);
    auto si = (int)scopes.size() - 1;
    scopes[si].ibsf = ib->sf;
    scopes[si].dst = d;
    LabelHere();
    L("switch {");
    L("default:");
    ind++;
    PushSc(SC_PLAIN);
    GenBlockInner(ib->body, d, first, si);
    PopSc();
    ind--;
    LS("}");
    auto s = PopLabeled();
    PlaceLabel(s);
    dead = dead && !s.usedbrk;
}

// ------------------------------------------------------------------
// Loops: a labeled `for`, whose scope is the Goose loop's. A break leaves
// it and a continue starts its next iteration, each running the restores of
// the scopes it leaves; the end of an iteration runs the loop scope's own.

inline void CodeGenGo::GenLoopBody(Block *bodyb, size_t first) {
    for (auto i = first; i < bodyb->stmts.size(); i++) GenStmt(bodyb->stmts[i]);
    if (bodyb->tail) GenStmt(bodyb->tail);
    if (Unreached()) GenUnreached([&] { EmitRestores(scopes.back()); });
    else EmitRestores(scopes.back());
    // Restored by every path out of an iteration already.
    scopes.back().defers.clear();
}

inline void CodeGenGo::GenWhile(While *w) {
    if (Unreached()) return GenUnreached([&] { GenWhile(w); });
    // The `while v > 0 { v--; ... }` of a countdown loop (While::countdown)
    // is the counted loop it is as written.
    OpenLabeled(SC_LOOP);
    auto si = (int)scopes.size() - 1;
    // A condition that runs statements is tested at the top of the body.
    auto incond = !HasStmts(w->cond);
    string c;
    if (incond) c = GenTruth(w->cond);
    if (Unreached()) {   // Not without statements; but nothing opens unreached.
        PopLabeled();
        return GenUnreached([&] { GenWhile(w); });
    }
    LabelHere();
    L(incond ? cat("for ", CondText(c), " {") : string("for {"));
    ind++;
    if (!incond) {
        PushSc(SC_PLAIN);
        c = GenTruth(w->cond);
        PopSc();
        // A condition that ends the flow itself, with a break or a return
        // in it, leaves no test to make, and the body unreached.
        if (!dead) {
            L("if !", c, " {");
            ind++;
            L("break ", UseLabel(si, true));
            ind--;
            LS("}");
        }
    }
    GenLoopBody(w->body);
    ind--;
    LS("}");
    auto s = PopLabeled();
    PlaceLabel(s);
    // Go's flow goes on past a `for` with a condition, and past one without
    // only where a break leaves it.
    dead = !incond && !s.usedbrk;
}

inline void CodeGenGo::GenLoop(LoopExpr *lp, const GoDst &d) {
    if (Unreached()) return GenUnreached([&] { GenLoop(lp, d); });
    OpenLabeled(SC_LOOP);
    scopes.back().dst = d;
    LabelHere();
    L("for {");
    ind++;
    GenLoopBody(lp->body);
    ind--;
    LS("}");
    auto s = PopLabeled();
    PlaceLabel(s);
    dead = !s.usedbrk;
}

inline void CodeGenGo::GenFor(ForLoop *f) {
    if (f->iterkind != IK_RANGE && f->iterkind != IK_COUNT)
        Unsupported(f->line, "a for loop over an array or a slice");
    // The loop variable runs at the range's own type: narrow counters stay
    // narrow (§6.5). A binder whose type ends where the range does (`for i:
    // u8 in 0..256`) is a copy of a counter of the range's own, wider type,
    // which cannot overflow at the end.
    auto r = Is<RangeExpr>(f->iter);
    auto ct = r ? r->exprtype : f->iter->exprtype;
    auto gt = GoT(ct, f->line);
    auto lov = GoIntLit(0, false, ct, f->line);
    if (r) lov = Snapshot(ct, Coerce(GenX(r->lo), r->lo->exprtype, ct, f->line));
    auto hi = r ? r->hi : f->iter;
    auto hiv = Snapshot(ct, Coerce(GenX(hi), hi->exprtype, ct, f->line));
    if (Unreached()) return GenUnreached([&] { GenFor(f); });
    auto vt = f->vdef->type;
    GoT(vt, f->line);
    auto wide = !SameGoT(vt, ct);
    OpenLabeled(SC_LOOP);
    auto ctr = wide ? T() : LocalName(f->vdef);
    string head;
    if (f->idxdef) {
        auto it = f->idxdef->type;
        auto ix = LocalName(f->idxdef);
        head = cat("for ", ctr, ", ", ix, " := ", lov, ", ", GoIntLit(0, false, it, f->line), "; ",
                   ctr, " < ", hiv, "; ", ctr, ", ", ix, " = ", ctr, "+1, ", ix, "+1 {");
    } else {
        head = cat("for ", ctr, " := ", lov, "; ", ctr, " < ", hiv, "; ", ctr, "++ {");
    }
    LabelHere();
    L(head);
    ind++;
    if (wide) Decl(LocalName(f->vdef), GoT(vt, f->line), cat(GoT(vt, f->line), "(", ctr, ")"));
    GenLoopBody(f->body);
    ind--;
    LS("}");
    auto s = PopLabeled();
    PlaceLabel(s);
    dead = false;
}

// ------------------------------------------------------------------
// Exits.

inline void CodeGenGo::GenBreak(Break *b) {
    auto si = -1;
    for (auto i = (int)scopes.size() - 1; i >= 0; i--) {
        if (scopes[i].kind == SC_LOOP || scopes[i].kind == SC_BLOCK) { si = i; break; }
        if (scopes[i].kind == SC_FN) break;
    }
    if (si < 0) Fail(b->line, "internal: break outside a loop or block");
    if (b->val) GenExitValue(b->val, si);
    // A value that ends the flow itself leaves the break unreached.
    if (Unreached()) return GenUnreached([&] { GenBreak(b); });
    EmitExitRestores(si);
    L("break ", UseLabel(si, true));
    dead = true;
}

inline void CodeGenGo::GenContinue(Continue *c) {
    auto si = -1;
    for (auto i = (int)scopes.size() - 1; i >= 0; i--) {
        if (scopes[i].kind == SC_LOOP) { si = i; break; }
        if (scopes[i].kind == SC_FN) break;
    }
    if (si < 0) Fail(c->line, "internal: continue outside a loop");
    // Through the loop scope itself: the next iteration starts afresh.
    EmitExitRestores(si);
    L("continue ", UseLabel(si, false));
    dead = true;
}

inline void CodeGenGo::GenReturn(Return *r) {
    // Leaving an inlined body?
    for (auto i = (int)scopes.size() - 1; i >= 0; i--) {
        if (scopes[i].kind == SC_IB && scopes[i].ibsf == r->target) {
            // A function with several results is never inlined (Optimizer::Scan).
            if (r->vals.size() > 1) Fail(r->line, "internal: an inlined body with several results");
            if (!r->vals.empty()) GenExitValue(r->vals[0], i);
            if (Unreached()) return GenUnreached([&] { GenReturn(r); });
            EmitExitRestores(i);
            L("break ", UseLabel(i, true));
            dead = true;
            return;
        }
        if (scopes[i].kind == SC_FN) break;
    }
    if (curspec && r->target == curspec->sf) {
        GenNormalReturn(r->vals);
        return;
    }
    Unsupported(r->line, "`return ... from` (a long-distance return)");
}

// A break's value, or a return's leaving an inlined body, for scope si.
inline void CodeGenGo::GenExitValue(Node *val, int si) {
    auto d = scopes[si].dst;
    GenAny(val, d);
}

inline void CodeGenGo::GenNormalReturn(const vector<Node *> &vals) {
    if (Unreached()) return GenUnreached([&] { GenNormalReturn(vals); });
    auto sp = curspec;
    // A return passes through the exits of every scope, whose `defer`
    // bodies run after its values are taken.
    auto restores = false;
    for (auto &s : scopes) restores = restores || !s.defers.empty();
    vector<string> rv;
    if (vals.size() == 1 && sp->rets.size() > 1) {
        // A single call forwards all its results (§7.1).
        auto c = Is<Call>(vals[0]);
        if (!c || !c->spec || c->spec->rets.size() != sp->rets.size())
            Fail(vals[0]->line, "internal: a return forwarding no call's results");
        auto same = true;
        for (size_t i = 0; i < sp->rets.size(); i++)
            same = same && SameGoT(c->spec->rets[i], sp->rets[i]);
        auto call = SpecCallX(c, c->spec);
        if (Unreached()) return GenUnreached([&] { GenNormalReturn(vals); });
        if (same && !restores) {
            L("return ", call);
            dead = true;
            return;
        }
        string names, marks;
        for (size_t i = 0; i < sp->rets.size(); i++) {
            auto t = T();
            rv.push_back(Coerce(t, c->spec->rets[i], sp->rets[i], c->line));
            Append(names, i ? ", " : "", t);
            Append(marks, i ? "," : "", t);
        }
        L(names, " := ", call, DECLMARK, marks);
    } else {
        if (vals.size() != sp->rets.size()) Fail(sp->sf->line, "internal: return value count");
        for (size_t i = 0; i < vals.size(); i++) {
            auto later = false;
            for (auto j = i + 1; j < vals.size(); j++) later = later || HasStmts(vals[j]);
            auto x = Coerce(GenX(vals[i]), vals[i]->exprtype, sp->rets[i], vals[i]->line);
            if ((later || restores) && !IsGoConst(x)) x = Snapshot(sp->rets[i], x);
            rv.push_back(x);
        }
    }
    if (Unreached()) return GenUnreached([&] { GenNormalReturn(vals); });
    EmitExitRestores(0);
    string list;
    for (size_t i = 0; i < rv.size(); i++) Append(list, i ? ", " : "", rv[i]);
    L(list.empty() ? string("return") : cat("return ", list));
    dead = true;
}

// ------------------------------------------------------------------
// Declarations and assignment.

inline void CodeGenGo::GenVarDecl(VarDecl *vd) {
    // A multi-name binding from one call takes each result into its variable.
    if (vd->names.size() > 1 && vd->inits.size() == 1) {
        auto c = Is<Call>(vd->inits[0]);
        if (!c) Fail(vd->line, "internal: a multiple binding from no call");
        GenCallInto(c, vd->defs);
        return;
    }
    for (size_t i = 0; i < vd->defs.size(); i++)
        BindLocal(vd->defs[i], i < vd->inits.size() ? vd->inits[i] : nullptr);
}

// A local's Go name, given afresh at each declaration: a `defer` body emitted
// at several exits declares its locals again each time.
inline string CodeGenGo::LocalName(VarDef *vd) {
    return vnames[vd] = Unique2(Sanitize(vd->name));
}

// A name unique within the function (and clear of package-level names).
inline string CodeGenGo::Unique2(const string &base) {
    auto name = base;
    if (fnused.count(name) || used.count(name)) {
        auto &next = fnnextsuffix.try_emplace(base, 2).first->second;
        do name = cat(base, "_", next++);
        while (fnused.count(name) || used.count(name));
    }
    fnused.insert(name);
    return name;
}

inline void CodeGenGo::BindLocal(VarDef *d, Node *init) {
    auto t = d->type;
    auto gt = GoT(t, d->line);
    if (!init) {
        Decl(LocalName(d), gt);
        return;
    }
    // A construct or a call delivers into the variable itself.
    if (IsCtl(init) || Is<Call>(init)) {
        auto name = LocalName(d);
        Decl(name, gt);
        GenAny(init, GoDst { GD_LVALUE, name, t });
        return;
    }
    auto x = Coerce(GenX(init), init->exprtype, t, init->line);
    Decl(LocalName(d), gt, x);
}

// An assignable place: a variable, as Go names it. t gets its type.
inline string CodeGenGo::GenLvalue(Node *n, TypeExpr *&t) {
    if (auto id = Is<Ident>(n); id && id->vdef) {
        t = id->vdef->type;
        GoT(t, n->line);
        return VarRef(id->vdef, n->line);
    }
    if (Is<Dot>(n)) Unsupported(n->line, "assignment to a field");
    if (Is<Index>(n)) Unsupported(n->line, "assignment to an element");
    Unsupported(n->line, "assignment to this place");
}

inline void CodeGenGo::GenAssign(Assign *a) {
    if (a->pointee) Unsupported(a->line, "assignment through a reference");
    if (a->op == T_DOTASSIGN) Unsupported(a->line, "rebinding a reference (.=)");
    TypeExpr *t = nullptr;
    auto lv = GenLvalue(a->lval, t);
    if (a->op == T_ASSIGN) {
        GenAny(a->rhs, GoDst { GD_LVALUE, lv, t });
        return;
    }
    // The old value is read before the right side runs where that runs
    // statements, as for any operator (§2).
    auto old = HasStmts(a->rhs) ? Snapshot(t, lv) : lv;
    auto r = GenX(a->rhs);
    if (a->op != T_SHLEQ && a->op != T_SHREQ) r = Coerce(r, a->rhs->exprtype, t, a->line);
    L(lv, " = ", ArithX(a->op, t, old, r, a->line));
}

inline void CodeGenGo::GenIncDec(IncDec *a) {
    TypeExpr *t = nullptr;
    auto lv = GenLvalue(a->lval, t);
    L(lv, " = ", ArithX(a->op == T_INC ? T_PLUS : T_MINUS, t, lv, "1", a->line));
}

}  // namespace goose
