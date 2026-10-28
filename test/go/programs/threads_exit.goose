// exit(code) in a worker ends the program with that status at once
// (§9.3), as it does in main.
thread_fn quitter(code: i64) {
    print("worker exiting");
    exit(code);
}

fn main() {
    print("start");
    thread_wait(thread_spawn(quitter, 3));
    print("not reached");
}
