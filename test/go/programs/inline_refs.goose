// By-reference parameters of inlined functions (issue #24): a local handed
// to an inlined body by reference is the Go local itself in the body, unless
// its address outlives it. The checks are modules/inline_refs.goose's, which
// inline_refs_debug.goose runs with --debug.
import modules.inline_refs;

fn main() { inline_refs_run(); }
