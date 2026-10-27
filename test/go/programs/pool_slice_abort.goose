// A slice handed back to a slice pool that the checker cannot place in it
// is checked to be the pool's, and aborts where it is not (GS_E_POOLSLICE).
struct Doc { text: u8[:] }
reusable[] var textbytes: u8[>..] = [];
var lines: u8[>..] = [];
var docs: Doc[>..] = [];

fn main() {
    lines.append("abc");
    docs.push(Doc { text: textbytes.alloc_slice(2) });
    docs.push(Doc { text: lines[..2] });
    textbytes.free_slice(docs[0].text);
    print(textbytes.alloc_slice(2).len, " ", textbytes.len);
    textbytes.free_slice(docs[1].text);
    print("not reached");
}
