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
    struct Mark { bool usedlabel, usedbrk; size_t defers, saves; string top0; };
    vector<Mark> marks;
    for (auto &s : scopes)
        marks.push_back({ s.usedlabel, s.usedbrk, s.defers.size(), s.saves.size(), s.top0 });
    auto savedsttop0 = dsttop0;
    auto nscopes = scopes.size();
    auto text = std::move(body);
    body.clear();
    int saveind = ind, savenext = stknext;
    dry++;
    dead = false;
    gen();
    dry--;
    if (scopes.size() != nscopes) Fail(Line {}, "internal: unbalanced scopes in unreached code");
    for (size_t i = 0; i < nscopes; i++) {
        scopes[i].usedlabel = marks[i].usedlabel;
        scopes[i].usedbrk = marks[i].usedbrk;
        scopes[i].defers.resize(marks[i].defers);
        scopes[i].saves.resize(marks[i].saves);
        scopes[i].top0 = marks[i].top0;
    }
    dsttop0 = savedsttop0;
    body = std::move(text);
    ind = saveind;
    stknext = savenext;
    dead = true;
}

// A data stack index for the innermost scope, free again once it ends;
// `forlocal` skips the statement scopes inside the surrounding block, so
// that a local's value survives to that block's end, and no further
// (CodeGen::AllocStk).
inline string CodeGenGo::AllocStk(bool forlocal) {
    auto k = stknext++;
    stkmax = std::max(stkmax, stknext);
    if (forlocal)
        for (auto i = (int)scopes.size() - 1; i >= 0 && scopes[i].kind == SC_STMT; i--)
            scopes[i].stkbase = std::max(scopes[i].stkbase, stknext);
    // Spaced as gofmt spaces an index.
    auto idx = SpIdx(k);
    idx.erase(std::remove(idx.begin(), idx.end(), ' '), idx.end());
    return idx;
}

// The top of stk to restore, to basevar, at every exit of the innermost
// scope (of the block, past the statement scopes, for a local).
inline void CodeGenGo::SaveBase(bool forlocal, const string &stk, const string &basevar) {
    for (auto i = (int)scopes.size() - 1; i >= 0; i--) {
        if (forlocal && scopes[i].kind == SC_STMT) continue;
        scopes[i].saves.push_back({ stk, basevar });
        return;
    }
    Fail(Line {}, "internal: a data stack outside every scope");
}

// What an exit through scope s runs: the restores of its stacks' tops, in
// reverse declaration order, and its `defer` bodies, each before the
// release of the storage declared before it (CodeGen::EmitRestores). A body
// opens scopes of its own, which may move the scope vector, so the lists
// are copied first.
inline void CodeGenGo::EmitRestores(const GoScope &s) {
    auto saves = s.saves;
    auto defers = s.defers;
    auto di = (int)defers.size() - 1;
    for (auto k = (int)saves.size(); k >= 0; k--) {
        for (; di >= 0 && (int)defers[di].first == k; di--) EmitDeferBody(defers[di].second);
        if (k > 0) L(Top(saves[k - 1].first), " = ", saves[k - 1].second);
    }
}

// What an exit runs on its way out down to (and including) scope `to`.
inline void CodeGenGo::EmitExitRestores(int to) {
    for (auto i = (int)scopes.size() - 1; i >= to; i--) EmitRestores(scopes[i]);
    // Leaving the function: a long-distance return's target gives its
    // channels back the destinations they had.
    if (to == 0)
        for (auto &s : fdstsaves) L(s);
}

// A `defer` statement (§6.6): its body joins the exits of the block it is
// in, the innermost scope that is not a statement's.
inline void CodeGenGo::RegisterDefer(Defer *d) {
    for (auto i = (int)scopes.size() - 1; i >= 0; i--) {
        if (scopes[i].kind == SC_STMT) continue;
        scopes[i].defers.push_back({ scopes[i].saves.size(), d });
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
    GenStmts(d->body->stmts);
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
    GenStmts(b->stmts, first);
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
    if (n->line.line > 0) curline = n->line;
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
    // A choice between values it computes nothing for: a select
    // (codegen_go_select.h).
    if (GenSelectIf(fi, d)) return;
    // A && or || condition, or an inlined bool call, as C branches on it
    // (IfExpr::CgAny), but where C hoists the arms' common check.
    auto c = BranchChain(fi->cond) && !HoistsCheck(fi) ? GenBranchTruth(fi->cond) : GenTruth(fi->cond);
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
        scopes.back().saves.clear();
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
    if (st && st->kind == TY_REF && st->ref->sub->kind == TY_ENUM) {
        GoT(st, m->line);
        return GenEnumMatch(m, st->ref->sub, true, d);
    }
    if (st && st->kind == TY_ENUM) {
        GoT(st, m->line);
        return GenEnumMatch(m, st, false, d);
    }
    if (!st || st->kind != TY_INT) Unsupported(m->line, cat("a match over ", TypeText(st)));
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
            // The arm's values as disjoint ranges in order, those that overlap
            // or touch made one: the checker lets an arm name one value twice
            // (`A, B` where both are 1), and the same comparison twice in one
            // `||` is what vet reports as redundant. Values order as the
            // checker orders them, by unsigned keys (typecheck_flow.h).
            auto key = [&](int64_t v) {
                return st->intstorage == IS_U64 ? (uint64_t)v : (uint64_t)v ^ (1ull << 63);
            };
            auto sorted = arm.ranges;
            std::sort(sorted.begin(), sorted.end(), [&](const ArmRange &a, const ArmRange &b) {
                return key(a.lo) < key(b.lo);
            });
            vector<ArmRange> ranges;
            for (auto &r : sorted) {
                if (ranges.empty() || (key(ranges.back().hi) != UINT64_MAX &&
                                       key(r.lo) > key(ranges.back().hi) + 1))
                    ranges.push_back(r);
                else if (key(r.hi) > key(ranges.back().hi))
                    ranges.back().hi = r.hi;
            }
            string c;
            auto several = ranges.size() > 1;
            for (auto &r : ranges) {
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
    EnterDst((int)scopes.size() - 1, d);
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
    if (Unreached()) return GenUnreached([&] { GenInlineBlock(ib, d); });
    // The body delivers the callee's own result type; one the call site
    // adapted to an ADT converts on the way out (InlineBlock::CgAny).
    if (auto from = AdtFrom(ib)) {
        GenAdtAdapted(from, ib->exprtype, d, ib->line, [&](const GoDst &nd) { EmitIbBody(ib, nd); });
        return;
    }
    // A call passed where a slice is expected was checked as that slice
    // (§3.10), while the body returns a resizable array: built where the
    // call would have put its result, a temporary of the caller's scope, and
    // sliced whole (InlineBlock::CgAny).
    auto rt = ib->spec && ib->spec->rets.size() == 1 ? ib->spec->rets[0] : nullptr;
    auto want = d.t ? d.t : ib->exprtype;
    if (d.k != GD_DISCARD && rt && rt->kind == TY_ARRAY && IsBytesT(rt) && want &&
        want->kind == TY_SLICE) {
        string stk;
        GoLoc l;
        if (IsResz(rt)) {
            auto h = RzTemp(rt, stk);
            EmitIbBody(ib, GoDst { GD_STACK, stk, rt, RzLenLv(rt, h) });
            l = RzTempLoc(rt, h, stk);
        } else {
            auto base = BytesTemp(stk);
            EmitIbBody(ib, GoDst { GD_STACK, stk, rt });
            l = BytesLoc(base, rt, GoLoc {});
        }
        if (Unreached()) return;
        auto x = LoadLoc(l, want, ib->line);
        if (d.k == GD_LVALUE) L(d.s, " = ", x);
        else EmitValStore(d.s, want, x);
        return;
    }
    EmitIbBody(ib, d);
}

// The body itself (InlineBlock::EmitBody): its named result bound to a stack
// destination (OpenIbNrvo), whose elements sit there from its declaration
// on, and its arguments bound in the caller's scope.
inline void CodeGenGo::EmitIbBody(InlineBlock *ib, const GoDst &d) {
    auto named = OpenIbNrvo(ib, d);
    if (named) openat[d.s]++;
    auto first = GenInlineArgs(ib->body);
    if (Unreached()) {
        if (named) {
            nrvo.erase(named);
            openat[d.s]--;
        }
        return GenUnreached([&] { GenInlineBlock(ib, d); });
    }
    // A body that only chooses among values -- std's min, max, clamp --
    // is Go's min or max, or assignments Go makes selects of, where the
    // switch's exits would be branches (codegen_go_select.h).
    if (!named && GenSelectIb(ib, d, first)) return;
    OpenLabeled(SC_IB);
    auto si = (int)scopes.size() - 1;
    scopes[si].ibsf = ib->sf;
    EnterDst(si, d);
    // The named result is in front of the value any other return builds.
    if (named) scopes[si].open0--;
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
    if (named) {
        nrvo.erase(named);
        openat[d.s]--;
    }
}

// ------------------------------------------------------------------
// Loops: a labeled `for`, whose scope is the Goose loop's. A break leaves
// it and a continue starts its next iteration, each running the restores of
// the scopes it leaves; the end of an iteration runs the loop scope's own.

inline void CodeGenGo::GenLoopBody(Block *bodyb, size_t first) {
    GenStmts(bodyb->stmts, first);
    if (bodyb->tail) GenStmt(bodyb->tail);
    if (Unreached()) GenUnreached([&] { EmitRestores(scopes.back()); });
    else EmitRestores(scopes.back());
    // Restored by every path out of an iteration already.
    scopes.back().defers.clear();
    scopes.back().saves.clear();
}

inline void CodeGenGo::GenWhile(While *w) {
    if (Unreached()) return GenUnreached([&] { GenWhile(w); });
    // The `while v > 0 { v--; ... }` of a countdown loop (While::countdown)
    // is the counted loop it is as written.
    OpenLabeled(SC_LOOP);
    auto si = (int)scopes.size() - 1;
    // A condition that runs statements is tested at the top of the body.
    string c;
    auto incond = !HasStmts(w->cond) && TryPure([&] { return GenTruth(w->cond); }, c);
    if (Unreached()) {   // Not without statements; but nothing opens unreached.
        PopLabeled();
        return GenUnreached([&] { GenWhile(w); });
    }
    LabelHere();
    L(incond ? cat("for ", CondText(c), " {") : string("for {"));
    ind++;
    if (!incond) {
        // In a scope of its own, or its operands' each (GenBranchTruth), as
        // C's branches on it are.
        c = GenBranchTruth(w->cond);
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
    EnterDst((int)scopes.size() - 1, d);
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
    if (f->iterkind != IK_RANGE && f->iterkind != IK_COUNT) return GenForArray(f);
    // A value's bytes stored one by one: one store (codegen_go_wide.h).
    if (GenWideStoreLoop(f)) return;
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
    // The counters are the loop's own, and the binders copies of them
    // declared in its body, as every local is (Decl): one whose address the
    // body takes then lives in the function's frame (FinishBody), one slot
    // for every iteration, as C's does, where a header's would be on Go's
    // moving stack.
    auto ctr = T();
    string head, ix;
    if (f->idxdef) {
        auto it = f->idxdef->type;
        ix = T();
        head = cat("for ", ctr, ", ", ix, " := ", lov, ", ", GoIntLit(0, false, it, f->line), "; ",
                   ctr, " < ", hiv, "; ", ctr, ", ", ix, " = ", ctr, "+1, ", ix, "+1 {");
    } else {
        head = cat("for ", ctr, " := ", lov, "; ", ctr, " < ", hiv, "; ", ctr, "++ {");
    }
    LabelHere();
    L(head);
    ind++;
    Decl(LocalName(f->vdef), LocalT(vt, f->line), wide ? cat(GoT(vt, f->line), "(", ctr, ")") : ctr);
    if (f->idxdef) Decl(LocalName(f->idxdef), LocalT(f->idxdef->type, f->line), ix);
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
    GenFromReturn(r);
}

// A target's channels (CodeGen::EnsureFromChannels): a fixed result's
// value; a non-fixed one's destination stack, where the value starts there,
// and a resizable one's count (or frame object).
inline void CodeGenGo::EnsureFromChannels(FnSpec *t) {
    if (!fromemitted.insert(t).second) return;
    auto tid = fromids.at(t);
    auto &rets = t->rets;
    // Package variables, or in a program with instances, whose thread
    // programs and export fns' calls each return on their own, members of
    // the instance.
    string decls;
    auto var = [&](const string &name, const string &gt) {
        if (instances) Append(decls, "\t", name, " ", gt, "\n");
        else Append(decls, "var ", name, " ", gt, "\n");
    };
    Append(decls, instances ? "\t" : "", "// The result channels of a long-distance return to ", t->sf->qname, " (§7.9).\n");
    for (size_t i = 0; i < rets.size(); i++) {
        if (IsFix(rets[i])) {
            var(cat("gsLret_", tid, "_", i), GoT(rets[i], t->sf->line));
            continue;
        }
        var(cat("gsFdst_", tid, "_", i), "int64");
        var(cat("gsFval_", tid, "_", i), "unsafe.Pointer");
        if (IsResz(rets[i]))
            var(cat("gsLret_", tid, "_", i), IsFrameObj(rets[i]) ? RzGoT(rets[i], t->sf->line) : string("int64"));
    }
    if (instances) {
        if (fromemitted.size() == 1)
            decls = cat("\t// The long-distance return in flight: its target's number, 0 for none.\n\tgsRf int32\n", decls);
        glfields += decls;
        return;
    }
    rfdecls += decls;
    if (fromemitted.size() == 1)
        rfdecls = cat("// The long-distance return in flight: its target's number, 0 for none.\nvar gsRf int32\n\n",
                      rfdecls);
    rfdecls += "\n";
}

// A long-distance return site (CodeGen::GenFromReturn): the values into
// the target's channels, then the return in flight.
inline void CodeGenGo::GenFromReturn(Return *r) {
    auto t = r->targetspec;
    if (!t || !fromids.count(t)) Fail(r->line, "internal: a long-distance return without its target");
    if (!curinfo || !curinfo->hasrf) Fail(r->line, "internal: a long-distance return no caller checks for");
    if (Unreached()) return GenUnreached([&] { GenFromReturn(r); });
    auto tid = fromids[t];
    EnsureFromChannels(t);
    auto &rets = t->rets;
    auto chan = [&](const char *k, size_t i) { return GlName(cat("gs", k, "_", tid, "_", i)); };
    // The values land at the channels' tops, which is not where the
    // target's caller expects them when anything was built in its
    // destination since: the catch moves them down (EmitRfCheck).
    for (size_t i = 0; i < rets.size(); i++)
        if (IsBytesT(rets[i])) L(chan("Fval", i), " = ", Top(chan("Fdst", i)));
    if (r->vals.size() == rets.size()) {
        for (size_t i = 0; i < rets.size(); i++) {
            if (IsResz(rets[i])) GenConstruct(r->vals[i], chan("Fdst", i), rets[i], chan("Lret", i));
            else if (IsBytesT(rets[i])) GenConstruct(r->vals[i], chan("Fdst", i), rets[i]);
            else GenAny(r->vals[i], GoDst { GD_LVALUE, chan("Lret", i), rets[i] });
        }
    } else {
        // One call's results forwarded into the channels.
        auto c = r->vals.size() == 1 ? Is<Call>(r->vals[0]) : nullptr;
        if (!c) Fail(r->line, "internal: a long-distance return forwarding no call's results");
        vector<GoDst> chans(rets.size());
        for (size_t i = 0; i < rets.size(); i++) {
            if (IsResz(rets[i])) chans[i] = GoDst { GD_STACK, chan("Fdst", i), rets[i], chan("Lret", i) };
            else if (IsBytesT(rets[i])) chans[i] = GoDst { GD_STACK, chan("Fdst", i), rets[i] };
            else chans[i] = GoDst { GD_LVALUE, chan("Lret", i), rets[i] };
        }
        auto res = GenCallVals(c, chans[0], &chans);
        if (Unreached()) return;
        auto rtypes = c->builtin >= 0 || c->fvbody || !c->dispatch.empty() ? c->rettypes
                                                                             : c->spec->rets;
        for (size_t i = 0; i < rets.size() && i < res.size(); i++) {
            if (IsBytesT(rets[i]) || res[i].empty()) continue;
            auto from = i < rtypes.size() ? rtypes[i] : rets[i];
            // A non-fixed result (a variable-mode ADT for a fixed one, say)
            // is read as the channel's type from where the call built it.
            auto x = IsBytesT(from) ? LoadLoc(IsResz(from) ? RzTempLoc(from, res[i], "") : BytesLoc(res[i], from, GoLoc {}),
                                              rets[i], r->line)
                                    : CoerceVal(res[i], from, rets[i], r->line);
            if (x != chans[i].s) L(chans[i].s, " = ", x);
        }
    }
    if (Unreached()) return;
    PropagateReturn(cat(tid));
}

// A return in flight leaving this function (CodeGen::PropagateReturn): its
// exits run, then it returns dummy values. `rfval` names the target where
// this starts the return, "" where gsRf holds it already.
inline void CodeGenGo::PropagateReturn(const string &rfval) {
    EmitExitRestores(0);
    if (!rfval.empty()) L(GlName("gsRf"), " = ", rfval);
    string list;
    for (auto &z : curzeros) Append(list, list.empty() ? "" : ", ", z);
    L(list.empty() ? string("return") : cat("return ", list));
    dead = true;
}

// After a call a long-distance return can come back through
// (CodeGen::EmitRfCheck): this function's own is caught here, its values
// taken from the channels and returned; any other goes on out.
inline void CodeGenGo::EmitRfCheck(FnSpec *callee) {
    if (Unreached()) return;
    auto rf = GlName("gsRf");
    L("if ", rf, " != 0 {");
    ind++;
    auto caught = curspec && callee->needs.count(curspec);
    if (caught) {
        auto tid = fromids.at(curspec);
        EnsureFromChannels(curspec);
        L("if ", rf, " == ", tid, " {");
        ind++;
        L(rf, " = 0");
        vector<string> rv;
        for (size_t i = 0; i < curspec->rets.size(); i++) {
            auto rt = curspec->rets[i];
            auto lret = GlName(cat("gsLret_", tid, "_", i));
            // The value is on this function's destination, behind anything
            // the calls it unwound built there: down to where the caller
            // expects it.
            if (IsBytesT(rt)) LandValue(cat("gs_dst", i), DstTop0((int)i), GlName(cat("gsFval_", tid, "_", i)), rt, lret);
            if (IsResz(rt)) {
                L(dstcounts[i], " = ", lret);
                rv.push_back(dstcounts[i]);
                continue;
            }
            if (IsBytesT(rt)) continue;
            rv.push_back(lret);
        }
        EmitExitRestores(0);
        string list;
        for (auto &v : rv) Append(list, list.empty() ? "" : ", ", v);
        L(list.empty() ? string("return") : cat("return ", list));
        ind--;
        LS("} else {");
        ind++;
        if (curinfo && curinfo->hasrf) PropagateReturn("");
        else L("panic(\"unexpected long-distance return\")");
        ind--;
        LS("}");
    } else if (curinfo && curinfo->hasrf) {
        PropagateReturn("");
    } else {
        L("panic(\"unexpected long-distance return\")");
    }
    ind--;
    LS("}");
    dead = false;
}

// A break's value, or a return's leaving an inlined body, for scope si.
inline void CodeGenGo::GenExitValue(Node *val, int si) {
    auto d = scopes[si].dst;
    // A value built at a stack's top, where a construction opened since the
    // scope was entered may have placed something in front of it, is moved
    // down to where the receiver expects it (CodeGen::GenExitValue).
    auto start = d.k == GD_STACK ? ExitStart(val, d.s, d.lenlv, scopes[si].open0) : "";
    GenAny(val, d);
    if (!start.empty())
        LandValue(d.s, ScopeTop0(si), start, d.t ? d.t : val->exprtype, d.lenlv);
}

// A call's results forwarded as the function's own, by the types rets they
// go out as, into the channels chans (CodeGen::GenForward), whatever is
// called: a result the call gives as its own type is built straight into
// its channel; one of another type (a variable array for a resizable, say)
// lands in a temporary of its own type first, and is built from there
// into its channel or read as its type. The fixed results' values, by
// result ("" for the others).
inline vector<string> CodeGenGo::GenForward(Call *c, const vector<TypeExpr *> &rets, const vector<GoDst> &chans) {
    auto ln = c->line;
    auto &ctypes = c->rettypes;
    if (ctypes.size() != rets.size()) Fail(ln, "internal: a return forwarding another count of results");
    vector<GoDst> dsts;
    vector<GoLoc> arrived(rets.size());
    for (size_t i = 0; i < rets.size(); i++) {
        auto ct = ctypes[i];
        if (TEq(ct, rets[i])) {
            auto d = chans[i];
            if (d.k == GD_DISCARD) {
                d = GoDst { GD_LVALUE, T(), ct };
                Decl(d.s, LocalT(ct, ln));
            }
            dsts.push_back(d);
            continue;
        }
        if (ct->kind == TY_VARIANT && IsBytesT(ct))
            Unsupported(ln, cat("forwarding a ", TypeText(ct), " result as a ", TypeText(rets[i])));
        auto &lv = arrived[i];
        string stk;
        if (IsResz(ct)) {
            auto h = RzTemp(ct, stk);
            lv = RzTempLoc(ct, h, stk);
            dsts.push_back(GoDst { GD_STACK, stk, ct, RzLenLv(ct, h) });
        } else if (IsBytesT(ct)) {
            lv = BytesLoc(BytesTemp(stk), ct, GoLoc {});
            lv.stk = stk;
            dsts.push_back(GoDst { GD_STACK, stk, ct });
        } else {
            auto v = T();
            Decl(v, LocalT(ct, ln));
            lv = AtVar(v, ct);
            dsts.push_back(GoDst { GD_LVALUE, v, ct });
        }
    }
    auto res = GenCallVals(c, dsts[0], &dsts);
    vector<string> vals(rets.size());
    if (Unreached()) return vals;
    for (size_t i = 0; i < rets.size(); i++) {
        auto rt = rets[i];
        auto r = i < res.size() ? res[i] : string();
        if (auto lv = arrived[i]; lv.t) {
            // Where the call left the value: its Go result, say.
            auto ct = ctypes[i];
            if (!IsResz(ct) && !r.empty()) {
                if (IsBytesT(ct)) lv.addr = r;
                else lv = AtVar(r, ct);
            }
            if (IsBytesT(rt)) ConstructFromLoc(lv, rt, chans[i].s, chans[i].lenlv, ln);
            else vals[i] = LoadLoc(lv, rt, ln);
        } else if (!IsBytesT(rt)) {
            vals[i] = CoerceVal(!r.empty() ? r : dsts[i].s, ctypes[i], rt, ln);
        }
    }
    return vals;
}

// Whether a call's results, forwarded by a return, include one its function
// gives out as a value of another type that is not fixed-size (a variant's
// ADT in variable mode, say), which GenForward builds at its destination.
inline bool CodeGenGo::ForwardsAdapted(Call *c) {
    auto &own = curspec->rets;
    if (c->rettypes.size() != own.size()) return false;
    for (size_t i = 0; i < own.size(); i++)
        if (IsBytesT(own[i]) && !TEq(c->rettypes[i], own[i])) return true;
    return false;
}

inline void CodeGenGo::GenNormalReturn(const vector<Node *> &vals) {
    if (Unreached()) return GenUnreached([&] { GenNormalReturn(vals); });
    auto sp = curspec;
    // A return passes through the exits of every scope, whose `defer`
    // bodies run after its values are taken.
    auto restores = !fdstsaves.empty();
    for (auto &s : scopes) restores = restores || !s.defers.empty() || !s.saves.empty();
    vector<string> rv;
    if (vals.size() == 1 && sp->rets.size() > 1) {
        // A single call forwards all its results (§7.1).
        auto c = Is<Call>(vals[0]);
        // A builtin's (from_bytes's value and its bool), a dispatch's that
        // are not all fixed-size, a function value's, and any call's one of
        // whose results goes out as a value of another type that is not
        // fixed-size (ForwardsAdapted): each result into its channel, a
        // non-fixed one built at this function's destination for it, its
        // count into gs_rl<i>, by the call's own convention (GenCallVals),
        // as the C backend forwards them (GenForward).
        if (c && c->rettypes.size() == sp->rets.size() &&
            (c->builtin >= 0 || c->fvbody || NonFixedDispatch(c) || ForwardsAdapted(c))) {
            vector<GoDst> chans(sp->rets.size());
            vector<string> starts(sp->rets.size());
            for (size_t i = 0; i < sp->rets.size(); i++) {
                auto rt = sp->rets[i];
                auto dst = cat("gs_dst", i);
                if (IsResz(rt)) chans[i] = GoDst { GD_STACK, dst, rt, dstcounts[i] };
                else if (IsBytesT(rt)) chans[i] = GoDst { GD_STACK, dst, rt };
                else continue;
                starts[i] = ExitStart(c, dst, chans[i].lenlv, 0);
            }
            auto fixed = GenForward(c, sp->rets, chans);
            if (Unreached()) return GenUnreached([&] { GenNormalReturn(vals); });
            for (size_t i = 0; i < sp->rets.size(); i++)
                if (!starts[i].empty())
                    LandValue(cat("gs_dst", i), DstTop0((int)i), starts[i], sp->rets[i], chans[i].lenlv);
            for (size_t i = 0; i < sp->rets.size(); i++) {
                auto rt = sp->rets[i];
                if (IsResz(rt)) rv.push_back(dstcounts[i]);
                else if (!IsBytesT(rt)) rv.push_back(Snapshot(rt, fixed[i]));
            }
            EmitExitRestores(0);
            string list;
            for (size_t i = 0; i < rv.size(); i++) Append(list, i ? ", " : "", rv[i]);
            L(list.empty() ? string("return") : cat("return ", list));
            dead = true;
            return;
        }
        if (c && !c->dispatch.empty() && c->dispatch[0]->rets.size() == sp->rets.size()) {
            // A case-function call's results, which its cases deliver into
            // temporaries.
            auto res = GenDispatch(c, GoDst {}, nullptr);
            for (size_t i = 0; i < sp->rets.size(); i++)
                rv.push_back(Coerce(res[i], c->dispatch[0]->rets[i], sp->rets[i], c->line));
            if (Unreached()) return GenUnreached([&] { GenNormalReturn(vals); });
            EmitExitRestores(0);
            string list;
            for (size_t i = 0; i < rv.size(); i++) Append(list, i ? ", " : "", rv[i]);
            L("return ", list);
            dead = true;
            return;
        }
        if (!c || !c->spec || c->spec->rets.size() != sp->rets.size())
            Fail(vals[0]->line, "internal: a return forwarding no call's results");
        if (!PlainSig(c->spec)) {
            // Each result into its channel: a non-fixed one built at this
            // function's destination for it, its count into gs_rl<i>
            // (CodeGen::GenForward).
            vector<GoDst> chans(sp->rets.size());
            for (size_t i = 0; i < sp->rets.size(); i++) {
                auto rt = sp->rets[i];
                if (!IsBytesT(rt)) continue;
                if (!TEq(c->spec->rets[i], rt))
                    Unsupported(c->line, cat("forwarding a ", TypeText(c->spec->rets[i]),
                                             " result as a ", TypeText(rt)));
                chans[i] = GoDst { GD_STACK, cat("gs_dst", i), rt,
                                   IsResz(rt) ? dstcounts[i] : string() };
            }
            auto res = EmitSpecCall(c, c->spec, chans[0], &chans);
            if (Unreached()) return GenUnreached([&] { GenNormalReturn(vals); });
            for (size_t i = 0; i < sp->rets.size(); i++) {
                auto rt = sp->rets[i];
                if (IsResz(rt)) rv.push_back(dstcounts[i]);
                else if (!IsBytesT(rt)) rv.push_back(Snapshot(rt, CoerceVal(res[i], c->spec->rets[i], rt, c->line)));
            }
            EmitExitRestores(0);
            string list;
            for (size_t i = 0; i < rv.size(); i++) Append(list, i ? ", " : "", rv[i]);
            L(list.empty() ? string("return") : cat("return ", list));
            dead = true;
            return;
        }
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
        vector<string> got;
        for (size_t i = 0; i < sp->rets.size(); i++) {
            auto t = T();
            NoteType(t, GoT(c->spec->rets[i], c->line));
            got.push_back(t);
            Append(names, i ? ", " : "", t);
            Append(marks, i ? "," : "", t);
        }
        L(names, " := ", call, DECLMARK, marks);
        // Each converted once the call has made it: a variant's ADT, say,
        // takes statements of its own (AdaptToEnum).
        for (size_t i = 0; i < sp->rets.size(); i++)
            rv.push_back(Coerce(got[i], c->spec->rets[i], sp->rets[i], c->line));
    } else {
        if (vals.size() != sp->rets.size()) Fail(sp->sf->line, "internal: return value count");
        for (size_t i = 0; i < vals.size(); i++) {
            auto rt = sp->rets[i];
            if (IsBytesT(rt)) {
                // Built at this function's destination for it (§7.3): a
                // named result in place already, any other value at the top
                // and moved down over whatever is in front of it
                // (CodeGen::GenNormalReturn).
                auto id = Is<Ident>(vals[i]);
                auto named = id && id->vdef ? nrvo.find(id->vdef) : nrvo.end();
                auto dst = cat("gs_dst", i);
                auto direct = named != nrvo.end() && !named->second.inlined && named->second.stk == dst;
                // A resizable's count, or the element-run twin's run's.
                auto run = IsResz(rt) || (emiter && i == 0);
                auto lenlv = run ? dstcounts[i] : string();
                if (direct) {
                    if (IsResz(rt))
                        L(lenlv, " = ", IsFrameObj(rt) ? VarRef(id->vdef, vals[i]->line)
                                                       : cat(VarRef(id->vdef, vals[i]->line), ".Len"));
                    // A resizable local's elements sit at the destination
                    // behind the length prefix reserved for them; the count
                    // goes in there now.
                    else if (IsResz(id->vdef->type)) EmitNrvoFinish(named->second);
                } else {
                    auto start = ExitStart(vals[i], dst, lenlv, 0);
                    GenConstruct(vals[i], dst, rt, lenlv);
                    if (!start.empty()) LandValue(dst, DstTop0(i), start, rt, lenlv);
                }
                if (run) rv.push_back(lenlv);
                continue;
            }
            auto later = false;
            for (auto j = i + 1; j < vals.size(); j++) later = later || HasStmts(vals[j]);
            auto x = Coerce(GenX(vals[i]), vals[i]->exprtype, rt, vals[i]->line);
            if ((later || restores) && !IsGoConst(x) && x != "nil") x = Snapshot(rt, x);
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
        BindLocal(vd->defs[i], i < vd->inits.size() ? vd->inits[i] : nullptr, !vd->inline_arg);
}

// A local's Go name, given afresh at each declaration: a `defer` body emitted
// at several exits declares its locals again each time.
inline string CodeGenGo::LocalName(VarDef *vd) {
    auto name = Unique2(Sanitize(vd->name));
    return vnames[vd] = name;
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

inline void CodeGenGo::BindLocal(VarDef *d, Node *init, bool forlocal) {
    auto t = d->type;
    // A resizable or bytes value, on a data stack of its own (or its
    // destination's, a named result), where it is built.
    if (IsBytesT(t)) {
        GoT(t, d->line);
        if (!init) Fail(d->line, cat("internal: ", d->name, " has no initializer"));
        auto dst = DeclStackLocal(d, forlocal);
        GenConstruct(init, dst.s, dst.t, dst.lenlv);
        return;
    }
    if (PrefVar(d)) {
        GoT(t, d->line);
        auto name = LocalName(d);
        if (!init) {
            Decl(name, "GsPref");
            return;
        }
        auto x = GenPrefVal(init);
        Decl(name, "GsPref", x);
        return;
    }
    // A large fixed-size value: on a data stack of its own, as C holds it
    // (FixedLocal), taken before its initializer runs, which builds the
    // value there; the name points at it.
    if (IsLargeFixed(t) && !(curspec && curspec->incycle)) {
        auto name = LocalName(d);
        auto lv = FixedLocal(t, name, "", forlocal, d->line, !init);
        fvptr.insert(d);
        if (init) GenAny(init, GoDst { GD_LVALUE, lv, t });
        return;
    }
    auto gt = LocalT(t, d->line);
    if (!init) {
        Decl(LocalName(d), gt);
        return;
    }
    // A relative reference variable holds the offset from itself (§3.9):
    // whatever the initializer, its value is the plain reference to encode.
    if (IsRelRef(t)) {
        auto name = LocalName(d);
        Decl(name, gt);
        GenRelStore(AtVar(name, t), init, false);
        return;
    }
    // An inlined call's reference parameter (forlocal false) handed a Go
    // local by reference names the local's place (issue #24).
    if (!forlocal && BindRefPlace(d, init)) return;
    // A construct or a call delivers into the variable itself, and so does
    // a literal holding relative references, whose offsets are measured from
    // where it stays.
    if (IsCtl(init) || Is<Call>(init) ||
        ((Is<StructLit>(init) || Is<ArrayLit>(init)) && HasRelRef(t))) {
        auto name = LocalName(d);
        Decl(name, gt);
        GenAny(init, GoDst { GD_LVALUE, name, t });
        return;
    }
    if (IsStaticLimited(t) && init->exprtype && !TEq(init->exprtype, t)) {
        auto name = LocalName(d);
        Decl(name, gt);
        GenAny(init, GoDst { GD_LVALUE, name, t });
        return;
    }
    auto x = Coerce(GenX(init), init->exprtype, t, init->line);
    Decl(LocalName(d), gt, x);
}

// Whether a destination names the same storage wherever its statement
// evaluates it (CodeGen::StableDest): a variable, or a field or a constant
// in-range element of a fixed-size value down from one, through no
// relative reference. An assignment to anything else takes its address
// first, before the right side runs (§2).
inline bool CodeGenGo::StableDest(Node *n) {
    auto fixed = [&](TypeExpr *t) -> TypeExpr * {
        if (t && t->kind == TY_REF) {
            if (t->ref->lenstorage >= 0) return nullptr;
            t = t->ref->sub;
        }
        return t && IsFix(t) ? t : nullptr;
    };
    for (;;) {
        if (auto d = Is<Dot>(n)) {
            if (!d->IsField() || !fixed(d->obj->exprtype)) return false;
            n = d->obj;
        } else if (auto ix = Is<Index>(n)) {
            auto at = fixed(ix->obj->exprtype);
            if (!at || at->kind != TY_ARRAY || at->arr->akind != A_FIXED) return false;
            auto il = Is<IntLit>(ix->idx);
            if (!(il && il->val >= 0 && il->val < ArrSize(at->arr))) return false;
            n = ix->obj;
        } else {
            return Is<Ident>(n) != nullptr;
        }
    }
}

// An assignment's place, resolved before its right side runs (§2): a
// reference written through, an element (whose bounds check runs first),
// a place reached through a reference that can be rebound, are pinned to
// the storage they name now.
inline CodeGenGo::GoLoc CodeGenGo::AssignLoc(Node *lval, bool pointee) {
    auto l = GenLoc(lval);
    if (pointee) DerefLoc(l);
    if (l.viaref || !StableDest(lval)) {
        if (!IsGoName(l.lv)) l = PinLoc(l);
    }
    return l;
}

inline void CodeGenGo::GenAssign(Assign *a) {
    if (a->op == T_ASSIGN) {
        // A whole resizable: emptied (its stack's top back to its base), then
        // its new contents constructed in place (§4.4).
        auto lt = a->lval->exprtype;
        if (lt && lt->kind == TY_REF && a->pointee) lt = lt->ref->sub;
        if (lt && IsResz(lt)) {
            auto l = GenLoc(a->lval);
            if (a->pointee || l.t->kind == TY_REF) DerefLoc(l);
            if (l.viaref) l = PinRz(l);
            if (l.stk.empty()) Fail(a->line, "internal: a resizable assigned without its stack");
            if (IsFrameObj(l.t) && !l.lv.empty()) {
                L(Top(l.stk), " = ", FoTailHdr(l.t, l.lv), ".Base");
                GenConstruct(a->rhs, l.stk, l.t, l.lv);
                return;
            }
            L(Top(l.stk), " = ", l.addr);
            // A frame object's type here is the tail of a value that is
            // no frame object, laid out as bytes.
            if (IsFrameObj(l.t)) GenFoAsBytes(a->rhs, l.stk, l.t, l.lenlv);
            else GenConstruct(a->rhs, l.stk, l.t, l.lenlv);
            return;
        }
        if (lt && IsBytesT(lt)) Unsupported(a->line, cat("assigning a value of type ", TypeText(lt)));
    }
    if (a->op == T_DOTASSIGN) {
        auto l = GenLoc(a->lval);
        if (l.ispref) {
            L(l.lv, " = ", GenPrefVal(a->rhs));
            return;
        }
    }
    auto l = AssignLoc(a->lval, a->pointee);
    auto t = l.t;
    if (a->op == T_DOTASSIGN) {
        // Rebinding a reference (§3.8): the reference itself, encoded where
        // the slot is relative.
        if (t->kind != TY_REF) {
            GenInto(a->rhs, l, t);
            return;
        }
        if (IsRelRef(t)) {
            GenRelStore(l, a->rhs, RelApartOf(a->lval, t));
            return;
        }
        if (Is<NullLit>(a->rhs)) {
            St(l, IsFatRef(t) ? "GsRref{}" : "nil");
            return;
        }
        GenInto(a->rhs, l, t);
        return;
    }
    if (a->op == T_ASSIGN) {
        if (IsRelRef(t)) {
            GenRelStore(l, a->rhs, RelApartOf(a->lval, t));
            return;
        }
        GenInto(a->rhs, l, t);
        return;
    }
    // The old value is read before the right side runs where that runs
    // statements, as for any operator (§2).
    auto old = HasStmts(a->rhs) ? Snapshot(t, l.lv) : l.lv;
    if (t->kind == TY_STRUCT || t->kind == TY_ARRAY) {
        // Elementwise (§6.1), both operands read before a member is written.
        auto rt = a->rhs->exprtype;
        if (IsPlainRef(rt)) rt = rt->ref->sub;
        auto r = Snapshot(rt, CoerceVal(GenX(a->rhs), a->rhs->exprtype, rt, a->line));
        ElemwiseInto(t, a->op, a->line, old, false, r, rt->kind != t->kind, l);
        return;
    }
    auto r = GenX(a->rhs);
    if (a->op != T_SHLEQ && a->op != T_SHREQ) r = Coerce(r, a->rhs->exprtype, t, a->line);
    L(l.lv, " = ", ArithX(a->op, t, old, r, a->line));
}

// Whether no target can lie at the relative slot an assignment writes, a
// field of a fixed struct (RelSlotApart).
inline bool CodeGenGo::RelApartOf(Node *lval, TypeExpr *rt) {
    auto d = Is<Dot>(lval);
    if (!d || !d->IsField()) return false;
    auto ht = d->obj->exprtype;
    if (ht && ht->kind == TY_REF) ht = ht->ref->sub;
    if (!ht || ht->kind != TY_STRUCT || !IsFix(ht)) return false;
    return RelSlotApart(rt, ht, StructLayout(SI(ht)).offs[(size_t)d->fieldidx]);
}

inline void CodeGenGo::GenIncDec(IncDec *a) {
    auto l = GenLoc(a->lval);
    while (l.t->kind == TY_REF) DerefLoc(l);
    L(l.lv, " = ", ArithX(a->op == T_INC ? T_PLUS : T_MINUS, l.t, l.lv, "1", a->line));
}

}  // namespace goose
