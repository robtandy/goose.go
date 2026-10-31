// Index and bound types (§3.11, issue #59): an integer of the array it
// names, checked against that array's length as a value converts to it --
// below it for `in`, at most it for `upto` -- unless BCE proved it in range,
// and then the array indexed with no check. Through calls, fields, a root
// owner's component and a serialized image, which is checked against the
// array as it is.
import slot_pool;

struct Edge { to: u32 in nodes, w: i32 }
var nodes: i64[>..] = [];
type NodeId = u32 in nodes;
type Bound = u32 upto nodes;
var edges: Edge[>..] = [];
var pool = slot_pool<i64> {};
type Slot = u16 in pool.items;

fn add(v: i64) -> NodeId {
    nodes.push(v);
    return (nodes.len - 1) as NodeId;
}

fn weight(a: NodeId) -> i64 { nodes[a] }

fn link(a: i64, b: i64, w: i32) { edges.push(Edge { to: b as NodeId, w: w }); }

fn main() {
    for v in 5 { add(v * 10); }
    link(0, 3, 7);
    link(1, 4, -2);
    var sum = 0;
    for e in edges { sum += weight(e.to) * e.w; }
    print(sum, " ", weight(add(55)), " ", nodes.len as Bound);
    let i = pool.alloc_index(42);
    print(pool.items[i as Slot]);
    // An image of index-typed values, checked against the array as it is.
    var image: u8[>..] = [];
    edges.to_bytes(image);
    let back, ok = from_bytes<Edge[]>(image[..]);
    print(ok, " ", back.len, " ", back[1].to, " ", (5 as NodeId), " ", (6 as Bound));
}
