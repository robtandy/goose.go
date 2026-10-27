// A relative reference's offset that does not fit its width aborts where it
// is stored (GS_E_RELOFF): an u8 link reaches 127 bytes either way.
struct Node { v: i64, pad 120, next: Node&<u8>? }

fn main() {
    var nodes = [Node { v: 1 }, Node { v: 2 }, Node { v: 3 }];
    nodes[0].next .= nodes[0];
    let n = nodes[0].next;
    guard n else { return; }
    print(n.v, " ", nodes[1].next == null);
    nodes[1].next .= nodes[2];      // 1 byte on: the next node starts there
    print(nodes[1].next == null);
    nodes[2].next .= nodes[0];      // 386 bytes back
    print("not reached");
}
