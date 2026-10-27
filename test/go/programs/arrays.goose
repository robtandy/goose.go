// Fixed arrays T[k] (§3.3): Go arrays of the element's representation.
// Literals and fills (whose value is evaluated once), indexing with and
// without the bounds checks BCE proved away, nested arrays, arrays of
// structs, every kind of for loop over them, equality, whole-array slices,
// elementwise arithmetic and the text of print.

struct Cell { v: i64, mark: bool }
struct Px { r: u8, g: u8, b: u8 }
struct Grid { cells: Cell[3][2], name: u8[4] }

var calls = 0;
fn next() -> i64 { calls += 1; calls * 10 }

fn total(a: i64[4]) -> i64 { var s = 0; for x in a { s += x; } s }
fn fill(a: i64[4]&, v: i64) { for &x in a { x = v; } }
fn sum_slice(s: const i64[:]) -> i64 { var t = 0; for x in s { t += x; } t }
fn at(a: i64[4], i: i64) -> i64 { a[i] }

fn main() {
    var a = [1, 2, 3, 4];
    a[1] = 20;
    a[3] += a[1];
    print(a, " ", a.len, " ", total(a), " ", at(a, 2));
    fill(a, 7);
    print(a, " ", sum_slice(a), " ", sum_slice(a[1..]));
    for i in 0..a.len { a[i] = a[i] * i; }   // bce: no checks
    print(a);

    var g: i64[3][2] = [[1, 2, 3], [4, 5, 6]];
    g[1][2] = 60;
    g[0] = [7, 8, 9];
    print(g, " ", g[1], " ", g.len, " ", g[0].len, " ", g[1][2] + g[0][0]);

    let f = [next(); 3];                        // one call, three copies
    print(f, " ", calls);

    var cells = [Cell { v: 1, mark: false }; 3];
    cells[2].mark = true;
    cells[0].v = -1;
    for c, i in cells { print(i, ": ", c); }
    for &c in cells { c.v *= 2; }
    for c, j: u8 in cells { print(j, " ", c.v, " ", c.mark); }

    let z = [0; 0];
    print(z, " ", z.len, " ", z == [0; 0]);
    let bs: u8[5] = "hello";
    print(bs, " ", bs[1], " ", bs[4] - bs[0]);
    var pixels: Px[2] = [Px { r: 1, g: 2, b: 3 }, Px { r: 4, g: 5, b: 6 }];
    pixels[1].g = 255;
    print(pixels, " ", pixels[0] == Px { r: 1, g: 2, b: 3 }, " ", pixels[0] == pixels[1]);

    print([1.5, 2.5] == [1.5, 2.5], " ", a == [0, 7, 14, 21], " ", [0.0, 1.0] == [-0.0, 1.0]);

    var grid = Grid { cells: [[Cell { v: 0, mark: false }; 3]; 2], name: "grid" };
    grid.cells[1][2] = Cell { v: 9, mark: true };
    var n = 0;
    for row in grid.cells { for c in row { if c.mark { n += c.v; } } }
    print(n, " ", grid.name, " ", grid);

    // Elementwise arithmetic on arrays of numbers (§6.1).
    let v = [1.0, 2.0, 3.0];
    let w = [0.5, 0.25, 2.0];
    var u = v + w * 2.0;
    u -= v;
    print(u, " ", -u, " ", v * w);
    var iv = [1, 2, 3];
    iv *= 3;
    print(iv, " ", iv - [1, 1, 1]);
}
