// Bodies that only choose among values (issue #26): std's min, max, clamp,
// abs and sign, chains of the program's own and ifs choosing between two
// values, which the Go backend writes as Go's builtin min and max or as
// assignments Go makes conditional selects of. The checks are
// modules/selects.goose's, which selects_debug.goose runs with --debug.
import modules.selects;

fn main() { selects_run(); }
