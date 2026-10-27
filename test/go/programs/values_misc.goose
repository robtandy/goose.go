// Fixed-size values in the rest of the language: captured by nested
// functions, returned several at once, in globals, copied and defaulted,
// and holding references and slices where C packs them unaligned (Go keeps
// those in a layout of its own, which nothing here can tell apart), an ADT
// whose payload holds a reference, an optional narrowed on the right of &&.

struct Row { name: const u8[:], total: i64, orders: i32 }
struct Tagged { flag: u8, who: Row&, n: i16 }
struct Cfg { level: i64 = 3, name: const u8[:] = "cfg", on: bool }
enum List { Nil, Cons { v: i64, next: List? } }
struct Acc { sum: i64, n: i64 }

var log: i64[..16] = [];
var acc = Acc { sum: 0, n: 0 };

fn add(v: i64) { acc.sum += v; acc.n += 1; log.push(v); }
fn minmax(a: const i64[:]) -> i64, i64, Acc {
    var lo = a[0];
    var hi = a[0];
    for x in a { if x < lo { lo = x; } if x > hi { hi = x; } }
    return lo, hi, Acc { sum: lo + hi, n: 2 };
}
recursive fn length(l: List) -> i64 {
    match l {
        Nil => 0,
        Cons c => {
            let n = c.next;
            guard n else { return 1; }
            1 + length(n)
        },
    }
}

fn main() {
    var grid = [[1, 2], [3, 4]];
    fn cell(r: i64, c: i64) -> i64 { grid[r][c] }
    fn bump() { grid[1][1] += 10; }
    bump();
    print(cell(1, 1), " ", grid);

    for x in [5, -2, 9] { add(x); }
    print(acc, " ", log);
    let lo, hi, both = minmax(log);
    print(lo, " ", hi, " ", both);

    var rows = [Row { name: "a", total: 10, orders: 1 }, Row { name: "bb", total: 20, orders: 2 }];
    rows[1].total += 5;
    let t = Tagged { flag: 1, who: rows[1], n: -3 };
    t.who.orders += 1;
    print(rows, " ", t);
    let copied = copy(rows[0]);
    rows[0].total = 0;
    print(copied, " ", rows[0], " ", copied == rows[0]);

    let d = default<Cfg>();
    let z = default<Row>();
    print(d, " ", z, " ", default<i64[3]>(), " ", default<List>(), " ", default<u8[..4]>());

    var cells = [List.Nil, List.Nil, List.Nil];
    cells[1] = List.Cons { v: 2, next: cells[0] };
    cells[2] = List.Cons { v: 3, next: cells[1] };
    print(length(cells[2]), " ", length(cells[0]), " ", cells[2]);

    var o: Row? = null;
    print(o != null && o.total > 0);
    o .= rows[1];
    print(o != null && o.total > 0, " ", o);
    {
        defer print("deferred ", grid[0]);
        grid[0] = [7, 7];
    }
}
