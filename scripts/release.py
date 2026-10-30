#!/usr/bin/env python3
"""Builds, packages and publishes the prebuilt compilers (issue #29): the
release that `go run github.com/robtandy/goose.go/cmd/goose@<version>` runs,
which holds a compiler for each of PLATFORMS. docs/releasing.md is the
procedure.

  python3 scripts/release.py build [--platform os/arch]... [--dist dist] [-j N]
  python3 scripts/release.py package --exe path/to/goose [--version v] [--dist dist]
  python3 scripts/release.py publish [--dist dist] [--remote origin] [--dry-run]

build compiles the compiler of this work tree, which must have no changes,
for each platform it is given (all of them by default), with the version
`git describe` gives the tree, as a build of the tree would have it
(cmake/version.cmake): the release's tag, where the commit has one. The
Linux compilers are built in an Alpine container of their architecture
(docker run --platform), and linked statically, with musl, so that they
run on any Linux, glibc's or musl's; the macOS one on this machine, which
must be an arm64 Mac, for macOS 13.3 and later. package lays out a
compiler built otherwise, for the platform its header names, of the
version it reports. Either writes, in dist/<version>/,

  goose-<version>-<os>-<arch>.tar.gz   one directory, goose-<version>-<os>-<arch>/:
                                       goose, the compiler; stdlib/, the standard
                                       library git tracks, which goose finds beside
                                       itself; LICENSE; and licenses/, those of the
                                       code goose is linked from that has its own
  SHA256SUMS                           sha256sum's lines, one for each archive there

an archive being the same bytes each time its compiler is packaged.

publish releases the tag at HEAD, an annotated one of a version: it writes
SHA256SUMS of the three archives in dist/<tag>/ again, tags the commit for
the modules beside it as the go command looks them up (cmd/goose/<tag>,
runtime/go/<tag>, lightweight tags, which git describe names the commit by
only after the release's), pushes the three tags and makes the GitHub
release of the tag with the archives and SHA256SUMS (gh release create).
--dry-run prints what it would run.
"""

import argparse
import gzip
import hashlib
import io
import os
import platform as host
import re
import shlex
import subprocess
import sys
import tarfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
UPSTREAM = REPO / "vendor" / "goose"
PLATFORMS = ("linux/amd64", "linux/arm64", "darwin/arm64")
SUMS = "SHA256SUMS"
# The image the Linux compilers are built in, whose C library, musl, they
# link statically. The image holds no license of musl's, which the archive
# needs: it comes from musl's repository, at the version the image has.
LINUX_IMAGE = "alpine:3.20"
MUSL_COPYRIGHT = "https://git.musl-libc.org/cgit/musl/plain/COPYRIGHT?h=v"
# The oldest macOS the macOS compiler runs on: floating-point
# std::to_chars, which goose prints numbers with, is in its C++ library
# from 13.3 on.
MACOS_TARGET = "13.3"
# What a release can be named, as cmd/goose reads it (releaseName): one
# element of a URL's path. A published one is a version the go command can
# name a module by, vMAJOR.MINOR.PATCH or one with a pre-release, which the
# modules beside it are tagged with too.
RELEASE_NAME = re.compile(r"^[0-9A-Za-z][0-9A-Za-z._+-]*$")
RELEASE_VERSION = re.compile(r"^v(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[0-9A-Za-z-]+(\.[0-9A-Za-z-]+)*)?$")
MODULES = ("cmd/goose", "runtime/go")
# The license files of the third-party code upstream vendors, as upstream's
# release collects them (vendor/goose/scripts/package_release.py).
LICENSE_NAME = re.compile(r"^(LICEN[CS]E|COPYING|RELICENSING)", re.IGNORECASE)

NOTES = """\
The Goose compiler of goose.go {version}, prebuilt for linux/amd64 and linux/arm64 \
(linked statically: any Linux) and darwin/arm64 (macOS {macos} or later), each with its \
standard library. SHA256SUMS lists the archives' checksums.

With nothing but Go installed, go generate runs it as

    //go:generate go run github.com/robtandy/goose.go/cmd/goose@{version} --target=go -o prog.go prog.goose

which downloads it once, checks it against SHA256SUMS and keeps it in the user's cache \
directory. The Go that --go-runtime=import writes requires \
github.com/robtandy/goose.go/runtime/go {version}.
"""


def fail(msg):
    sys.exit(f"release.py: {msg}")


def run(argv, cwd=REPO):
    """What argv prints on stdout, run in cwd; a failure ends the script,
    with what it printed."""
    argv = [str(a) for a in argv]
    try:
        r = subprocess.run(argv, cwd=cwd, capture_output=True, text=True)
    except OSError as e:
        fail(f"cannot run {argv[0]}: {e}")
    if r.returncode != 0:
        fail(f"{shlex.join(argv)} failed (exit {r.returncode}):\n{r.stdout}{r.stderr}")
    return r.stdout


def logged(argv, log, cwd=REPO):
    """Runs argv in cwd, what it prints going to the file log; a failure
    ends the script, with the end of the log."""
    argv = [str(a) for a in argv]
    shown = " ".join("<script>" if "\n" in a else shlex.quote(a) for a in argv)
    print(f"+ {shown}  (> {log})", flush=True)
    with open(log, "ab") as f:
        try:
            code = subprocess.run(argv, cwd=cwd, stdout=f, stderr=subprocess.STDOUT).returncode
        except OSError as e:
            fail(f"cannot run {argv[0]}: {e}")
    if code != 0:
        tail = Path(log).read_text(encoding="utf-8", errors="replace").splitlines()[-40:]
        fail(f"{argv[0]} failed (exit {code}); the end of {log}:\n" + "\n".join(tail))


def git(*args):
    return run(["git", *args]).strip()


def describe():
    """The version a build of this tree has: git describe's of it, as
    vendor/goose/cmake/version.cmake asks it."""
    return git("describe", "--tags", "--always", "--dirty")


def asset_name(version, platform):
    """The name of the release's archive for platform, os/arch."""
    return f"goose-{version}-{platform.replace('/', '-')}.tar.gz"


def binary_platform(path):
    """The platform, os/arch, that the executable at path is for, as its
    header says (ELF's or Mach-O's), or None for any other."""
    with open(path, "rb") as f:
        head = f.read(20)
    if len(head) == 20 and head[:4] == b"\x7fELF" and head[4] == 2 and head[5] == 1 and head[7] in (0, 3):
        return {0x3E: "linux/amd64", 0xB7: "linux/arm64"}.get(int.from_bytes(head[18:20], "little"))
    if len(head) >= 8 and head[:4] == b"\xcf\xfa\xed\xfe":
        return {0x01000007: "darwin/amd64", 0x0100000C: "darwin/arm64"}.get(int.from_bytes(head[4:8], "little"))
    return None


def reported_version(exe):
    """The version `goose version` says exe is, or None where it does not
    run here."""
    try:
        r = subprocess.run([str(exe), "version"], capture_output=True, text=True)
    except OSError:
        return None
    m = re.fullmatch(r"goose (\S+) \(runtime [0-9a-f]+\)\n", r.stdout)
    return m[1] if r.returncode == 0 and m else None


def tracked(subdir, repo):
    """The files git tracks under subdir of the repository repo, relative to
    it, in order."""
    out = run(["git", "-C", repo, "ls-files", "-z", "--", subdir])
    return sorted(Path(p) for p in out.split("\0") if p)


def release_files(exe, extra=()):
    """(name in the archive's directory, file, mode) for each file of a
    release of the compiler exe: it, the standard library git tracks, the
    license, the license files of the third-party code upstream vendors,
    which goose is linked from, and those extra names, (name, file)."""
    files = [("goose", Path(exe), 0o755), ("LICENSE", REPO / "LICENSE", 0o644)]
    files += [(p.as_posix(), UPSTREAM / p, 0o644) for p in tracked("stdlib", UPSTREAM)]
    files += [(f"licenses/{p.parts[1]}/{p.name}", UPSTREAM / p, 0o644)
              for p in tracked("third_party", UPSTREAM) if len(p.parts) == 3 and LICENSE_NAME.match(p.name)]
    files += [(name, Path(src), 0o644) for name, src in extra]
    return sorted(files)


def write_archive(path, top, files, mtime):
    """files, (name, file, mode), under the directory top, in a gzipped tar
    at path that is the same bytes for the same files: its entries in the
    order of their names, every directory with them, each root's and of
    mtime, compressed at gzip's own level (9 makes an archive of goose a
    tenth of a percent smaller, in three times the time)."""
    entries = {}
    for name, src, mode in files:
        entries[f"{top}/{name}"] = (src, mode)
        parts = name.split("/")
        for i in range(len(parts)):
            entries.setdefault("/".join([top, *parts[:i]]), (None, 0o755))
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for name in sorted(entries):
            src, mode = entries[name]
            info = tarfile.TarInfo(name)
            info.mode, info.mtime = mode, mtime
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            if src is None:
                info.type = tarfile.DIRTYPE
                tar.addfile(info)
            else:
                data = Path(src).read_bytes()
                info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
    out = io.BytesIO()
    with gzip.GzipFile(filename="", mode="wb", fileobj=out, mtime=0, compresslevel=6) as gz:
        gz.write(buf.getvalue())
    Path(path).write_bytes(out.getvalue())


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def write_sums(directory):
    """SHA256SUMS of the archives in directory, as sha256sum writes it, in
    the order of their names."""
    directory = Path(directory)
    lines = [f"{sha256(p)}  {p.name}\n" for p in sorted(directory.glob("goose-*.tar.gz"))]
    (directory / SUMS).write_bytes("".join(lines).encode())
    return directory / SUMS


def package(exe, platform, version, dist, extra=()):
    """Lays out the release archive of the compiler exe for platform, of
    version, in dist/<version>/, and writes SHA256SUMS of the archives there
    again; returns the archive's path."""
    if not RELEASE_NAME.match(version) or ".." in version:
        fail(f"{version!r} cannot name a release: one is named by its version, as v0.2.0 is")
    files = release_files(exe, extra)
    if not any(name.startswith("stdlib/") for name, _, _ in files):
        fail("vendor/goose holds no standard library: git submodule update --init vendor/goose")
    out = Path(dist) / version
    out.mkdir(parents=True, exist_ok=True)
    name = asset_name(version, platform)
    write_archive(out / name, name[:-len(".tar.gz")], files, int(git("log", "-1", "--format=%ct")))
    write_sums(out)
    return out / name


def check_built(exe, platform, version, says):
    """That the compiler a build made is for platform, and reports version."""
    if binary_platform(exe) != platform:
        fail(f"{exe} is not a compiler for {platform}")
    m = re.fullmatch(r"goose (\S+) \(runtime [0-9a-f]+\)\n", says)
    if not m or m[1] != version:
        fail(f"{exe} says {says.strip()!r}, not goose {version}")


def build_linux(platform, version, work, jobs):
    """Builds the compiler for a Linux platform in an Alpine container of
    its architecture, statically linked and stripped, and compiles a
    program to Go with it there, where it runs; returns it, and the files
    of musl's license, which the archive holds with it."""
    out = work / platform.replace("/", "-")
    out.mkdir(parents=True, exist_ok=True)
    for f in ("goose", "version", "musl-COPYRIGHT", "build.log"):
        (out / f).unlink(missing_ok=True)
    script = f"""set -eu
apk add --no-cache build-base cmake linux-headers
cmake -S /src -B /tmp/build -DCMAKE_BUILD_TYPE=Release "-DGOOSE_VERSION=$GOOSE_VERSION" '-DCMAKE_EXE_LINKER_FLAGS=-static -s'
cmake --build /tmp/build --target goose -j "$JOBS"
cp /tmp/build/goose /out/goose
/out/goose version > /out/version
/out/goose --target=go --stdlib /src/vendor/goose/stdlib -o /tmp/smoke.go /src/test/go/programs/print.goose
musl=$(apk list -I musl 2>/dev/null | sed -n 's/^musl-\\([0-9.]*\\)-r.*/\\1/p')
wget -q -O /out/musl-COPYRIGHT "{MUSL_COPYRIGHT}$musl"
chown -R "$OWNER" /out || true
"""
    logged(["docker", "run", "--rm", "--platform", platform, "-v", f"{REPO}:/src:ro", "-v", f"{out}:/out",
            "-e", f"GOOSE_VERSION={version}", "-e", f"JOBS={jobs}", "-e", f"OWNER={os.getuid()}:{os.getgid()}",
            LINUX_IMAGE, "sh", "-c", script], out / "build.log")
    check_built(out / "goose", platform, version, (out / "version").read_text(encoding="utf-8"))
    if not (out / "musl-COPYRIGHT").read_bytes().startswith(b"musl as a whole is licensed"):
        fail(f"{out / 'musl-COPYRIGHT'} is not musl's license")
    return out / "goose", [("licenses/musl/COPYRIGHT", out / "musl-COPYRIGHT")]


def build_macos(platform, version, work, jobs):
    """Builds the compiler for macOS on this machine, an arm64 Mac, for the
    macOS of MACOS_TARGET and later; returns it."""
    out = work / platform.replace("/", "-")
    out.mkdir(parents=True, exist_ok=True)
    log = out / "build.log"
    log.unlink(missing_ok=True)
    logged(["cmake", "-S", REPO, "-B", out / "build", "-DCMAKE_BUILD_TYPE=Release", f"-DGOOSE_VERSION={version}",
            f"-DCMAKE_OSX_DEPLOYMENT_TARGET={MACOS_TARGET}", "-DCMAKE_OSX_ARCHITECTURES=arm64"], log)
    logged(["cmake", "--build", out / "build", "--target", "goose", "--config", "Release", "-j", jobs], log)
    exe = out / "build" / "goose"
    check_built(exe, platform, version, run([exe, "version"]))
    return exe, []


def check_annotated(version):
    """That the tag version is annotated: git describe names a commit by
    an annotated tag before the lightweight ones on it, the modules' tags
    beside a release's, so that a build of the release's commit is goose
    <version> as the release's compilers are."""
    if git("cat-file", "-t", f"refs/tags/{version}") != "tag":
        fail(f"the tag {version} is a lightweight one: tag a release with an annotated tag (git tag -a {version} "
             f"-m 'goose.go {version}'), which git describe names the commit by, rather than by the tags of the "
             "modules beside it")


def cmd_build(args):
    platforms = args.platform or list(PLATFORMS)
    for p in platforms:
        if p not in PLATFORMS:
            fail(f"there is no release for {p}: the platforms are {', '.join(PLATFORMS)}")
    if "darwin/arm64" in platforms and (sys.platform != "darwin" or host.machine() != "arm64"):
        fail("darwin/arm64's compiler is built on an arm64 Mac: build the others here (--platform linux/amd64 "
             "--platform linux/arm64), that one there, and bring the archives into one dist/<version>/ to publish")
    if any(p.startswith("linux/") for p in platforms):
        run(["docker", "version"])
    if not (UPSTREAM / "src" / "ast.h").exists():
        fail("vendor/goose is not checked out: git submodule update --init vendor/goose")
    version = describe()
    if version.endswith("-dirty"):
        fail(f"the work tree has changes ({version}): a release is built from a commit")
    if RELEASE_VERSION.match(version):
        check_annotated(version)
    work = REPO / "build-release" / version
    for p in platforms:
        print(f"building goose {version} for {p}", flush=True)
        exe, extra = (build_linux if p.startswith("linux/") else build_macos)(p, version, work, args.jobs)
        print(f"wrote {package(exe, p, version, args.dist, extra)}")
    print(f"wrote {Path(args.dist) / version / SUMS}")


def cmd_package(args):
    exe = Path(args.exe)
    if not exe.is_file():
        fail(f"there is no compiler at {exe}")
    platform = binary_platform(exe)
    if platform is None:
        fail(f"{exe} is not a compiler for Linux or macOS on amd64 or arm64")
    says = reported_version(exe)
    version = args.version or says
    if version is None:
        fail(f"{exe} does not run here, to say its version: give it with --version")
    if says is not None and says != version:
        fail(f"{exe} is goose {says}, as goose version says, not {version}")
    path = package(exe, platform, version, args.dist)
    print(f"wrote {path}\nwrote {path.parent / SUMS}")


def cmd_publish(args):
    versions = [t for t in git("tag", "--points-at", "HEAD").split() if RELEASE_VERSION.match(t)]
    if len(versions) != 1:
        fail(f"HEAD has {'the tags of two versions, ' + ' and '.join(versions) if versions else 'no tag of a version'}: "
             "a release's tag is one version the go command can name, vMAJOR.MINOR.PATCH or one with a "
             "pre-release (git tag -a v0.2.0 -m 'goose.go v0.2.0')")
    version = versions[0]
    check_annotated(version)
    if describe() != version:
        fail(f"git describe says {describe()}, not {version}: a release is of its tag's commit, with no changes")
    d = Path(args.dist) / version
    assets = [d / asset_name(version, p) for p in PLATFORMS]
    missing = [a.name for a in assets if not a.is_file()]
    if missing:
        fail(f"{d} has no {', '.join(missing)}: release.py build makes them (darwin/arm64's on an arm64 Mac)")
    others = sorted({p.name for p in d.glob("goose-*.tar.gz")} - {a.name for a in assets})
    if others:
        fail(f"{d} holds archives the release has no place for: {', '.join(others)}")
    write_sums(d)
    commit = git("rev-parse", "HEAD")
    tags = [version] + [f"{m}/{version}" for m in MODULES]

    def step(argv):
        print("+ " + shlex.join(str(a) for a in argv), flush=True)
        if not args.dry_run:
            run(argv)

    for t in tags[1:]:
        r = subprocess.run(["git", "rev-parse", "-q", "--verify", f"refs/tags/{t}^{{commit}}"], cwd=REPO,
                           capture_output=True, text=True)
        if r.returncode != 0:
            step(["git", "tag", t, commit])
        elif r.stdout.strip() != commit:
            fail(f"the tag {t} is on {r.stdout.strip()[:12]}, not on {version}'s commit, {commit[:12]}")
    step(["git", "push", args.remote, *(f"refs/tags/{t}" for t in tags)])
    step(["gh", "release", "create", version, "--verify-tag", "--title", f"goose.go {version}",
          "--notes", NOTES.format(version=version, macos=MACOS_TARGET),
          *(["--prerelease"] if "-" in version else []), *assets, d / SUMS])


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    sub = ap.add_subparsers(dest="command", required=True)
    b = sub.add_parser("build", help="build and package the compilers of this work tree")
    b.add_argument("--platform", action="append", metavar="OS/ARCH",
                   help=f"a platform to build for (default: all of {', '.join(PLATFORMS)})")
    b.add_argument("-j", "--jobs", type=int, default=os.cpu_count() or 4)
    p = sub.add_parser("package", help="package a compiler built otherwise")
    p.add_argument("--exe", required=True, help="the compiler")
    p.add_argument("--version", help="its version, where it does not run here to say it")
    u = sub.add_parser("publish", help="tag and publish the release of the tag at HEAD")
    u.add_argument("--remote", default="origin", help="where to push the tags (default origin)")
    u.add_argument("--dry-run", action="store_true", help="print what it would run, and run none of it")
    for s in (b, p, u):
        s.add_argument("--dist", default=str(REPO / "dist"), help="where the releases go (default dist/)")
    args = ap.parse_args()
    {"build": cmd_build, "package": cmd_package, "publish": cmd_publish}[args.command](args)


if __name__ == "__main__":
    main()
