// Goose compiler — the Go backend's workers, queues and program instances
// (definitions of CodeGenGo members, codegen_go.h): thread_spawn and its
// worker's entry thunk, thread_wait, hardware_threads, the typed queues
// (goose_spec.md §11.2), and the program instances a program with workers
// keeps its globals in (implementation.md §6.3, §6.8), as the C backend has
// them (codegen_builtins.h, codegen_emit.h) and the Go runtime runs them
// (go_runtime_threads, codegen_go_runtime.h).
//
// * A worker is a goroutine running its thread program on a GsStackBlock of
//   its own, its own data stacks, fixed-frame stack and program instance
//   (GsThreadSpawn). thread_spawn packs the arguments contiguously on a
//   scratch stack, then each global the worker's program uses as [int64
//   size][image], as C's does; the runtime copies the packet into memory
//   of the worker's own, and the worker's thunk (gsTmain_<worker>) unpacks
//   it, the resizable values onto fresh stacks of its block, before it runs
//   the body.
// * In a program with workers, every global that is not read-only static
//   data (CStaticInit) is a member of gsGlobals, of which each thread
//   program has an instance of its own, and every function that names one
//   reaches it through the block it is handed: gs_gl := (*gsGlobals)(gs.Gl),
//   one load at its start. A global's dedicated stack is the stack of the
//   block its instance names (gs_gl.gsGStk_<name>). Main's instance is the
//   static gsGlobalsMain. A program without workers keeps its globals in
//   package variables, as it always did.
// * The export fns' entry points of a program with workers run each call on
//   an instance of the block's own (EnsureInstanceFn), so that goroutines
//   calling them at once share no globals: a copy of the package's instance
//   of the globals the export's calls reach, taken as the call starts.
// * A queue is a GsQueue per element type, holding the image of each value
//   put: a fixed-size value's bytes, a bytes value's image, a resizable's
//   count, its frame object's fixed fields and its elements (EmitRzImage).
#pragma once

namespace goose {

// Whether the program spawns workers, which decides where its globals live:
// any live call of thread_spawn or thread_spawn_contained, in a function or
// in a global's initializer.
inline void CodeGenGo::FindWorkers() {
    function<void(Node *)> walk = [&](Node *n) {
        if (!n || workers) return;
        if (auto c = Is<Call>(n); c && (c->builtin == B_THREAD_SPAWN || c->builtin == B_THREAD_SPAWN_CONTAINED))
            workers = true;
        RunChildren(n, walk);
    };
    for (auto sp : livespecs) walk(sp->body);
    for (auto g : ast.globals)
        for (auto i : g->inits) walk(i);
}

// A finished body that names the instance reads it from its block first,
// once: whether it does (the function then takes the block, ComputeGsNeeds).
inline bool CodeGenGo::UsesGl(string &fb) {
    if (!workers) return false;
    unordered_map<string, int> uses;
    for (size_t pos = 0; pos < fb.size();) {
        auto nl = fb.find('\n', pos);
        if (nl == string::npos) nl = fb.size();
        GoLineUses(string_view(fb).substr(pos, nl - pos), uses);
        pos = nl + 1;
    }
    if (!uses.count("gs_gl")) return false;
    fb = cat("\tgs_gl := (*gsGlobals)(gs.Gl)\n", fb);
    return true;
}

// The queue of the element type t: one package variable per type, by its
// mangle, as C's gs_q_<mangle> (CodeGen::QueueFor).
inline string CodeGenGo::QueueFor(TypeExpr *t) {
    usesthreads = true;
    auto m = Mangle(t);
    if (auto it = queues.find(m); it != queues.end()) return it->second;
    auto name = Unique(cat("gsQ_", m));
    Append(qdecls, "var ", name, " GsQueue // ", TypeText(t), "\n");
    return queues[m] = name;
}

// The globals of a program instance the thread program of `entry` can name,
// in declaration order (CodeGen::ThreadGlobals): every one its calls reach,
// the workers' it spawns included, which it hands on, but those every
// instance shares as they are, read-only static data. A null entry is every
// member of the instance.
inline vector<VarDef *> &CodeGenGo::ThreadGlobals(FnSpec *entry) {
    auto it = threadglobals.find(entry);
    if (it != threadglobals.end()) return it->second;
    auto member = [&](const VarDef *v) {
        return v && v->isglobal && usedglobals.count(v) && !cstatic.count(v) && !gstatic.count(v);
    };
    set<FnSpec *> seen;
    set<const VarDef *> named;
    function<void(FnSpec *)> rec = [&](FnSpec *sp) {
        if (!sp || !sp->body || !seen.insert(sp).second) return;
        function<void(Node *)> walk = [&](Node *n) {
            if (!n) return;
            if (auto id = Is<Ident>(n); id && member(id->vdef)) named.insert(id->vdef);
            if (auto c = Is<Call>(n)) {
                rec(c->spec);
                for (auto d : c->dispatch) rec(d);
                for (auto &fs : c->fmtspecs) rec(fs.second);
            }
            RunChildren(n, walk);
        };
        walk(sp->body);
    };
    rec(entry);
    auto &out = threadglobals[entry];
    for (auto g : ast.globals)
        for (auto d : g->defs)
            if (entry ? named.count(d) != 0 : member(d)) out.push_back(d);
    return out;
}

// The thread programs, by their entry's name: the order the generated Go
// and the --stacks report take them in (CodeGen::WorkerEntries).
inline vector<pair<string, FnSpec *>> CodeGenGo::WorkerEntries() {
    vector<pair<string, FnSpec *>> entries;
    for (auto &[sp, name] : thunks) entries.push_back({ string(sp->sf->qname), sp });
    sort(entries.begin(), entries.end());
    return entries;
}

// The worker's entry thunk, by name; its body follows once every thread
// program's stack count is known (EmitThreadThunk, from EmitProgram), with
// the count of stacks its block takes, gsTstk_<worker>.
inline string CodeGenGo::EnsureThreadThunk(FnSpec *sp) {
    if (auto it = thunks.find(sp); it != thunks.end()) return it->second;
    auto name = Unique(cat("gsTmain_", Sanitize(sp->sf->ns, sp->sf->name)));
    return thunks[sp] = name;
}

// ------------------------------------------------------------------
// Images: what a queue holds of a value, and a spawn packet of a global.

// The image of the resizable value at src at stk's top (CodeGen::EmitRzImage):
// its count, then a frame object's fixed fields, packed as the bytes of a
// value that is no frame object hold them (FoBytes), then its elements. Any
// other's count, then the bytes of its value, from its base to its stack's
// top, which it tops.
inline void CodeGenGo::EmitRzImage(GoLoc src, TypeExpr *t, const string &stk, Line ln) {
    auto i64 = ast.inttypes[IS_I64];
    if (IsFrameObj(t)) {
        if (IsFoBytesLoc(src)) src = FoView(src);
        if (src.lv.empty()) Fail(ln, cat("internal: a ", TypeText(t), " that is no frame object"));
        auto fo = T();
        Decl(fo, RzGoT(t, ln), src.lv);
        auto th = FoTailHdr(t, fo);
        EmitValStore(stk, i64, cat(th, ".Len"));
        if (auto pre = FoBytesPrefix(t)) {
            auto pp = T();
            Decl(pp, "unsafe.Pointer", Top(stk));
            Bump(stk, cat(pre));
            FoBytes(t, fo, pp, true);
        }
        EmitCopyElems(stk, FoTailArr(t)->arr->sub, cat(th, ".Base"), cat(th, ".Len"));
        return;
    }
    if (src.addr.empty() || src.lenlv.empty()) Fail(ln, cat("internal: a ", TypeText(t), " without its header"));
    auto cnt = T(), base = T();
    Decl(cnt, "int64", src.lenlv);
    Decl(base, "unsafe.Pointer", src.addr);
    EmitValStore(stk, i64, cnt);
    if (t->kind == TY_ARRAY) {
        EmitCopyElems(stk, t->arr->sub, base, cnt);
        return;
    }
    if (src.stk.empty()) Unsupported(ln, cat("the image of a ", TypeText(t), " reached here"));
    auto n = T();
    Decl(n, "int64", cat("int64(uintptr(", Top(src.stk), ") - uintptr(", base, "))"));
    L("copy(unsafe.Slice((*byte)(", Top(stk), "), ", n, "), unsafe.Slice((*byte)(", base, "), ", n, "))");
    Bump(stk, n);
}

// The resizable value of type t whose image (EmitRzImage) is the sz bytes
// at img, received at stk's top: its elements land there, and its count,
// with a frame object's fixed fields, in the receiving lenlv, a header's
// count or the frame object, whose tail starts at the top. `bs`, where the
// image is the Go slice of a queue's message, is that slice, which the
// elements are copied out of as the slice they are.
static string GoImageFrom(const string &img, const string &bs, int64_t off, const string &n) {
    if (!bs.empty()) return cat(bs, "[", off, ":]");
    return cat("unsafe.Slice((*byte)(unsafe.Add(", img, ", ", off, ")), ", n, ")");
}

inline void CodeGenGo::EmitRecvImage(TypeExpr *t, const string &img, const string &sz, const string &stk,
                                     const string &lenlv) {
    int64_t pre = 0;
    if (IsFrameObj(t)) {
        pre = FoBytesPrefix(t);
        if (pre) FoBytes(t, lenlv, AddOff(img, 8), false);
        auto th = FoTailHdr(t, lenlv);
        L(th, ".Base = ", Top(stk));
        L(th, ".Len = *(*int64)(", img, ")");
    } else {
        L(lenlv, " = *(*int64)(", img, ")");
    }
    auto n = T();
    Decl(n, "int64", cat(sz, " - ", 8 + pre));
    // A queue's message is a Go slice, which is copied from as one: its
    // elements' address would be one past its end where there are none.
    auto bs = img.size() > 34 && img.compare(0, 34, "unsafe.Pointer(unsafe.SliceData(") == 0
                  ? img.substr(34, img.size() - 36) : string();
    L("copy(unsafe.Slice((*byte)(", Top(stk), "), ", n, "), ", GoImageFrom(img, bs, 8 + pre, n), ")");
    Bump(stk, n);
}

// The global d's image in a spawn packet at stk's top, [int64 size][image]
// (CodeGen::EmitThreadSpawn): a resizable's as a queue has it (EmitRzImage),
// a pool's freelist after it, its count and its nodes; a bytes value's
// image; a fixed-size value's bytes.
inline void CodeGenGo::EmitGlobalImage(VarDef *d, const string &stk, Line ln) {
    auto i64 = ast.inttypes[IS_I64];
    auto szp = T();
    Decl(szp, "unsafe.Pointer", Top(stk));
    Bump(stk, "8");
    auto t = d->type;
    auto lv = VarLoc(d, ln);
    if (IsResz(t)) {
        EmitRzImage(lv, t, stk, ln);
        if (d->reusable) {
            auto &p = gpools.at(d);
            auto flsz = d->reusable == RU_SLICES ? int64_t(512) : int64_t(8);
            auto fn = T();
            Decl(fn, "int64", cat(p.first, ".Len"));
            EmitValStore(stk, i64, fn);
            CopyElemBytes(Top(stk), cat(p.first, ".Base"), fn, flsz);
            Bump(stk, cat(fn, "*", flsz));
        }
    } else if (IsBytesT(t)) {
        auto n = T();
        Decl(n, "int64", SizeX(t, lv.addr));
        L("copy(unsafe.Slice((*byte)(", Top(stk), "), ", n, "), unsafe.Slice((*byte)(", lv.addr, "), ", n, "))");
        Bump(stk, n);
    } else {
        EmitValStore(stk, t, Ld(lv));
    }
    L("*(*int64)(", szp, ") = int64(uintptr(", Top(stk), ") - uintptr(", szp, ")) - 8");
}

// The global d's image, the sz bytes at img (EmitGlobalImage), into the
// worker's instance (CodeGen::EmitThreadThunk): a value on a stack lands on
// the dedicated stack(s) of the block from index k on, which its instance
// names.
inline void CodeGenGo::EmitGlobalRecv(VarDef *d, const string &img, const string &sz, const string &k) {
    auto t = d->type;
    auto gn = gnames.at(d);
    if (IsResz(t)) {
        L(gstks.at(d), " = ", k);
        if (d->reusable) {
            // [count][elements][freelist count][nodes]: fixed-size elements,
            // the count's bytes of them.
            auto &p = gpools.at(d);
            auto esz = FixedSize(t->arr->sub);
            auto flsz = d->reusable == RU_SLICES ? int64_t(512) : int64_t(8);
            auto k2 = IsGoConst(k) ? cat(stoll(k) + 1) : cat(k, " + 1");
            auto cnt = T(), eb = T(), fln = T();
            Decl(cnt, "int64", cat("*(*int64)(", img, ")"));
            Decl(eb, "int64", cat(cnt, "*", esz));
            L(gn, " = GsRhdr{", Top(k), ", ", cnt, "}");
            L("copy(unsafe.Slice((*byte)(", Top(k), "), ", eb, "), unsafe.Slice((*byte)(unsafe.Add(", img, ", 8)), ",
              eb, "))");
            Bump(k, eb);
            L(p.second, " = ", k2);
            Decl(fln, "int64", cat("*(*int64)(unsafe.Add(", img, ", 8+", eb, "))"));
            L(p.first, " = GsRhdr{", Top(k2), ", ", fln, "}");
            CopyElemBytes(Top(k2), cat("unsafe.Add(", img, ", 16+", eb, ")"), fln, flsz);
            Bump(k2, cat(fln, "*", flsz));
            return;
        }
        if (!IsFrameObj(t)) L(gn, ".Base = ", Top(k));
        EmitRecvImage(t, img, sz, k, RzLenLv(t, gn));
        return;
    }
    if (IsBytesT(t)) {
        L(gstks.at(d), " = ", k);
        L(gn, " = ", Top(k));
        L("copy(unsafe.Slice((*byte)(", Top(k), "), ", sz, "), unsafe.Slice((*byte)(", img, "), ", sz, "))");
        Bump(k, sz);
        return;
    }
    if (TI(t, d->line).size) L(gn, " = ", Ld(AtAddr(img, t)));
}

// ------------------------------------------------------------------
// Workers.

// thread_spawn(worker, args...) and thread_spawn_contained
// (CodeGen::EmitThreadSpawn): the arguments constructed contiguously on a
// scratch stack of this function's, a resizable one as [int64 size][int64
// count][a frame object's fixed fields][its elements], then the worker
// program's globals as copies of this instance's, each as [int64
// size][image], which the runtime copies for the worker and its thunk
// unpacks into the worker's own stacks and instance. Its id.
inline string CodeGenGo::EmitThreadSpawn(Call *c, vector<Node *> &an) {
    usesthreads = true;
    auto sp = c->spec;
    if (!sp || !sinfo.count(sp)) Fail(c->line, "internal: thread_spawn without its worker");
    auto thunk = EnsureThreadThunk(sp);
    auto i64 = ast.inttypes[IS_I64];
    string stk;
    auto base = BytesTemp(stk);
    for (size_t i = 0; i < sp->argtypes.size(); i++) {
        auto pt = sp->argtypes[i];
        auto node = an[1 + i];
        if (Unreached()) break;
        if (!IsResz(pt)) {
            GenConstruct(node, stk, pt);
            continue;
        }
        // A resizable value has a header of the spawner's: what crosses is
        // its count and its bytes, never an address of this instance's.
        auto szp = T(), cntp = T();
        Decl(szp, "unsafe.Pointer", Top(stk));
        Bump(stk, "8");
        Decl(cntp, "unsafe.Pointer", Top(stk));
        EmitValStore(stk, i64, "0");
        if (IsFrameObj(pt)) {
            // Room for the fixed fields, then the value built into a frame
            // object whose elements land behind it; the fields after.
            auto pre = FoBytesPrefix(pt);
            auto pp = T(), h = T();
            Decl(pp, "unsafe.Pointer", Top(stk));
            if (pre) Bump(stk, cat(pre));
            Decl(h, RzGoT(pt, c->line));
            GenConstruct(node, stk, pt, h);
            if (Unreached()) break;
            L("*(*int64)(", cntp, ") = ", FoTailHdr(pt, h), ".Len");
            if (pre) FoBytes(pt, h, pp, true);
        } else {
            GenConstruct(node, stk, pt, cat("*(*int64)(", cntp, ")"));
            if (Unreached()) break;
        }
        L("*(*int64)(", szp, ") = int64(uintptr(", Top(stk), ") - uintptr(", szp, ")) - 8");
    }
    if (Unreached()) return "0";
    for (auto d : ThreadGlobals(sp)) EmitGlobalImage(d, stk, c->line);
    auto id = T();
    Decl(id, "int64", cat("GsThreadSpawn(", thunk, ", ", thunk, "_stacks, unsafe.Sizeof(gsGlobals{}), ", base,
                          ", int64(uintptr(", Top(stk), ") - uintptr(", base, ")), ",
                          c->builtin == B_THREAD_SPAWN_CONTAINED ? "true" : "false", ")"));
    return id;
}

// The worker's entry (CodeGen::EmitThreadThunk), run by its goroutine on the
// block GsThreadSpawn made: `stacks` data stacks for its thread program,
// then the dedicated ones its thunk fills, those of its resizable arguments
// and of its globals. The arguments unpacked from the packet at p, a
// resizable one onto its stack, the globals into the block's instance, then
// the body run with its stacks from 0.
inline void CodeGenGo::EmitThreadThunk(FnSpec *sp, int64_t stacks) {
    auto name = thunks.at(sp);
    auto &ki = sinfo.at(sp);
    auto ln = sp->sf->line;
    if (!ki.freevars.empty() || ki.hasrf || !sp->rets.empty())
        Fail(ln, cat("internal: the thread_fn ", sp->sf->name, " captures, returns from afar or has results"));
    auto save = SaveFnState();
    ResetFnState();
    curspec = nullptr;
    curinfo = nullptr;
    spexpr = "0";
    PushSc(SC_FN);
    vector<string> args;
    auto k = stacks;    // The next dedicated stack.
    int regions = 0;    // How many the thunk fills (thunkregions).
    for (size_t i = 0; i < sp->argtypes.size(); i++) {
        auto pt = sp->argtypes[i];
        auto a = cat("a", i);
        if (IsResz(pt)) {
            auto ks = cat(k++);
            regions++;
            Decl(a, RzGoT(pt, ln));
            L("{");
            ind++;
            auto sz = T();
            Decl(sz, "int64", "*(*int64)(p)");
            L("p = unsafe.Add(p, 8)");
            if (!IsFrameObj(pt)) L(a, ".Base = ", Top(ks));
            EmitRecvImage(pt, "p", sz, ks, RzLenLv(pt, a));
            L("p = unsafe.Add(p, ", sz, ")");
            ind--;
            LS("}");
            args.push_back(a);
            args.push_back(ks);
        } else if (IsBytesT(pt)) {
            // The packet is the worker's own, as long as it runs: the value
            // stays where it lies.
            Decl(a, "unsafe.Pointer", "p");
            L("p = unsafe.Add(p, ", SizeX(pt, a), ")");
            args.push_back(a);
        } else {
            auto &ti = TI(pt, ln);
            if (!ti.size) {
                // A value of no bytes (§3.4): the image holds none.
                Decl(a, ParamT(pt, ln));
            } else if (IsLargeFixed(pt)) {
                // The packet is the worker's own: a large value's bytes there
                // are the callee's copy already, which it takes by its
                // address, as C's does (the parameter is a pointer).
                Decl(a, cat("*", GoT(pt, ln)), cat("(*", GoT(pt, ln), ")(p)"));
                L("p = unsafe.Add(p, ", ti.size, ")");
            } else {
                Decl(a, ParamT(pt, ln), Ld(AtAddr("p", pt)));
                L("p = unsafe.Add(p, ", ti.size, ")");
            }
            args.push_back(a);
        }
    }
    for (auto d : ThreadGlobals(sp)) {
        L("{");
        ind++;
        auto sz = T(), img = T();
        Decl(sz, "int64", "*(*int64)(p)");
        Decl(img, "unsafe.Pointer", "unsafe.Add(p, 8)");
        L("p = unsafe.Add(", img, ", ", sz, ")");
        EmitGlobalRecv(d, img, sz, cat(k));
        auto n = IsResz(d->type) ? (d->reusable ? 2 : 1) : IsBytesT(d->type) ? 1 : 0;
        k += n;
        regions += n;
        ind--;
        LS("}");
    }
    auto list = ki.needssp ? string("gs, 0") : ki.needsgs ? string("gs") : GsArgPh(sp);
    for (auto &a : args) Append(list, list.empty() ? "" : ", ", a);
    L(ki.name, "(", list, ")");
    // Under GS_STACK_STATS a worker reports its stacks as its program ends.
    if (opts.stackstats) L("gsStackStats(gs, ", stacks, ")");
    PopSc();
    L(FRAMEEND);
    auto fb = FinishBody(body, true);
    UsesGl(fb);
    RestoreFnState(save);
    Append(code, "// The entry of the worker `", sp->sf->qname, "` (line ", ln.line, "), whose goroutine runs it on a\n"
           "// block of its own: the arguments and the globals of its spawn at p, unpacked.\n"
           "func ", name, "(gs *GsStackBlock, p unsafe.Pointer) {\n", fb, "}\n\n");
    Append(gdecls, "\n// The data stacks the worker `", sp->sf->qname, "`'s block takes: its program's, then\n"
           "// those its arguments and globals land on.\nconst ", name, "_stacks = ", stacks + regions, "\n");
    thunkregions[sp] = regions;
}

// thread_wait(id) and thread_wait(id, msg) (CodeGen::EmitBuiltin): whether
// the worker returned rather than aborted, once it has ended. A message the
// wait takes is appended to msg, as to_bytes(a, out) appends its image
// (AppendBytes). Emitted here, not returned as text: a discarded answer
// still waits.
inline string CodeGenGo::EmitThreadWait(Call *c, vector<Node *> &an) {
    usesthreads = true;
    auto i64 = ast.inttypes[IS_I64];
    auto id = Coerce(GenX(an[0]), an[0]->exprtype, i64, c->line);
    if (Unreached()) return "false";
    auto ok = T();
    NoteType(ok, "bool");
    if (an.size() == 1) {
        L(ok, ", _ := GsThreadWait(gs, ", id, ", false, ", LocArgs(c->line), ")", DECLMARK, ok);
        return ok;
    }
    auto m = T();
    NoteType(m, "string");
    L(ok, ", ", m, " := GsThreadWait(gs, ", id, ", true, ", LocArgs(c->line), ")", DECLMARK, ok, ",", m);
    L("if ", m, " != \"\" {");
    ind++;
    auto ot = an[1]->exprtype;
    while (ot->kind == TY_REF) ot = ot->ref->sub;
    auto out = IsResz(ot) ? RzRecvLoc(an[1]) : RecvLoc(an[1]);
    AppendBytes(out, cat("unsafe.Pointer(unsafe.StringData(", m, "))"), cat("int64(len(", m, "))"), c->line);
    ind--;
    LS("}");
    return ok;
}

// ------------------------------------------------------------------
// Queues (§11.2).

// qput(v) (CodeGen::EmitBuiltin): a copy of v's image queued, whether it
// was. Emitted here, not returned as text: a discarded result still puts.
inline string CodeGenGo::EmitQPut(Call *c, vector<Node *> &an) {
    auto t = an[0]->exprtype;
    auto q = QueueFor(t);
    auto ok = T();
    if (IsResz(t)) {
        auto src = GenLoc(an[0]);
        while (src.t->kind == TY_REF) DerefLoc(src);
        if (Unreached()) return "false";
        string stk;
        auto base = BytesTemp(stk);
        EmitRzImage(src, t, stk, c->line);
        Decl(ok, "bool", cat("GsQPut(&", q, ", ", base, ", int64(uintptr(", Top(stk), ") - uintptr(", base, ")))"));
        return ok;
    }
    if (IsBytesT(t)) {
        auto l = GenLoc(an[0]);
        while (l.t->kind == TY_REF) DerefLoc(l);
        if (Unreached()) return "false";
        auto p = T();
        Decl(p, "unsafe.Pointer", l.addr);
        Decl(ok, "bool", cat("GsQPut(&", q, ", ", p, ", ", SizeX(t, p), ")"));
        return ok;
    }
    auto &ti = TI(t, c->line);
    auto x = GenXD(an[0], t);
    if (Unreached()) return "false";
    if (!ti.size) {
        // A value of no bytes (§3.4) crosses as none.
        if (!IsGoName(x) && !IsGoConst(x)) L("_ = ", x);
        Decl(ok, "bool", cat("GsQPut(&", q, ", nil, 0)"));
        return ok;
    }
    if (IsLargeFixed(t)) {
        // Copied to a slot on a data stack first, as C's is (FixedLocal).
        auto tv = FixedLocal(t, T(), x, false, c->line);
        Decl(ok, "bool", cat("GsQPut(&", q, ", unsafe.Pointer(&", tv, "), ", ti.size, ")"));
        return ok;
    }
    Decl(ok, "bool", cat("GsQPutV(&", q, ", ", x, ", ", ti.size, ")"));
    return ok;
}

// qget<T>(), qget<T>(ms) and qpoll<T>() (CodeGen::EmitBuiltin): the queue's
// first value, and for the last two whether one came, T's zero value where
// none did. A resizable value lands where d0 has it built, its elements at
// its stack's top and its count (or frame object) in d0's receiver, or on a
// temporary's of its own; a bytes value's image at d0's stack's top, or a
// temporary's; a fixed-size value in a temporary. The results, as
// GenCallVals gives them: the value (its header or base, "" where d0
// received it), then the bool.
inline vector<string> CodeGenGo::EmitQGet(Call *c, const GoDst &d0) {
    auto an = c->ArgNodes();
    auto ln = c->line;
    auto i64 = ast.inttypes[IS_I64];
    auto t = c->rettypes[0];
    auto q = QueueFor(t);
    auto poll = c->rettypes.size() > 1;
    auto nn = T();
    string got;
    if (poll) {
        auto ms = c->builtin == B_QPOLL ? string("0") : Coerce(GenX(an[0]), an[0]->exprtype, i64, ln);
        if (Unreached()) return { "", "false" };
        got = T();
        NoteType(nn, "[]byte");
        NoteType(got, "bool");
        L(nn, ", ", got, " := GsQWait(&", q, ", ", ms, ")", DECLMARK, nn, ",", got);
    } else {
        Decl(nn, "[]byte", cat("GsQGet(&", q, ")"));
    }
    auto ret = [&](const string &v) { return poll ? vector<string> { v, got } : vector<string> { v }; };
    auto img = cat("unsafe.Pointer(unsafe.SliceData(", nn, "))");
    auto sz = cat("int64(len(", nn, "))");
    if (IsResz(t)) {
        // The image is [int64 count][a frame object's fixed fields][the
        // elements] (EmitRzImage); a missed poll leaves the zero value.
        string stk = d0.k == GD_STACK && !d0.lenlv.empty() ? d0.s : "";
        string lenlv = d0.k == GD_STACK ? d0.lenlv : "";
        string hv;
        if (stk.empty()) {
            hv = RzTemp(t, stk);
            lenlv = RzLenLv(t, hv);
        }
        if (IsFrameObj(t)) {
            L(lenlv, " = ", RzGoT(t, ln), "{}");
            L(FoTailHdr(t, lenlv), ".Base = ", Top(stk));
        } else if (poll) {
            L(lenlv, " = 0");
        }
        L(poll ? cat("if ", got, " {") : string("{"));
        ind++;
        EmitRecvImage(t, img, sz, stk, lenlv);
        ind--;
        LS("}");
        return ret(hv);
    }
    if (IsBytesT(t)) {
        // An element run's destination takes the elements of the image,
        // which lands on a temporary first.
        auto run = d0.k == GD_STACK && !d0.lenlv.empty();
        string stk = d0.k == GD_STACK && !run ? d0.s : "";
        string base;
        if (stk.empty()) {
            base = BytesTemp(stk);
        } else {
            base = T();
            Decl(base, "unsafe.Pointer", Top(stk));
        }
        L(poll ? cat("if ", got, " {") : string("{"));
        ind++;
        auto n = T();
        Decl(n, "int64", sz);
        L("copy(unsafe.Slice((*byte)(", Top(stk), "), ", n, "), ", nn, ")");
        Bump(stk, n);
        ind--;
        if (poll) {
            // A missed poll still yields a valid value, the zero one.
            LS("} else {");
            ind++;
            auto mb = MinBytes(t, true);
            if (mb) {
                L("clear(unsafe.Slice((*byte)(", Top(stk), "), ", mb, "))");
                Bump(stk, cat(mb));
            }
            ind--;
        }
        LS("}");
        if (run) {
            GenArrayFromLoc(BytesLoc(base, t, GoLoc {}), d0.t ? d0.t : t, d0.s, ln, d0.lenlv);
            return ret("");
        }
        return ret(base);
    }
    auto &ti = TI(t, ln);
    auto tv = T();
    if (!ti.size) {
        // A value of no bytes (§3.4): the message holds none to read.
        Decl(tv, LocalT(t, ln));
    } else if (IsLargeFixed(t)) {
        // Into a slot on a data stack, as C's is (FixedLocal).
        tv = FixedLocal(t, tv, "", false, ln, false);
        L("copy(unsafe.Slice((*byte)(unsafe.Pointer(&", tv, ")), ", ti.size, "), ", nn, ")");
    } else {
        Decl(tv, LocalT(t, ln), cat("GsQValue[", ti.name, "](", nn, ")"));
    }
    return ret(tv);
}

// ------------------------------------------------------------------
// The export fns' instances.

// The globals an export's calls reach, of the instance (ThreadGlobals): what
// its entry points copy for it. One holding a reference to a data stack
// cannot be copied into another block: the copy would reach the package's
// instance's storage, which it shares with every other call.
inline vector<VarDef *> &CodeGenGo::InstanceGlobals(FnSpec *entry) {
    auto &gl = ThreadGlobals(entry);
    for (auto d : gl)
        if (PrefVar(d) || HoldsFatRef(d->type))
            Unsupported(entry->sf->line, cat("export fn ", entry->sf->name, ", whose calls reach the global ", d->name,
                                             ", which holds a reference to a data stack, in a program with workers,"));
    return gl;
}

// The function giving a block a program instance of its own for a call of
// the export fn sp (gsEnter_<export>), or of any export, where sp is null
// (gsEnter): copies of the package's globals they reach, from the package's
// instance on the block that main's program (or the package's init) runs,
// a value on a stack of its own landing on the block's stack the main
// program's layout gives it (gsGStk_<name>).
inline string CodeGenGo::EnsureInstanceFn(FnSpec *sp) {
    if (auto it = instfns.find(sp); it != instfns.end()) return it->second;
    auto name = Unique(sp ? cat("gsEnter_", sinfo.at(sp).name) : string("gsEnter"));
    instfns[sp] = name;
    auto save = SaveFnState();
    ResetFnState();
    curspec = nullptr;
    curinfo = nullptr;
    spexpr = "0";
    PushSc(SC_FN);
    // The package's instance's member of the one of the block's instance.
    auto src = [&](const string &member) {
        return member.compare(0, 6, "gs_gl.") == 0 ? cat("gsGlobalsMain.", member.substr(6)) : member;
    };
    // The bytes from start to the top of the package's stack of stkfield,
    // which the value there tops, onto the block's stack of it, newstart
    // pointing at them.
    auto copyrun = [&](const string &stkfield, const string &start, const string &newstart) {
        auto k = stkfield.substr(6);   // The main program's index of it, a constant.
        L(stkfield, " = ", k);
        L(Top(k), " = gs.Base[", k, "]");
        auto n = T();
        Decl(n, "int64", cat("int64(uintptr(gsMainBlock.Stks[", k, "]) - uintptr(", start, "))"));
        L(newstart, " = ", Top(k));
        L("copy(unsafe.Slice((*byte)(", Top(k), "), ", n, "), unsafe.Slice((*byte)(", start, "), ", n, "))");
        Bump(k, n);
    };
    auto stacks = false;
    for (auto d : sp ? InstanceGlobals(sp) : ThreadGlobals(nullptr)) {
        auto t = d->type;
        if (!sp && (PrefVar(d) || HoldsFatRef(t))) continue;   // No export reaches it.
        auto gn = gnames.at(d);
        if (!IsBytesT(t)) {
            if (TI(t, d->line).size) L(gn, " = ", src(gn));
            continue;
        }
        stacks = true;
        if (!IsResz(t)) {
            copyrun(gstks.at(d), src(gn), gn);
            continue;
        }
        L(gn, " = ", src(gn));
        auto start = IsFrameObj(t) ? cat(FoTailHdr(t, gn), ".Base") : cat(gn, ".Base");
        copyrun(gstks.at(d), src(start), start);
        if (d->reusable) {
            auto &p = gpools.at(d);
            L(p.first, " = ", src(p.first));
            copyrun(p.second, cat(src(p.first), ".Base"), cat(p.first, ".Base"));
        }
    }
    PopSc();
    auto fb = FinishBody(body);
    UsesGl(fb);
    RestoreFnState(save);
    Append(xcode, "// ", name, " gives gs a program instance of its own for a call of ",
           sp ? cat("export fn ", sp->sf->name) : string("any export fn"),
           " (§11.2):\n// copies of the package's globals ", sp ? "its" : "their",
           " calls reach, as main's program has them.\nfunc ", name, "(gs *GsStackBlock) {\n",
           stacks ? "\tgs.EnsureStacks(gsGStkEnd)\n" : "", "\tgs.NewInstance(unsafe.Sizeof(gsGlobals{}))\n", fb,
           "}\n\n");
    return name;
}

}  // namespace goose
