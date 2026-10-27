// Slice bounds outside the length abort (GS_E_SLICE).
fn cut(s: const u8[:], lo: i64, hi: i64) -> const u8[:] { s[lo..hi] }

fn main() {
    let msg = "hello";
    print(cut(msg, 1, 3), "|", cut(msg, 5, 5), "|", cut(msg, 0, 5));
    print(cut(msg, 3, 2));
    print("not reached");
}
