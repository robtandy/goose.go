// A module of names.goose (test/go/programs/modules): a namespace whose names match global ones.
namespace geo;

let SCALE: i64 = 1000;

fn norm(x: i64) -> i64 { x * 2 }
fn scale(x: i64) -> i64 { x * SCALE }
