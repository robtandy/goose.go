//go:build !goose_import

package goose

// The runtime's count of its mappings, which the file the compiler wrote
// holds. A library compiled with --go-runtime=import has it in the runtime's
// package (import_shim_test.go, which test/run_go_tests.py adds).
func mapCount() int64 { return gsMaps.Load() }
