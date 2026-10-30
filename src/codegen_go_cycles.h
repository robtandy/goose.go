// Goose compiler — the Go backend's inlining into recursive cycles (issue #42,
// DESIGN.md §3.14), a pass over the optimizer's output that src/main.cpp
// runs for --target=go before BCE.
//
// The optimizer inlines nothing into a function of a recursive cycle, a
// `recursive fn` or a member of its cycle (Optimizer::TryInline, "Never into
// a recursive cycle": the inlined body's locals would become the cycle
// function's own, upsetting §7.8's stack assignment). So every call a
// recursive kernel makes stays a call, a leaf helper's and an `inline fn`'s
// too: a hash function or a comparator called at each step costs the call,
// and its body is optimized apart from the facts around it. What the rule
// guards against is a local holding a data stack: a callee that takes none
// adds nothing to the cycle's stacks wherever its body is. This pass gives
// each function of a cycle the inlining any other function has, of the
// callees that are in no cycle themselves and whose code takes no data stack
// at all, nor any function's they call:
//
// * The optimizer's own walk inlines them (Optimizer::OptBlock over the
//   function's body, its rule lifted for it alone), with its own rules of
//   size, use and nesting, so that a callee is inlined into a recursive fn
//   where it would be into any other, and the copies are folded as any are.
//   A callee in a cycle never is (its InlineInfo::noinline, which Scan
//   gives every member of one); nor is a callee that takes a data stack
//   (a nonfixed value, a value of more than NATIVE_VALUE_LIMIT bytes, which
//   C holds on one, a print or a str, a spawn or a queue), which this pass
//   marks so for its walk. So every activation of a cycle holds the stacks
//   it held, at the same indices: the static stack count of §10.3 is what
//   it was, and the two backends report the same stacks (--stacks).
// * Only the inlining is new: a self-call's base case was copied already
//   (BaseCaseInliner, set aside here), and folding and propagation, which
//   the walk does again, find nothing more but in the copies.
// * The optimizer's last steps follow: the liveness of what its trees reach,
//   a helper inlined everywhere going dead, as at the end of its own pass.
//
// The C backend keeps upstream's rule: docs/upstream/recursive-inlining.md
// proposes the same change there.
#pragma once

namespace goose {

// An upper bound of the bytes of a fixed-size value of type t, or -1 where t
// is not plainly fixed-size: a scalar's, a slice's or a reference's, a fixed
// array's, struct's, ADT's or variant's from its parts and pads, and 0 for
// what has no storage of its own (a statement's void, a function's name).
// Past NATIVE_VALUE_LIMIT the count stops at one more.
inline int64_t GoCycleValueBound(TypeExpr *t, int depth = 0) {
    const int64_t over = CodeGenGo::NATIVE_VALUE_LIMIT + 1;
    if (!t || depth > 32) return -1;
    auto runs = [&](span<const FieldRun> rs, int64_t base) -> int64_t {
        int64_t most = 0;
        for (auto &run : rs) {
            int64_t n = 0;
            for (size_t i = 0; i < run.fields->size(); i++) {
                auto &f = (*run.fields)[i];
                auto b = f.ispad ? (f.padsize >= 0 ? f.padsize : 8) : GoCycleValueBound((*run.ftypes)[i], depth + 1);
                if (b < 0) return -1;
                n = std::min(n + b, over);
            }
            most = std::max(most, n);
        }
        return std::min(base + most, over);
    };
    switch (t->kind) {
        case TY_VOID: case TY_FN: return 0;
        case TY_BOOL: return 1;
        case TY_INT: return t->intstorage == IS_VARINT ? -1 : IntBits(t->intstorage) / 8;
        case TY_FLT: return t->fltstorage == FS_F32 ? 4 : 8;
        case TY_SLICE: return 16;
        case TY_REF: return t->ref->lenstorage == IS_VARINT ? -1 : 16;
        case TY_ARRAY: {
            auto a = t->arr;
            if ((a->akind != A_FIXED && a->akind != A_LIMITED) || a->size < 0) return -1;
            auto e = GoCycleValueBound(a->sub, depth + 1);
            if (e < 0) return -1;
            auto len = a->akind == A_LIMITED ? 8 : 0;
            if (e && a->size > over / e) return over;
            return std::min(len + e * a->size, over);
        }
        case TY_STRUCT: {
            auto si = t->struc->inst;
            if (!si || si->failed || si->sclass != SC_FIXED || si->frameobj) return -1;
            return runs(RunsOf(si), 0);
        }
        case TY_ENUM: {
            auto ei = t->enu->inst;
            if (t->enu->varmode || !ei || ei->failed || !ei->allfixed) return -1;
            return runs(RunsOf(ei), 8);
        }
        case TY_VARIANT: return GoCycleValueBound(t->var->adt, depth + 1);
        default: return -1;
    }
}

// Whether a builtin's call takes no data stack of its own: one that reads or
// changes what its receiver or its arguments are, or aborts, where print,
// str and format render text on one, to_bytes and from_bytes build an image
// there, and a spawn, a wait or a queue's put and get pack values there.
inline bool GoBuiltinTakesNoStack(int b) {
    switch (b) {
        case B_ASSERT: case B_ABORT: case B_EXIT: case B_LEN: case B_CAP: case B_INDEX_OF:
        case B_PUSH: case B_APPEND: case B_POP: case B_POP_N: case B_RESIZE: case B_CLEAR:
        case B_DEFAULT: case B_COPY: case B_BYTES_OF:
            return true;
        default:
            return false;
    }
}

// Whether nothing a call of sp runs takes a data stack: sp is in no cycle,
// and its body, and those of the functions it calls, hold no value of a
// type that is not fixed-size or is larger than NATIVE_VALUE_LIMIT -- no
// parameter, result, local, binding or temporary -- and make no call of a
// builtin that takes one. A function of no body is an extern fn, whose call
// takes none. memo holds what is known: 1 while sp is being looked at, 2
// for a function that takes none, 3 for one that may.
inline bool GoTakesNoStack(FnSpec *sp, unordered_map<FnSpec *, int> &memo) {
    if (auto it = memo.find(sp); it != memo.end()) return it->second == 2;
    memo[sp] = 1;
    auto small = [](TypeExpr *t) {
        if (!t) return true;
        auto b = GoCycleValueBound(t);
        return b >= 0 && b <= CodeGenGo::NATIVE_VALUE_LIMIT;
    };
    auto ok = sp->body && !sp->incycle && !sp->sf->isrec && !sp->sf->isthread;
    for (auto t : sp->argtypes) ok = ok && small(t);
    for (auto t : sp->rets) ok = ok && small(t);
    function<void(Node *)> walk = [&](Node *n) {
        if (!n || !ok) return;
        ok = small(n->exprtype);
        auto var = [&](VarDef *v) {
            if (v && !small(v->type)) ok = false;
        };
        if (auto d = Is<VarDecl>(n)) for (auto v : d->defs) var(v);
        if (auto fl = Is<ForLoop>(n)) {
            var(fl->vdef);
            var(fl->idxdef);
        }
        if (auto m = Is<MatchExpr>(n)) for (auto &arm : m->arms) var(arm.binder);
        if (auto c = Is<Call>(n)) {
            for (auto t : c->rettypes) ok = ok && small(t);
            for (auto v : c->fvparams) var(v);
            if (!c->fmtspecs.empty()) ok = false;
            if (c->builtin >= 0) ok = ok && GoBuiltinTakesNoStack(c->builtin);
            else if (!c->spec) ok = ok && !c->dispatch.empty();
            else if (c->spec->body) ok = ok && GoTakesNoStack(c->spec, memo);
            else ok = ok && c->spec->sf->isextern;
            for (auto k : c->dispatch) ok = ok && GoTakesNoStack(k, memo);
        }
        if (ok) RunChildren(n, walk);
    };
    walk(sp->body);
    memo[sp] = ok ? 2 : 3;
    return ok;
}

// The pass: each live function of a recursive cycle, its body walked by the
// optimizer again with the rule that keeps its calls lifted, every callee
// that takes a data stack marked not to inline for the walk; then the
// liveness the optimizer's driver ends with. At -O0 nothing is inlined.
inline void InlineIntoCycles(Optimizer &o) {
    if (!o.caninline) return;
    vector<FnSpec *> cycles;
    for (auto sp : o.postorder)
        if (sp->body && (sp->incycle || sp->sf->isrec)) cycles.push_back(sp);
    if (cycles.empty()) return;
    unordered_map<FnSpec *, int> memo;
    vector<pair<FnSpec *, Optimizer::InlineInfo>> kept;
    for (auto sp : o.postorder) {
        auto &info = o.inlineinfo[sp];
        if (info.noinline || GoTakesNoStack(sp, memo)) continue;
        kept.push_back({ sp, info });
        info.noinline = true;
        info.c_inlinable = false;
    }
    // The base cases were copied into the self-calls as the bodies were
    // optimized (optimize_basecase.h), from the bodies as they were.
    if (o.basecase) o.basecase->basespec = nullptr;
    for (auto sp : cycles) {
        auto incycle = sp->incycle, isrec = sp->sf->isrec;
        sp->incycle = sp->sf->isrec = false;
        o.curspec = sp;
        o.deferonce = false;
        o.OptBlock(sp->body);
        sp->incycle = incycle;
        sp->sf->isrec = isrec;
    }
    o.curspec = nullptr;
    for (auto &[sp, info] : kept) o.inlineinfo[sp] = info;
    // Specs whose every call is inlined now go dead, as at the end of the
    // optimizer's own pass.
    for (auto sp : o.ast.fnspecs) {
        sp->live = false;
        sp->uses = 0;
    }
    o.postorder.clear();
    o.ReachRoots();
}

}  // namespace goose
