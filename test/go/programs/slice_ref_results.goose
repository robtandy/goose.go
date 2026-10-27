// A returned reference to a slice (an address in Go) read as the slice,
// through a call that stays one at every optimization level.
var views: (i64[:])[>..] = [];
var nums: i64[>..] = [];
recursive fn view(i: i64, depth: i64) -> (i64[:])& {
    if depth > 0 { return view(i, depth - 1); }
    return views[i];
}
fn main() {
    for k in 5 { nums.push(k * 11); }
    views.push(nums[1..3]);
    views.push(nums[..]);
    var grown: i64[>..] = view(0, 2);
    grown.push(7);
    print(grown, " ", grown.len);
    let all: i64[>..] = view(1, 3);
    print(all, " ", view(1, 1).len);
}
