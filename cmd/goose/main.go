// Command goose runs the Goose compiler of goose.go without building it:
// the prebuilt compiler of the release this module's version names, which
// it downloads the first time, checks against the release's SHA256SUMS and
// keeps in a cache. With nothing but Go installed,
//
//	//go:generate go run github.com/robtandy/goose.go/cmd/goose@v0.2.0 --target=go -o stats.go stats.goose
//
// runs goose v0.2.0 on the arguments that follow, in the directory go
// generate runs it in, and exits as it does: the version pins the
// compiler, and with it the Go it writes, whose header records it.
//
// Releases hold compilers for linux/amd64, linux/arm64 and darwin/arm64
// (scripts/release.py, docs/releasing.md). The environment can say where
// one comes from:
//
//	GOOSE_RELEASE      the release to run, in place of the module's version
//	GOOSE_RELEASE_URL  where the releases are, each in a directory named
//	                   after it: an http, https or file URL, or a directory
//	                   (by default the GitHub releases of robtandy/goose.go)
//	GOOSE_CACHE        where releases are kept once downloaded (by default
//	                   goose.go in the user's cache directory)
//	GH_TOKEN, GITHUB_TOKEN  a GitHub token, with which a release GitHub
//	                   serves to no one else, as a private repository's,
//	                   is downloaded through GitHub's API
package main

import (
	"archive/tar"
	"compress/gzip"
	"context"
	"crypto/sha256"
	"encoding/hex"
	"encoding/json"
	"errors"
	"fmt"
	"io"
	"io/fs"
	"net/http"
	"net/url"
	"os"
	"path/filepath"
	"regexp"
	"runtime"
	"runtime/debug"
	"sort"
	"strings"
	"time"
)

const (
	// The module this command is, whose version names the release it runs.
	modulePath = "github.com/robtandy/goose.go/cmd/goose"
	// The repository whose GitHub releases hold the compilers.
	repository = "robtandy/goose.go"
	// A release's checksums: sha256sum's lines, one for each archive.
	sumsName = "SHA256SUMS"
	// The most an archive may hold, as it is downloaded.
	maxArchive = 1 << 30
)

// Where GitHub is, which the tests replace.
var (
	githubWeb = "https://github.com"
	githubAPI = "https://api.github.com"
)

var client = &http.Client{Timeout: 10 * time.Minute}

func main() {
	bi, _ := debug.ReadBuildInfo()
	exe, err := compiler(context.Background(), os.Getenv, bi, os.Stderr)
	if err != nil {
		fmt.Fprintf(os.Stderr, "goose: %v\n", err)
		os.Exit(1)
	}
	err = execCompiler(exe, os.Args[1:])
	fmt.Fprintf(os.Stderr, "goose: running %s: %v\n", exe, err)
	os.Exit(1)
}

// compiler is the path of the compiler of the release that the
// environment and the build name, for this platform, in the cache: where
// it is not there yet, it is downloaded, checked and put there first.
func compiler(ctx context.Context, getenv func(string) string, bi *debug.BuildInfo, log io.Writer) (string, error) {
	release, err := releaseOf(getenv, bi)
	if err != nil {
		return "", err
	}
	cache, err := cacheDir(getenv)
	if err != nil {
		return "", err
	}
	platform := runtime.GOOS + "-" + runtime.GOARCH
	dir := filepath.Join(cache, release, platform)
	exe := filepath.Join(dir, "goose")
	if isFile(exe) {
		return exe, nil
	}
	srcs, hint, err := sources(getenv, release)
	if err != nil {
		return "", err
	}
	if err := install(ctx, srcs, hint, release, platform, dir, log); err != nil {
		return "", err
	}
	return exe, nil
}

// pseudoVersion is the form of the version the go command gives a commit
// that no tag names (golang.org/x/mod/module's IsPseudoVersion), and
// releaseName what a release can be named: one element of a URL's path.
var (
	pseudoVersion = regexp.MustCompile(`^v[0-9]+\.(0\.0-|\d+\.\d+-([^+]*\.)?0\.)\d{14}-[A-Za-z0-9]+(\+[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$`)
	releaseName   = regexp.MustCompile(`^[0-9A-Za-z][0-9A-Za-z._+-]*$`)
)

// releaseOf is the release to run: GOOSE_RELEASE, else the version of
// this module the build has, which `go run module@version` gives it.
func releaseOf(getenv func(string) string, bi *debug.BuildInfo) (string, error) {
	if r := getenv("GOOSE_RELEASE"); r != "" {
		if !releaseName.MatchString(r) || strings.Contains(r, "..") {
			return "", fmt.Errorf("GOOSE_RELEASE=%s names no release: a release is named by its version, as v0.2.0 is", r)
		}
		return r, nil
	}
	v := moduleVersion(bi)
	switch {
	case v == "" || v == "(devel)":
		return "", fmt.Errorf("this build of %s has no version, which names the release of the compiler it runs: "+
			"run it as %s@<version> (go run %s@v0.2.0), or name the release with GOOSE_RELEASE", modulePath, modulePath, modulePath)
	case pseudoVersion.MatchString(v):
		return "", fmt.Errorf("%s@%s is a commit, of which there is no release: run a version that is one, "+
			"or name the release with GOOSE_RELEASE", modulePath, v)
	case !releaseName.MatchString(v):
		return "", fmt.Errorf("%s@%s names no release: name one with GOOSE_RELEASE", modulePath, v)
	}
	return v, nil
}

// moduleVersion is the version of this module that the build has: the
// main module's where it is this one (`go run module@version`), else the
// dependency's (`go run` of it in a module that requires it), as replaced.
func moduleVersion(bi *debug.BuildInfo) string {
	if bi == nil {
		return ""
	}
	m := &bi.Main
	if m.Path != modulePath {
		m = nil
		for _, d := range bi.Deps {
			if d.Path == modulePath {
				m = d
				break
			}
		}
	}
	if m == nil {
		return ""
	}
	if m.Replace != nil {
		m = m.Replace
	}
	return m.Version
}

// cacheDir is where releases are kept: GOOSE_CACHE, else goose.go in the
// user's cache directory. Release r's compiler for linux/amd64 is
// r/linux-amd64/goose in it.
func cacheDir(getenv func(string) string) (string, error) {
	if d := getenv("GOOSE_CACHE"); d != "" {
		return filepath.Abs(d)
	}
	d, err := os.UserCacheDir()
	if err != nil {
		return "", fmt.Errorf("there is no cache directory to keep the compiler in (%v): set GOOSE_CACHE", err)
	}
	return filepath.Join(d, "goose.go"), nil
}

// A source is the place of a release's files: a directory, or one at a
// URL, or the release as GitHub's API serves it.
type source struct {
	// where names a file of the release, for messages: its URL.
	where func(name string) string
	// open reads one, or fails with errNotFound where there is none.
	open func(ctx context.Context, name string) (io.ReadCloser, error)
}

var errNotFound = errors.New("not found")

// sources are the places the release may be, in the order to look in, and
// what to add where none of them has it.
func sources(getenv func(string) string, release string) ([]source, string, error) {
	if u := getenv("GOOSE_RELEASE_URL"); u != "" {
		s, err := mirror(u, release)
		return []source{s}, "", err
	}
	srcs := []source{webSource(githubWeb + "/" + repository + "/releases/download/" + release)}
	for _, k := range []string{"GH_TOKEN", "GITHUB_TOKEN"} {
		if tok := getenv(k); tok != "" {
			return append(srcs, apiSource(release, tok)), "", nil
		}
	}
	return srcs, "; GitHub serves a private repository's releases to a token that can read it, in GH_TOKEN", nil
}

// mirror is the release's directory under GOOSE_RELEASE_URL, an http or
// https URL, a file URL or a directory.
func mirror(u, release string) (source, error) {
	if strings.HasPrefix(u, "http://") || strings.HasPrefix(u, "https://") {
		return webSource(strings.TrimRight(u, "/") + "/" + release), nil
	}
	dir := u
	if strings.HasPrefix(u, "file:") {
		p, err := url.Parse(u)
		if err != nil || (p.Host != "" && p.Host != "localhost") || p.Path == "" {
			return source{}, fmt.Errorf("GOOSE_RELEASE_URL=%s is no file URL of a directory", u)
		}
		dir = filepath.FromSlash(p.Path)
	} else if strings.Contains(u, "://") {
		return source{}, fmt.Errorf("GOOSE_RELEASE_URL=%s: releases are at an http, https or file URL, or in a directory", u)
	}
	dir = filepath.Join(dir, release)
	return source{
		where: func(name string) string { return filepath.Join(dir, name) },
		open: func(_ context.Context, name string) (io.ReadCloser, error) {
			f, err := os.Open(filepath.Join(dir, name))
			if errors.Is(err, fs.ErrNotExist) {
				return nil, errNotFound
			}
			return f, err
		},
	}, nil
}

// webSource is a release's directory at an http or https URL.
func webSource(base string) source {
	return source{
		where: func(name string) string { return base + "/" + name },
		open: func(ctx context.Context, name string) (io.ReadCloser, error) {
			return get(ctx, base+"/"+name, nil)
		},
	}
}

// apiSource is the release as GitHub's API serves it to the token: the
// files of a release of a private repository, which are nowhere else.
func apiSource(release, token string) source {
	rel := githubAPI + "/repos/" + repository + "/releases/tags/" + release
	header := http.Header{}
	header.Set("Authorization", "Bearer "+token)
	header.Set("X-GitHub-Api-Version", "2022-11-28")
	var assets map[string]string // Each file's URL in the API, once the release is read.
	return source{
		where: func(name string) string { return name + " of " + rel },
		open: func(ctx context.Context, name string) (io.ReadCloser, error) {
			if assets == nil {
				a, err := releaseAssets(ctx, rel, header)
				if err != nil {
					return nil, err
				}
				assets = a
			}
			u, ok := assets[name]
			if !ok {
				return nil, errNotFound
			}
			h := header.Clone()
			h.Set("Accept", "application/octet-stream")
			return get(ctx, u, h)
		},
	}
}

// releaseAssets reads the release at rel, GitHub's API's URL of it: the
// URL of each of its files, by name.
func releaseAssets(ctx context.Context, rel string, header http.Header) (map[string]string, error) {
	h := header.Clone()
	h.Set("Accept", "application/vnd.github+json")
	r, err := get(ctx, rel, h)
	if err != nil {
		return nil, err
	}
	defer r.Close()
	var release struct {
		Assets []struct {
			Name string `json:"name"`
			URL  string `json:"url"`
		} `json:"assets"`
	}
	if err := json.NewDecoder(io.LimitReader(r, 16<<20)).Decode(&release); err != nil {
		return nil, fmt.Errorf("reading %s: %v", rel, err)
	}
	assets := map[string]string{}
	for _, a := range release.Assets {
		assets[a.Name] = a.URL
	}
	return assets, nil
}

// get is the body of u's response: errNotFound where it is a 404, and an
// error where it is not a 200.
func get(ctx context.Context, u string, header http.Header) (io.ReadCloser, error) {
	req, err := http.NewRequestWithContext(ctx, http.MethodGet, u, nil)
	if err != nil {
		return nil, err
	}
	for k, v := range header {
		req.Header[k] = v
	}
	req.Header.Set("User-Agent", modulePath)
	resp, err := client.Do(req)
	if err != nil {
		return nil, err
	}
	if resp.StatusCode == http.StatusOK {
		return resp.Body, nil
	}
	resp.Body.Close()
	if resp.StatusCode == http.StatusNotFound {
		return nil, errNotFound
	}
	return nil, fmt.Errorf("%s: %s", u, resp.Status)
}

// install downloads the release's archive for platform from the first of
// srcs that has its SHA256SUMS, checks it against them and unpacks it into
// dir, whole or not at all: into a directory beside it, renamed to it once
// complete, so that a run that stops, or one at the same time, leaves no
// part of a compiler there.
func install(ctx context.Context, srcs []source, hint, release, platform, dir string, log io.Writer) error {
	name := "goose-" + release + "-" + platform + ".tar.gz"
	src, sums, err := fetchSums(ctx, srcs, hint, release)
	if err != nil {
		return err
	}
	want, ok := sums[name]
	if !ok {
		return fmt.Errorf("release %s has no compiler for %s: its %s lists %s", release,
			strings.Replace(platform, "-", "/", 1), sumsName, platforms(sums, release))
	}
	if err := os.MkdirAll(filepath.Dir(dir), 0o755); err != nil {
		return err
	}
	tmp, err := os.MkdirTemp(filepath.Dir(dir), "."+filepath.Base(dir)+".")
	if err != nil {
		return err
	}
	defer os.RemoveAll(tmp)
	fmt.Fprintf(log, "goose: downloading %s\n", src.where(name))
	archive := filepath.Join(tmp, name)
	if err := download(ctx, src, name, archive, want); err != nil {
		return err
	}
	root := filepath.Join(tmp, "release")
	if err := unpack(archive, root); err != nil {
		return fmt.Errorf("unpacking %s: %v", src.where(name), err)
	}
	if !isFile(filepath.Join(root, "goose")) {
		return fmt.Errorf("%s holds no compiler: no goose in its directory", src.where(name))
	}
	if err := os.Rename(root, dir); err != nil {
		if isFile(filepath.Join(dir, "goose")) {
			return nil // Another run put it there first.
		}
		return fmt.Errorf("putting release %s in its place: %v", release, err)
	}
	return nil
}

// fetchSums reads the release's SHA256SUMS from the first of srcs that has
// it, which is the one to download the archive from.
func fetchSums(ctx context.Context, srcs []source, hint, release string) (source, map[string]string, error) {
	var tried []string
	for _, s := range srcs {
		r, err := s.open(ctx, sumsName)
		if errors.Is(err, errNotFound) {
			tried = append(tried, s.where(sumsName))
			continue
		}
		if err != nil {
			return source{}, nil, fmt.Errorf("reading %s: %v", s.where(sumsName), err)
		}
		data, err := io.ReadAll(io.LimitReader(r, 1<<20))
		r.Close()
		if err != nil {
			return source{}, nil, fmt.Errorf("reading %s: %v", s.where(sumsName), err)
		}
		sums, err := parseSums(data)
		if err != nil {
			return source{}, nil, fmt.Errorf("%s: %v", s.where(sumsName), err)
		}
		return s, sums, nil
	}
	return source{}, nil, fmt.Errorf("no release %s: there is no %s%s", release, strings.Join(tried, ", nor "), hint)
}

// download writes the release's file name to path, and checks that its
// SHA-256 is want.
func download(ctx context.Context, src source, name, path, want string) error {
	r, err := src.open(ctx, name)
	if errors.Is(err, errNotFound) {
		return fmt.Errorf("the release's %s lists %s, which is not there: %s", sumsName, name, src.where(name))
	}
	if err != nil {
		return fmt.Errorf("downloading %s: %v", src.where(name), err)
	}
	defer r.Close()
	f, err := os.Create(path)
	if err != nil {
		return err
	}
	h := sha256.New()
	n, err := io.Copy(io.MultiWriter(f, h), io.LimitReader(r, maxArchive+1))
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		return fmt.Errorf("downloading %s: %v", src.where(name), err)
	}
	if n > maxArchive {
		return fmt.Errorf("%s is larger than a release's archive can be", src.where(name))
	}
	if got := hex.EncodeToString(h.Sum(nil)); got != want {
		return fmt.Errorf("%s is not the archive the release's %s names: its SHA-256 is %s, not %s",
			src.where(name), sumsName, got, want)
	}
	return nil
}

// unpack writes the files of the gzipped tar at path into dir: those of the
// one directory it holds, which must be all it holds, and only directories
// and regular files.
func unpack(path, dir string) error {
	f, err := os.Open(path)
	if err != nil {
		return err
	}
	defer f.Close()
	zr, err := gzip.NewReader(f)
	if err != nil {
		return err
	}
	tr := tar.NewReader(zr)
	top := ""
	for {
		h, err := tr.Next()
		if err == io.EOF {
			break
		}
		if err != nil {
			return err
		}
		if h.Typeflag == tar.TypeXGlobalHeader {
			continue
		}
		name := strings.TrimSuffix(h.Name, "/")
		first, rest, _ := strings.Cut(name, "/")
		if top == "" {
			top = first
		}
		if !fs.ValidPath(name) || strings.Contains(name, `\`) || first != top {
			return fmt.Errorf("%q is outside the directory the archive holds, %s", h.Name, top)
		}
		target := filepath.Join(dir, filepath.FromSlash(rest))
		switch {
		case h.Typeflag == tar.TypeDir:
			err = os.MkdirAll(target, 0o755)
		case h.Typeflag == tar.TypeReg && rest != "":
			err = writeFile(target, tr, h.Mode)
		default:
			return fmt.Errorf("%q is neither a directory nor a file", h.Name)
		}
		if err != nil {
			return err
		}
	}
	if top == "" {
		return errors.New("the archive is empty")
	}
	return nil
}

// writeFile writes what r holds to a new file at path, executable where
// mode, the archive's, says it is.
func writeFile(path string, r io.Reader, mode int64) error {
	perm := fs.FileMode(0o644)
	if mode&0o111 != 0 {
		perm = 0o755
	}
	if err := os.MkdirAll(filepath.Dir(path), 0o755); err != nil {
		return err
	}
	f, err := os.OpenFile(path, os.O_WRONLY|os.O_CREATE|os.O_EXCL, perm)
	if err != nil {
		return err
	}
	_, err = io.Copy(f, r)
	if cerr := f.Close(); err == nil {
		err = cerr
	}
	return err
}

// parseSums reads sha256sum's lines: a checksum, a space, a space or a *,
// and a file's name.
func parseSums(data []byte) (map[string]string, error) {
	sums := map[string]string{}
	for i, line := range strings.Split(string(data), "\n") {
		line = strings.TrimSuffix(line, "\r")
		if line == "" {
			continue
		}
		if len(line) < 67 || line[64] != ' ' || (line[65] != ' ' && line[65] != '*') {
			return nil, fmt.Errorf("line %d is not a SHA-256 and a file's name", i+1)
		}
		sum, name := strings.ToLower(line[:64]), line[66:]
		if _, err := hex.DecodeString(sum); err != nil {
			return nil, fmt.Errorf("line %d is not a SHA-256 and a file's name", i+1)
		}
		if old, ok := sums[name]; ok && old != sum {
			return nil, fmt.Errorf("line %d lists %s again, with another checksum", i+1, name)
		}
		sums[name] = sum
	}
	return sums, nil
}

// platforms are those of the release's archives that sums lists, for a
// message.
func platforms(sums map[string]string, release string) string {
	var ps []string
	for name := range sums {
		p, ok := strings.CutPrefix(name, "goose-"+release+"-")
		if p, isArchive := strings.CutSuffix(p, ".tar.gz"); ok && isArchive {
			ps = append(ps, strings.Replace(p, "-", "/", 1))
		}
	}
	if len(ps) == 0 {
		return "no compiler"
	}
	sort.Strings(ps)
	return "those for " + strings.Join(ps, ", ")
}

func isFile(path string) bool {
	fi, err := os.Stat(path)
	return err == nil && fi.Mode().IsRegular()
}
