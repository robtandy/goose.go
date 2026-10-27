// pop on an empty limited array aborts (GS_E_POP); pop_n of more than the
// length, and resize to a negative length, would too.
fn main() {
    var l: u8[..4] = [1, 2];
    print(l.pop(), " ", l.pop(), " ", l.len);
    l.push(3);
    l.pop_n(1);
    print(l.len);
    print(l.pop());
    print("not reached");
}
