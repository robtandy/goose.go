// Where export fns grow a global on a data stack of its own, each call runs
// on a program instance of its block's own (issue #25): a copy of the
// package's globals, whose values on data stacks are on the block's. A
// reference or a slice that the program's start points into the package's
// storage, as `first` into the package's arena, would go on pointing there
// in every copy, which each call would read and write through, sharing it
// with every other: such a global is refused. One the start leaves null, or
// viewing static data, is copied as it is (test/go/export/arena/'s
// `newest`).
var arena: i64[>..] = [1, 2, 3];
var first: i64& = arena[0];

export fn bump() -> i64 {
    first += 1;
    first
}

export fn grow(x: i64) -> i64 {
    arena.push(x);
    arena.len
}
// error: export fn bump, whose calls reach the global first, which may hold a reference or a slice into the package's storage as the program starts
