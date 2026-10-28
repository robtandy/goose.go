// A call's reference result stored into a relative-reference slot (§3.9):
// an element of a variable-size array literal, a pushed element, a field of
// a literal built on a data stack, directly and through an if or a match,
// at several widths and as a varint, each its offset from the pool's base.
// -O0 inlines no call, so the result reaches the slot as the call's.
struct Cell { v: i64 }
var cells: Cell[>..] = [];

struct Named { name: u8[], r: Cell&<u16 in cells>? }
struct Packed { name: u8[], r: Cell&<varint in cells>? }

fn pick(i: i64) -> Cell& { cells[i] }
fn pick_opt(i: i64) -> Cell&? {
    if i < 0 { return null; }
    return cells[i];
}
fn show(r: Cell&?) -> i64 {
    if r { return r.v; }
    return -1;
}

fn main() {
    for i in 50 { cells.push(Cell { v: i * 3 }); }
    var named: Named[>..] = [];
    var packed: Packed[>..] = [];
    var links: (Cell&<u32 in cells>?)[>..] = [];
    var narrow: (Cell&<u8 in cells>)[>..] = [];
    for k in 4 {
        let built: (Cell&<u32 in cells>?)[] = [pick(k + 1), pick_opt(k - 2),
                                               if k == 0 { pick(9) } else { null }];
        print(show(built[0]), " ", show(built[1]), " ", show(built[2]));
        named.push(Named { name: "n", r: pick_opt(k * 7 - 1) });
        packed.push(Packed { name: "p", r: match k { 0 => null, 1 => pick(40), _ => pick_opt(k + 40) } });
        links.push(pick(k + 20));
        links.push(pick_opt(k - 1));
        narrow.push(pick(k + 10));
    }
    for x in named { print(x.name, " ", show(x.r)); }
    for x in packed { print(x.name, " ", show(x.r)); }
    for x in links { print(show(x)); }
    for x in narrow { print(x.v); }
}
