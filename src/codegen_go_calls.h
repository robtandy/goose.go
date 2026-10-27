// Goose compiler — the Go backend's calls (definitions of CodeGenGo members,
// codegen_go.h): calls to specializations, the free variables a nested
// function reaches (§7.5), function values spliced inline (§7.6), and the
// builtins of the scalar core: print (§3.7), assert, abort and exit (§9.3),
// default and copy.
#pragma once

namespace goose {

// The arguments the callee's parameters take (Call::ArgNodes): the
// function-value arguments that follow them are compile-time only.
inline vector<Node *> CodeGenGo::CallArgNodes(Call *c, size_t nparams) {
    auto an = c->ArgNodes();
    an.resize(nparams);
    return an;
}

// A free variable of the callee, from the caller: the address of its own
// variable, or the pointer it was handed itself.
inline string CodeGenGo::FreeVarArg(const VarDef *fv) {
    auto it = vnames.find(fv);
    if (it == vnames.end()) Fail(fv->line, cat("internal: free variable ", fv->name, " not in scope"));
    return fvptr.count(fv) ? it->second : cat("&", it->second);
}

// The Go call of a specialization, its arguments evaluated: the data stack
// block and the first index free here where the callee needs them, the
// declared parameters, then the free variables. An argument read before a
// later one that runs statements is taken into a temporary first (§2).
inline string CodeGenGo::SpecCallX(Call *c, FnSpec *sp) {
    if (!sinfo.count(sp)) Fail(c->line, cat("internal: no Go function for ", sp->sf->name));
    auto &ki = sinfo[sp];
    auto an = CallArgNodes(c, sp->params.size());
    vector<bool> later(an.size() + 1, false);
    for (size_t i = an.size(); i-- > 0;) later[i] = later[i + 1] || HasStmts(an[i]);
    vector<string> args;
    for (size_t i = 0; i < an.size(); i++) {
        auto pt = sp->argtypes[i];
        auto x = Coerce(GenX(an[i]), an[i]->exprtype, pt, an[i]->line);
        if (later[i + 1] && !IsGoConst(x)) x = Snapshot(pt, x);
        args.push_back(x);
    }
    for (auto fv : ki.freevars) args.push_back(FreeVarArg(c->FreeVarArg(fv)));
    string list;
    if (ki.needssp) {
        NoStackAcrossCycleCall(sp, c->line);
        NoteStackCall(sp, c->line);
        list = cat("gs, ", SpTop());
    }
    for (auto &a : args) Append(list, list.empty() ? "" : ", ", a);
    return cat(ki.name, "(", list, ")");
}

// The targets that take a call's first result of n into t, and drop the rest.
static string FirstResult(const string &t, size_t n) {
    auto s = t;
    for (size_t i = 1; i < n; i++) s += ", _";
    return s;
}

// A call as a value: made at this point, its first result in a temporary.
inline string CodeGenGo::GenCall(Call *c) {
    if (c->builtin >= 0 || c->fvbody) return CtlValX(c);
    if (!c->dispatch.empty()) Unsupported(c->line, "a case-function dispatch");
    if (!c->spec) Fail(c->line, "internal: unresolved call");
    auto sp = c->spec;
    if (sp->sf->isextern) Unsupported(c->line, "a call to an extern fn");
    if (sp->rets.empty()) Fail(c->line, "internal: a call without results as a value");
    auto call = SpecCallX(c, sp);
    auto t = T();
    if (sp->rets.size() > 1) {
        // A call in an ordinary value context yields its first result.
        GoT(sp->rets[0], c->line);
        L(FirstResult(t, sp->rets.size()), " := ", call, DECLMARK, t);
    } else {
        Decl(t, LocalT(sp->rets[0], c->line), call);
    }
    return Coerce(t, sp->rets[0], c->exprtype, c->line);
}

inline void CodeGenGo::GenCallAny(Call *c, const GoDst &d) {
    if (c->builtin >= 0) { GenBuiltin(c, d); return; }
    if (c->fvbody) { GenFvCall(c, d); return; }
    if (!c->dispatch.empty()) Unsupported(c->line, "a case-function dispatch");
    if (!c->spec) Fail(c->line, "internal: unresolved call");
    auto sp = c->spec;
    if (sp->sf->isextern) Unsupported(c->line, "a call to an extern fn");
    auto call = SpecCallX(c, sp);
    // A call's results may go unused, as an expression statement.
    if (d.k == GD_DISCARD || sp->rets.empty()) {
        L(call);
        return;
    }
    auto want = d.t ? d.t : c->exprtype;
    string r = call;
    if (sp->rets.size() > 1) {
        r = T();
        L(FirstResult(r, sp->rets.size()), " := ", call, DECLMARK, r);
    }
    if (IsRelRef(want)) {
        // A reference into a relative slot: the plain one, encoded there.
        auto pt = ast.RefTo(want->ref->sub, c->line, want->ref->optional);
        auto v = Snapshot(pt, Coerce(r, sp->rets[0], pt, c->line));
        EmitRelStore(AtVar(d.s, want), v, NonNullRef(c), false, c->line);
        return;
    }
    L(d.s, " = ", Coerce(r, sp->rets[0], want, c->line));
}

// A multi-name binding from one call (`let a, b = f();`): each result into
// its variable, through temporaries of the callee's types where those are
// not the variables'.
inline void CodeGenGo::GenCallInto(Call *c, const vector<VarDef *> &defs) {
    if (c->builtin >= 0 || c->fvbody || !c->dispatch.empty() || !c->spec)
        Unsupported(c->line, "a multiple binding from this call");
    auto sp = c->spec;
    if (sp->sf->isextern) Unsupported(c->line, "a call to an extern fn");
    if (sp->rets.size() != defs.size()) Fail(c->line, "internal: result count of a binding");
    auto same = true;
    for (size_t i = 0; i < defs.size(); i++) {
        GoT(defs[i]->type, defs[i]->line);
        same = same && SameGoT(sp->rets[i], defs[i]->type);
    }
    auto call = SpecCallX(c, sp);
    string names, marks;
    vector<string> temps;
    for (size_t i = 0; i < defs.size(); i++) {
        auto n = same ? LocalName(defs[i]) : T();
        temps.push_back(n);
        Append(names, i ? ", " : "", n);
        Append(marks, i ? "," : "", n);
    }
    L(names, " := ", call, DECLMARK, marks);
    if (same) return;
    for (size_t i = 0; i < defs.size(); i++)
        Decl(LocalName(defs[i]), LocalT(defs[i]->type, defs[i]->line),
             Coerce(temps[i], sp->rets[i], defs[i]->type, c->line));
}

// Calling a function value: the checked body instance is spliced inline
// (§7.6), its parameters ordinary locals of this function.
inline void CodeGenGo::GenFvCall(Call *c, const GoDst &d) {
    auto et = c->fvbody->exprtype;
    auto wantsval = !IsVoidT(et) && d.k != GD_DISCARD;
    if (Unreached()) return GenUnreached([&] { GenFvCall(c, d); });
    PushSc(SC_PLAIN);
    L("{");
    ind++;
    for (size_t i = 0; i < c->fvparams.size(); i++)
        BindLocal(c->fvparams[i], i < c->args.size() ? c->args[i] : nullptr);
    for (auto st : c->fvbody->stmts) GenStmt(st);
    if (c->fvbody->tail) {
        if (wantsval) GenAny(c->fvbody->tail, GoDst { GD_LVALUE, d.s, d.t ? d.t : c->exprtype });
        else GenAny(c->fvbody->tail, GoDst {});
    }
    PopSc();
    ind--;
    LS("}");
}

// ------------------------------------------------------------------
// Builtins.

// The format overloads the call's argument is rendered with, which this
// version renders none of (CodeGen::FmtContext, FmtSpecFor).
static Call *GoFmtContext(Call *c, Node *arg) {
    if (!c || c->fmtcontexts.empty()) return c;
    auto args = c->ArgNodes();
    for (size_t i = 0; i < args.size(); i++)
        if (args[i] == arg && i < c->fmtcontexts.size()) return c->fmtcontexts[i];
    return c;
}

inline void CodeGenGo::GenBuiltin(Call *c, const GoDst &d) {
    auto an = c->ArgNodes();
    auto ln = c->line;
    switch ((BuiltinKind)c->builtin) {
        case B_PRINT: EmitPrint(c); return;
        case B_ASSERT: {
            auto x = GenTruth(an[0]);
            if (Unreached()) return GenUnreached([&] { GenBuiltin(c, d); });
            L("if !", x, " {");
            ind++;
            L("GsAbort(GsEAssert, ", LocArgs(ln), ")");
            ind--;
            LS("}");
            return;
        }
        case B_ABORT: {
            auto s = Is<StrLit>(an[0]);
            if (!s) Unsupported(ln, "abort with a message that is not a string literal");
            L("GsAbortMsg(", GoStr(s->val), ", ", LocArgs(ln), ")");
            return;
        }
        case B_EXIT: {
            auto x = GenX(an[0]);
            L("GsExit(", Coerce(x, an[0]->exprtype, ast.inttypes[IS_I64], ln), ")");
            return;
        }
        case B_DEFAULT: {
            // All-zero bytes are every fixed type's default (§4.2), which is
            // Go's zero value of its representation, but where a field
            // declares a default of its own: the checker built that as a
            // literal (Call::defaultinit).
            auto t = c->rettypes.empty() ? c->exprtype : c->rettypes[0];
            GoT(t, ln);
            auto want = d.t ? d.t : c->exprtype;
            if (!c->defaultinit) {
                if (d.k == GD_DISCARD) return;
                if (IsRelRef(want)) {
                    L(d.s, " = 0");
                    return;
                }
                L(d.s, " = ", Coerce(ZeroOf(t, ln), t, want, ln));
                return;
            }
            auto v = T();
            Decl(v, LocalT(t, ln));
            if (t->kind == TY_ARRAY) {
                auto av = ArrayView(AtVar(v, t), ln);
                auto i = T();
                L("for ", i, " := int64(0); ", i, " < ", ArrSize(t->arr), "; ", i, "++ {");
                ind++;
                PushSc(SC_PLAIN);
                GenAny(c->defaultinit, GoDst { GD_LVALUE, ElemAt(av, i, ln).lv, t->arr->sub });
                PopSc();
                ind--;
                LS("}");
            } else {
                GenAny(c->defaultinit, GoDst { GD_LVALUE, v, t });
            }
            if (d.k != GD_DISCARD) L(d.s, " = ", Coerce(v, t, want, ln));
            return;
        }
        case B_COPY: {
            // A copy is the value the place holds (§4.1).
            auto want = d.t ? d.t : c->exprtype;
            if (IsScalarT(an[0]->exprtype)) {
                GenAny(an[0], GoDst { d.k, d.s, want });
                return;
            }
            auto rt = c->rettypes.empty() ? c->exprtype : c->rettypes[0];
            auto x = LoadLoc(GenLoc(an[0]), rt, ln);
            if (want && want->kind == TY_SLICE && rt->kind != TY_SLICE) {
                // A slice destination views the copy, not its source.
                auto v = Snapshot(rt, x);
                x = LoadLoc(AtVar(v, rt), want, ln);
            } else {
                x = Coerce(x, rt, want, ln);
            }
            if (d.k == GD_DISCARD) {
                if (!IsGoName(x)) L("_ = ", x);
                return;
            }
            if (x != d.s) L(d.s, " = ", x);
            return;
        }
        case B_LEN: case B_CAP: case B_INDEX_OF: case B_PUSH: case B_APPEND: case B_POP:
        case B_POP_N: case B_CLEAR: case B_RESIZE: {
            auto x = GenArrayBuiltin(c);
            if (x.empty() || Unreached()) return;
            auto want = d.t ? d.t : c->exprtype;
            auto rt = c->rettypes.empty() ? c->exprtype : c->rettypes[0];
            if (d.k == GD_DISCARD) {
                if (!IsGoName(x) && !IsGoConst(x)) L("_ = ", x);
                return;
            }
            if (want && IsRelRef(want)) {
                auto v = Snapshot(rt, x);
                EmitRelStore(AtVar(d.s, want), v, NonNullRef(c), false, ln);
                return;
            }
            x = Coerce(x, rt, want, ln);
            if (x != d.s) L(d.s, " = ", x);
            return;
        }
        default:
            Unsupported(ln, cat("the builtin ", BuiltinByKind(c->builtin).name, "()"));
    }
}

// print(a, b, ...): the whole line, newline included, is rendered on a data
// stack of the statement's before any of it is written, so that an argument
// that prints on its own cannot interleave with it, and it goes out as one
// write (§3.7), as the C backend's does (TempBuilder). The bytes are written
// at the stack's top through a cursor of the line's own: nothing else uses
// that stack while it is rendered, since a call in an argument is handed the
// indices above it. Each write is bounded by the end of the stack's region:
// a line that would run past it is a data stack overflow, as the C
// runtime's guard gap makes it. An argument that ends the flow (a return
// out of an inlined body, say) leaves the cursors declared and the rest of
// the line unreached, which FinishBody gives a use of its own.
inline void CodeGenGo::EmitPrint(Call *c) {
    if (Unreached()) return GenUnreached([&] { EmitPrint(c); });
    auto k = AllocStk();
    auto base = T(), cur = T(), end = T();
    Decl(base, "unsafe.Pointer", cat("gs.Stks[", k, "]"));
    Decl(cur, "unsafe.Pointer", base);
    Decl(end, "unsafe.Pointer", cat("gs.End(", k, ")"));
    for (auto a : c->ArgNodes()) RenderInto(cur, end, a, c);
    L(cur, " = gsPutStr(", cur, ", ", end, ", \"\\n\")");
    L("gsOut(", base, ", ", cur, ")");
}

// The text of argument a at the cursor `cur` (§3.7): a scalar's, or a string
// literal's bytes as they are.
inline void CodeGenGo::RenderInto(const string &cur, const string &end, Node *a, Call *c) {
    auto ctx = GoFmtContext(c, a);
    auto t = a->exprtype;
    if (ctx)
        for (auto &fs : ctx->fmtspecs)
            if (fs.first == t || (IsScalarT(t) && IsScalarT(fs.first) && SameGoT(fs.first, t)))
                Unsupported(a->line, "a format overload");
    if (auto s = Is<StrLit>(a)) {
        L(cur, " = gsPutStr(", cur, ", ", end, ", ", GoStr(s->val), ")");
        return;
    }
    if (!IsScalarT(t)) {
        if (t->kind == TY_REF && !t->ref->optional) t = t->ref->sub;
        GoT(t, a->line);
        auto l = GenLoc(a);
        if (Unreached()) return;
        RenderLoc(cur, end, l, t, false, a->line);
        return;
    }
    auto x = GenX(a);
    switch (t->kind) {
        case TY_INT:
            if (t->intstorage == IS_U64) L(cur, " = gsFmtU64(", cur, ", ", end, ", ", x, ")");
            else L(cur, " = gsFmtI64(", cur, ", ", end, ", ",
                   Coerce(x, t, ast.inttypes[IS_I64], a->line), ")");
            return;
        case TY_FLT:
            L(cur, " = gsFmt", t->fltstorage == FS_F32 ? "F32" : "F64", "(", cur, ", ", end, ", ", x,
              ")");
            return;
        default:
            L(cur, " = gsFmtBool(", cur, ", ", end, ", ", x, ")");
            return;
    }
}

}  // namespace goose
