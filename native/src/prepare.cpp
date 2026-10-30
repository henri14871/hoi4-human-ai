// Prepare a pack's AI folders for loading: copy them into <pack>\.hai\common\<folder> with the
// pack's own scripted triggers expanded in place.
//
// AI files from AI mods often call the mod's scripted triggers (`has_x = yes`). We don't load
// scripted triggers into the game (they'd be shared with gameplay script, and a redefined vanilla
// trigger would desync multiplayer), so instead each call is replaced by the trigger's body:
//   name = yes  ->  AND = { <body> }
//   name = no   ->  NOT = { AND = { <body> } }
// which evaluates the same, in the same scope. Triggers that take $PARAMETERS$ are left as is.
#include "prepare.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "hai.h"

namespace prepare {

static std::string ReadFile(const std::string& path) {
    std::string s;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return s;
    char buf[64 * 1024];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) s.append(buf, n);
    fclose(f);
    if (s.size() >= 3 && (uint8_t)s[0] == 0xEF && (uint8_t)s[1] == 0xBB && (uint8_t)s[2] == 0xBF) s.erase(0, 3);
    return s;
}

static bool WriteFile(const std::string& path, const std::string& text) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "wb") || !f) return false;
    fwrite("\xEF\xBB\xBF", 1, 3, f);  // the game reads these files as UTF-8
    fwrite(text.data(), 1, text.size(), f);
    fclose(f);
    return true;
}

static std::vector<std::string> ListFiles(const std::string& dir, const char* pattern) {
    std::vector<std::string> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\" + pattern).c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) out.push_back(fd.cFileName);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    return out;
}

static bool IsIdent(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '@' || c == '.' || c == ':';
}

// Skip whitespace and # comments.
static size_t SkipSpace(const std::string& s, size_t i) {
    while (i < s.size()) {
        if (s[i] == '#') {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n') {
            ++i;
        } else {
            break;
        }
    }
    return i;
}

// From the '{' at i, return the index just past its matching '}' (or npos).
static size_t MatchBrace(const std::string& s, size_t i) {
    int depth = 0;
    for (; i < s.size(); ++i) {
        char c = s[i];
        if (c == '#') {
            while (i < s.size() && s[i] != '\n') ++i;
        } else if (c == '"') {
            for (++i; i < s.size() && s[i] != '"'; ++i) {}
        } else if (c == '{') {
            ++depth;
        } else if (c == '}' && --depth == 0) {
            return i + 1;
        }
    }
    return std::string::npos;
}

// Top-level `name = { body }` blocks.
static void ParseTriggers(const std::string& s, std::map<std::string, std::string>& out) {
    size_t i = 0;
    while ((i = SkipSpace(s, i)) < s.size()) {
        size_t start = i;
        while (i < s.size() && IsIdent(s[i])) ++i;
        if (i == start) { ++i; continue; }
        std::string name = s.substr(start, i - start);
        i = SkipSpace(s, i);
        if (i >= s.size() || s[i] != '=') continue;
        i = SkipSpace(s, i + 1);
        if (i >= s.size() || s[i] != '{') continue;
        size_t end = MatchBrace(s, i);
        if (end == std::string::npos) return;
        out[name] = s.substr(i + 1, end - i - 2);  // inside the braces
        i = end;
    }
}

// One pass: replace `name = yes|no` calls of known triggers. Returns number replaced.
static int Expand(std::string& s, const std::map<std::string, std::string>& triggers, int& skipped_params) {
    std::string out;
    out.reserve(s.size() + 1024);
    int replaced = 0;
    size_t i = 0;
    while (i < s.size()) {
        char c = s[i];
        if (c == '#') {  // copy comments verbatim
            size_t e = s.find('\n', i);
            if (e == std::string::npos) e = s.size();
            out.append(s, i, e - i);
            i = e;
            continue;
        }
        if (c == '"') {
            size_t e = s.find('"', i + 1);
            e = e == std::string::npos ? s.size() : e + 1;
            out.append(s, i, e - i);
            i = e;
            continue;
        }
        if (IsIdent(c) && (i == 0 || !IsIdent(s[i - 1]))) {
            size_t start = i;
            while (i < s.size() && IsIdent(s[i])) ++i;
            std::string word = s.substr(start, i - start);
            auto it = triggers.find(word);
            if (it != triggers.end()) {
                size_t j = SkipSpace(s, i);
                if (j < s.size() && s[j] == '=') {
                    j = SkipSpace(s, j + 1);
                    bool yes = s.compare(j, 3, "yes") == 0 && (j + 3 >= s.size() || !IsIdent(s[j + 3]));
                    bool no = s.compare(j, 2, "no") == 0 && (j + 2 >= s.size() || !IsIdent(s[j + 2]));
                    if (it->second.find('$') != std::string::npos) {
                        ++skipped_params;
                    } else if (yes || no) {
                        out += yes ? "AND = {" : "NOT = { AND = {";
                        out += it->second;
                        out += yes ? "}" : "} }";
                        i = j + (yes ? 3 : 2);
                        ++replaced;
                        continue;
                    }
                }
            }
            out += word;
            continue;
        }
        out += c;
        ++i;
    }
    s.swap(out);
    return replaced;
}

std::string PreparePack(const std::string& pack_dir, const char* const* folders, int nfolders) {
    std::map<std::string, std::string> triggers;
    std::string tdir = pack_dir + "\\common\\scripted_triggers";
    for (auto& f : ListFiles(tdir, "*.txt")) ParseTriggers(ReadFile(tdir + "\\" + f), triggers);

    std::string root = pack_dir + "\\.hai";
    CreateDirectoryA(root.c_str(), nullptr);
    CreateDirectoryA((root + "\\common").c_str(), nullptr);
    int files = 0, calls = 0, params = 0;
    for (int k = 0; k < nfolders; ++k) {
        std::string src = pack_dir + "\\common\\" + folders[k];
        std::string dst = root + "\\common\\" + folders[k];
        auto list = ListFiles(src, "*.txt");
        if (list.empty()) continue;
        CreateDirectoryA(dst.c_str(), nullptr);
        for (auto& old : ListFiles(dst, "*.txt")) DeleteFileA((dst + "\\" + old).c_str());
        for (auto& f : list) {
            std::string text = ReadFile(src + "\\" + f);
            for (int pass = 0; pass < 8 && !triggers.empty(); ++pass) {  // triggers may call triggers
                int n = Expand(text, triggers, params);
                calls += n;
                if (!n) break;
            }
            if (WriteFile(dst + "\\" + f, text)) ++files;
        }
    }
    hai::Log("prepare: %zu pack scripted triggers; %d AI files prepared, %d trigger calls expanded, %d with "
             "parameters left as is",
             triggers.size(), files, calls, params);
    return root;
}

}  // namespace prepare
