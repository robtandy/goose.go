// Goose compiler — the Go backend's fixed-size types (definitions of
// CodeGenGo members, codegen_go.h): the packed layouts the C backend lays
// values out in (FixedSize, LayoutFields, LenStore, TagStore: codegen_types.h,
// implementation.md §6.1), and the Go type each fixed-size type is
// represented as (DESIGN.md §3.2), with its declaration, its structural
// equality (§4.5) and its default value (§4.2).
//
// The Go representation of every fixed-size type is its C layout, byte for
// byte: FixedSize bytes, each field at its LayoutFields offset, an ADT's
// payloads overlapping behind its tag.
// * A struct, a variant or a static-capacity limited array that holds no
//   address, whose every field lies at an offset its Go alignment divides,
//   and whose size leaves Go no tail padding to add, is a native Go struct
//   (GR_NATIVE), the pads blank fields: its fields are Go's own, which the
//   Go compiler keeps in registers.
// * Any other is the bytes of its layout, `[FixedSize]byte` (GR_BYTES), a
//   field of it reached at its offset through unsafe.Add, which Go compiles
//   to one unaligned load or store -- one holding references or slices too.
// * A payload-less ADT is its tag, a named integer (GR_TAG); any other
//   fixed-mode ADT is the bytes of its tag and its largest payload
//   (GR_BYTES), the payloads overlapping behind the tag.
// * A reference is a Go pointer (or an unsafe.Pointer, RefIsAddr) and a
//   slice a GsSlice, whose Data is one. Every address the program holds
//   points outside the Go heap and never moves: into a data stack, the
//   fixed-frame stack its address-taken locals live on (gs.Frame,
//   codegen_go_emit.h), a global or a literal. So the bytes of a value can
//   hold one where the collector does not look, and a value holding Go
//   pointers at an address that may be unaligned is reached byte for byte
//   (GoLoc::unal), which checkptr accepts and needs no write barrier.
#pragma once

namespace goose {

// ------------------------------------------------------------------
// Type utilities on concrete (post-typecheck) types, as the C backend's
// (codegen_types.h): what the checker left behind, read and never built.

inline bool CodeGenGo::TEq(TypeExpr *a, TypeExpr *b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    switch (a->kind) {
        case TY_INT:  return a->intstorage == b->intstorage;
        case TY_FLT:  return a->fltstorage == b->fltstorage;
        case TY_BOOL: case TY_VOID: case TY_FN: return true;
        case TY_STRUCT: return SI(a) == SI(b);
        case TY_ENUM:   return EIOf(a) == EIOf(b) && a->enu->varmode == b->enu->varmode;
        case TY_ARRAY: {
            auto &x = *a->arr, &y = *b->arr;
            if (x.akind != y.akind || !TEq(x.sub, y.sub)) return false;
            switch (x.akind) {
                case A_FIXED: case A_LIMITED: return ArrSize(a->arr) == ArrSize(b->arr);
                case A_VAR:     return LenStore(a->arr) == LenStore(b->arr);
                default:        return true;
            }
        }
        case TY_SLICE: return TEq(a->sub, b->sub);
        case TY_REF:
            return TEq(a->ref->sub, b->ref->sub) && a->ref->optional == b->ref->optional &&
                   a->ref->lenstorage == b->ref->lenstorage && a->ref->pool == b->ref->pool;
        case TY_VARIANT:
            return a->var->variant == b->var->variant && TEq(a->var->adt, b->var->adt);
        default: return false;
    }
}

inline StructInst *CodeGenGo::SI(TypeExpr *t) {
    if (t->struc->inst) return t->struc->inst;
    for (auto inst : t->struc->st->insts) {
        if (inst->args.size() != t->struc->args.size()) continue;
        auto ok = true;
        for (size_t i = 0; i < inst->args.size(); i++) ok = ok && TEq(inst->args[i], t->struc->args[i]);
        if (ok) return t->struc->inst = inst;
    }
    Fail(t->line, "internal: a struct type without an instance");
}

inline EnumInst *CodeGenGo::EIOf(TypeExpr *t) {
    if (t->enu->inst) return t->enu->inst;
    for (auto inst : t->enu->en->insts) {
        if (inst->args.size() != t->enu->args.size()) continue;
        auto ok = true;
        for (size_t i = 0; i < inst->args.size(); i++) ok = ok && TEq(inst->args[i], t->enu->args[i]);
        if (ok) return t->enu->inst = inst;
    }
    Fail(t->line, "internal: an enum type without an instance");
}

inline span<const FieldRun> CodeGenGo::FieldRuns(TypeExpr *t) {
    switch (t->kind) {
        case TY_STRUCT: return RunsOf(SI(t));
        case TY_ENUM: return RunsOf(EIOf(t));
        case TY_VARIANT: {
            auto ei = EIVar(t);
            return RunsOf(ei, ei->en->VariantIndex(t->var->variant));
        }
        default: return {};
    }
}

inline SizeClass CodeGenGo::Cls(TypeExpr *t) {
    switch (t->kind) {
        case TY_INT: return t->intstorage == IS_VARINT ? SC_VARIABLE : SC_FIXED;
        case TY_REF: return t->ref->lenstorage == IS_VARINT ? SC_VARIABLE : SC_FIXED;
        case TY_STRUCT: return SI(t)->sclass;
        case TY_ENUM: return t->enu->varmode ? EIOf(t)->varclass : SC_FIXED;
        case TY_ARRAY:
            switch (t->arr->akind) {
                case A_FIXED:   return SC_FIXED;
                case A_VAR:     return SC_VARIABLE;
                case A_LIMITED: return ArrSize(t->arr) >= 0 ? SC_FIXED : SC_VARIABLE;
                default:        return SC_RESIZABLE;
            }
        case TY_VARIANT: {
            auto c = SC_FIXED;
            EachFieldOf(FieldRuns(t), [&](TypeExpr *ft) { c = std::max(c, Cls(ft)); });
            return c;
        }
        default: return SC_FIXED;
    }
}

inline IntStorage CodeGenGo::LenStore(TypeArray *a) {
    if (a->akind == A_VAR) return a->lenstorage < 0 ? IS_U32 : (IntStorage)a->lenstorage;
    auto k = ArrSize(a);
    return k <= 255 ? IS_U8 : k <= 65535 ? IS_U16 : k <= (int64_t)UINT32_MAX ? IS_U32 : IS_U64;
}

// The alignment a bare `pad` gives the next field: its scalar width (§3.2).
inline int64_t CodeGenGo::PadAlign(TypeExpr *t) {
    switch (t->kind) {
        case TY_INT: return t->intstorage == IS_VARINT ? 1 : IntSize(t->intstorage);
        case TY_FLT: return t->fltstorage == FS_F32 ? 4 : 8;
        case TY_REF:
            if (t->ref->lenstorage < 0) return 8;
            return t->ref->lenstorage == IS_VARINT ? 1 : IntSize((IntStorage)t->ref->lenstorage);
        case TY_SLICE: return 8;
        default: return 1;
    }
}

// The packed layout of a run of fields (CodeGen::LayoutFields): each
// field's byte offset, the pads' included, and the total.
inline CodeGenGo::Layout CodeGenGo::LayoutFields(const vector<Field> &fields,
                                                 const vector<TypeExpr *> &ftypes) {
    Layout lo;
    int64_t off = 0;
    for (size_t i = 0; i < fields.size(); i++) {
        auto &f = fields[i];
        lo.offs.push_back(off);
        if (f.ispad) {
            if (f.padsize >= 0) {
                off += f.padsize;
            } else {
                TypeExpr *nt = nullptr;
                for (auto j = i + 1; j < fields.size(); j++)
                    if (!fields[j].ispad) { nt = ftypes[j]; break; }
                auto a = nt ? PadAlign(nt) : 1;
                off = (off + a - 1) / a * a;
            }
            continue;
        }
        off += FixedSize(ftypes[i]);
    }
    lo.size = off;
    return lo;
}

inline const CodeGenGo::Layout &CodeGenGo::StructLayout(StructInst *si) {
    auto key = pair<const void *, int>(si, -1);
    if (auto it = layouts.find(key); it != layouts.end()) return it->second;
    auto lo = LayoutFields(si->st->fields, si->ftypes);
    return layouts[key] = std::move(lo);
}

inline const CodeGenGo::Layout &CodeGenGo::VariantLayout(EnumInst *ei, int vi) {
    auto key = pair<const void *, int>(ei, vi);
    if (auto it = layouts.find(key); it != layouts.end()) return it->second;
    auto lo = LayoutFields(ei->en->variants[vi].fields, ei->vftypes[vi]);
    return layouts[key] = std::move(lo);
}

// A fixed-size type's size in its C layout (CodeGen::FixedSize).
inline int64_t CodeGenGo::FixedSize(TypeExpr *t) {
    switch (t->kind) {
        case TY_INT:  return IntSize(t->intstorage);
        case TY_FLT:  return t->fltstorage == FS_F32 ? 4 : 8;
        case TY_BOOL: return 1;
        case TY_REF:
            if (t->ref->lenstorage >= 0) return IntSize((IntStorage)t->ref->lenstorage);
            return Cls(t->ref->sub) == SC_RESIZABLE ? 16 : 8;
        case TY_SLICE: return 16;
        case TY_STRUCT: {
            auto sz = StructLayout(SI(t)).size;
            return sz ? sz : 1;
        }
        case TY_ENUM: {
            auto ei = EIOf(t);
            int64_t maxp = 0;
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++)
                maxp = std::max(maxp, VariantLayout(ei, (int)vi).size);
            return TagSize(ei->en) + maxp;
        }
        case TY_VARIANT: {
            auto ei = EIVar(t);
            return VariantLayout(ei, ei->en->VariantIndex(t->var->variant)).size;
        }
        case TY_ARRAY:
            if (t->arr->akind == A_FIXED) return ArrSize(t->arr) * FixedSize(t->arr->sub);
            return IntSize(LenStore(t->arr)) + ArrSize(t->arr) * FixedSize(t->arr->sub);
        default: return 0;
    }
}

inline TypeExpr *CodeGenGo::VariantType(TypeExpr *enumtype, int vi) {
    auto ei = EIOf(enumtype);
    auto key = pair<EnumInst *, int>(ei, vi);
    if (auto it = varianttypes.find(key); it != varianttypes.end()) return it->second;
    auto t = ast.VariantTypeOf(enumtype, &ei->en->variants[vi], enumtype->line);
    if (t->var->adt != enumtype) t->var->adt->enu->inst = ei;
    return varianttypes[key] = t;
}

// Whether a fixed type holds a relative reference by value (HasRelRef).
inline bool CodeGenGo::HasRelRef(TypeExpr *t) {
    switch (t->kind) {
        case TY_REF: return t->ref->lenstorage >= 0;
        case TY_STRUCT: case TY_ENUM: case TY_VARIANT:
            if (t->kind == TY_ENUM && t->enu->varmode) return false;
            return AnyFieldOf(FieldRuns(t), [&](TypeExpr *ft) { return HasRelRef(ft); });
        case TY_ARRAY:
            return (t->arr->akind == A_FIXED || t->arr->akind == A_LIMITED) && HasRelRef(t->arr->sub);
        default: return false;
    }
}

// Whether the bytes of two equal values are equal (CodeGen::BitwiseEq): no
// pad, float, ADT or limited array in them, whose bytes equality ignores.
inline bool CodeGenGo::BitwiseEq(TypeExpr *t) {
    switch (t->kind) {
        case TY_STRUCT: case TY_VARIANT:
            for (auto &run : FieldRuns(t))
                for (size_t i = 0; i < run.fields->size(); i++)
                    if ((*run.fields)[i].ispad || !BitwiseEq((*run.ftypes)[i])) return false;
            return true;
        case TY_ENUM: return false;
        case TY_ARRAY: return t->arr->akind == A_FIXED && BitwiseEq(t->arr->sub);
        case TY_FLT: return false;
        default: return true;
    }
}

// ------------------------------------------------------------------
// Large fixed-size values. The C backend holds a fixed-size value of more
// than NATIVE_VALUE_LIMIT bytes on a data stack wherever it is a local or a
// temporary, outside a recursive cycle (CodeGen::FixedLocal), and passes
// and returns one through such a copy. Those stacks are part of the counts
// both backends report and reserve (--stacks), so this version keeps such a
// value where it takes the stacks the C backend does: in a global, which is
// filled in place; in the locals of a recursive cycle, which take none; and
// on a data stack where C takes one too (FixedLocal): a named local, a
// literal's or a default's temporary, a queue's value; a thread_fn's
// parameter is the worker's copy in its spawn packet. Anything else is not
// supported yet.

inline bool CodeGenGo::IsLargeFixed(TypeExpr *t) {
    return t && t->kind != TY_VOID && t->kind != TY_FN && t->kind != TY_GENERIC && IsFix(t) &&
           FixedSize(t) > NATIVE_VALUE_LIMIT;
}

// A local or temporary of fixed-size type t named `name`, with an initial
// value where init is not empty (CodeGen::FixedLocal): a Go variable, or one
// of more than NATIVE_VALUE_LIMIT bytes outside a recursive cycle a slot on
// a data stack, which the innermost scope (the block's, for a local) gives
// back, and `name` points at. Without an initial value it holds T's zero
// value, as a Go variable does, but where `zero` is false, for a slot that
// is written whole next. The lvalue it is: name, or (*name).
inline string CodeGenGo::FixedLocal(TypeExpr *t, const string &name, const string &init, bool forlocal,
                                    Line ln, bool zero) {
    if (!IsLargeFixed(t) || (curspec && curspec->incycle)) {
        Decl(name, LocalT(t, ln), init);
        return name;
    }
    auto gt = GoT(t, ln);
    auto stk = AllocStk(forlocal);
    Decl(name, cat("*", gt), cat("(*", gt, ")(", Top(stk), ")"));
    SaveBase(forlocal, stk, cat("unsafe.Pointer(", name, ")"));
    Bump(stk, cat(FixedSize(t)));
    auto lv = cat("(*", name, ")");
    if (!init.empty()) L(lv, " = ", init);
    else if (zero) L("clear(unsafe.Slice((*byte)(unsafe.Pointer(", name, ")), ", FixedSize(t), "))");
    return lv;
}

// The Go type of a local or a temporary of type t.
inline string CodeGenGo::LocalT(TypeExpr *t, Line ln) {
    if (IsLargeFixed(t) && !(curspec && curspec->incycle))
        Unsupported(ln, cat("a local or temporary value of ", TypeText(t), " (", FixedSize(t),
                            " bytes: C holds one of more than ", NATIVE_VALUE_LIMIT,
                            " on a data stack)"));
    auto gt = GoT(t, ln);
    if (IsLargeFixed(t)) largegts.insert(gt);
    return gt;
}

// Whether the C backend makes a global of this initializer static data
// (CodeGen::StaticInitX), which its initializers then never build.
inline bool CodeGenGo::CStaticInit(Node *n, TypeExpr *t) {
    if (!n || !t || !IsFix(t) || HasRelRef(t)) return false;
    if (auto id = Is<Ident>(n)) {
        auto it = id->vdef ? cstatic.find(id->vdef) : cstatic.end();
        if (it == cstatic.end() ||
            !(TEq(id->vdef->type, t) || (t->kind == TY_INT && id->vdef->type->kind == TY_INT)))
            return false;
        return CStaticInit(it->second, t);
    }
    switch (t->kind) {
        case TY_INT: return Is<IntLit>(n) && t->intstorage != IS_VARINT;
        case TY_FLT: return Is<FltLit>(n) != nullptr;
        case TY_BOOL: return Is<BoolLit>(n) != nullptr;
        case TY_STRUCT: {
            auto sl = Is<StructLit>(n);
            auto si = SI(t);
            if (!sl || sl->sinst != si) return false;
            for (size_t fi = 0; fi < si->st->fields.size(); fi++) {
                if (si->st->fields[fi].ispad) continue;
                if (!CStaticInit(sl->InitFor((int)fi), si->ftypes[fi])) return false;
            }
            return true;
        }
        case TY_ARRAY: {
            auto &a = *t->arr;
            if (a.akind != A_FIXED) return false;
            auto k = ArrSize(t->arr);
            if (k < 1) return false;
            if (auto str = Is<StrLit>(n))
                return k <= 256 && IsU8(a.sub) && (int64_t)str->val.size() == k;
            auto al = Is<ArrayLit>(n);
            if (!al) return false;
            if (al->fillval) return k <= 256 && CStaticInit(al->fillval, a.sub);
            if ((int64_t)al->elems.size() != k) return false;
            for (auto e : al->elems)
                if (!CStaticInit(e, a.sub)) return false;
            return true;
        }
        default: return false;
    }
}

// ------------------------------------------------------------------
// Mangled type identities, as the C backend's (CodeGen::Mangle): the key of
// every per-type artifact, and a named Go type's name.

inline string CodeGenGo::Mangle(TypeExpr *t) {
    auto raw = RawMangle(t);
    auto &owners = mangleowners[raw];
    for (auto &[o, m] : owners)
        if (SameMangle(o, t)) return m;
    auto m = raw;
    for (auto n = 1; mangles.count(m); n++) m = cat(raw, "_m", n);
    mangles.insert(m);
    owners.push_back({ t, m });
    return m;
}

inline bool CodeGenGo::SameMangle(TypeExpr *a, TypeExpr *b) {
    if (a == b) return true;
    if (a->kind != b->kind) return false;
    auto sameargs = [&](const vector<TypeExpr *> &x, const vector<TypeExpr *> &y) {
        if (x.size() != y.size()) return false;
        for (size_t i = 0; i < x.size(); i++)
            if (!SameMangle(x[i], y[i])) return false;
        return true;
    };
    switch (a->kind) {
        case TY_INT: return a->intstorage == b->intstorage && !a->ix == !b->ix;
        case TY_STRUCT:
            return a->struc->st == b->struc->st && sameargs(a->struc->args, b->struc->args);
        case TY_ENUM:
            return a->enu->en == b->enu->en && a->enu->varmode == b->enu->varmode &&
                   sameargs(a->enu->args, b->enu->args);
        case TY_VARIANT:
            return a->var->variant == b->var->variant && SameMangle(a->var->adt, b->var->adt);
        case TY_ARRAY: {
            auto &x = *a->arr, &y = *b->arr;
            if (x.akind != y.akind || !SameMangle(x.sub, y.sub)) return false;
            switch (x.akind) {
                case A_FIXED: case A_LIMITED: return ArrSize(a->arr) == ArrSize(b->arr);
                case A_VAR:                   return LenStore(a->arr) == LenStore(b->arr);
                default:                      return true;
            }
        }
        case TY_SLICE: return SameMangle(a->sub, b->sub);
        case TY_REF:
            return a->ref->optional == b->ref->optional && a->ref->lenstorage == b->ref->lenstorage &&
                   a->ref->pool == b->ref->pool && SameMangle(a->ref->sub, b->ref->sub);
        default: return TEq(a, b);
    }
}

inline string CodeGenGo::RawMangle(TypeExpr *t) {
    switch (t->kind) {
        case TY_INT: return t->intstorage == IS_VARINT ? "vi" : IntStorageName(t->intstorage);
        case TY_FLT: return t->fltstorage == FS_F32 ? "f32" : "f64";
        case TY_BOOL: return "b";
        case TY_STRUCT: {
            auto s = Sanitize(t->struc->st->ns, t->struc->st->name);
            for (auto a : t->struc->args) Append(s, "_", Mangle(a));
            return s;
        }
        case TY_ENUM: {
            auto s = Sanitize(t->enu->en->ns, t->enu->en->name);
            for (auto a : t->enu->args) Append(s, "_", Mangle(a));
            if (t->enu->varmode) s += "_vm";
            return s;
        }
        case TY_VARIANT: return cat(Mangle(t->var->adt), "_", t->var->variant->name, "_p");
        case TY_ARRAY: {
            auto &a = *t->arr;
            switch (a.akind) {
                case A_FIXED:   return cat("a", ArrSize(t->arr), "_", Mangle(a.sub));
                case A_VAR:     return cat("v", IntStorageName(LenStore(t->arr)), "_", Mangle(a.sub));
                case A_LIMITED: return ArrSize(t->arr) >= 0 ? cat("l", ArrSize(t->arr), "_", Mangle(a.sub))
                                                            : cat("lv_", Mangle(a.sub));
                case A_GROW:    return cat("gw_", Mangle(a.sub));
                default:        return cat("gx_", Mangle(a.sub));
            }
        }
        case TY_SLICE: return cat("sl_", Mangle(t->sub));
        case TY_REF: {
            auto &r = *t->ref;
            string s = r.optional ? "o" : "r";
            if (r.lenstorage >= 0) Append(s, "l", IntStorageName(r.lenstorage));
            Append(s, "_", Mangle(r.sub));
            return s;
        }
        default: return "void";
    }
}

// ------------------------------------------------------------------
// Go types.

// Whether t is a named Go type of its own: a struct, an ADT, a variant, a
// static-capacity limited array.
inline bool CodeGenGo::NamedGoT(TypeExpr *t) {
    return t->kind == TY_STRUCT || t->kind == TY_ENUM || t->kind == TY_VARIANT ||
           (t->kind == TY_ARRAY && t->arr->akind == A_LIMITED);
}

// What this version represents: every fixed-size type but a varint
// relative reference and a reference to a value that is not fixed-size, and
// an `in pool` reference, whose pool is a resizable.
inline void CodeGenGo::CheckRepresentable(TypeExpr *t, Line ln) {
    switch (t->kind) {
        case TY_INT:
            // An index or bound type is its integer, checked against its
            // array where a value converts to it (GenCast); a varint is
            // storage in a bytes value (§3.6).
            return;
        case TY_FLT: case TY_BOOL: return;
        case TY_REF: {
            auto &r = *t->ref;
            // An `in pool` one measures from its pool's base, which is a
            // resizable global's (§3.9).
            if (r.pool && (!r.pool->isglobal || !IsResz(r.pool->type)))
                Unsupported(ln, cat("an `in pool` relative reference (", TypeText(t), ")"));
            // A fat reference to a resizable value (GsRref), a reference to
            // a bytes value (its address); what they lead to is checked where
            // a value of it is reached.
            return;
        }
        case TY_SLICE: return;
        case TY_ARRAY: case TY_STRUCT: case TY_ENUM: case TY_VARIANT: {
            if (Cls(t) == SC_FIXED && !(t->kind == TY_ENUM && t->enu->varmode)) {
                if (t->kind == TY_ARRAY && t->arr->akind != A_FIXED && t->arr->akind != A_LIMITED)
                    Unsupported(ln, cat("a value of type ", TypeText(t)));
                return;
            }
            // A value that is not fixed-size: a resizable's header (a frame
            // object's struct), or a bytes value's image on a data stack
            // (codegen_go_rz.h, codegen_go_bytes.h), whose parts are what
            // this version holds. Each nominal type is checked once.
            if (t->kind != TY_ARRAY && !repchecked.insert(Mangle(t)).second) return;
            if (t->kind == TY_ARRAY) {
                CheckRepresentable(t->arr->sub, ln);
                return;
            }
            EachFieldOf(FieldRuns(t), [&](TypeExpr *ft) { CheckRepresentable(ft, ln); });
            return;
        }
        case TY_FN: Unsupported(ln, "a function value outside a call");
        default: Fail(ln, cat("internal: no Go type for ", TypeText(t)));
    }
}

// The Go type of a fixed-size type: a name for a named type, which its
// first sight gives it, and queues its declaration for (EmitTypeDecls).
inline string CodeGenGo::GoT(TypeExpr *t, Line ln) {
    if (!t) Fail(ln, "internal: a value without a type");
    CheckRepresentable(t, ln);
    switch (t->kind) {
        case TY_INT: return IntGoT(t->intstorage);
        case TY_FLT: return t->fltstorage == FS_F32 ? "float32" : "float64";
        case TY_BOOL: return "bool";
        case TY_REF:
            if (t->ref->lenstorage == IS_VARINT) return "unsafe.Pointer";
            if (t->ref->lenstorage >= 0) return RelGoT(t);
            if (IsResz(t->ref->sub)) return "GsRref";
            // A root owner's descriptor is a Go variable's own, a local's or
            // a global's, which no value lies unaligned in: a typed pointer.
            if (IsRoot(t->ref->sub)) return cat("*", GoT(t->ref->sub, ln));
            // A reference to a bytes value is its address, as one to a value
            // whose Go type holds Go pointers is (RefIsAddr).
            if (IsBytesT(t->ref->sub) || GoHasPtrs(t->ref->sub)) {
                GoT(t->ref->sub, ln);
                return "unsafe.Pointer";
            }
            return cat("*", GoT(t->ref->sub, ln));
        case TY_SLICE: return "GsSlice";
        case TY_ARRAY:
            if (t->arr->akind == A_FIXED) return cat("[", ArrSize(t->arr), "]", GoT(t->arr->sub, ln));
            if (t->arr->akind == A_GROW || t->arr->akind == A_GROWSHRINK) return "GsRhdr";
            break;
        default: break;
    }
    // A bytes value is the address of its image; a resizable that is no
    // frame object, its header (C.2).
    if (IsBytesT(t) && !IsFrameObj(t)) return IsResz(t) ? "GsRhdr" : "unsafe.Pointer";
    auto m = Mangle(t);
    if (auto it = gonames.find(m); it != gonames.end()) return it->second;
    auto name = Unique(m);
    gonames[m] = name;
    pendingtypes.push_back({ t, ln });
    return name;
}

inline bool CodeGenGo::GoHasPtrs(TypeExpr *t) {
    switch (t->kind) {
        case TY_REF: return t->ref->lenstorage < 0;
        case TY_SLICE: return true;
        case TY_ARRAY:
            switch (t->arr->akind) {
                case A_FIXED: return ArrSize(t->arr) > 0 && GoHasPtrs(t->arr->sub);
                case A_GROW: case A_GROWSHRINK: return true;
                default: return false;
            }
        case TY_STRUCT: return IsFrameObj(t) || IsRoot(t);
        default: return false;
    }
}

inline bool CodeGenGo::RefIsAddr(TypeExpr *rt) {
    if (rt->kind != TY_REF || rt->ref->lenstorage >= 0 || IsResz(rt->ref->sub) || IsRoot(rt->ref->sub))
        return false;
    return IsBytesT(rt->ref->sub) || GoHasPtrs(rt->ref->sub);
}

// A reference of type rt to the value at addr, an unsafe.Pointer.
inline string CodeGenGo::RefFromAddr(TypeExpr *rt, const string &addr) {
    if (RefIsAddr(rt)) return addr;
    return cat("(*", GoT(rt->ref->sub, rt->line), ")(", addr, ")");
}

// The address the reference x of type rt holds, an unsafe.Pointer.
inline string CodeGenGo::AddrOfRef(TypeExpr *rt, const string &x) {
    if (RefIsAddr(rt)) return x;
    return cat("unsafe.Pointer(", x, ")");
}

// The integer a relative reference is stored as (§3.9): signed, measured
// both ways from its own slot, or for the `in pool` form unsigned, measured
// up from its pool's base.
inline const char *CodeGenGo::RelGoT(TypeExpr *rt) {
    auto pool = rt->ref->pool != nullptr;
    switch (IntSize((IntStorage)rt->ref->lenstorage)) {
        case 1: return pool ? "uint8" : "int8";
        case 2: return pool ? "uint16" : "int16";
        case 4: return pool ? "uint32" : "int32";
        default: return pool ? "uint64" : "int64";
    }
}

// What is known of a fixed-size type's Go representation, worked out (and
// its declaration emitted) on first use.
inline const CodeGenGo::GoTypeInfo &CodeGenGo::TI(TypeExpr *t, Line ln) {
    auto gt = GoT(t, ln);
    auto m = Mangle(t);
    if (auto it = typeinfo.find(m); it != typeinfo.end()) {
        if (it->second.building) Fail(ln, cat("internal: ", TypeText(t), " contains itself"));
        return it->second;
    }
    GoTypeInfo ti;
    ti.name = gt;
    // A bytes value as a Go value is its address (a varint, its decoded
    // i64), a resizable that is no frame object its header.
    if (IsBytesT(t) && !IsFrameObj(t)) {
        ti.size = gt == "GsRhdr" ? 16 : 8;
        ti.align = 8;
        ti.ptrs = ti.addrs = gt != "int64";
        ti.exact = false;
        ti.goeq = false;
        return typeinfo[m] = ti;
    }
    switch (t->kind) {
        case TY_INT:
            ti.size = ti.align = IntSize(t->intstorage);
            return typeinfo[m] = ti;
        case TY_FLT:
            ti.size = ti.align = t->fltstorage == FS_F32 ? 4 : 8;
            return typeinfo[m] = ti;
        case TY_BOOL:
            ti.size = ti.align = 1;
            return typeinfo[m] = ti;
        case TY_REF:
            if (t->ref->lenstorage >= 0) {
                ti.size = ti.align = IntSize((IntStorage)t->ref->lenstorage);
            } else if (IsResz(t->ref->sub)) {
                // A GsRref: the header's address and the stack.
                ti.size = 16;
                ti.align = 8;
                ti.ptrs = ti.addrs = true;
            } else {
                ti.size = ti.align = 8;
                ti.ptrs = ti.addrs = true;
            }
            return typeinfo[m] = ti;
        case TY_SLICE:
            ti.size = 16;
            ti.align = 8;
            ti.ptrs = ti.addrs = true;
            return typeinfo[m] = ti;
        case TY_ARRAY:
            if (t->arr->akind == A_FIXED) {
                auto &e = TI(t->arr->sub, ln);
                auto k = ArrSize(t->arr);
                ti.size = k * e.size;
                ti.align = e.align;
                ti.ptrs = k > 0 && e.ptrs;
                ti.addrs = k > 0 && e.addrs;
                ti.exact = e.exact;
                ti.goeq = e.goeq;
                return typeinfo[m] = ti;
            }
            if (t->arr->akind == A_GROW || t->arr->akind == A_GROWSHRINK) {
                // Its header (GsRhdr).
                ti.size = 16;
                ti.align = 8;
                ti.ptrs = ti.addrs = true;
                ti.goeq = false;
                return typeinfo[m] = ti;
            }
            break;
        default: break;
    }
    typeinfo[m].building = true;
    string decl;
    switch (t->kind) {
        case TY_STRUCT: {
            auto si = SI(t);
            if (IsFrameObj(t)) {
                BuildFrameObj(ti, decl, t, ln);
                break;
            }
            if (IsRoot(t)) {
                BuildRootStruct(ti, decl, t, ln);
                break;
            }
            BuildStructLike(ti, decl, si->st->fields, si->ftypes, StructLayout(si),
                            EmptyLayout(si->st->fields) ? 1 : StructLayout(si).size, BitwiseEq(t), ln);
            break;
        }
        case TY_VARIANT: {
            auto ei = EIVar(t);
            auto vi = ei->en->VariantIndex(t->var->variant);
            auto &lo = VariantLayout(ei, vi);
            BuildStructLike(ti, decl, ei->en->variants[vi].fields, ei->vftypes[vi], lo, lo.size,
                            BitwiseEq(t), ln);
            break;
        }
        case TY_ENUM: BuildEnum(ti, decl, t, ln); break;
        default: BuildLimited(ti, decl, t, ln); break;
    }
    if (IsFix(t))
        Append(tdecls, "// ", TypeText(t), ": ", FixedSize(t), " bytes",
               ti.exact ? "" : cat(" in its C layout, ", ti.size, " in this one"), ".\n", "type ",
               gt, " ", decl, "\n\n");
    else if (IsRoot(t))
        Append(tdecls, "// ", TypeText(t), ": a root owner's descriptor, a reference to each of its\n",
               "// components, which lie apart.\ntype ", gt, " ", decl, "\n\n");
    else
        Append(tdecls, "// ", TypeText(t), ": a frame object, its fixed fields and its tail's header\n",
               "// (the tail's elements on a data stack).\ntype ", gt, " ", decl, "\n\n");
    ti.building = false;
    return typeinfo[m] = ti;
}

// A struct's or a variant's representation (see the top of this file):
// `fields` laid out by `lo`, `csize` bytes in all.
inline void CodeGenGo::BuildStructLike(GoTypeInfo &ti, string &decl, const vector<Field> &fields,
                                       const vector<TypeExpr *> &ftypes, const Layout &lo,
                                       int64_t csize, bool bitwise, Line ln) {
    auto exact = true, aligned = true, addrs = false, goeq = true, empty = false;
    int64_t maxalign = 1;
    auto real = 0;
    for (size_t i = 0; i < fields.size(); i++) {
        if (fields[i].ispad) continue;
        auto &f = TI(ftypes[i], ln);
        real++;
        exact = exact && f.exact && f.size == FixedSize(ftypes[i]);
        aligned = aligned && lo.offs[i] % f.align == 0;
        addrs = addrs || f.addrs;
        goeq = goeq && f.goeq;
        // A field of no bytes would get Go's padding at the end of a struct.
        empty = empty || f.size == 0;
        maxalign = std::max(maxalign, f.align);
    }
    ti.addrs = addrs;
    ti.offs.assign(fields.size(), 0);
    if (!real && csize <= 1) {
        // No bytes of its own: a struct's one byte C gives it (`gs_empty`),
        // or a variant's none.
        ti.rep = GR_BYTES;
        ti.size = csize;
        decl = cat("[", csize, "]byte");
        return;
    }
    if (exact && aligned && csize % maxalign == 0 && !addrs && !empty) {
        // Go lays the fields out where C does: a native struct, its gaps
        // (the pads) blank fields that Go's == leaves out.
        ti.rep = GR_NATIVE;
        ti.size = csize;
        ti.align = maxalign;
        ti.goeq = goeq;
        decl = "struct {\n";
        int64_t cur = 0;
        for (size_t i = 0; i < fields.size(); i++) {
            ti.offs[i] = lo.offs[i];
            if (fields[i].ispad) continue;
            if (lo.offs[i] > cur) Append(decl, "\t_ [", lo.offs[i] - cur, "]byte\n");
            Append(decl, "\t", Sanitize(fields[i].name), " ", TI(ftypes[i], ln).name, "\n");
            cur = lo.offs[i] + FixedSize(ftypes[i]);
        }
        if (csize > cur) Append(decl, "\t_ [", csize - cur, "]byte\n");
        decl += "}";
        return;
    }
    // The bytes of the C layout, each field at its own offset: references
    // and slices among them too, where C packs them unaligned or not.
    ti.rep = GR_BYTES;
    ti.size = csize;
    ti.goeq = bitwise;
    for (size_t i = 0; i < fields.size(); i++) ti.offs[i] = lo.offs[i];
    decl = cat("[", csize, "]byte");
}

// A frame object (C.2): the Go struct of its fixed fields, its tail's
// header (or nested frame object) last, in Go's own layout, which nothing
// the program can observe depends on.
inline void CodeGenGo::BuildFrameObj(GoTypeInfo &ti, string &decl, TypeExpr *t, Line ln) {
    auto si = SI(t);
    auto &fields = si->st->fields;
    ti.rep = GR_NATIVE;
    ti.exact = false;
    ti.ptrs = true;
    ti.goeq = false;
    ti.offs.assign(fields.size(), 0);
    decl = "struct {\n";
    int64_t off = 0, maxalign = 8;
    for (size_t i = 0; i < fields.size(); i++) {
        if (fields[i].ispad) {
            ti.offs[i] = off;
            if (fields[i].padsize > 0) {
                Append(decl, "\t_ [", fields[i].padsize, "]byte\n");
                off += fields[i].padsize;
            }
            continue;
        }
        auto &f = TI(si->ftypes[i], ln);
        off = (off + f.align - 1) / f.align * f.align;
        ti.offs[i] = off;
        Append(decl, "\t", Sanitize(fields[i].name), " ", f.name, "\n");
        off += f.size;
        maxalign = std::max(maxalign, f.align);
    }
    ti.align = maxalign;
    ti.size = (off + maxalign - 1) / maxalign * maxalign;
    decl += "}";
}

// A root owner's descriptor (CodeGen::CT of a root struct): a reference to
// each of its fields, which are its components' own places, each where a
// variable of its type would be -- a resizable's GsRref, a bytes value's
// address, a pointer to a fixed-size value -- in Go's own layout, which
// nothing the program can observe depends on. The descriptor is no value:
// never copied, compared or serialized, it is always a variable's own, a
// local's or a global's, and a reference to the owner is a pointer to it.
inline void CodeGenGo::BuildRootStruct(GoTypeInfo &ti, string &decl, TypeExpr *t, Line ln) {
    auto si = SI(t);
    auto &fields = si->st->fields;
    ti.rep = GR_NATIVE;
    ti.exact = false;
    ti.ptrs = ti.addrs = true;
    ti.goeq = false;
    ti.offs.assign(fields.size(), 0);
    decl = "struct {\n";
    int64_t off = 0;
    for (size_t i = 0; i < fields.size(); i++) {
        if (fields[i].ispad || !si->ftypes[i]) continue;
        auto &f = TI(ast.RefTo(si->ftypes[i], t->line), ln);
        ti.offs[i] = off;
        Append(decl, "\t", Sanitize(fields[i].name), " ", f.name, "\n");
        off += f.size;
    }
    ti.align = 8;
    ti.size = off;
    decl += "}";
}

// A fixed-mode ADT's representation: its tag, then the largest payload's
// bytes (or, holding Go pointers, each payload in a field of its own).
inline void CodeGenGo::BuildEnum(GoTypeInfo &ti, string &decl, TypeExpr *t, Line ln) {
    auto ei = EIOf(t);
    auto ts = TagSize(ei->en);
    auto tagt = IntGoT(TagStore(ei->en));
    auto nv = ei->en->variants.size();
    auto allempty = true, addrs = false;
    for (size_t vi = 0; vi < nv; vi++) {
        if (EmptyLayout(ei->en->variants[vi].fields)) continue;
        allempty = false;
        addrs = addrs || TI(VariantType(t, (int)vi), ln).addrs;
    }
    ti.voffs.assign(nv, -1);
    if (allempty) {
        // Its tag and nothing else: a named integer, which Go compares.
        ti.rep = GR_TAG;
        ti.size = ti.align = ts;
        decl = tagt;
        return;
    }
    // The tag, then the payloads overlapping behind it, as C's union.
    ti.rep = GR_BYTES;
    ti.size = FixedSize(t);
    ti.goeq = false;
    ti.addrs = addrs;
    for (size_t vi = 0; vi < nv; vi++)
        if (!EmptyLayout(ei->en->variants[vi].fields)) ti.voffs[vi] = ts;
    decl = cat("[", ti.size, "]byte");
}

// A static-capacity limited array (§3.3): its length, then its slots.
inline void CodeGenGo::BuildLimited(GoTypeInfo &ti, string &decl, TypeExpr *t, Line ln) {
    auto k = ArrSize(t->arr);
    auto lw = IntSize(LenStore(t->arr));
    auto lt = IntGoT(LenStore(t->arr));
    auto csize = FixedSize(t);
    ti.goeq = false;
    ti.elemoff = lw;
    if (k == 0) {
        ti.rep = GR_BYTES;
        ti.size = csize;
        decl = cat("[", csize, "]byte");
        return;
    }
    auto &e = TI(t->arr->sub, ln);
    auto ma = std::max(lw, e.align);
    ti.addrs = e.addrs;
    if (e.exact && e.size == FixedSize(t->arr->sub) && lw % e.align == 0 && csize % ma == 0 && !e.addrs) {
        ti.rep = GR_NATIVE;
        ti.size = csize;
        ti.align = ma;
        decl = cat("struct {\n\tlen ", lt, "\n\te   [", k, "]", e.name, "\n}");
        return;
    }
    ti.rep = GR_BYTES;
    ti.size = csize;
    decl = cat("[", csize, "]byte");
}

// The declarations of every named type the program's Go names, whatever
// named them: a reference needs only the name, so a node type can hold
// references to itself, and the body follows here.
inline void CodeGenGo::EmitTypeDecls() {
    for (size_t i = 0; i < pendingtypes.size(); i++) TI(pendingtypes[i].first, pendingtypes[i].second);
}

// ------------------------------------------------------------------
// Structural equality (§4.5) of two values at Go lvalues: Go's == where it
// is that (GoTypeInfo::goeq), a function per type otherwise, which compares
// a limited array's live elements and an ADT's live payload only, and a
// float by its value. A reference or a slice compares as itself here, as
// a member does; a slice at the top of an == compares its elements
// (GenSliceEq).

inline string CodeGenGo::EqX(TypeExpr *t, const GoLoc &a, const GoLoc &b, Line ln) {
    if (IsBytesT(t) && !IsResz(t)) return cat(EqFnBytes(t, ln), "(", a.addr, ", ", b.addr, ")");
    auto &ti = TI(t, ln);
    if (ti.goeq) return cat("(", Ld(a), " == ", Ld(b), ")");
    return cat(EqFn(t, ln), "(", PtrOf(a), ", ", PtrOf(b), ")");
}

inline string CodeGenGo::EqFn(TypeExpr *t, Line ln) {
    auto m = Mangle(t);
    if (auto it = eqfns.find(m); it != eqfns.end()) return it->second;
    auto name = Unique(cat("gsEq_", m));
    eqfns[m] = name;
    auto gt = TI(t, ln).name;
    // Generated on its own, as a function of its own; the function being
    // emitted resumes afterwards.
    auto save = SaveFnState();
    string b;
    auto A = AtTyped("a", t), B = AtTyped("b", t);
    auto line = [&](const string &s) { Append(b, "\t", s, "\n"); };
    switch (t->kind) {
        case TY_STRUCT: case TY_VARIANT: {
            for (auto &run : FieldRuns(t))
                for (size_t i = 0; i < run.fields->size(); i++) {
                    if ((*run.fields)[i].ispad) continue;
                    auto ft = (*run.ftypes)[i];
                    line(cat("if !", EqX(ft, FieldLocAt(A, (int)i, ln), FieldLocAt(B, (int)i, ln), ln),
                             " {\n\t\treturn false\n\t}"));
                }
            line("return true");
            break;
        }
        case TY_ENUM: {
            auto ei = EIOf(t);
            line(cat("if ", TagOf(A, ln), " != ", TagOf(B, ln), " {\n\t\treturn false\n\t}"));
            for (size_t vi = 0; vi < ei->en->variants.size(); vi++) {
                if (EmptyLayout(ei->en->variants[vi].fields)) continue;
                auto vt = VariantType(t, (int)vi);
                line(cat("if ", TagOf(A, ln), " == ", vi, " {\n\t\treturn ",
                         EqX(vt, PayloadLoc(A, (int)vi, ln), PayloadLoc(B, (int)vi, ln), ln), "\n\t}"));
            }
            line("return true");
            break;
        }
        case TY_ARRAY: {
            auto va = ArrayView(A, ln), vb = ArrayView(B, ln);
            if (t->arr->akind == A_LIMITED) line(cat("if ", va.len, " != ", vb.len, " {\n\t\treturn false\n\t}"));
            line(cat("for i := int64(0); i < ", va.len, "; i++ {"));
            line(cat("\tif !", EqX(t->arr->sub, ElemAt(va, "i", ln), ElemAt(vb, "i", ln), ln),
                     " {\n\t\t\treturn false\n\t\t}"));
            line("}");
            line("return true");
            break;
        }
        default: Fail(ln, cat("internal: an equality function for ", TypeText(t)));
    }
    RestoreFnState(save);
    Append(eqcode, "// == on ", TypeText(t), " (§4.5).\nfunc ", name, "(a, b *", gt, ") bool {\n", b,
           "}\n\n");
    return name;
}

}  // namespace goose
