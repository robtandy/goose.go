// Goose compiler — goose --check of the Go a compile writes (issue #21).
// With --target=go and -o out.go, --check (and goose check) compiles the Go
// as the compile without it would, writes nothing, and holds what it would
// write against the files there: out.go, and the package's runtime file
// where --go-runtime-file names one. That is a CI check that the Go checked
// in is the Go its source compiles to, as `go generate` and a diff would
// make it: a file that differs, or is missing, is stale, which ends the
// compile with exit code 1 and a short diff of each such file.
#pragma once

namespace goose {

// A line of the diff of two texts: kept (' '), removed from the first
// ('-') or added from the second ('+'), with its index in each; where a
// side has no such line, the index of the line it would come before.
struct GoDiffLine {
    char op;
    int a, b;
};

// A text's lines, each with its line break: the last without one where the
// text has none.
static vector<string_view> GoLines(string_view s) {
    vector<string_view> lines;
    for (size_t p = 0; p < s.size();) {
        auto nl = s.find('\n', p);
        auto e = nl == string_view::npos ? s.size() : nl + 1;
        lines.push_back(s.substr(p, e - p));
        p = e;
    }
    return lines;
}

// The diff of a[a0, a1) and b[b0, b1) with as few lines removed and added
// as can be, Myers's O(ND) algorithm, appended to out: false, and nothing
// appended, where that is more than maxd lines.
static bool GoMyersDiff(const vector<string_view> &a, int a0, int a1, const vector<string_view> &b, int b0, int b1,
                        vector<GoDiffLine> &out, int maxd) {
    // v[off + k] is how far along diagonal k (x - y) the round reached, and
    // trace[d] what round d started from, on diagonals -d - 1 to d + 1.
    int na = a1 - a0, nb = b1 - b0;
    int maxD = std::min(na + nb, maxd), off = maxD + 1, D = -1;
    vector<int> v(2 * maxD + 3, 0);
    vector<vector<int>> trace;
    for (int d = 0; d <= maxD && D < 0; d++) {
        trace.emplace_back(v.begin() + (off - d - 1), v.begin() + (off + d + 2));
        for (int k = -d; k <= d; k += 2) {
            auto down = k == -d || (k != d && v[off + k - 1] < v[off + k + 1]);
            int x = down ? v[off + k + 1] : v[off + k - 1] + 1, y = x - k;
            while (x < na && y < nb && a[a0 + x] == b[b0 + y]) x++, y++;
            v[off + k] = x;
            if (x >= na && y >= nb) {
                D = d;
                break;
            }
        }
    }
    if (D < 0) return false;
    // Back from the end, round by round: the run of lines kept, then the
    // line the round removed (a step right) or added (a step down).
    vector<GoDiffLine> path;
    for (int d = D, x = na, y = nb; d >= 0; d--) {
        auto at = [&](int k) { return trace[d][k + d + 1]; };
        int k = x - y;
        int pk = k == -d || (k != d && at(k - 1) < at(k + 1)) ? k + 1 : k - 1;
        int px = at(pk), py = px - pk;
        for (; x > px && y > py; x--, y--) path.push_back({ ' ', a0 + x - 1, b0 + y - 1 });
        if (d > 0) path.push_back(x == px ? GoDiffLine { '+', a0 + x, b0 + py } : GoDiffLine { '-', a0 + px, b0 + y });
        x = px;
        y = py;
    }
    out.insert(out.end(), path.rbegin(), path.rend());
    return true;
}

// The lines a[a0, a1) and b[b0, b1) both hold once and only once, in the
// longest run of them in the same order in both: where a diff too long for
// Myers's to find the fewest lines of is split, as a patience diff does.
static vector<pair<int, int>> GoDiffAnchors(const vector<string_view> &a, int a0, int a1,
                                            const vector<string_view> &b, int b0, int b1) {
    unordered_map<string_view, pair<int, int>> ina, inb;   // A line's count and place.
    auto count = [](auto &in, const vector<string_view> &lines, int from, int to) {
        for (int i = from; i < to; i++) {
            auto &c = in[lines[i]];
            c.first++;
            c.second = i;
        }
    };
    count(ina, a, a0, a1);
    count(inb, b, b0, b1);
    vector<pair<int, int>> both;
    for (int i = a0; i < a1; i++) {
        auto it = inb.find(a[i]);
        if (ina[a[i]].first == 1 && it != inb.end() && it->second.first == 1) both.push_back({ i, it->second.second });
    }
    // The longest run increasing in b too: patience sorting's piles.
    vector<int> tops, below(both.size(), -1);
    for (int p = 0; p < (int)both.size(); p++) {
        auto it = lower_bound(tops.begin(), tops.end(), both[p].second,
                              [&](int t, int j) { return both[t].second < j; });
        if (it != tops.begin()) below[p] = *(it - 1);
        if (it == tops.end()) tops.push_back(p);
        else *it = p;
    }
    vector<pair<int, int>> run;
    for (int p = tops.empty() ? -1 : tops.back(); p >= 0; p = below[p]) run.push_back(both[p]);
    reverse(run.begin(), run.end());
    return run;
}

// The diff of a[a0, a1) and b[b0, b1), appended to out: the lines both
// start and end with kept, and those between as few removed and added as
// Myers's algorithm finds within maxd of them; past that, split where the
// lines both hold once are (GoDiffAnchors), each part diffed so, and a part
// with none every line removed and added.
static void GoDiffRange(const vector<string_view> &a, int a0, int a1, const vector<string_view> &b, int b0, int b1,
                        vector<GoDiffLine> &out, int maxd) {
    for (; a0 < a1 && b0 < b1 && a[a0] == b[b0]; a0++, b0++) out.push_back({ ' ', a0, b0 });
    int e = 0;
    while (a1 - e > a0 && b1 - e > b0 && a[a1 - e - 1] == b[b1 - e - 1]) e++;
    int ma = a1 - e, mb = b1 - e;
    auto done = a0 == ma && b0 == mb;
    if (a0 < ma && b0 < mb) {
        if (GoMyersDiff(a, a0, ma, b, b0, mb, out, maxd)) {
            done = true;
        } else if (auto anchors = GoDiffAnchors(a, a0, ma, b, b0, mb); !anchors.empty()) {
            for (auto [i, j] : anchors) {
                GoDiffRange(a, a0, i, b, b0, j, out, maxd);
                out.push_back({ ' ', i, j });
                a0 = i + 1;
                b0 = j + 1;
            }
            GoDiffRange(a, a0, ma, b, b0, mb, out, maxd);
            done = true;
        }
    }
    if (!done) {
        // Lines of one side alone, or too many to find the fewest of and
        // nothing to split them at: removed and added whole.
        for (int i = a0; i < ma; i++) out.push_back({ '-', i, b0 });
        for (int j = b0; j < mb; j++) out.push_back({ '+', ma, j });
    }
    for (int k = 0; k < e; k++) out.push_back({ ' ', ma + k, mb + k });
}

// The diff of the lines a and b, in order (GoDiffRange).
static vector<GoDiffLine> GoDiff(const vector<string_view> &a, const vector<string_view> &b, int maxd = 1000) {
    vector<GoDiffLine> out;
    GoDiffRange(a, 0, (int)a.size(), b, 0, (int)b.size(), out, maxd);
    return out;
}

// What --check says of the file at `path`, which the compile of `srcname`
// would write as `text`: nothing where the file holds that already, and
// else that it is stale, with the diff of the two, unified and with no
// lines around each change, as `diff -U0` gives it, its first `limit` lines.
static string GoCheckFile(const string &path, const string &text, const string &srcname, int limit = 24) {
    string old;
    if (!LoadFile(path, old))
        return cat("cannot read ", path, ", which --check compares with what compiling ", srcname, " writes\n");
    if (old == text) return "";
    auto plural = [](size_t k, const char *what) { return cat(k, " ", what, k == 1 ? "" : "s"); };
    string lf;
    for (size_t i = 0; i < old.size(); i++)
        if (old[i] != '\r' || i + 1 == old.size() || old[i + 1] != '\n') lf += old[i];
    if (lf == text)
        return cat(path, " is stale: it differs from what compiling ", srcname,
                   " writes in its line breaks alone, which goose writes as \\n, not \\r\\n\n");
    auto a = GoLines(old), b = GoLines(text);
    auto diff = GoDiff(a, b);
    // The places that differ: each a run of the diff's lines between those
    // kept, its removed lines before its added ones.
    vector<pair<vector<GoDiffLine>, vector<GoDiffLine>>> hunks;
    size_t removed = 0, added = 0;
    for (size_t i = 0; i < diff.size(); i++) {
        if (diff[i].op == ' ') continue;
        if (i == 0 || diff[i - 1].op == ' ') hunks.emplace_back();
        (diff[i].op == '-' ? hunks.back().first : hunks.back().second).push_back(diff[i]);
        (diff[i].op == '-' ? removed : added)++;
    }
    // A side's range, as a unified diff's header gives it: its first line
    // and how many, 1 left out, or for none the line before.
    auto range = [](const vector<GoDiffLine> &lines, int at, bool first) {
        auto start = lines.empty() ? at : (first ? lines[0].a : lines[0].b) + 1;
        return lines.size() == 1 ? cat(start) : cat(start, ",", lines.size());
    };
    auto out = cat(path, " is stale: compiling ", srcname, " writes it otherwise, in ", plural(hunks.size(), "place"),
                   ": ", plural(removed, "line"), " removed, ", added, " added\n--- ", path, "\n+++ ", path,
                   ", as goose writes it\n");
    int shown = 0;
    size_t lines = 0, places = 0;
    for (auto &[minus, plus] : hunks) {
        if (shown >= limit) break;
        auto &first = minus.empty() ? plus[0] : minus[0];
        Append(out, "@@ -", range(minus, first.a, true), " +", range(plus, first.b, false), " @@\n");
        shown++;
        places++;
        for (auto side : { &minus, &plus })
            for (auto &l : *side) {
                if (shown >= limit) break;
                auto s = l.op == '-' ? a[l.a] : b[l.b];
                Append(out, string(1, l.op), s.substr(0, s.size() - (s.back() == '\n')), "\n");
                if (s.back() != '\n') out += "\\ No newline at end of file\n";
                shown++;
                lines++;
            }
    }
    if (lines < removed + added)
        Append(out, "... and ", plural(removed + added - lines, "more line"), " that differ",
               places < hunks.size() ? cat(", in ", plural(hunks.size() - places, "more place")) : string(), "\n");
    return out;
}

}  // namespace goose
