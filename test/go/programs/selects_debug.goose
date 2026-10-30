// goose-flags: --debug
// selects.goose's checks with --debug (issue #26): a negation, which a debug
// build checks, is computed only where the choice takes it, as a branch.
import modules.selects;

fn main() { selects_run(); }
