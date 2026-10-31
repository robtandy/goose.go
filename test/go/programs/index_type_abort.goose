// A conversion to a bound type above its array's length aborts in every
// build, as indexing past it would (§3.11, issue #59), after the program's
// own output.
var nodes: i64[>..] = [];
type Bound = u32 upto nodes;
type NodeId = u32 in nodes;

fn bound(x: i64) -> Bound { x as Bound }

fn main() {
    for v in 3 { nodes.push(v); }
    print(bound(3), " ", (2 as NodeId));
    print(bound(4));
    print("not reached");
}
