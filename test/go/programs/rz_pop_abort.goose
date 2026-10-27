// pop on an empty resizable array aborts (GS_E_POP): the stack's top would
// drop below the array's elements.
fn take(xs: i64[>..<]&) -> i64 { xs.pop() }

fn main() {
    var g: i64[>..<] = [1, 2];
    print(take(g), " ", take(g), " ", g.len);
    g.push(3);
    print(take(g));
    print(take(g));
    print("not reached");
}
