# examples/records

The worked example of [`docs/go-integration.md`](../../docs/go-integration.md): package `records` counts the records of a segment, running its Go original (`count.go`) by default and a Goose kernel (`_goose/records.goose`) when built with `-tags goose`.

```bash
go test ./...                       # the Go original
go generate -tags goose ./...       # goose on PATH: writes records_goose.go
go test -tags goose ./...           # the kernel, held to the original
```

In your module `records_goose.go` is checked in. Here it is not: this repository's compiler is built from each commit, which the file's header names, so a file checked in would be stale at the next one. `python3 test/run_go_tests.py example` generates it, checks it and runs the tests of both builds.
