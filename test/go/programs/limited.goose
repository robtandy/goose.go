// Static-capacity limited arrays T[..k] (§3.3, §5.3): the length, in the
// smallest unsigned type that holds k, then k slots. push, pop, pop_n,
// resize, clear, append, .len and .cap; construction from literals and from
// any array or slice of the element type, which checks the capacity;
// equality and print, which see the live elements only.

struct Item { id: u16, weight: f32 }
struct Bag { label: u8[..8], items: Item[..3], count: u8 }

fn push_all(dst: i64[..8]&, src: const i64[:]) { for x in src { dst.push(x); } }
fn sum(s: const i64[:]) -> i64 { var t = 0; for x in s { t += x; } t }
fn last(l: i64[..8]) -> i64 { l[l.len - 1] }

fn main() {
    var lim: i64[..4] = [];
    lim.push(5);
    lim.push(6);
    let r .= lim.push(7);
    r += 100;
    print(lim, " ", lim.len, " ", lim.cap, " ", lim.pop(), " ", lim);
    lim.resize(4, 9);
    print(lim, " ", sum(lim));
    lim.pop_n(3);
    print(lim);
    lim.clear();
    print(lim, " ", lim.len);

    var name: u8[..16] = "abc";
    name.push('d');
    name.append("ef");
    print(name, " ", name.len, " ", name.cap, " ", name == "abcdef");

    // From slices and other arrays: copied in, checked against the capacity.
    let a = [1, 2, 3, 4, 5];
    var l8: i64[..8] = a;
    print(l8, " ", l8.len, " ", last(l8));
    l8 = a[1..3];
    print(l8, " ", last(l8));
    push_all(l8, [7, 8, 9]);
    print(l8, " ", sum(l8[1..^1]));
    let small: i64[..2] = a[3..];
    print(small, " ", small.cap);

    // Equality compares the live elements, whatever the dead slots hold.
    var x: i64[..4] = [1, 2, 3];
    var y: i64[..4] = [1, 2];
    print(x == y, " ", x.pop() == 3, " ", x == y);
    y.push(4);
    y.pop();
    print(x == y, " ", x != [1, 2]);

    var bag = Bag { label: "bag", items: [], count: 0 };
    bag.items.push(Item { id: 1, weight: 0.5 });
    bag.items.push(Item { id: 2, weight: 1.25 });
    bag.count = bag.items.len as u8;
    bag.items[1].weight *= 2.0;
    print(bag);
    for &it in bag.items { it.id += 10; }
    for it, i in bag.items { print(i, " ", it.id, " ", it.weight); }
    var copy2 = bag;
    copy2.items.pop();
    print(copy2.items.len, " ", bag.items.len, " ", copy2 == bag);

    // A limited array of 300 slots stores its length in a u16.
    var wide: u8[..300] = [];
    for i in 0..300 { wide.push(i as! u8); }
    print(wide.len, " ", wide[299], " ", wide.cap);
    wide.resize(2);
    print(wide.len, " ", wide[1]);
}
