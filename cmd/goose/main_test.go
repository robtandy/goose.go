package main

import (
	"archive/tar"
	"bytes"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"fmt"
	"net/http"
	"net/http/httptest"
	"net/url"
	"os"
	"os/exec"
	"path/filepath"
	"runtime"
	"runtime/debug"
	"sort"
	"strings"
	"sync"
	"testing"
)

// The compiler of the releases the tests serve: a script that prints its
// arguments and the standard library beside it, and exits with 3.
const fakeGoose = "#!/bin/sh\necho \"goose $*\"\ncat \"$(dirname \"$0\")/stdlib/std.goose\"\nexit 3\n"

func TestMain(m *testing.M) {
	// The command itself, which TestRunsTheCompiler runs.
	if os.Getenv("GOOSE_WRAPPER_TEST_MAIN") == "1" {
		main()
		return
	}
	os.Exit(m.Run())
}

func platform() string { return runtime.GOOS + "-" + runtime.GOARCH }

func archiveName(release string) string { return "goose-" + release + "-" + platform() + ".tar.gz" }

// An entry of an archive: a name ending in / is a directory.
type entry struct {
	name, body string
	mode       int64
	typ        byte
}

func makeArchive(t *testing.T, entries ...entry) []byte {
	t.Helper()
	var buf bytes.Buffer
	zw := gzip.NewWriter(&buf)
	tw := tar.NewWriter(zw)
	for _, e := range entries {
		h := &tar.Header{Name: e.name, Mode: e.mode, Typeflag: e.typ, Size: int64(len(e.body))}
		if h.Typeflag == 0 {
			h.Typeflag = tar.TypeReg
			if strings.HasSuffix(e.name, "/") {
				h.Typeflag = tar.TypeDir
			}
		}
		if h.Typeflag != tar.TypeReg {
			h.Size = 0
		}
		if h.Typeflag == tar.TypeSymlink {
			h.Linkname = e.body
		}
		if err := tw.WriteHeader(h); err != nil {
			t.Fatal(err)
		}
		if h.Typeflag == tar.TypeReg {
			if _, err := tw.Write([]byte(e.body)); err != nil {
				t.Fatal(err)
			}
		}
	}
	if err := tw.Close(); err != nil {
		t.Fatal(err)
	}
	if err := zw.Close(); err != nil {
		t.Fatal(err)
	}
	return buf.Bytes()
}

// releaseArchive is the archive of a release as scripts/release.py lays
// one out: one directory of the compiler and the standard library.
func releaseArchive(t *testing.T, release string) []byte {
	top := strings.TrimSuffix(archiveName(release), ".tar.gz")
	return makeArchive(t, entry{name: top + "/"}, entry{name: top + "/goose", body: fakeGoose, mode: 0o755},
		entry{name: top + "/stdlib/"}, entry{name: top + "/stdlib/std.goose", body: "// std\n", mode: 0o644},
		entry{name: top + "/LICENSE", body: "license\n", mode: 0o644})
}

func sumOf(data []byte) string {
	h := sha256.Sum256(data)
	return hex.EncodeToString(h[:])
}

// sumsOf is SHA256SUMS of the files, as sha256sum writes it.
func sumsOf(files map[string][]byte) []byte {
	var names []string
	for n := range files {
		names = append(names, n)
	}
	sort.Strings(names)
	var b bytes.Buffer
	for _, n := range names {
		fmt.Fprintf(&b, "%s  %s\n", sumOf(files[n]), n)
	}
	return b.Bytes()
}

// A server of releases, which records the paths asked for.
type server struct {
	*httptest.Server
	mu    sync.Mutex
	files map[string][]byte
	hits  []string
	// handle, where set, answers first: true where it has.
	handle func(w http.ResponseWriter, r *http.Request) bool
}

func newServer(t *testing.T) *server {
	s := &server{files: map[string][]byte{}}
	s.Server = httptest.NewServer(http.HandlerFunc(func(w http.ResponseWriter, r *http.Request) {
		s.mu.Lock()
		s.hits = append(s.hits, r.URL.Path)
		data, ok := s.files[r.URL.Path]
		handle := s.handle
		s.mu.Unlock()
		if handle != nil && handle(w, r) {
			return
		}
		if !ok {
			http.NotFound(w, r)
			return
		}
		w.Write(data)
	}))
	t.Cleanup(s.Close)
	return s
}

// addRelease serves the release's files under prefix/release/, with
// SHA256SUMS of them.
func (s *server) addRelease(prefix, release string, files map[string][]byte) {
	s.mu.Lock()
	defer s.mu.Unlock()
	for n, data := range files {
		s.files[prefix+"/"+release+"/"+n] = data
	}
	s.files[prefix+"/"+release+"/"+sumsName] = sumsOf(files)
}

func (s *server) takeHits() []string {
	s.mu.Lock()
	defer s.mu.Unlock()
	h := s.hits
	s.hits = nil
	return h
}

func envOf(m map[string]string) func(string) string { return func(k string) string { return m[k] } }

func run(t *testing.T, env map[string]string) (string, string, error) {
	t.Helper()
	var log bytes.Buffer
	exe, err := compiler(context.Background(), envOf(env), nil, &log)
	return exe, log.String(), err
}

// entries lists what dir holds, each path relative to it.
func entries(t *testing.T, dir string) []string {
	t.Helper()
	var got []string
	filepath.Walk(dir, func(p string, fi os.FileInfo, err error) error {
		if err == nil && p != dir {
			rel, _ := filepath.Rel(dir, p)
			got = append(got, filepath.ToSlash(rel))
		}
		return nil
	})
	return got
}

func TestReleaseOf(t *testing.T) {
	own := func(v string) *debug.BuildInfo {
		return &debug.BuildInfo{Main: debug.Module{Path: modulePath, Version: v}}
	}
	dep := func(m *debug.Module) *debug.BuildInfo {
		return &debug.BuildInfo{Main: debug.Module{Path: "example.com/gen", Version: "(devel)"}, Deps: []*debug.Module{m}}
	}
	for _, c := range []struct {
		what    string
		env     map[string]string
		bi      *debug.BuildInfo
		want    string
		wantErr string
	}{
		{"go run module@version", nil, own("v0.2.0"), "v0.2.0", ""},
		{"a pre-release", nil, own("v0.2.0-rc.1"), "v0.2.0-rc.1", ""},
		{"a pre-release that looks like a commit's", nil, own("v0.0.0-test"), "v0.0.0-test", ""},
		{"go run of a dependency", nil, dep(&debug.Module{Path: modulePath, Version: "v0.2.1"}), "v0.2.1", ""},
		{"a dependency replaced by a version", nil,
			dep(&debug.Module{Path: modulePath, Version: "v0.2.1", Replace: &debug.Module{Path: "example.com/fork/cmd/goose", Version: "v0.2.2"}}),
			"v0.2.2", ""},
		{"GOOSE_RELEASE", map[string]string{"GOOSE_RELEASE": "v0.3.0"}, own("v0.2.0"), "v0.3.0", ""},
		{"GOOSE_RELEASE without a version", map[string]string{"GOOSE_RELEASE": "409c1dc"}, nil, "409c1dc", ""},
		{"no build information", nil, nil, "", "has no version"},
		{"a build of the module's own directory", nil, own("(devel)"), "", "GOOSE_RELEASE"},
		{"a dependency replaced by a directory", nil,
			dep(&debug.Module{Path: modulePath, Version: "v0.2.1", Replace: &debug.Module{Path: "../goose.go/cmd/goose"}}),
			"", "has no version"},
		{"a module that does not require this one", nil, dep(&debug.Module{Path: "example.com/other", Version: "v1.0.0"}), "", "has no version"},
		{"a commit", nil, own("v0.0.0-20261030063300-409c1dc5f2a1"), "", "is a commit"},
		{"a commit after a release", nil, own("v0.2.1-0.20261030063300-409c1dc5f2a1"), "", "is a commit"},
		{"a commit of a work tree with changes", nil, own("v0.2.1-0.20261030063300-409c1dc5f2a1+dirty"), "", "is a commit"},
		{"GOOSE_RELEASE up a directory", map[string]string{"GOOSE_RELEASE": "../v0.2.0"}, nil, "", "names no release"},
		{"GOOSE_RELEASE of a path", map[string]string{"GOOSE_RELEASE": "v0.2.0/x"}, nil, "", "names no release"},
	} {
		got, err := releaseOf(envOf(c.env), c.bi)
		if c.wantErr == "" && (err != nil || got != c.want) {
			t.Errorf("%s: got %q, %v; want %q", c.what, got, err, c.want)
		}
		if c.wantErr != "" && (err == nil || !strings.Contains(err.Error(), c.wantErr)) {
			t.Errorf("%s: got %q, %v; want an error saying %q", c.what, got, err, c.wantErr)
		}
	}
}

func TestParseSums(t *testing.T) {
	a, b := strings.Repeat("ab", 32), strings.Repeat("0F", 32)
	got, err := parseSums([]byte(a + "  goose-v1-linux-amd64.tar.gz\n" + b + " *goose-v1-darwin-arm64.tar.gz\r\n\n"))
	want := map[string]string{"goose-v1-linux-amd64.tar.gz": a, "goose-v1-darwin-arm64.tar.gz": strings.ToLower(b)}
	if err != nil || fmt.Sprint(got) != fmt.Sprint(want) {
		t.Errorf("got %v, %v; want %v", got, err, want)
	}
	for _, bad := range []string{
		"not a checksum\n",
		a + " goose.tar.gz\n",
		a[:63] + "g  goose.tar.gz\n",
		a + "  x\n" + b + "  x\n",
	} {
		if got, err := parseSums([]byte(bad)); err == nil {
			t.Errorf("parseSums(%q) = %v, want an error", bad, got)
		}
	}
}

func TestInstall(t *testing.T) {
	s := newServer(t)
	s.addRelease("", "v0.2.0", map[string][]byte{archiveName("v0.2.0"): releaseArchive(t, "v0.2.0")})
	cache := t.TempDir()
	env := map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_RELEASE_URL": s.URL, "GOOSE_CACHE": cache}
	exe, log, err := run(t, env)
	if err != nil {
		t.Fatal(err)
	}
	if want := filepath.Join(cache, "v0.2.0", platform(), "goose"); exe != want {
		t.Errorf("the compiler is %s, want %s", exe, want)
	}
	if fi, err := os.Stat(exe); err != nil || fi.Mode().Perm()&0o111 == 0 {
		t.Errorf("the compiler is not executable: %v, %v", fi, err)
	}
	if data, err := os.ReadFile(filepath.Join(filepath.Dir(exe), "stdlib", "std.goose")); err != nil || string(data) != "// std\n" {
		t.Errorf("the standard library is not beside the compiler: %q, %v", data, err)
	}
	if want := []string{"/v0.2.0/" + sumsName, "/v0.2.0/" + archiveName("v0.2.0")}; fmt.Sprint(s.takeHits()) != fmt.Sprint(want) {
		t.Errorf("the release was not read as %v", want)
	}
	if want := "goose: downloading " + s.URL + "/v0.2.0/" + archiveName("v0.2.0") + "\n"; log != want {
		t.Errorf("logged %q, want %q", log, want)
	}
	if got, want := entries(t, filepath.Join(cache, "v0.2.0")), []string{
		platform(), platform() + "/LICENSE", platform() + "/goose", platform() + "/stdlib", platform() + "/stdlib/std.goose",
	}; fmt.Sprint(got) != fmt.Sprint(want) {
		t.Errorf("the cache holds %v, want %v", got, want)
	}
	// Kept: nothing is downloaded again, nor logged.
	if again, log, err := run(t, env); err != nil || again != exe || log != "" || len(s.takeHits()) != 0 {
		t.Errorf("run again: %s, %q, %v, want %s from the cache", again, log, err, exe)
	}
}

// A directory where the compiler goes that holds no compiler is not one,
// and is left for whoever made it.
func TestNotACompiler(t *testing.T) {
	s := newServer(t)
	s.addRelease("", "v0.2.0", map[string][]byte{archiveName("v0.2.0"): releaseArchive(t, "v0.2.0")})
	cache := t.TempDir()
	other := filepath.Join(cache, "v0.2.0", platform(), "notes.txt")
	os.MkdirAll(filepath.Dir(other), 0o755)
	os.WriteFile(other, []byte("mine\n"), 0o644)
	_, _, err := run(t, map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_RELEASE_URL": s.URL, "GOOSE_CACHE": cache})
	if err == nil || !strings.Contains(err.Error(), "putting release v0.2.0 in its place: ") {
		t.Errorf("got %v, want the release refused its place", err)
	}
	if got := entries(t, filepath.Join(cache, "v0.2.0")); fmt.Sprint(got) != fmt.Sprint([]string{platform(), platform() + "/notes.txt"}) {
		t.Errorf("the cache holds %v", got)
	}
}

func TestInstallFromDirectory(t *testing.T) {
	dir := t.TempDir()
	files := map[string][]byte{archiveName("v0.2.0"): releaseArchive(t, "v0.2.0")}
	files[sumsName] = sumsOf(files)
	os.Mkdir(filepath.Join(dir, "v0.2.0"), 0o755)
	for n, data := range files {
		if err := os.WriteFile(filepath.Join(dir, "v0.2.0", n), data, 0o644); err != nil {
			t.Fatal(err)
		}
	}
	for _, u := range []string{dir, (&url.URL{Scheme: "file", Path: filepath.ToSlash(dir)}).String()} {
		cache := t.TempDir()
		exe, _, err := run(t, map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_RELEASE_URL": u, "GOOSE_CACHE": cache})
		if err != nil || !isFile(exe) {
			t.Errorf("GOOSE_RELEASE_URL=%s: %s, %v", u, exe, err)
		}
	}
	_, _, err := run(t, map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_RELEASE_URL": "ftp://example.com/goose", "GOOSE_CACHE": t.TempDir()})
	if err == nil || !strings.Contains(err.Error(), "an http, https or file URL, or in a directory") {
		t.Errorf("GOOSE_RELEASE_URL=ftp://...: %v, want it refused", err)
	}
}

func TestDefaultCache(t *testing.T) {
	s := newServer(t)
	s.addRelease("", "v0.2.0", map[string][]byte{archiveName("v0.2.0"): releaseArchive(t, "v0.2.0")})
	home := t.TempDir()
	t.Setenv("HOME", home)
	t.Setenv("XDG_CACHE_HOME", "")
	t.Setenv("LocalAppData", home)
	user, err := os.UserCacheDir()
	if err != nil {
		t.Skip(err)
	}
	exe, _, err := run(t, map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_RELEASE_URL": s.URL})
	if want := filepath.Join(user, "goose.go", "v0.2.0", platform(), "goose"); err != nil || exe != want {
		t.Errorf("got %s, %v; want %s", exe, err, want)
	}
}

func TestChecksums(t *testing.T) {
	good := releaseArchive(t, "v0.2.0")
	for _, c := range []struct {
		what    string
		files   map[string][]byte // under /v0.2.0/
		wantErr string
	}{
		{"an archive that is not the one SHA256SUMS names",
			map[string][]byte{sumsName: []byte(strings.Repeat("0", 64) + "  " + archiveName("v0.2.0") + "\n"), archiveName("v0.2.0"): good},
			"is not the archive the release's SHA256SUMS names: its SHA-256 is " + sumOf(good) + ", not " + strings.Repeat("0", 64)},
		{"no compiler for this platform",
			map[string][]byte{sumsName: sumsOf(map[string][]byte{"goose-v0.2.0-plan9-mips.tar.gz": good, "goose-v0.2.0-aix-ppc64.tar.gz": good})},
			"release v0.2.0 has no compiler for " + runtime.GOOS + "/" + runtime.GOARCH + ": its SHA256SUMS lists those for aix/ppc64, plan9/mips"},
		{"an archive SHA256SUMS lists that is not there",
			map[string][]byte{sumsName: sumsOf(map[string][]byte{archiveName("v0.2.0"): good})},
			"the release's SHA256SUMS lists " + archiveName("v0.2.0") + ", which is not there"},
		{"SHA256SUMS that is not sha256sum's",
			map[string][]byte{sumsName: []byte("<html>\n")},
			"SHA256SUMS: line 1 is not a SHA-256 and a file's name"},
		{"no release", map[string][]byte{}, "no release v0.2.0: there is no "},
	} {
		s := newServer(t)
		for n, data := range c.files {
			s.files["/v0.2.0/"+n] = data
		}
		cache := t.TempDir()
		_, _, err := run(t, map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_RELEASE_URL": s.URL, "GOOSE_CACHE": cache})
		if err == nil || !strings.Contains(err.Error(), c.wantErr) {
			t.Errorf("%s: %v, want an error saying %q", c.what, err, c.wantErr)
		}
		if got := entries(t, cache); len(got) > 1 || len(got) == 1 && got[0] != "v0.2.0" {
			t.Errorf("%s: the cache holds %v, want nothing", c.what, got)
		}
	}
}

func TestUnsafeArchives(t *testing.T) {
	top := strings.TrimSuffix(archiveName("v0.2.0"), ".tar.gz")
	goose := entry{name: top + "/goose", body: fakeGoose, mode: 0o755}
	for _, c := range []struct {
		what    string
		entries []entry
		wantErr string
	}{
		{"a file up a directory", []entry{goose, {name: top + "/../escaped", body: "x"}}, "is outside the directory"},
		{"a file at an absolute path", []entry{goose, {name: "/tmp/escaped", body: "x"}}, "is outside the directory"},
		{"two directories", []entry{goose, {name: "other/goose", body: "x"}}, "is outside the directory"},
		{"a symbolic link", []entry{goose, {name: top + "/stdlib", body: "/etc", typ: tar.TypeSymlink}}, "neither a directory nor a file"},
		{"a file that is the directory", []entry{{name: top, body: "x"}}, "neither a directory nor a file"},
		{"no compiler", []entry{{name: top + "/stdlib/std.goose", body: "x"}}, "holds no compiler"},
		{"nothing", nil, "the archive is empty"},
	} {
		s := newServer(t)
		s.addRelease("", "v0.2.0", map[string][]byte{archiveName("v0.2.0"): makeArchive(t, c.entries...)})
		root := t.TempDir()
		cache := filepath.Join(root, "cache")
		_, _, err := run(t, map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_RELEASE_URL": s.URL, "GOOSE_CACHE": cache})
		if err == nil || !strings.Contains(err.Error(), c.wantErr) {
			t.Errorf("%s: %v, want an error saying %q", c.what, err, c.wantErr)
		}
		if got := entries(t, root); fmt.Sprint(got) != "[cache cache/v0.2.0]" {
			t.Errorf("%s: left %v", c.what, got)
		}
	}
}

func TestGitHub(t *testing.T) {
	s := newServer(t)
	defer func(web, api string) { githubWeb, githubAPI = web, api }(githubWeb, githubAPI)
	githubWeb, githubAPI = s.URL, s.URL+"/api"
	files := map[string][]byte{archiveName("v0.2.0"): releaseArchive(t, "v0.2.0")}
	files[sumsName] = sumsOf(files)
	// A public repository's release: its files at their URLs.
	s.addRelease("/robtandy/goose.go/releases/download", "v0.2.0", files)
	exe, _, err := run(t, map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_CACHE": t.TempDir()})
	if hits := s.takeHits(); err != nil || !isFile(exe) || len(hits) != 2 {
		t.Errorf("a public release: %s, %v, read as %v", exe, err, hits)
	}
	// A private one's: GitHub's API's, to a token that can read it.
	release := s.URL + "/api/repos/robtandy/goose.go/releases/tags/v0.2.0"
	s.mu.Lock()
	s.files = map[string][]byte{}
	s.handle = func(w http.ResponseWriter, r *http.Request) bool {
		if r.Header.Get("Authorization") != "Bearer secret" || r.Header.Get("X-Github-Api-Version") == "" {
			return false
		}
		if s.URL+r.URL.Path == release && r.Header.Get("Accept") == "application/vnd.github+json" {
			var assets []map[string]string
			for i, n := range []string{"notes.txt", sumsName, archiveName("v0.2.0")} {
				assets = append(assets, map[string]string{"name": n, "url": fmt.Sprintf("%s/api/assets/%d", s.URL, i),
					"browser_download_url": s.URL + "/robtandy/goose.go/releases/download/v0.2.0/" + n})
			}
			json.NewEncoder(w).Encode(map[string]any{"tag_name": "v0.2.0", "assets": assets})
			return true
		}
		if strings.HasPrefix(r.URL.Path, "/api/assets/") && r.Header.Get("Accept") == "application/octet-stream" {
			switch r.URL.Path {
			case "/api/assets/1":
				w.Write(files[sumsName])
			case "/api/assets/2":
				w.Write(files[archiveName("v0.2.0")])
			default:
				http.NotFound(w, r)
			}
			return true
		}
		return false
	}
	s.mu.Unlock()
	_, _, err = run(t, map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_CACHE": t.TempDir()})
	want := "no release v0.2.0: there is no " + s.URL + "/robtandy/goose.go/releases/download/v0.2.0/SHA256SUMS; " +
		"GitHub serves a private repository's releases to a token that can read it, in GH_TOKEN"
	if err == nil || err.Error() != want {
		t.Errorf("a private release without a token: %v, want %q", err, want)
	}
	s.takeHits()
	for _, k := range []string{"GH_TOKEN", "GITHUB_TOKEN"} {
		exe, log, err := run(t, map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_CACHE": t.TempDir(), k: "secret"})
		if err != nil || !isFile(exe) {
			t.Errorf("a private release with %s: %s, %v", k, exe, err)
		}
		if want := "goose: downloading " + archiveName("v0.2.0") + " of " + release + "\n"; log != want {
			t.Errorf("logged %q, want %q", log, want)
		}
		want := []string{"/robtandy/goose.go/releases/download/v0.2.0/SHA256SUMS", "/api/repos/robtandy/goose.go/releases/tags/v0.2.0",
			"/api/assets/1", "/api/assets/2"}
		if hits := s.takeHits(); fmt.Sprint(hits) != fmt.Sprint(want) {
			t.Errorf("read %v, want %v", hits, want)
		}
	}
	_, _, err = run(t, map[string]string{"GOOSE_RELEASE": "v0.3.0", "GOOSE_CACHE": t.TempDir(), "GH_TOKEN": "secret"})
	want = "no release v0.3.0: there is no " + s.URL + "/robtandy/goose.go/releases/download/v0.3.0/SHA256SUMS, " +
		"nor SHA256SUMS of " + s.URL + "/api/repos/robtandy/goose.go/releases/tags/v0.3.0"
	if err == nil || err.Error() != want {
		t.Errorf("no release, with a token: %v, want %q", err, want)
	}
	_, _, err = run(t, map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_CACHE": t.TempDir(), "GH_TOKEN": "wrong"})
	if err == nil || !strings.Contains(err.Error(), "no release v0.2.0") {
		t.Errorf("a token that cannot read the release: %v", err)
	}
}

func TestConcurrentInstalls(t *testing.T) {
	s := newServer(t)
	s.addRelease("", "v0.2.0", map[string][]byte{archiveName("v0.2.0"): releaseArchive(t, "v0.2.0")})
	cache := t.TempDir()
	env := map[string]string{"GOOSE_RELEASE": "v0.2.0", "GOOSE_RELEASE_URL": s.URL, "GOOSE_CACHE": cache}
	var wg sync.WaitGroup
	exes, errs := make([]string, 8), make([]error, 8)
	for i := range exes {
		wg.Add(1)
		go func(i int) {
			defer wg.Done()
			var log bytes.Buffer
			exes[i], errs[i] = compiler(context.Background(), envOf(env), nil, &log)
		}(i)
	}
	wg.Wait()
	for i := range exes {
		if errs[i] != nil || exes[i] != exes[0] {
			t.Errorf("install %d: %s, %v", i, exes[i], errs[i])
		}
	}
	if data, err := os.ReadFile(exes[0]); err != nil || string(data) != fakeGoose {
		t.Errorf("the compiler is %q, %v", data, err)
	}
	if got := entries(t, filepath.Join(cache, "v0.2.0")); len(got) != 5 {
		t.Errorf("the cache holds %v, want the one release", got)
	}
}

// The command as go run runs it: the compiler in its place, with its
// arguments, its output and its exit status.
func TestRunsTheCompiler(t *testing.T) {
	if runtime.GOOS == "windows" {
		t.Skip("the fake compiler is a shell script")
	}
	s := newServer(t)
	s.addRelease("", "v0.2.0", map[string][]byte{archiveName("v0.2.0"): releaseArchive(t, "v0.2.0")})
	cache := t.TempDir()
	command := func(env ...string) (int, string, string) {
		cmd := exec.Command(os.Args[0], "--target=go", "-o", "out file.go", "in.goose")
		cmd.Env = append(os.Environ(), append([]string{"GOOSE_WRAPPER_TEST_MAIN=1", "GOOSE_RELEASE_URL=" + s.URL,
			"GOOSE_CACHE=" + cache, "GOOSE_RELEASE="}, env...)...)
		var stdout, stderr bytes.Buffer
		cmd.Stdout, cmd.Stderr = &stdout, &stderr
		err := cmd.Run()
		code := 0
		if exit, ok := err.(*exec.ExitError); ok {
			code = exit.ExitCode()
		} else if err != nil {
			t.Fatal(err)
		}
		return code, stdout.String(), stderr.String()
	}
	code, stdout, stderr := command("GOOSE_RELEASE=v0.2.0")
	if code != 3 || stdout != "goose --target=go -o out file.go in.goose\n// std\n" ||
		stderr != "goose: downloading "+s.URL+"/v0.2.0/"+archiveName("v0.2.0")+"\n" {
		t.Errorf("exit %d, stdout %q, stderr %q", code, stdout, stderr)
	}
	if code, stdout, stderr = command("GOOSE_RELEASE=v0.2.0"); code != 3 || stderr != "" {
		t.Errorf("run again: exit %d, stdout %q, stderr %q", code, stdout, stderr)
	}
	// The test's own build names no release.
	if code, stdout, stderr = command(); code != 1 || stdout != "" ||
		!strings.HasPrefix(stderr, "goose: ") || !strings.Contains(stderr, "name the release with GOOSE_RELEASE") {
		t.Errorf("no version: exit %d, stdout %q, stderr %q", code, stdout, stderr)
	}
	if code, stdout, stderr = command("GOOSE_RELEASE=v9.9.9"); code != 1 || stdout != "" ||
		stderr != "goose: no release v9.9.9: there is no "+s.URL+"/v9.9.9/SHA256SUMS\n" {
		t.Errorf("no release: exit %d, stdout %q, stderr %q", code, stdout, stderr)
	}
}
