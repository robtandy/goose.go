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
        Decl(t, GoT(sp->rets[0], c->line), call);
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
    if (sp->rets.size() > 1) {
        auto t = T();
        L(FirstResult(t, sp->rets.size()), " := ", call, DECLMARK, t);
        L(d.s, " = ", Coerce(t, sp->rets[0], want, c->line));
        return;
    }
    L(d.s, " = ", Coerce(call, sp->rets[0], want, c->line));
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
        Decl(LocalName(defs[i]), GoT(defs[i]->type, defs[i]->line),
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
            // All-zero bytes are every scalar's default (§4.2).
            auto t = c->rettypes.empty() ? c->exprtype : c->rettypes[0];
            if (!IsScalarT(t)) Unsupported(ln, cat("default<", TypeText(t), ">()"));
            if (d.k == GD_DISCARD) return;
            string zero = t->kind == TY_BOOL ? "false"
                        : t->kind == TY_FLT ? GoFltLit(0, t, ln) : GoIntLit(0, false, t, ln);
            L(d.s, " = ", Coerce(zero, t, d.t ? d.t : c->exprtype, ln));
            return;
        }
        case B_COPY: {
            // A scalar's copy is its value.
            if (!IsScalarT(an[0]->exprtype)) Unsupported(ln, cat("copy of ", TypeText(an[0]->exprtype)));
            GenAny(an[0], GoDst { d.k, d.s, d.t ? d.t : c->exprtype });
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
    if (!IsScalarT(t)) Unsupported(a->line, cat("printing a value of type ", TypeText(t)));
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
