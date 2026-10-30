// goose-flags: --debug
// inline_refs.goose's checks with --debug (issue #24): the arithmetic on a
// place an inlined body's parameter names is checked as it is on any other.
import modules.inline_refs;

fn main() { inline_refs_run(); }
