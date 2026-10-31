package goose

import (
	"bytes"
	"io"
	"math"
	"os"
	"path/filepath"
	"runtime"
	"strings"
	"sync"
	"syscall"
	"time"
	"unsafe"
)

// ---------------------------------------------------------------------------
// The OS primitives behind stdlib/os.goose (runtime_os.h), as the extern
// fns there name them, gs_os_read_file being GsOsReadFile. A path, or an
// environment variable's name, is UTF-8 bytes, fewer than gsOsPathMax of
// them and none of them NUL, where an OS call would end it: any other is
// refused, since read another way it could name another file. Text goes
// into the builders the program hands them (GsBldAppend), and nothing on a
// failure.

const gsOsPathMax = 4096

// gsOsPath is the Go string of a path, false where it is refused
// (gs_os_native).
func gsOsPath(s GsSlice) (string, bool) {
	b := GsSliceToBytes(s)
	if len(b) >= gsOsPathMax {
		return "", false
	}
	for _, c := range b {
		if c == 0 {
			return "", false
		}
	}
	return string(b), true
}

// gsOsKind is what a path names: 0 nothing that can be reached, 1 something
// other than a directory, 2 a directory (gs_os_kind).
func gsOsKind(p string) int {
	st, err := os.Stat(p)
	if err != nil {
		return 0
	}
	if st.IsDir() {
		return 2
	}
	return 1
}

// gsBldReadAll appends all of r to out: false on a read error, with out as
// it was.
func gsBldReadAll(gs *GsStackBlock, out GsRref, r io.Reader) bool {
	n0, top0 := out.Hdr.Len, gs.Stks[out.Stk]
	buf := make([]byte, 65536)
	for {
		n, err := r.Read(buf)
		GsBldAppend(gs, out, buf[:n])
		if err == io.EOF {
			return true
		}
		if err != nil {
			out.Hdr.Len, gs.Stks[out.Stk] = n0, top0
			return false
		}
	}
}

// GsOsReadFile appends the whole file to out.
func GsOsReadFile(gs *GsStackBlock, path GsSlice, out GsRref) bool {
	p, ok := gsOsPath(path)
	if !ok {
		return false
	}
	f, err := os.Open(p)
	if err != nil {
		return false
	}
	defer f.Close()
	return gsBldReadAll(gs, out, f)
}

// GsOsWriteFile and GsOsAppendFile write data to a file, which they create
// where there is none.
func GsOsWriteFile(path, data GsSlice) bool { return gsOsPutFile(path, data, os.O_TRUNC) }

func GsOsAppendFile(path, data GsSlice) bool { return gsOsPutFile(path, data, os.O_APPEND) }

func gsOsPutFile(path, data GsSlice, mode int) bool {
	p, ok := gsOsPath(path)
	if !ok {
		return false
	}
	f, err := os.OpenFile(p, os.O_WRONLY|os.O_CREATE|mode, 0666)
	if err != nil {
		return false
	}
	_, err = f.Write(GsSliceToBytes(data))
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	return err == nil
}

// GsOsWriteFileAtomic writes data to a new file beside path, flushes it to
// the disk and renames it over path, so that path holds its old contents or
// all of the new ones whatever happens meanwhile; on a failure the new file
// is removed. Its name is path's with a random tag and .tmp after, created
// only where nothing has that name already, and it takes the permissions
// of the file it replaces.
func GsOsWriteFileAtomic(path, data GsSlice) bool {
	p, ok := gsOsPath(path)
	if !ok || p == "" {
		return false
	}
	for tries := 0; tries < 8; tries++ {
		t := p + "." + gsHex16(GsOsRandomU64()) + ".tmp"
		f, err := os.OpenFile(t, os.O_WRONLY|os.O_CREATE|os.O_EXCL, 0666)
		if os.IsExist(err) {
			continue
		}
		if err != nil {
			return false
		}
		if st, err := os.Stat(p); err == nil {
			ok = f.Chmod(st.Mode().Perm()) == nil
		}
		if ok {
			_, err = f.Write(GsSliceToBytes(data))
			ok = err == nil
		}
		ok = ok && f.Sync() == nil
		if f.Close() != nil {
			ok = false
		}
		ok = ok && syscall.Rename(t, p) == nil
		if ok {
			gsOsSyncDir(p)
		} else {
			_ = syscall.Unlink(t)
		}
		return ok
	}
	return false
}

// gsHex16 is v in 16 hex digits.
func gsHex16(v uint64) string {
	var b [16]byte
	for i := range b {
		b[i] = "0123456789abcdef"[v>>(60-4*uint(i))&15]
	}
	return string(b[:])
}

// gsOsSyncDir puts a rename on the disk, flushing the directory that holds
// the new name; best effort, since the rename has happened either way.
func gsOsSyncDir(p string) {
	d, err := os.Open(filepath.Dir(p))
	if err != nil {
		return
	}
	_ = d.Sync()
	_ = d.Close()
}

// GsOsFileExists says whether path names something that is not a directory.
func GsOsFileExists(path GsSlice) bool {
	p, ok := gsOsPath(path)
	return ok && gsOsKind(p) == 1
}

// GsOsRemoveFile removes a file, and nothing else (unlink, not remove).
func GsOsRemoveFile(path GsSlice) bool {
	p, ok := gsOsPath(path)
	return ok && syscall.Unlink(p) == nil
}

// GsOsRenameFile replaces to, if there is one, in one step: to names the old
// file or the moved one, never neither.
func GsOsRenameFile(from, to GsSlice) bool {
	f, ok := gsOsPath(from)
	t, ok2 := gsOsPath(to)
	if !ok || !ok2 || syscall.Rename(f, t) != nil {
		return false
	}
	gsOsSyncDir(t)
	return true
}

// GsOsMakeDir is true where path is a directory afterwards, one that was
// there already included.
func GsOsMakeDir(path GsSlice) bool {
	p, ok := gsOsPath(path)
	if !ok {
		return false
	}
	return os.Mkdir(p, 0777) == nil || gsOsKind(p) == 2
}

func GsOsIsDir(path GsSlice) bool {
	p, ok := gsOsPath(path)
	return ok && gsOsKind(p) == 2
}

// GsOsRemoveDir removes an empty directory.
func GsOsRemoveDir(path GsSlice) bool {
	p, ok := gsOsPath(path)
	return ok && syscall.Rmdir(p) == nil
}

// GsOsListDir appends the name of every entry of a directory but . and ..,
// each followed by a newline, in bytewise order, so that a listing does not
// depend on the file system. A name holding a newline cannot be a line of
// it, and is left out.
func GsOsListDir(gs *GsStackBlock, path GsSlice, out GsRref) bool {
	p, ok := gsOsPath(path)
	if !ok || p == "" {
		return false
	}
	entries, err := os.ReadDir(p)
	if err != nil {
		return false
	}
	for _, e := range entries {
		name := e.Name()
		if strings.IndexByte(name, '\n') >= 0 {
			continue
		}
		GsBldAppendString(gs, out, name)
		GsBldAppendString(gs, out, "\n")
	}
	return true
}

// The standard streams. Go's are unbuffered, as print's writes are, so
// flushing has nothing to do; stdin is read through a buffer of its own,
// which read_line and read_stdin share, as C's share stdio's.
func GsOsWriteStdout(s GsSlice) { _, _ = os.Stdout.Write(GsSliceToBytes(s)) }

func GsOsWriteStderr(s GsSlice) { _, _ = os.Stderr.Write(GsSliceToBytes(s)) }

func GsOsFlushStdout() {}

var gsStdin struct {
	mu       sync.Mutex
	buf      []byte
	pos, end int
}

// gsStdinFill reads more of stdin into its buffer: false at its end.
func gsStdinFill() bool {
	if gsStdin.buf == nil {
		gsStdin.buf = make([]byte, 65536)
	}
	n, _ := os.Stdin.Read(gsStdin.buf)
	gsStdin.pos, gsStdin.end = 0, n
	return n > 0
}

// GsOsReadLine appends one line of stdin, without its newline or a carriage
// return before it: false at the end of the input. A last line that ends
// at the end of the input, with no newline, keeps a carriage return it
// ends with, as C's gs_os_read_line does.
func GsOsReadLine(gs *GsStackBlock, out GsRref) bool {
	gsStdin.mu.Lock()
	defer gsStdin.mu.Unlock()
	if gsStdin.pos == gsStdin.end && !gsStdinFill() {
		return false
	}
	start := out.Hdr.Len
	newline := false
	for {
		chunk := gsStdin.buf[gsStdin.pos:gsStdin.end]
		if i := bytes.IndexByte(chunk, '\n'); i >= 0 {
			GsBldAppend(gs, out, chunk[:i])
			gsStdin.pos += i + 1
			newline = true
			break
		}
		GsBldAppend(gs, out, chunk)
		gsStdin.pos = gsStdin.end
		if !gsStdinFill() {
			break
		}
	}
	if newline && out.Hdr.Len > start && *(*byte)(unsafe.Add(out.Hdr.Base, out.Hdr.Len-1)) == '\r' {
		out.Hdr.Len--
		gs.Stks[out.Stk] = unsafe.Add(gs.Stks[out.Stk], -1)
	}
	return true
}

// GsOsReadStdin appends everything until the end of the input.
func GsOsReadStdin(gs *GsStackBlock, out GsRref) {
	gsStdin.mu.Lock()
	defer gsStdin.mu.Unlock()
	for gsStdin.pos < gsStdin.end || gsStdinFill() {
		GsBldAppend(gs, out, gsStdin.buf[gsStdin.pos:gsStdin.end])
		gsStdin.pos = gsStdin.end
	}
}

// The program's arguments, the program itself first, and its environment.
func GsOsArgCount() int64 { return int64(len(os.Args)) }

func GsOsArg(gs *GsStackBlock, i int64, out GsRref) {
	if i >= 0 && i < int64(len(os.Args)) {
		GsBldAppendString(gs, out, os.Args[i])
	}
}

func GsOsGetenv(gs *GsStackBlock, name GsSlice, out GsRref) bool {
	n, ok := gsOsPath(name)
	if !ok {
		return false
	}
	v, ok := os.LookupEnv(n)
	if ok {
		GsBldAppendString(gs, out, v)
	}
	return ok
}

// GsOsResourceDir appends the application's resource directory, with its
// final separator: the directory of the running executable (Resources/ in a
// macOS .app), or dir where that is not empty, the entry source's for
// `goose run`, as a JIT run has it (gsJitResourceDir).
func GsOsResourceDir(gs *GsStackBlock, dir string, out GsRref) bool {
	if dir == "" {
		exe, err := os.Executable()
		if err == nil {
			exe, err = filepath.EvalSymlinks(exe)
		}
		if err != nil || len(exe) >= gsOsPathMax {
			return false
		}
		dir = exe[:strings.LastIndexByte(exe, '/')+1]
		if dir == "" {
			return false
		}
		if runtime.GOOS == "darwin" && strings.HasSuffix(dir, ".app/Contents/MacOS/") {
			dir = dir[:len(dir)-len("MacOS/")] + "Resources/"
		}
	}
	GsBldAppendString(gs, out, dir)
	return true
}

// Time: the wall clock in nanoseconds since the Unix epoch, a monotonic
// clock for intervals, and a sleep of whole milliseconds.
var gsClockStart = time.Now()

func GsOsTimeNs() int64 { return time.Now().UnixNano() }

func GsOsClockNs() int64 { return int64(time.Since(gsClockStart)) }

func GsOsSleepMs(ms int64) {
	if ms <= 0 {
		return
	}
	if ms > math.MaxInt64/int64(time.Millisecond) {
		ms = math.MaxInt64 / int64(time.Millisecond)
	}
	time.Sleep(time.Duration(ms) * time.Millisecond)
}

// GsOsRandomU64 is entropy for seeding a PRNG, not for cryptography:
// /dev/urandom's, or else a mix of the clocks and of which call this is.
var gsRandomCalls struct {
	mu sync.Mutex
	n  uint64
}

func gsMix64(x uint64) uint64 {
	x += 0x9e3779b97f4a7c15
	x = (x ^ x>>30) * 0xbf58476d1ce4e5b9
	x = (x ^ x>>27) * 0x94d049bb133111eb
	return x ^ x>>31
}

func GsOsRandomU64() uint64 {
	if f, err := os.Open("/dev/urandom"); err == nil {
		var b [8]byte
		n, _ := io.ReadFull(f, b[:])
		_ = f.Close()
		if n == 8 {
			var v uint64
			for i := range b {
				v |= uint64(b[i]) << (8 * uint(i))
			}
			return v
		}
	}
	gsRandomCalls.mu.Lock()
	gsRandomCalls.n++
	count := gsRandomCalls.n
	gsRandomCalls.mu.Unlock()
	seed := gsMix64(uint64(GsOsTimeNs()) ^ uint64(GsOsClockNs()))
	seed = gsMix64(seed ^ uint64(os.Getpid()))
	return gsMix64(seed ^ count)
}

// GsTrimMemory is os.trim_memory: the complete pages above each stack's
// top in gs, its fixed-frame stack's included, given back to the OS, their
// contents read as zeros if they are used again; false where the OS refused
// (gs_trim_memory).
func GsTrimMemory(gs *GsStackBlock) bool {
	page := uintptr(syscall.Getpagesize())
	ok := true
	trim := func(r []byte, top unsafe.Pointer, usable uintptr) {
		if r == nil {
			return
		}
		from := (uintptr(top) - uintptr(unsafe.Pointer(unsafe.SliceData(r))) + page - 1) &^ (page - 1)
		if from < usable && syscall.Madvise(r[from:usable], syscall.MADV_DONTNEED) != nil {
			ok = false
		}
	}
	for k, r := range gs.regions {
		trim(r, gs.Stks[k], uintptr(gs.Cap[k]))
	}
	trim(gs.frame, gs.Frame, uintptr(len(gs.frame)))
	return ok
}
