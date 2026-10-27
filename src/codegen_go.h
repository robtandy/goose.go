// Goose compiler — Go code generation (--target=go). Consumes the same
// checked, optimized, bounds-checked specializations the C backend does
// (codegen.h) and emits one Go source file that builds with nothing but the
// Go standard library: the runtime prologue (codegen_go_runtime.h), then the
// program.
//
// Representation (DESIGN.md §3):
// * Scalars are Go's own types: i8..i64 int8..int64, u8..u64 uint8..uint64,
//   f32/f64 float32/float64, bool bool. Go's +, - and * wrap at every width
//   as a release build's do (§6.2); division goes through helpers that check
//   for zero, and `%` is Euclidean. A debug build (--debug, -DGS_DEBUG=1)
//   checks signed arithmetic and `as` conversions as the C runtime does.
//   Every float product is rounded on its own (`float64(a * b)`), which keeps
//   Go from fusing it with an add into one FMA rounding, as the C runtime
//   keeps its C compiler from doing.
// * Data stacks are regions of a GsStackBlock: a function that uses one, or
//   calls one that does, takes the block and its first stack index as its
//   leading parameters, `(gs *GsStackBlock, gs_sp int64, ...)`, the C
//   backend's hidden gs_sp (§6.2), and addresses its own as gs.Stks[gs_sp +
//   k]. The counts are the C backend's analysis (BoundStacks,
//   codegen_stacks.h) over the same accounting, so --stacks reports the
//   same numbers for both targets.
// * Names from the program carry the C backend's _g suffix (ns::x is
//   ns_x_g<len>), which keeps them clear of Go's keywords and predeclared
//   names and of the runtime's gs and Gs names; locals and temporaries are
//   unique within their function.
// * Control flow is Go's own, with no goto: a Goose loop is a labeled `for`,
//   a `block` and an inlined call's body a labeled `switch { default: }`
//   that a break or a return leaves, and every break and continue names its
//   label. Scope exits run the `defer` bodies of the scopes they leave, as
//   the C backend's do (§6.6).
// * Go rejects what C accepts silently: unused locals, constant expressions
//   that overflow their type, code after a return. Constants that would meet
//   in an operation are held in variables first (Materialize), a local whose
//   value nothing reads is marked used (FinishBody), and code is emitted only
//   where Go's own flow analysis can reach it (`dead`).
//
// * Fixed-size values (§3.2-§3.10) keep their C layout wherever Go can hold
//   them there (codegen_go_types.h): a struct whose fields Go aligns where C
//   packs them is a native Go struct, one that is packed tighter is the
//   bytes of its layout, `[FixedSize]byte`, its fields reached through
//   unsafe.Add; a payload-less ADT is its tag, any other the bytes of its tag
//   and its largest payload; fixed arrays are Go arrays, a static-capacity
//   limited array its length and slots. A reference is a Go pointer and a
//   slice a GsSlice, so that the Go collector sees every address into memory
//   it manages. A place -- a variable, field, element or pointee -- is a Go
//   lvalue (GoLoc, codegen_go_places.h).
//
// * The foreign boundary (codegen_go_export.h, DESIGN.md §3.5): an extern
//   fn calls the Go runtime's function its C symbol names (std's, math's
//   and os's), Go's math, or a Go function of the package that an
//   --include file defines; an export fn gets two exported Go entry points,
//   NameWithStack(gs, ...) and Name(...), the second on a block from the
//   runtime's pool, both taking Go's own slices, strings and pointers.
//
// This version covers the single-threaded language: scalars and their
// operators and conversions, fixed-size values, the values on data stacks
// (codegen_go_rz.h, codegen_go_bytes.h), control flow, calls, extern and
// export fns. Anything else -- threads, among them -- stops the compile
// with an error naming the construct (Unsupported) rather than emitting Go
// that would be wrong.
//
// The members are declared here in the order they are defined across
// codegen_go_values.h, codegen_go_types.h, codegen_go_places.h,
// codegen_go_aggr.h, codegen_go_rz.h, codegen_go_stmts.h,
// codegen_go_calls.h, codegen_go_export.h and codegen_go_emit.h;
// codegen_go_rz.h is included after codegen_go_calls.h, whose helpers it
// uses.
#pragma once

namespace goose {

// Where a value goes: nowhere (evaluated for effect), into the Go lvalue
// `s`, whose type `t` is, where the receiver knows it, or built at the top of
// the data stack `s` (an int64 index into gs.Stks), as the C backend's
// DK_STACK (codegen.h): a resizable value's elements go there and its count
// (or a frame object, whole) to `lenlv`. A `pool` lvalue is a GsPref.
enum GoDstKind { GD_DISCARD, GD_LVALUE, GD_STACK };
struct GoDst {
    GoDstKind k = GD_DISCARD;
    string s;
    TypeExpr *t = nullptr;
    string lenlv;
    bool pool = false;
};

// An --include file's Go split into its imports and the rest
// (codegen_go_export.h).
static bool GoSplitInclude(const string &text, vector<pair<string, string>> &imports, string &body,
                           string &why);

// What the driver hands the Go backend.
struct GoOptions {
    string package = "main";   // --go-package.
    bool library = false;      // A package without func main (--library).
    bool debug = false;        // --debug, or -DGS_DEBUG=1.
    bool stackstats = false;   // -DGS_STACK_STATS=1: report the stacks taken as main ends.
    CodeGen::StackConfig stacks { 0, 0, 0, 0 };   // As main.cpp's StackConfigOf reads them.
    // The --include files, Go source spliced into the file (codegen_go_export.h):
    // the name its //line directive gives each, and its text.
    struct Include {
        string name, text;
    };
    vector<Include> includes;
    // os.resource_dir's answer where it is not the executable's directory: the
    // entry source's, for goose run, as a JIT run has it.
    string resourcedir;
};

struct CodeGenGo {
    Ast &ast;
    GoOptions opts;
    bool debug = false;

    // Output sections, which Source() assembles after the header and the
    // runtime prologue.
    string filedecls;   // The source file names the aborts report.
    string tdecls;      // The named types of fixed-size values.
    string gdecls;      // Package-level globals.
    string eqcode;      // Equality functions.
    string bcode;       // Size walkers and verifiers of bytes values.
    string code;        // Functions, gsInitGlobals and main.
    string xcode;       // The export fns' Go entry points (codegen_go_export.h).
    // The --include files' imports, (name, path), and the rest of each,
    // (name, Go), which follow the program (GoSplitInclude).
    vector<pair<string, string>> incimports, incbodies;

    // The line of the node being generated, which a message about code with
    // no line of its own (a temporary's type) names.
    Line curline;
    [[noreturn]] void Fail(Line l, const string &msg) {
        if (l.line <= 0) l = curline;
        auto s = cat("codegen: ", msg);
        if (l.fileidx >= 0 && l.fileidx < (int)ast.sources.size() && l.line > 0)
            s = cat(ast.sources[l.fileidx].first, ":", l.line, ": ", s);
        throw CompileError { s };
    }
    // A construct this version of the backend does not compile: an error
    // naming it, rather than Go that might be wrong.
    [[noreturn]] void Unsupported(Line l, const string &what) {
        Fail(l, cat(what, " is not supported by --target=go yet"));
    }

    // Abort locations: one string constant per source file, named by the
    // calls that report a place in it.
    map<int, string> filerefs;
    string FileRef(Line l);
    string LocArgs(Line l) { return cat(FileRef(l), ", ", l.line); }

    // ------------------------------------------------------------------
    // Names (codegen_go_values.h).

    set<string> used;   // Package-level names.
    static string Sanitize(string_view name);
    static string Sanitize(string_view ns, string_view name);
    string Unique(string base);

    // ------------------------------------------------------------------
    // Types: the Go type of a checked type, and what is known of it.

    static bool IsVoidT(TypeExpr *t) { return !t || t->kind == TY_VOID; }
    static bool IsScalarT(TypeExpr *t) {
        return t && ((t->kind == TY_INT && t->intstorage != IS_VARINT) || t->kind == TY_FLT ||
                     t->kind == TY_BOOL);
    }
    static const char *IntGoT(IntStorage s);
    string TypeText(TypeExpr *t);
    bool SameGoT(TypeExpr *a, TypeExpr *b);

    // ------------------------------------------------------------------
    // Fixed-size types (codegen_go_types.h): the C layout, and the Go
    // representation of it.

    static int64_t ArrSize(TypeArray *a) { return a->size; }
    static int64_t IntSize(IntStorage s) { return IntBits(s) / 8; }
    bool TEq(TypeExpr *a, TypeExpr *b);
    StructInst *SI(TypeExpr *t);
    EnumInst *EIOf(TypeExpr *t);
    EnumInst *EIVar(TypeExpr *t) { return EIOf(t->var->adt); }
    span<const FieldRun> FieldRuns(TypeExpr *t);
    SizeClass Cls(TypeExpr *t);
    bool IsFix(TypeExpr *t) { return Cls(t) == SC_FIXED; }
    bool IsStaticLimited(TypeExpr *t) {
        return t && t->kind == TY_ARRAY && t->arr->akind == A_LIMITED && ArrSize(t->arr) >= 0;
    }
    IntStorage LenStore(TypeArray *a);
    static IntStorage TagStore(SEnum *en) { return en->variants.size() <= 256 ? IS_U8 : IS_U16; }
    static int64_t TagSize(SEnum *en) { return IntSize(TagStore(en)); }
    struct Layout {
        vector<int64_t> offs;
        int64_t size = 0;
    };
    map<pair<const void *, int>, Layout> layouts;
    int64_t PadAlign(TypeExpr *t);
    Layout LayoutFields(const vector<Field> &fields, const vector<TypeExpr *> &ftypes);
    const Layout &StructLayout(StructInst *si);
    const Layout &VariantLayout(EnumInst *ei, int vi);
    int64_t FixedSize(TypeExpr *t);
    map<pair<EnumInst *, int>, TypeExpr *> varianttypes;
    TypeExpr *VariantType(TypeExpr *enumtype, int vi);
    bool HasRelRef(TypeExpr *t);
    bool BitwiseEq(TypeExpr *t);
    static constexpr int64_t NATIVE_VALUE_LIMIT = 4096;
    bool IsLargeFixed(TypeExpr *t);
    string LocalT(TypeExpr *t, Line ln);
    // The globals the C backend makes static data, with their initializers.
    unordered_map<const VarDef *, Node *> cstatic;
    bool CStaticInit(Node *n, TypeExpr *t);
    unordered_map<string, vector<pair<TypeExpr *, string>>> mangleowners;
    unordered_set<string> mangles;
    string Mangle(TypeExpr *t);
    bool SameMangle(TypeExpr *a, TypeExpr *b);
    string RawMangle(TypeExpr *t);
    static bool NamedGoT(TypeExpr *t);
    set<string> repchecked;   // The nominal types CheckRepresentable has seen.
    void CheckRepresentable(TypeExpr *t, Line ln);
    string GoT(TypeExpr *t, Line ln);
    // Whether t's Go representation holds Go pointers: a plain reference,
    // a slice, a resizable's header, an array of them. Every aggregate that
    // holds an address is the bytes of its C layout instead, which hold
    // none (codegen_go_types.h).
    bool GoHasPtrs(TypeExpr *t);
    // A plain reference to a fixed-size value whose Go type holds Go
    // pointers is its address, an unsafe.Pointer: such a value can lie
    // unaligned, where no typed Go pointer may point (RefIsAddr); any other
    // to a fixed-size value is a typed Go pointer.
    bool RefIsAddr(TypeExpr *rt);
    string RefFromAddr(TypeExpr *rt, const string &addr);
    string AddrOfRef(TypeExpr *rt, const string &x);
    static const char *RelGoT(TypeExpr *rt);
    // How a fixed-size type is held: as Go holds a scalar, a pointer, a
    // GsSlice or an array of those (GR_PLAIN); as a native Go struct
    // (GR_NATIVE); as the bytes of its C layout (GR_BYTES); or, a
    // payload-less ADT, as its tag (GR_TAG).
    enum GoRep { GR_PLAIN, GR_NATIVE, GR_BYTES, GR_TAG };
    struct GoTypeInfo {
        string name;            // Its Go type.
        GoRep rep = GR_PLAIN;
        int64_t size = 0, align = 1;   // In Go's memory.
        bool ptrs = false;      // Its Go type holds Go pointers (GoHasPtrs).
        bool addrs = false;     // It holds addresses: references or slices, in any form.
        bool exact = true;      // Its Go layout is its C layout.
        bool goeq = true;       // Go's == is its structural equality (§4.5).
        vector<int64_t> offs;   // A struct or variant: each field's offset.
        vector<int64_t> voffs;  // An ADT: each variant's payload's offset, -1 for none.
        int64_t elemoff = 0;    // A limited array: its first slot's offset.
        bool building = false;
    };
    unordered_map<string, GoTypeInfo> typeinfo;   // By mangle.
    unordered_map<string, string> gonames;        // Mangle -> named Go type.
    vector<pair<TypeExpr *, Line>> pendingtypes;  // Named, to be declared.
    const GoTypeInfo &TI(TypeExpr *t, Line ln);
    void BuildStructLike(GoTypeInfo &ti, string &decl, const vector<Field> &fields,
                         const vector<TypeExpr *> &ftypes, const Layout &lo, int64_t csize,
                         bool bitwise, Line ln);
    void BuildEnum(GoTypeInfo &ti, string &decl, TypeExpr *t, Line ln);
    void BuildFrameObj(GoTypeInfo &ti, string &decl, TypeExpr *t, Line ln);
    void BuildLimited(GoTypeInfo &ti, string &decl, TypeExpr *t, Line ln);
    void EmitTypeDecls();
    unordered_map<string, string> eqfns;

    // ------------------------------------------------------------------
    // Places (codegen_go_places.h): a value's storage as a Go lvalue of its
    // Go type, with its address where that is known without `&`.

    struct GoLoc {
        string lv;              // The Go lvalue, of the Go type of t.
        string addr;            // An unsafe.Pointer to it, where it is one.
        string ptr;             // A Go pointer to it, where it is one.
        TypeExpr *t = nullptr;  // The type it holds (a relative reference: the slot's).
        // A reference on the way here can be rebound, so evaluating lv later
        // may reach other storage.
        bool viaref = false;
        // A value whose Go type holds Go pointers, at addr, which may be
        // unaligned: it has no lvalue, and is read and written byte for byte
        // (Ld, St), as checkptr requires and the collector never needs to
        // see -- everything it can point at is outside the Go heap.
        bool unal = false;
        // A value on a data stack (codegen_go_rz.h), as the C backend's Loc
        // has it: the stack it lives on (an int64 index into gs.Stks), and,
        // for a resizable, its header's lvalue (a GsRhdr, or a frame
        // object's Go struct) and the int64 lvalue of its count. A resizable
        // array's addr is its elements' base and its lv is empty; a bytes
        // value's addr is where its image starts. A reusable pool's freelist
        // rides along (fl, a GsRhdr lvalue, and flstk).
        string stk, lenlv, hdr, fl, flstk;
        bool ispref = false;    // lv is a GsPref: a pool reference variable.
    };
    // An array's or a slice's elements.
    struct GoView {
        string data;            // An unsafe.Pointer to the first.
        string len;             // Their count, an int64.
        string lenlv;           // A limited array's stored length.
        IntStorage lenst = IS_U8;
        string arr;             // The Go array they are, where they are one.
        TypeExpr *elem = nullptr;
        int64_t esize = 0;      // One's size in Go's memory.
        int64_t fixed = -1;     // A fixed array's length.
        string cap;             // A limited array's capacity, an int64.
    };
    string EqX(TypeExpr *t, const GoLoc &a, const GoLoc &b, Line ln);
    string EqFn(TypeExpr *t, Line ln);
    static string AddOff(const string &p, int64_t off);
    GoLoc AtAddr(const string &p, TypeExpr *t);
    static GoLoc AtTyped(const string &x, TypeExpr *t);
    static GoLoc AtVar(const string &x, TypeExpr *t);
    static string AddrOf(const GoLoc &l);
    string PtrOf(const GoLoc &l);
    string RefOfLoc(const GoLoc &l);
    // The pointee of the plain reference value x of type rt, in whatever
    // form the reference takes: a fat reference's header, a bytes value's
    // address, an address (RefIsAddr), a typed Go pointer.
    GoLoc AtRef(const string &x, TypeExpr *rt);
    GoLoc PinLoc(const GoLoc &l);
    // A place's value, and a store into it: where it holds Go pointers at
    // an address (GoLoc::unal), byte for byte through gsLd_/gsSt_ helpers,
    // which have alignment 1 and leave the collector nothing to see.
    string Ld(const GoLoc &l);
    void St(const GoLoc &l, const string &x);
    string LdStFn(TypeExpr *t, bool store);
    // n elements of esize bytes copied (memmove) or zeroed, byte for byte,
    // as elements holding Go pointers that may lie unaligned must be.
    void CopyElemBytes(const string &dst, const string &src, const string &n, int64_t esize);
    void ClearElemBytes(const string &dst, const string &n, int64_t esize);
    unordered_map<string, pair<string, string>> ldstfns;   // Go type -> its gsLd_, gsSt_.
    string ldstcode;
    // n's value into the place l: straight into its lvalue, or through a
    // temporary where it is reached by byte copies.
    void GenInto(Node *n, const GoLoc &l, TypeExpr *t = nullptr);
    GoLoc FieldLocAt(const GoLoc &base, int fi, Line ln);
    string TagOf(const GoLoc &e, Line ln);
    GoLoc PayloadLoc(const GoLoc &e, int vi, Line ln);
    GoView ArrayView(const GoLoc &l, Line ln);
    GoLoc ElemAt(const GoView &v, const string &i, Line ln);
    string IndexValue(Node *idx);
    GoLoc IndexLoc(GoLoc l, Node *idxnode, Line ln, bool nobc);
    GoLoc VarLoc(const VarDef *vd, Line ln);
    void DerefLoc(GoLoc &l);
    string RelLoad(const GoLoc &slot);
    // A fill's value, evaluated once for all its repetitions (FreezeFill).
    unordered_map<Node *, GoLoc> fillvalues;
    GoLoc GenLoc(Node *n);
    string LoadLoc(GoLoc l, TypeExpr *et, Line ln);
    string AdaptToEnum(const GoLoc &l, TypeExpr *et, Line ln);
    string AdaptBytesToEnum(const GoLoc &l, TypeExpr *et, Line ln);
    string AdaptToLimited(const GoLoc &l, TypeExpr *et, Line ln);
    void CopyIntoLimited(const GoLoc &l, TypeExpr *et, Line ln, const GoLoc &dst);
    string GenRefVal(Node *child, Line ln);
    string CoerceVal(const string &x, TypeExpr *from, TypeExpr *to, Line ln);
    // The widest fixed-size value holding relative references (§3.9).
    int64_t relrootmax = 0;
    void EmitRelRangeCheck(TypeExpr *rt, const string &off, Line ln);
    void EmitRelStore(const GoLoc &slot, const string &rv, bool nonnull, bool apart, Line ln);
    void EmitRelSelf(const GoLoc &slot, int64_t fieldoff, Line ln);
    static bool NonNullRef(Node *v);
    bool RelSlotApart(TypeExpr *rt, TypeExpr *holder, int64_t off);
    void GenRelStore(const GoLoc &slot, Node *val, bool apart);
    void ComputeRelRootMax();

    // ------------------------------------------------------------------
    // Aggregate values (codegen_go_aggr.h).

    string ZeroOf(TypeExpr *t, Line ln);
    void LitInto(Node *n, const GoLoc &dst, bool fresh);
    void StructLitInto(StructLit *sl, const GoLoc &dst, bool fresh);
    void ArrayLitInto(ArrayLit *al, const GoLoc &dst, bool fresh);
    void FreezeFill(Node *n, TypeExpr *t, vector<Node *> &added);
    string GenLitVal(Node *n);
    string GenStrLit(StrLit *s);
    void StrLitInto(StrLit *s, const GoLoc &dst);
    string GenSlice(SliceExpr *se);
    string GenDot(Dot *d);
    string GenEquality(Binary *b);
    string RangeEq(TypeExpr *elem, const string &ad, const string &an, const string &bd,
                   const string &bn, Line ln);
    void ElemwiseInto(TypeExpr *t, TType op, Line ln, const string &l, bool lscalar,
                      const string &r, bool rscalar, const GoLoc &dst);
    void ElemwiseNegInto(TypeExpr *t, Line ln, const string &x, const GoLoc &dst);
    void ElemwiseOperands(Binary *b, string &l, bool &ls, string &r, bool &rs);
    void GenForArray(ForLoop *f);
    void GenEnumMatch(MatchExpr *m, TypeExpr *enumtype, bool isref, const GoDst &d);
    GoLoc RecvLoc(Node *n);
    void CheckLimited(const GoLoc &l, Line ln);
    string ElemValue(Node *n, TypeExpr *elem);
    void StoreElem(Node *n, const GoLoc &slot, const string &ev);
    string NewElemResult(const GoLoc &slot, Line ln);
    string GenArrayBuiltin(Call *c);
    static string ElemAddr(const GoView &v, const string &i);
    // The nominal types the rendering being emitted is inside of, and the
    // render functions of the types that reach themselves through
    // references (RenderFn).
    vector<TypeExpr *> rendering;
    unordered_map<string, string> renderfns;
    struct RenderReq {
        TypeExpr *t;
        Line ln;
        string name;
    };
    vector<RenderReq> renderqueue;
    void RenderLoc(const string &cur, const string &end, GoLoc l, TypeExpr *t, bool nested, Line ln);
    // A user format overload (§3.7) appends to a u8[>..] builder: the text
    // being rendered is one, at the top of the stack its cursor runs on --
    // the builder being formatted into (b, with the cursor's start), or the
    // line print renders, from base -- which an overload is handed whole,
    // its text so far included, as C hands it its builder; the cursor goes
    // on after what the builder holds then (EmitUserFormat). rctx is the
    // call whose overloads apply to the value being rendered.
    struct RenderTarget {
        string stk, base, start;
        GoLoc b;
        bool used = false;   // An overload ran: the stack's top moved.
    };
    RenderTarget *rtarget = nullptr;
    Call *rctx = nullptr;
    FnSpec *FmtSpecFor(Call *c, TypeExpr *t);
    GoLoc RenderedLoc(Node *a);
    void EmitUserFormat(const string &cur, const GoLoc &l, FnSpec *sp, Line ln);
    void RenderVariant(const string &cur, const string &end, const GoLoc &l, TypeExpr *t, Line ln);
    void RenderScalar(const string &cur, const string &end, const string &x, TypeExpr *t, Line ln);
    string RenderFn(TypeExpr *t, Line ln);
    void EmitRenderFns();

    // ------------------------------------------------------------------
    // Values on data stacks (codegen_go_rz.h): resizable values and their
    // fat references, frame objects, reusable pools, and the construction of
    // a value at a stack's top (§4.2, §4.3), as the C backend builds them
    // (codegen_construct.h, codegen_stmts.h, codegen_builtins.h): every stack
    // it takes, it takes where C does, so that both report and reserve the
    // same stacks (--stacks).

    bool IsResz(TypeExpr *t) { return Cls(t) == SC_RESIZABLE; }
    bool IsBytesT(TypeExpr *t) { return Cls(t) != SC_FIXED; }
    bool NeedsStack(TypeExpr *t) { return IsBytesT(t) || IsLargeFixed(t); }
    bool IsFrameObj(TypeExpr *t) { return t->kind == TY_STRUCT && SI(t)->frameobj; }
    bool IsFatRef(TypeExpr *t) {
        return t->kind == TY_REF && t->ref->lenstorage < 0 && IsResz(t->ref->sub);
    }
    bool PrefVar(const VarDef *vd) {
        return vd->type && CarriesPool(vd->type) && vd->ref.reusable;
    }
    bool IsPoolParam(FnSpec *sp, size_t i) {
        return sp->roots[i].reusable && CarriesPool(sp->argtypes[i]);
    }
    // The header type of a resizable: a frame object's own struct, a GsRhdr
    // otherwise.
    string RzGoT(TypeExpr *t, Line ln);
    string FoTailHdr(TypeExpr *t, const string &obj);
    TypeExpr *FoTailArr(TypeExpr *t);
    // A frame object's type as the tail of a value that is no frame object
    // (C.2): bytes of that value like its other fields, its fixed fields in
    // declaration order (an explicit pad's bytes between), then its own
    // tail -- the innermost one's elements, counted by the owner's header.
    // Such a place has an address and no Go lvalue (IsFoBytes).
    static bool IsFoBytesLoc(const GoLoc &l) { return l.lv.empty() && !l.addr.empty(); }
    int64_t FoBytesPrefix(TypeExpr *t);
    GoLoc FoFieldLoc(const GoLoc &base, int fi, Line ln);
    void FoBytes(TypeExpr *t, const string &obj, const string &p, bool tobytes);
    GoLoc FoView(const GoLoc &l);
    void GenFoAsBytes(Node *n, const string &stk, TypeExpr *t, const string &lenlv);
    string Top(const string &stk) { return cat("gs.Stks[", stk, "]"); }
    void Bump(const string &stk, const string &n);
    TypeExpr *growu8 = nullptr;
    TypeExpr *GrowU8();
    // A named result built at its destination (§7.3), as the C backend's
    // NrvoDest: the local's elements are written where the value ends up.
    struct NrvoDest {
        string stk;               // Destination stack expression.
        string lenlv;             // Receiving count; empty for a bytes value.
        IntStorage ls = IS_U32;   // The prefix's length storage.
        bool prefix = false;      // Reserve prefix bytes and patch the count.
        bool inlined = false;     // Bound by an InlineBlock, not by DetectNrvo.
        bool fo = false;          // A frame object: lenlv receives the whole object.
        string pref, hdr;         // Reserved prefix address, header name (DeclStackLocal).
    };
    unordered_map<const VarDef *, NrvoDest> nrvo;
    // Constructions under way per stack, while their parts are emitted
    // (GenConstruct, an inlined body's named result bound there): what they
    // placed sits in front of the top an exit finds on that stack.
    unordered_map<string, int> openat;
    struct OpenAt {
        CodeGenGo &cg;
        string stk;
        OpenAt(CodeGenGo &cg, const string &stk) : cg(cg), stk(stk) { cg.openat[stk]++; }
        ~OpenAt() { cg.openat[stk]--; }
    };
    vector<string> dsttop0;       // gs_dst<i>'s top on entry, once needed.
    vector<string> dstcounts;     // The count each resizable result returns.
    GoDst DeclStackLocal(VarDef *d, bool forlocal);
    string BytesTemp(string &stk);
    string RzTemp(TypeExpr *t, string &stk);
    string RzLenLv(TypeExpr *t, const string &h);
    GoLoc RzTempLoc(TypeExpr *t, const string &h, const string &stk);
    GoLoc GenRzTmp(Node *n);
    GoLoc FatRefLoc(const string &x, TypeExpr *sub);
    GoLoc RzVarLoc(const VarDef *vd, const string &name);
    string RefOfRz(const GoLoc &l, Line ln);
    string GenPrefVal(Node *n);
    string GenXD(Node *n, TypeExpr *want);
    void EmitValStore(const string &stk, TypeExpr *t, const string &x);
    void EmitCopyElems(const string &stk, TypeExpr *elem, const string &src, const string &n);
    struct SrcElems {
        string elems, n;
    };
    SrcElems GenSrcElems(Node *n);
    void GenConstruct(Node *n, const string &stk, TypeExpr *want = nullptr,
                      const string &lenlv = "");
    void ConstructFromLoc(GoLoc l, TypeExpr *et, const string &stk, const string &lenlv, Line ln);
    void ConstructCall(Call *c, TypeExpr *et, const string &stk, TypeExpr *want,
                       const string &lenlv);
    void GenArrayFromLoc(GoLoc l, TypeExpr *et, const string &stk, Line ln, const string &lenlv);
    void EmitRzCopy(GoLoc l, TypeExpr *et, const string &stk, const string &lenlv, Line ln);
    void GenArrayLitAt(ArrayLit *al, const string &stk, const string &lenlv, TypeExpr *as);
    void GenFrameObjLit(StructLit *sl, const string &stk, const string &obj);
    void EmitArrayCount(const string &stk, TypeExpr *at, const string &lenlv, const string &n);
    void EnterDst(int si, const GoDst &d);
    string ScopeTop0(int si);
    string DstTop0(size_t i);
    void DeclareTop0(size_t at, int indent, const string &name, const string &stk);
    const NrvoDest *NrvoBoundAt(Node *val, const string &stk, const string &lenlv);
    string ExitStart(Node *val, const string &stk, const string &lenlv, int open0);
    void LandValue(const string &stk, const string &top0, const string &start, TypeExpr *t,
                   const string &lenlv);
    void EmitNrvoFinish(const NrvoDest &nd);
    void DetectNrvo(FnSpec *sp);
    const VarDef *OpenIbNrvo(InlineBlock *ib, const GoDst &d);
    vector<string> GenRzBuiltin(Call *c, const GoDst &d0);
    vector<string> EmitPush(vector<Node *> &an, Line ln);
    void EmitAppend(vector<Node *> &an, Line ln);
    string NewElemResultRz(const string &e, TypeExpr *elem, Line ln);
    vector<string> EmitStr(Call *c, vector<Node *> &an, const GoDst &d0, Line ln);
    void EmitFormatInto(GoLoc l, Node *a, Line ln, Call *c);
    GoLoc TempBuilder();
    void AppendBytes(const GoLoc &l, const string &src, const string &n, Line ln);
    void RenderToBuilder(const GoLoc &b, Node *a, Call *c);
    bool SimpleText(Call *c, TypeExpr *t);
    GoLoc RzRecvLoc(Node *n);
    GoLoc PinRz(GoLoc l);
    string GenRzArrayBuiltin(Call *c);
    bool HasFieldDefaults(TypeExpr *t);
    vector<string> EmitAlloc(Call *c, vector<Node *> &an, Line ln);
    void EmitFree(vector<Node *> &an, Line ln);
    string SpanArgs(const GoLoc &l);
    string PlaceRun(const GoLoc &l, const string &n);
    string SliceLen(Node *n, int64_t esz, Line ln);
    void EmitSliceExtend(const GoLoc &l, const string &end, int64_t esz);
    void EmitDefaultElems(const GoView &v, const string &first, const string &count, Node *init);
    vector<string> EmitSlicePool(Call *c, vector<Node *> &an, Line ln);

    // ------------------------------------------------------------------
    // Variable-size values (codegen_go_bytes.h): bytes images on data
    // stacks, their sizes and fields, their construction, their equality,
    // and serialization.

    unordered_map<string, string> sizefns, verifyfns;
    int64_t ImgSize(TypeExpr *t);
    string SizeX(TypeExpr *t, const string &p);
    string SizeFn(TypeExpr *t);
    void EmitSizeWalk(string &b, TypeExpr *t, const string &p, const string &q, int &tmp, int in);
    int64_t PrefixBytes(IntStorage ls);
    string StrImage(const string &v, IntStorage ls);
    int64_t MinBytes(TypeExpr *t, bool zero = false);
    string FieldPtr(const string &base, const vector<Field> &fields, const vector<TypeExpr *> &ftypes,
                    int fieldidx);
    GoLoc BytesLoc(const string &p, TypeExpr *t, const GoLoc &from);
    void EmitLenCheck(IntStorage ls, const string &n);
    void EmitLenStore(const string &stk, IntStorage ls, const string &n);
    void EmitVarintStore(const string &stk, const string &x);
    void EmitPrefixPatch(const string &pref, IntStorage ls, const string &stk, const string &count,
                         const string &elems);
    void EmitRelStoreStk(const string &stk, TypeExpr *rt, const string &rv, bool nonnull, Line ln);
    void GenStructLitStk(StructLit *sl, const string &stk, const string &lenlv);
    void EmitValStoreTag(const string &stk, IntStorage ts, const string &x);
    void GenFieldInits(StructLit *sl, const vector<Field> &fields, const vector<TypeExpr *> &ftypes,
                       const string &stk, const string &lenlv, const string &selfbase);
    void GenVarEnumFromLoc(GoLoc l, TypeExpr *et, const string &stk);
    void EmitBytesCount(const string &stk, TypeExpr *at, const string &lenlv, const string &n, bool check);
    string EqFnBytes(TypeExpr *t, Line ln);
    void EmitEqWalk(string &b, TypeExpr *t, int &tmp, int in, Line ln);
    bool HasRelRefAny(TypeExpr *t);
    bool NeedsVerifyWalk(TypeExpr *t);
    string VerifyFn(TypeExpr *elem);
    void EmitVerifyLink(string &b, TypeExpr *rt, TypeExpr *elem, const string &q, const string &off,
                        int &tmp, int in);
    void EmitVerifyFields(string &b, const vector<Field> &fields, const vector<TypeExpr *> &ftypes,
                          const Layout *lo, int64_t total, TypeExpr *elem, const string &q, bool links,
                          int &tmp, int in);
    void EmitVerifyWalk(string &b, TypeExpr *t, TypeExpr *elem, const string &q, bool links, int &tmp,
                        int in);
    void PayloadOf(Node *n, string &src, string &sz);
    struct RzDest {
        string stk, lenlv, hdr, pref, elems;
        IntStorage ls = IS_U32;
    };
    RzDest OpenRzDest(TypeExpr *t, const GoDst &d0, Line ln, const char *what);
    void CloseRzDest(RzDest &rd, const string &count);
    void EmitLeCheck(Line ln);
    vector<string> EmitBytesOf(Call *c, vector<Node *> &an, Line ln);
    vector<string> EmitToBytes(vector<Node *> &an, const GoDst &d0, Line ln);
    vector<string> EmitFromBytes(Call *c, vector<Node *> &an, const GoDst &d0, Line ln);

    // ------------------------------------------------------------------
    // Per-specialization interface. A function's Go parameters are the data
    // stack block and index where it needs them (needssp), its declared
    // parameters, then the free variables it or its callees reach (§7.5),
    // each a pointer to the variable; its results are Go's multiple results.

    // needsgs: it takes the block without the index, where it touches a
    // data stack only through what it is handed -- a reference to a
    // resizable, a destination, a global's stack -- as a C function does
    // through a gs_stack * without needing gs_sp.
    struct SpecInfo {
        string name;
        vector<VarDef *> freevars;
        bool needssp = false;
        bool needsgs = false;
        // A long-distance return (§7.9) can come back through a call of it:
        // its callers check for one in flight (EmitRfCheck).
        bool hasrf = false;
    };
    // Long-distance returns (`return ... from`, a block's return out of the
    // functions it is handed to, §7.9), as the C backend has them
    // (CodeGen::GenFromReturn): the return site puts the values in its
    // target's channels (gsLret_/gsFdst_/gsFval_<id>_<i>, package
    // variables: the Go backend runs one thread program) and sets gsRf to
    // the target's number; every function on the way back returns at once
    // with dummy values, until the target's own check after its call takes
    // the values and returns them. A target with a non-fixed result keeps
    // its destination stack in its channel while it runs (fdstsaves).
    unordered_map<FnSpec *, int> fromids;
    set<FnSpec *> fromemitted;
    string rfdecls;
    vector<string> fdstsaves;
    vector<string> curzeros;            // A dummy value per Go result of the function emitted.
    vector<string> curgorets;           // The Go type of each.
    bool HasRf(FnSpec *sp) { auto it = sinfo.find(sp); return it != sinfo.end() && it->second.hasrf; }
    void EnsureFromChannels(FnSpec *t);
    void GenFromReturn(Return *r);
    void PropagateReturn(const string &rfval);
    void EmitRfCheck(FnSpec *callee);
    unordered_map<FnSpec *, SpecInfo> sinfo;
    vector<FnSpec *> livespecs;

    // Data stack accounting, as the C backend keeps it (codegen.h), for the
    // analysis both share (codegen_stacks.h): the calls each function hands
    // a stack index, with that index relative to its own, and the most
    // stacks its own body opens. The nullptr function is gsInitGlobals.
    using StackCall = CodeGen::StackCall;
    unordered_map<FnSpec *, vector<StackCall>> stackcalls;
    unordered_map<FnSpec *, int> stackown;
    unordered_map<FnSpec *, int64_t> stackneed;
    // Regions outside the indexed block: the dedicated stacks of the
    // resizable and variable-size globals (and a pool's freelist), which
    // follow the main program's stacks in its block (gsGlobalStacks).
    int globalregions = 0;
    unordered_map<FnSpec *, int> thunkregions;
    int64_t maxstacks = 0;
    // No thread programs yet.
    vector<pair<string, FnSpec *>> WorkerEntries() { return {}; }
    void NoteStackCall(FnSpec *callee, Line ln) {
        stackcalls[curspec].push_back({ callee, stknext, ln });
    }
    // As CodeGen's: a call into the recursive cycle the function is in is
    // made with none of its stacks in use (§7.8), which the checker has
    // made sure of.
    void NoStackAcrossCycleCall(FnSpec *callee, Line ln) {
        if (!curspec || !curspec->incycle || !callee->incycle || !stknext) return;
        auto cycle = [](FnSpec *s) { while (s->cyclelink) s = s->cyclelink; return s; };
        if (cycle(callee) != cycle(curspec)) return;
        Fail(ln, cat("internal: ", curspec->sf->name, " holds ", stknext, " data stack(s) "
                     "across its call into the recursive cycle through ", callee->sf->name,
                     " (§7.8)"));
    }
    void StackReport(FILE *out, const CodeGen::StackConfig &cfg) { StackReportOf(*this, out, cfg); }

    // ------------------------------------------------------------------
    // Per-function emission state.

    FnSpec *curspec = nullptr;
    SpecInfo *curinfo = nullptr;
    string body;
    int ind = 1;   // The indentation of the lines being emitted, one tab per Go block.
    int tmpn = 0;
    int stknext = 0, stkmax = 0;
    string spexpr;                      // "gs_sp" in a function, "0" in gsInitGlobals.
    set<string> fnused;                 // The function's local names.
    unordered_map<string, int> fnnextsuffix;
    unordered_map<const VarDef *, string> vnames;
    set<const VarDef *> fvptr;          // Free variables, which arrive as pointers.
    unordered_map<const VarDef *, string> gnames;
    // The data stack of each local and global value that lives on one, and a
    // reusable pool's freelist (its GsRhdr and its stack), as the C
    // backend's vstk, vpool, gstks and gpools.
    unordered_map<const VarDef *, string> vstk;
    unordered_map<const VarDef *, pair<string, string>> vpool;
    unordered_map<const VarDef *, string> gstks;
    unordered_map<const VarDef *, pair<string, string>> gpools;
    // Control cannot reach the code being emitted: it follows a statement Go
    // takes to end the flow (a return, a break or continue, a panic, a `for`
    // without a condition or a break). Go's vet reports such code, so none
    // of it is emitted (L), up to where another path joins again: the end of
    // a construct whose opening line was. What the program has there is
    // generated all the same, into text that is dropped (GenUnreached, `dry`
    // deep), from every entry point (GenStmt, GenStmt2, GenAny, GenX) and
    // from every construct whose first part (a condition, a scrutinee, an
    // argument) ends the flow before the construct opens. So only a
    // construct whose opening line was emitted closes, or joins paths again.
    bool dead = false;
    int dry = 0;
    bool Unreached() const { return dead && !dry; }
    void GenUnreached(const function<void()> &gen);

    // Scopes mirror Go's blocks, as the C backend's mirror C's (CScope).
    enum { SC_PLAIN, SC_FN, SC_LOOP, SC_BLOCK, SC_IB, SC_STMT };
    struct GoScope {
        int kind = SC_PLAIN;
        int stkbase = 0;
        // The data stack tops to restore at every exit through this scope,
        // (stack, base) in declaration order, and the `defer` bodies
        // registered in it (§6.6), each with how many saves came before it:
        // an exit runs them among the restores, latest first (EmitRestores).
        vector<pair<string, string>> saves;
        vector<pair<size_t, Defer *>> defers;
        SFunction *ibsf = nullptr;      // SC_IB: which returns exit here.
        // SC_LOOP, SC_BLOCK, SC_IB: the Go label a break, continue or
        // return names, written in front of the statement once one does
        // (labelat, at labelind); usedbrk: one leaves it, so Go's flow
        // goes on past it.
        string label;
        size_t labelat = 0;
        int labelind = 0;
        bool usedlabel = false, usedbrk = false;
        GoDst dst;                      // Where a break's or a return's value goes.
        // A stack dst: the constructions open there on entry, and where its
        // top on entry is declared once an exit needs it (ScopeTop0).
        int open0 = 0;
        size_t topat = 0;
        int topind = 0;
        string top0;
    };
    vector<GoScope> scopes;

    // The emission state of a function, which one generated on the side (an
    // equality, a render function) sets aside meanwhile.
    struct FnState {
        string body;
        int ind = 1, tmpn = 0, stknext = 0, stkmax = 0;
        string spexpr;
        set<string> fnused;
        unordered_map<string, int> fnnextsuffix;
        unordered_map<const VarDef *, string> vnames;
        set<const VarDef *> fvptr;
        vector<GoScope> scopes;
        bool dead = false;
        int dry = 0;
        FnSpec *curspec = nullptr;
        SpecInfo *curinfo = nullptr;
        vector<TypeExpr *> rendering;
        RenderTarget *rtarget = nullptr;
        Call *rctx = nullptr;
        unordered_map<const VarDef *, string> vstk;
        unordered_map<const VarDef *, pair<string, string>> vpool;
        unordered_map<const VarDef *, NrvoDest> nrvo;
        unordered_map<string, int> openat;
        vector<string> dsttop0, dstcounts;
        unordered_map<string, string> ltypes;
        vector<pair<string, string>> ptypes;
        vector<string> fdstsaves, curzeros, curgorets;
    };
    FnState SaveFnState();
    void RestoreFnState(FnState &s);

    // Statement lines: none where control cannot reach them. LS is for the
    // lines that close or continue a construct whose opening line was
    // emitted, which have to be there either way.
    template<typename... Ts> void L(const Ts &...args) {
        if (dead) return;
        LS(args...);
    }
    template<typename... Ts> void LS(const Ts &...args) {
        body.append((size_t)ind, '\t');
        Append(body, args...);
        body += '\n';
    }

    string T() { return cat("t", tmpn++); }
    string Lbl() { return cat("L", tmpn++); }

    // The marker a local's declaration ends with: FinishBody gives the
    // locals nothing reads a use of their own, which Go requires.
    static constexpr const char *DECLMARK = "\x01";
    // Declares a local of Go type gt, with an initial value where init is
    // not empty.
    void Decl(const string &name, const string &gt, const string &init = "") {
        L("var ", name, " ", gt, init.empty() ? "" : " = ", init, DECLMARK, name);
        NoteType(name, gt);
    }

    // The fixed-frame stack (gs.Frame, codegen_go_emit.h). A local or a
    // parameter whose address the function takes lives in the function's
    // frame there rather than on Go's stack, which moves as it grows and
    // which the collector scans where nothing a value's bytes hold is seen:
    // FinishBody finds them, by the Go types the declarations give
    // (ltypes; ptypes, the parameters'). FRAMEEND marks where the end of a
    // body is reached, which gives the frame back as a return does.
    unordered_map<string, string> ltypes;
    vector<pair<string, string>> ptypes;
    // The Go types of the fixed-size values of more than NATIVE_VALUE_LIMIT
    // bytes a recursive cycle's locals hold: in the frame too, as C would
    // keep them off its native stack were they outside a cycle.
    set<string> largegts;
    void NoteType(const string &name, const string &gt) { ltypes[name] = gt; }
    static constexpr const char *FRAMEEND = "\x03";
    // Whether a callee takes the block is known once every body is: one
    // whose frame holds a local takes it, as does any calling one. Until
    // then a call's and a signature's place for it holds GSARG/GSPARAM and
    // the specialization's number, which ResolveGsArgs replaces.
    static constexpr const char *GSARG = "\x05";
    static constexpr const char *GSPARAM = "\x06";
    unordered_map<FnSpec *, int> specnum;
    vector<FnSpec *> numspec;
    unordered_map<FnSpec *, set<FnSpec *>> phcalls;
    set<FnSpec *> hasframe;
    string GsArgPh(FnSpec *sp);
    // Which take the block, by specnum, once every body is emitted.
    vector<char> gsneeds;
    void ComputeGsNeeds();
    void ResolveGsArgs();

    // The data stack index k of this function, and the first one free.
    string SpIdx(int k) { return spexpr == "0" ? cat(k) : k ? cat(spexpr, " + ", k) : spexpr; }
    string SpTop() { return SpIdx(stknext); }
    string AllocStk(bool forlocal = false);
    void SaveBase(bool forlocal, const string &stk, const string &basevar);

    // ------------------------------------------------------------------
    // Values (codegen_go_values.h). GenX gives a Go expression of the Go type
    // of the node's checked type, emitting what has to run first; GenAny
    // delivers a node's value to a destination, which control constructs
    // pass on to each branch.

    static bool IsCtl(Node *n) {
        return Is<IfExpr>(n) || Is<MatchExpr>(n) || Is<Block>(n) || Is<EarlyBlock>(n) ||
               Is<LoopExpr>(n) || Is<InlineBlock>(n);
    }
    static bool HasStmts(Node *n);
    bool TryPure(const function<string()> &gen, string &x);
    static bool IsGoName(const string &x);
    static bool GoConstInt(const string &x, int64_t &v, bool &uns);
    static bool IsGoConst(const string &x);
    static string GoUnparen(string x);
    static vector<string> GoSplitTop(const string &x, const string &op);
    static void GoChainOperands(const string &x, bool andand, vector<string> &out);
    static bool VetBoolClash(const string &l, const string &r, bool andand);
    static string CondText(const string &x);
    string Materialize(TypeExpr *t, const string &x);
    string Coerce(const string &x, TypeExpr *from, TypeExpr *to, Line ln);
    string Snapshot(TypeExpr *t, const string &x);
    string GenPure(Node *n);
    string GenOperandVal(Node *n, bool later);
    string GoIntLit(int64_t v, bool uns, TypeExpr *t, Line ln);
    string GoFltLit(double v, TypeExpr *t, Line ln);
    static string GoStr(string_view s);
    string VarRef(const VarDef *vd, Line ln);
    string ArithX(TType op, TypeExpr *t, string l, string r, Line ln, bool nonneg = false);
    string DivX(bool div, TypeExpr *t, const string &l, const string &r, Line ln);
    string ShiftX(TType op, TypeExpr *t, const string &l, const string &r);
    string GenX(Node *n);
    string GenUnary(Unary *u);
    string GenBinary(Binary *b);
    string GenCast(AsCast *c);
    string NullTestOperand(Node *n);
    string GenTruth(Node *n);
    string CtlValX(Node *n);
    void GenAny(Node *n, const GoDst &d);
    void LeafAny(Node *n, const GoDst &d);

    // ------------------------------------------------------------------
    // Statements and control flow (codegen_go_stmts.h).

    void PushSc(int kind);
    void PopSc();
    void EmitRestores(const GoScope &s);
    void EmitExitRestores(int to);
    void RegisterDefer(Defer *d);
    void EmitDeferBody(Defer *d);
    void OpenLabeled(int kind);
    void LabelHere();
    string UseLabel(int si, bool brk);
    GoScope PopLabeled();
    void PlaceLabel(const GoScope &s);
    void GenBlockInner(Block *b, const GoDst &d, size_t first = 0, int exitscope = -1);
    size_t GenInlineArgs(Block *b);
    void GenStmt(Node *n);
    void GenStmt2(Node *n);
    void GenBlock(Block *b, const GoDst &d);
    void GenIf(IfExpr *fi, const GoDst &d);
    void GenMatch(MatchExpr *m, const GoDst &d);
    void GenEarlyBlock(EarlyBlock *eb, const GoDst &d);
    void GenInlineBlock(InlineBlock *ib, const GoDst &d);
    void EmitIbBody(InlineBlock *ib, const GoDst &d);
    void GenWhile(While *w);
    void GenLoop(LoopExpr *lp, const GoDst &d);
    void GenFor(ForLoop *f);
    void GenLoopBody(Block *bodyb, size_t first = 0);
    void GenBreak(Break *b);
    void GenContinue(Continue *c);
    void GenReturn(Return *r);
    void GenExitValue(Node *val, int si);
    vector<string> GenForward(Call *c, const vector<TypeExpr *> &rets, const vector<GoDst> &chans);
    void GenNormalReturn(const vector<Node *> &vals);
    void GenVarDecl(VarDecl *vd);
    string LocalName(VarDef *vd);
    string Unique2(const string &base);
    void BindLocal(VarDef *d, Node *init, bool forlocal = true);
    bool StableDest(Node *n);
    GoLoc AssignLoc(Node *lval, bool pointee);
    void GenAssign(Assign *a);
    bool RelApartOf(Node *lval, TypeExpr *rt);
    void GenIncDec(IncDec *a);

    // ------------------------------------------------------------------
    // Calls and builtins (codegen_go_calls.h).

    vector<Node *> CallArgNodes(Call *c, size_t nparams);
    string FreeVarArg(const VarDef *fv);
    bool PlainSig(FnSpec *sp);
    string StackArgs(FnSpec *sp, Line ln);
    string SpecCallX(Call *c, FnSpec *sp);
    string ExternCallX(Call *c, FnSpec *sp);
    bool usesbits = false;   // A call needs math/bits (ExternCallX).
    vector<string> EmitSpecCall(Call *c, FnSpec *sp, const GoDst &d0, vector<GoDst> *alldst);
    bool NeedsReprefix(const GoDst &d, TypeExpr *rt);
    void EmitSlidePrefix(const string &base, IntStorage ls, const string &stk, const string &lenlv);
    void EmitReprefix(FnSpec *sp, const GoDst &dd, const string &base, const string &cnt);
    // The element-run twins of the specializations (C.3, CodeGen::EnsureEr):
    // the same body, emitting a variable-array result's raw elements at the
    // destination and its count, which the twin returns. "" where a
    // specialization has none.
    bool emiter = false;
    unordered_map<FnSpec *, string> ernames;
    vector<FnSpec *> erqueue;
    string EnsureEr(FnSpec *sp);
    // The foreign boundary (codegen_go_export.h): extern fns' calls, export
    // fns' Go entry points.
    bool usesos = false;   // A call needs the OS layer (go_runtime_os).
    bool ExternTakesGs(FnSpec *sp);
    bool GoLibExtern(Call *c, FnSpec *sp, const vector<string> &a, string &x);
    void EmitExports();
    // The specializations whose code names a global's own data stack
    // (gsGStk_), which an export's entry points cannot reach, and what each
    // calls (CollectSpecs).
    set<FnSpec *> gstkusers;
    unordered_map<FnSpec *, vector<FnSpec *>> calleesof;
    vector<string> GenCallVals(Call *c, const GoDst &d0, vector<GoDst> *alldst = nullptr);
    string CallVal0(Call *c, const string &r0, TypeExpr *want);
    GoLoc CallResLoc(Call *c, const string &r0);
    vector<string> GenDispatch(Call *c, const GoDst &d0, vector<GoDst> *alldst);
    bool NonFixedDispatch(Call *c);
    string GenCall(Call *c);
    void GenCallAny(Call *c, const GoDst &d);
    void GenCallInto(Call *c, const vector<VarDef *> &defs);
    string GenFvCall(Call *c, const GoDst &d0);
    void GenBuiltin(Call *c, const GoDst &d);
    void EmitPrint(Call *c);
    void RenderInto(const string &cur, const string &end, Node *a, Call *c);
    // An argument read for its text before any of it is placed, which its
    // own side effects (a call appending to the builder it goes into, say)
    // must not overwrite: a string literal, a scalar's value, or the place
    // anything else is rendered from.
    struct RenderArg {
        Call *ctx = nullptr;
        TypeExpr *t = nullptr, *vt = nullptr;
        StrLit *lit = nullptr;
        bool scalar = false;
        string x;
        GoLoc l;
    };
    RenderArg PrepRender(Node *a, Call *c);
    void PlaceRender(const string &cur, const string &end, const RenderArg &ra, Line ln);

    // ------------------------------------------------------------------
    // Functions, globals and the program (codegen_go_emit.h).

    void CollectSpecs();
    string ParamT(TypeExpr *t, Line ln);
    string Signature(FnSpec *sp);
    void ResetFnState();
    string FinishBody(const string &b, bool frames = false, const vector<string> &rtypes = {},
                      bool *framed = nullptr);
    void EmitSpec(FnSpec *sp, bool er = false);
    // Globals initialized at their declaration, which gsInitGlobals skips,
    // and those the program can tell are there (FindUsedGlobals).
    set<const VarDef *> gstatic;
    set<const VarDef *> usedglobals;
    bool PureInit(Node *n);
    bool HoldsFatRef(TypeExpr *t);
    void FindUsedGlobals();
    void EmitGlobalDecls();
    bool StaticInit(Node *n, TypeExpr *t, string &out);
    void EmitGlobalInit();
    // The dedicated stacks of the globals, in the order gsInitGlobals sets
    // them up: each one's index follows the main program's stacks.
    vector<string> gstkorder;
    void InitGlobalStack(VarDef *d);
    void EmitProgram();
    string Source(const string &srcname);

    CodeGenGo(Ast &_ast, const GoOptions &o) : ast(_ast), opts(o) {
        debug = opts.debug;
        maxstacks = opts.stacks.maxstacks;
        for (auto &inc : opts.includes) {
            string body, why;
            if (!GoSplitInclude(inc.text, incimports, body, why))
                throw CompileError { cat("--include ", inc.name, ": ", why) };
            incbodies.push_back({ inc.name, std::move(body) });
        }
        CollectSpecs();
        ComputeRelRootMax();
        EmitGlobalDecls();
        for (auto sp : livespecs) EmitSpec(sp);
        EmitGlobalInit();
        // The element-run twins last: the globals' initializers can ask for
        // one too, and a twin for another.
        for (size_t i = 0; i < erqueue.size(); i++) EmitSpec(erqueue[i], true);
        EmitProgram();
        EmitRenderFns();
        EmitTypeDecls();
        ResolveGsArgs();
    }
};

}  // namespace goose
