// SPDX-License-Identifier: GPL-2.0-or-later
// CTR Nitro-Fueled launcher for Linux and the Steam Deck: GetLostIn's Windows launcher v0.4
// (GetLostIn/CTR-NitroFueled-PC, GPL-2.0) on SDL3 instead of Win32 + Direct3D 11. Sets up the game
// folder (the 1.21 update merged over the base game with hard links), edits the settings the game
// reads (bbport.ini, the same keys as the in-game menu) and starts the game through
// launcher/sacredfire_launcher.py --game (run.sh, with the player's Discord status alongside).
//
// Files: the launcher's settings in ~/.config/sacredfire-launcher/settings.json (shared with
// sacredfire_launcher.py, which reads them for --play and the Steam shortcut) and the game's in
// the data folder (BB_DATA_DIR: bbport.ini, user/, dlc/, patches.json, backups/).

#include <SDL3/SDL.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstring>
#include <ctime>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "gamepads.h"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

extern char** environ;

namespace fs = std::filesystem;

namespace {

// The Americas build (CUSA13795) and the European one (CUSA14876, as in martijnhe's
// OCTR-NitroFueled-PC-EU): the port runs both.
constexpr const char* kTitleId = "CUSA13795";
constexpr const char* kTitleIds[] = {"CUSA13795", "CUSA14876"};
bool IsCtrTitle(const std::string& title) {
    for (const char* id : kTitleIds)
        if (title == id) return true;
    return false;
}
constexpr const char* kUpdateVersion = "01.21";
constexpr const char* kRepoUrl = "https://github.com/martijnhe/OCTR-NitroFueled-PC-EU";
// The 1.21 update alone is about 8.5 GB; base game + update is much larger.
constexpr std::uintmax_t kCompleteGameMinBytes = 10ull << 30;

// ---------------------------------------------------------------------------------------------
// Text helpers

// Paths are UTF-8 bytes on Linux; the wide strings of the Windows code are UTF-32 here.
std::wstring Widen(const std::string& s) {
    std::wstring w;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = (unsigned char)s[i];
        const int n = c < 0x80 ? 1 : c >> 5 == 6 ? 2 : c >> 4 == 14 ? 3 : c >> 3 == 30 ? 4 : 1;
        uint32_t cp = n == 1 ? c : c & (0x3f >> (n - 1));
        for (int k = 1; k < n && i + k < s.size(); ++k) cp = cp << 6 | ((unsigned char)s[i + k] & 0x3f);
        w += wchar_t(cp);
        i += size_t(n);
    }
    return w;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (const wchar_t wc : w) {
        const uint32_t c = uint32_t(wc);
        if (c < 0x80) s += char(c);
        else if (c < 0x800) s += {char(0xc0 | c >> 6), char(0x80 | (c & 0x3f))};
        else if (c < 0x10000) s += {char(0xe0 | c >> 12), char(0x80 | (c >> 6 & 0x3f)), char(0x80 | (c & 0x3f))};
        else s += {char(0xf0 | c >> 18), char(0x80 | (c >> 12 & 0x3f)), char(0x80 | (c >> 6 & 0x3f)), char(0x80 | (c & 0x3f))};
    }
    return s;
}

std::string PathText(const fs::path& p) { return p.string(); }
fs::path TextPath(const std::string& s) { return fs::path(s); }


std::string Trim(std::string s) {
    while (!s.empty() && (s.back() == '\r' || s.back() == '\n' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string ReadText(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

bool WriteTextAtomic(const fs::path& p, const std::string& text) {
    fs::path temp = p;
    temp += ".tmp";
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << text;
        if (!f) return false;
    }
    std::error_code ec;
    fs::rename(temp, p, ec);
    return !ec;
}

std::string FormatBytes(std::uintmax_t bytes) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "%.1f GB", double(bytes) / double(1ull << 30));
    return buf;
}

// ---------------------------------------------------------------------------------------------
// Language: Spanish or English, chosen in the launcher (default: the system's, LANG).

bool g_spanish = true;
const char* T(const char* es, const char* en) { return g_spanish ? es : en; }

// ---------------------------------------------------------------------------------------------
// key=value files, edited in place: comments, order and unknown keys are kept.

struct Ini {
    std::vector<std::string> lines;
    std::map<std::string, std::string> values;

    void Load(const fs::path& path) {
        lines.clear();
        values.clear();
        std::stringstream ss(ReadText(path));
        std::string line;
        while (std::getline(ss, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            lines.push_back(line);
            const auto t = Trim(line);
            if (t.empty() || t[0] == '#' || t[0] == ';') continue;
            const auto eq = t.find('=');
            if (eq == std::string::npos) continue;
            values[Trim(t.substr(0, eq))] = Trim(t.substr(eq + 1));
        }
    }
    std::string Get(const std::string& key, const std::string& fallback = "") const {
        const auto it = values.find(key);
        return it == values.end() ? fallback : it->second;
    }
    bool Flag(const std::string& key, bool fallback) const {
        const auto v = Get(key);
        return v.empty() ? fallback : v != "0";
    }
    int Int(const std::string& key, int fallback) const {
        const auto v = Get(key);
        if (v.empty()) return fallback;
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        return end && *end == '\0' ? int(n) : fallback;
    }
    void Set(const std::string& key, const std::string& value) {
        values[key] = value;
        erased.erase(key);
    }
    // Removes the key: its line is dropped on Save.
    void Erase(const std::string& key) {
        values.erase(key);
        erased.insert(key);
    }
    std::set<std::string> erased;
    bool Save(const fs::path& path, const char* header) {
        std::vector<std::string> out;
        std::map<std::string, bool> written;
        for (const auto& line : lines) {
            const auto t = Trim(line);
            const auto eq = t.find('=');
            if (!t.empty() && t[0] != '#' && t[0] != ';' && eq != std::string::npos) {
                const auto key = Trim(t.substr(0, eq));
                if (erased.count(key)) continue;
                const auto it = values.find(key);
                if (it != values.end()) {
                    if (!written[key]) out.push_back(key + "=" + it->second);
                    written[key] = true;
                    continue;
                }
            }
            out.push_back(line);
        }
        if (lines.empty() && header) out.push_back(header);
        for (const auto& [key, value] : values) {
            if (!written[key]) out.push_back(key + "=" + value);
        }
        std::string text;
        for (const auto& line : out) text += line + "\n";
        if (!WriteTextAtomic(path, text)) return false;
        lines = out;
        erased.clear();
        return true;
    }
};

// ---------------------------------------------------------------------------------------------
// The launcher's own settings: ~/.config/sacredfire-launcher/settings.json, one flat JSON object
// shared with launcher/sacredfire_launcher.py (its --play reads them). The same calls as Ini;
// true/false read as "1"/"0" and are written back as JSON booleans; unknown keys are kept.

struct JsonSettings {
    std::vector<std::string> order;
    std::map<std::string, std::string> values;  // decoded text
    std::set<std::string> booleans, numbers;    // written back as JSON true/false, numbers

    static std::string Unescape(const std::string& s, size_t& i) {  // s[i] is after the opening quote
        std::string out;
        for (; i < s.size() && s[i] != '"'; ++i) {
            if (s[i] != '\\' || i + 1 >= s.size()) {
                out += s[i];
                continue;
            }
            const char e = s[++i];
            if (e == 'n') out += '\n';
            else if (e == 't') out += '\t';
            else if (e == 'r') out += '\r';
            else if (e == 'b') out += '\b';
            else if (e == 'f') out += '\f';
            else if (e == 'u' && i + 4 < s.size()) {
                uint32_t cp = uint32_t(std::strtoul(s.substr(i + 1, 4).c_str(), nullptr, 16));
                i += 4;
                if (cp >= 0xd800 && cp < 0xdc00 && i + 6 < s.size() && s[i + 1] == '\\' && s[i + 2] == 'u') {
                    const uint32_t lo = uint32_t(std::strtoul(s.substr(i + 3, 4).c_str(), nullptr, 16));
                    cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                    i += 6;
                }
                out += Narrow(std::wstring(1, wchar_t(cp)));
            } else out += e;
        }
        ++i;
        return out;
    }
    static std::string Escape(const std::string& s) {
        std::string out = "\"";
        for (const char c : s) {
            if (c == '"' || c == '\\') out += '\\', out += c;
            else if (c == '\n') out += "\\n";
            else if (c == '\t') out += "\\t";
            else if (c == '\r') out += "\\r";
            else if ((unsigned char)c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else out += c;
        }
        return out + "\"";
    }
    void Load(const fs::path& path) {
        order.clear();
        values.clear();
        booleans.clear();
        numbers.clear();
        const std::string s = ReadText(path);
        size_t i = s.find('{');
        if (i == std::string::npos) return;
        ++i;
        auto skip = [&] { while (i < s.size() && std::isspace((unsigned char)s[i])) ++i; };
        while (true) {
            skip();
            if (i >= s.size() || s[i] != '"') break;
            ++i;
            const std::string key = Unescape(s, i);
            skip();
            if (i >= s.size() || s[i] != ':') break;
            ++i;
            skip();
            if (i >= s.size()) break;
            std::string value;
            if (s[i] == '"') {
                ++i;
                value = Unescape(s, i);
            } else if (s.compare(i, 4, "true") == 0) {
                value = "1", i += 4, booleans.insert(key);
            } else if (s.compare(i, 5, "false") == 0) {
                value = "0", i += 5, booleans.insert(key);
            } else if (s.compare(i, 4, "null") == 0) {
                i += 4;
            } else {
                const size_t start = i;
                while (i < s.size() && s[i] != ',' && s[i] != '}' && !std::isspace((unsigned char)s[i])) ++i;
                value = s.substr(start, i - start);
                numbers.insert(key);
            }
            if (!values.count(key)) order.push_back(key);
            values[key] = value;
            skip();
            if (i < s.size() && s[i] == ',') ++i;
        }
    }
    std::string Get(const std::string& key, const std::string& fallback = "") const {
        const auto it = values.find(key);
        return it == values.end() || (it->second.empty() && !fallback.empty()) ? fallback : it->second;
    }
    bool Flag(const std::string& key, bool fallback) const {
        const auto v = Get(key);
        return v.empty() ? fallback : v != "0";
    }
    int Int(const std::string& key, int fallback) const {
        const auto v = Get(key);
        if (v.empty()) return fallback;
        char* end = nullptr;
        const long n = std::strtol(v.c_str(), &end, 10);
        return end && *end == '\0' ? int(n) : fallback;
    }
    void Set(const std::string& key, const std::string& value) {
        if (!values.count(key)) order.push_back(key);
        values[key] = value;
        numbers.erase(key);
    }
    /// A JSON boolean (the keys sacredfire_launcher.py reads as true/false).
    void SetFlag(const std::string& key, bool on) {
        Set(key, on ? "1" : "0");
        booleans.insert(key);
    }
    void Erase(const std::string& key) {
        values.erase(key);
        order.erase(std::remove(order.begin(), order.end(), key), order.end());
    }
    bool Save(const fs::path& path, const char*) {
        std::string out = "{";
        bool first = true;
        for (const auto& key : order) {
            const auto it = values.find(key);
            if (it == values.end()) continue;
            out += first ? "\n  " : ",\n  ";
            first = false;
            out += Escape(key) + ": ";
            if (booleans.count(key)) out += it->second != "0" && !it->second.empty() ? "true" : "false";
            else if (numbers.count(key) && !it->second.empty()) out += it->second;
            else out += Escape(it->second);
        }
        out += "\n}";
        std::error_code ec;
        fs::create_directories(path.parent_path(), ec);
        return WriteTextAtomic(path, out);
    }
};

// ---------------------------------------------------------------------------------------------
// param.sfo: the game's title, version and category (gd = game, gp = patch).

std::map<std::string, std::string> ParseSfo(const std::string& data) {
    std::map<std::string, std::string> out;
    auto u32 = [&](size_t at) -> uint32_t {
        if (at + 4 > data.size()) return 0;
        return uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8 |
               uint32_t(uint8_t(data[at + 2])) << 16 | uint32_t(uint8_t(data[at + 3])) << 24;
    };
    auto u16 = [&](size_t at) -> uint32_t {
        if (at + 2 > data.size()) return 0;
        return uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8;
    };
    if (data.size() < 20 || data.compare(0, 4, std::string("\0PSF", 4)) != 0) return out;
    const uint32_t keys = u32(8), values = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && 20 + i * 16 + 16 <= data.size(); ++i) {
        const size_t entry = 20 + size_t(i) * 16;
        const uint32_t key_off = u16(entry), fmt = u16(entry + 2), len = u32(entry + 4), off = u32(entry + 12);
        const size_t key_at = size_t(keys) + key_off;
        if (key_at >= data.size()) continue;
        const size_t key_end = data.find('\0', key_at);
        if (key_end == std::string::npos) continue;
        const std::string key = data.substr(key_at, key_end - key_at);
        const size_t value_at = size_t(values) + off;
        if (value_at + len > data.size()) continue;
        if (fmt == 0x0404) {
            out[key] = std::to_string(u32(value_at));
        } else {
            std::string v = data.substr(value_at, len);
            while (!v.empty() && v.back() == '\0') v.pop_back();
            out[key] = v;
        }
    }
    return out;
}

std::map<std::string, std::string> ReadSfo(const fs::path& file) { return ParseSfo(ReadText(file)); }

enum class Kind { Empty, Missing, NotGame, OtherTitle, OtherVersion, Base, Update, Complete };

struct FolderInfo {
    Kind kind = Kind::Empty;
    std::string title, version, category;
    std::uintmax_t bytes = 0;
    bool split = false;  // Image0 + Sc0 (PS4 PKG Tool): needs preparing before it runs
};

std::uintmax_t FolderBytes(const fs::path& dir) {
    std::uintmax_t total = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec), end;
         !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (it->is_regular_file(e2)) total += it->file_size(e2);
    }
    return total;
}

// Where a dump keeps its files. Most tools give the PS4 layout (eboot.bin and sce_sys/param.sfo
// side by side); PS4 PKG Tool splits it into Image0/ (eboot.bin, sce_module, the data) and Sc0/
// (what goes in sce_sys). Such a folder is "split": preparing the game flattens it.
struct Layout {
    fs::path files, sys;  // the folder that becomes the game root, the one that becomes sce_sys
    bool split = false;
    explicit operator bool() const { return !files.empty(); }
};

Layout FindLayout(const fs::path& dir) {
    std::error_code ec;
    const auto has = [&](const fs::path& p) { return fs::is_regular_file(p, ec); };
    const bool flat = has(dir / L"eboot.bin") && has(dir / L"sce_sys" / L"param.sfo");
    const fs::path image = dir / L"Image0";
    // A folder fixed by hand keeps Image0 next to a copied eboot.bin: it is flat only once
    // sce_module is in the root too.
    if (flat && (!has(image / L"eboot.bin") || fs::is_directory(dir / L"sce_module", ec))) return {dir, dir / L"sce_sys", false};
    if (!has(image / L"eboot.bin")) return {};
    for (const fs::path& sys : {image / L"sce_sys", dir / L"Sc0", dir / L"sce_sys"}) {
        if (has(sys / L"param.sfo")) return {image, sys, true};
    }
    return {};
}

// A folder picked one level too high (the folder that holds CUSA13795) stands for its only game.
fs::path GameRoot(const fs::path& dir) {
    std::error_code ec;
    if (dir.empty() || FindLayout(dir)) return dir;
    // Image0 itself, picked from a PS4 PKG Tool dump, stands for the dump.
    if (strcasecmp(dir.filename().c_str(), "Image0") == 0 && FindLayout(dir.parent_path())) return dir.parent_path();
    fs::path found;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
        if (it->is_directory(ec) && FindLayout(it->path())) {
            if (!found.empty()) return dir;  // more than one: the player chooses
            found = it->path();
        }
    }
    return found.empty() ? dir : found;
}

/// The files of a dump as (source, path in the PS4 layout).
std::vector<std::pair<fs::path, fs::path>> LayoutFiles(const fs::path& dir, std::error_code& ec) {
    std::vector<std::pair<fs::path, fs::path>> files;
    const Layout layout = FindLayout(dir);
    if (!layout) return files;
    auto add = [&](const fs::path& from, const fs::path& prefix) {
        for (fs::recursive_directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e2;
            if (it->is_regular_file(e2)) files.emplace_back(it->path(), prefix / fs::relative(it->path(), from, e2));
        }
    };
    add(layout.files, {});
    if (!ec && layout.split && layout.sys != layout.files / L"sce_sys") add(layout.sys, L"sce_sys");
    return files;
}

FolderInfo Analyze(const fs::path& dir) {
    FolderInfo info;
    if (dir.empty()) return info;
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) {
        info.kind = Kind::Missing;
        return info;
    }
    const Layout layout = FindLayout(dir);
    if (!layout) {
        info.kind = Kind::NotGame;
        return info;
    }
    info.split = layout.split;
    const auto sfo = ReadSfo(layout.sys / L"param.sfo");
    auto get = [&](const char* k) {
        const auto it = sfo.find(k);
        return it == sfo.end() ? std::string() : it->second;
    };
    info.title = get("TITLE_ID");
    info.version = get("APP_VER");
    info.category = get("CATEGORY");
    if (!IsCtrTitle(info.title)) {
        info.kind = Kind::OtherTitle;
    } else if (info.category == "gd") {
        info.kind = Kind::Base;
    } else if (info.version != kUpdateVersion) {
        info.kind = Kind::OtherVersion;
    } else {
        info.bytes = FolderBytes(layout.split ? layout.files : dir);
        info.kind = info.bytes >= kCompleteGameMinBytes ? Kind::Complete : Kind::Update;
    }
    return info;
}

// ---------------------------------------------------------------------------------------------
// Processes

fs::path ExeDir() {
    std::error_code ec;
    const fs::path self = fs::read_symlink("/proc/self/exe", ec);
    return ec ? fs::current_path(ec) : self.parent_path();
}

/// The port folder (run.sh, scripts/, launcher/): above out/ in a checkout, above bin/ in the
/// AppImage (share/sacredfire).
fs::path FindRoot() {
    fs::path dir = ExeDir();
    for (int i = 0; i < 3 && !dir.empty(); ++i) {
        std::error_code ec;
        if (fs::is_regular_file(dir / "run.sh", ec)) return dir;
        dir = dir.parent_path();
    }
    return ExeDir();
}

std::string HomeDir() {
    const char* home = std::getenv("HOME");
    return home && *home ? home : "/tmp";
}

struct Python {
    std::string exe;
    std::string description;
};

/// The Python sacredfire_launcher.py runs with (BB_LAUNCHER_PYTHON: the AppImage's, with GTK),
/// else $PYTHON, else python3 on the PATH.
std::optional<Python> FindPython(const fs::path&) {
    for (const char* name : {"BB_LAUNCHER_PYTHON", "PYTHON"})
        if (const char* env = std::getenv(name); env && *env) return Python{env, env};
    const char* path = std::getenv("PATH");
    std::stringstream ss(path ? path : "/usr/bin:/bin");
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        const fs::path candidate = fs::path(dir.empty() ? "." : dir) / "python3";
        if (access(candidate.c_str(), X_OK) == 0) return Python{candidate.string(), candidate.string()};
    }
    return std::nullopt;
}

struct Captured {
    bool started = false;
    int code = 1;
    std::string output;
};

/// Starts argv[0] (searched on the PATH) in cwd with stdout and stderr into a pipe (read end
/// returned in *out_fd, -1 for none) and stdin from /dev/null, in a process group of its own.
pid_t Spawn(const std::vector<std::string>& argv, const fs::path& cwd, int* out_fd) {
    int pipefd[2] = {-1, -1};
    if (out_fd && pipe2(pipefd, O_CLOEXEC) != 0) return -1;
    const pid_t pid = fork();
    if (pid == 0) {
        setpgid(0, 0);
        const int nul = open("/dev/null", O_RDWR);
        if (nul >= 0) dup2(nul, 0);
        if (out_fd) {
            dup2(pipefd[1], 1);
            dup2(pipefd[1], 2);
        } else if (nul >= 0) {
            dup2(nul, 1);
        }
        if (!cwd.empty() && chdir(cwd.c_str()) != 0) _exit(126);
        std::vector<char*> args;
        for (const auto& a : argv) args.push_back(const_cast<char*>(a.c_str()));
        args.push_back(nullptr);
        execvp(args[0], args.data());
        _exit(127);
    }
    if (out_fd) {
        close(pipefd[1]);
        if (pid < 0) {
            close(pipefd[0]);
            *out_fd = -1;
        } else {
            *out_fd = pipefd[0];
        }
    }
    return pid;
}

/// Runs a program and returns its output (stdout + stderr); stopped after timeout_ms.
Captured RunCaptured(const std::vector<std::string>& argv, const fs::path& cwd, int timeout_ms) {
    Captured result;
    int fd = -1;
    const pid_t pid = Spawn(argv, cwd, &fd);
    if (pid <= 0) return result;
    result.started = true;
    std::atomic<bool> finished{false};
    std::thread killer([&] {
        for (int waited = 0; waited < timeout_ms && !finished; waited += 50) SDL_Delay(50);
        if (!finished) kill(-pid, SIGKILL);
    });
    char buf[4096];
    for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;) result.output.append(buf, size_t(n));
    close(fd);
    int status = 0;
    waitpid(pid, &status, 0);
    finished = true;
    killer.join();
    result.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    return result;
}

void OpenInShell(const fs::path& target) {
    std::error_code ec;
    if (!fs::exists(target, ec)) return;
    const pid_t pid = Spawn({"xdg-open", target.string()}, {}, nullptr);
    if (pid > 0) std::thread([pid] { waitpid(pid, nullptr, 0); }).detach();
}

// File and folder pickers: SDL's dialogs (the desktop portal, or zenity/kdialog), waited for
// here as the Windows launcher's modal dialogs are.
SDL_Window* g_window = nullptr;
struct DialogWait {
    std::atomic<bool> done{false};
    std::vector<fs::path> paths;
};
void SDLCALL DialogDone(void* user, const char* const* list, int) {
    auto* wait = static_cast<DialogWait*>(user);
    for (int i = 0; list && list[i]; ++i) wait->paths.emplace_back(list[i]);
    wait->done = true;
}
std::vector<fs::path> WaitDialog(DialogWait& wait) {
    while (!wait.done) {
        SDL_PumpEvents();
        SDL_Delay(15);
    }
    SDL_FlushEvents(SDL_EVENT_KEY_DOWN, SDL_EVENT_MOUSE_WHEEL);  // what was pressed in the dialog
    return wait.paths;
}

std::vector<fs::path> PickFiles(SDL_Window* owner, const char* title, const char* filter_name, const char* pattern) {
    (void)title;
    DialogWait wait;
    const SDL_DialogFileFilter filter{filter_name, pattern};
    SDL_ShowOpenFileDialog(DialogDone, &wait, owner, &filter, 1, nullptr, true);
    return WaitDialog(wait);
}

std::optional<fs::path> PickFolder(SDL_Window* owner, const char* title, const fs::path& start) {
    (void)title;
    DialogWait wait;
    std::error_code ec;
    const std::string from = !start.empty() && fs::is_directory(start, ec) ? start.string() : HomeDir();
    SDL_ShowOpenFolderDialog(DialogDone, &wait, owner, from.c_str(), false);
    const auto paths = WaitDialog(wait);
    if (paths.empty()) return std::nullopt;
    return paths.front();
}

// ---------------------------------------------------------------------------------------------
// The launcher

struct Choice {
    const char* es;
    const char* en;
    const char* value;
};

// PS4 system language codes (BB_LANGUAGE) of the languages CUSA13795 (the Americas build) has:
// English (1, US), Latin American Spanish (20), French (2) and Brazilian Portuguese (17). Other
// codes (Spain's Spanish 3, German, Italian...) are not in this build and fall back to English.
constexpr Choice kGameLanguages[] = {
    {"Inglés", "English", "1"},
    {"Español latinoamericano", "Spanish (Latin America)", "20"},
    {"Francés", "French", "2"},
    {"Portugués (Brasil)", "Portuguese (Brazil)", "17"},
};
// The European build (CUSA14876): the languages of the PAL release (its store page: English,
// French, German, Italian, Spanish and Arabic), as PS4 codes. A language the files do not hold
// falls back to English in the game.
constexpr Choice kGameLanguagesEu[] = {
    {"Inglés (Reino Unido)", "English (UK)", "18"},
    {"Español (España)", "Spanish (Spain)", "3"},
    {"Francés", "French", "2"},
    {"Alemán", "German", "4"},
    {"Italiano", "Italian", "5"},
    {"Árabe", "Arabic", "21"},
};
constexpr Choice kPresentModes[] = {
    {"Mailbox (recomendado)", "Mailbox (recommended)", "Mailbox"},
    {"FIFO (VSync)", "FIFO (VSync)", "Fifo"},
    {"FIFO relajado", "FIFO Relaxed", "FifoRelaxed"},
    {"Inmediato (sin VSync)", "Immediate (no VSync)", "Immediate"},
};
constexpr int kFpsLimits[] = {0, 30, 60, 72, 90, 120, 144, 240};

struct SaveParam {  // OrbisSaveDataParam
    char title[128], subtitle[128], detail[1024];
    uint32_t user_param;
    int32_t pad;
    int64_t mtime;
    uint8_t reserved[32];
};
static_assert(sizeof(SaveParam) == 1328, "OrbisSaveDataParam layout");

struct SaveSlot {
    std::string dir, title, subtitle, detail, modified;
    std::uintmax_t bytes = 0;
};

struct Backup {
    fs::path path;
    std::string name, modified;
    std::uintmax_t bytes = 0;
};

struct PatchInfo {
    std::string key, name, author, note, version;
    bool default_on = false;
    const char* ini_key = nullptr;
};

struct DlcInfo {
    fs::path path;
    std::string file, title, label, problem;
    bool ok = false;
};

enum class Job { None, Merge, Check, Play };

struct App {
    SDL_Window* hwnd = nullptr;
    fs::path root, bin;  // the port (read-only in the AppImage) and its programs
    fs::path data;       // BB_DATA_DIR: bbport.ini, user/, dlc/, patches.json, backups/
    std::string version;
    std::optional<Python> python;

    JsonSettings launcher;  // ~/.config/sacredfire-launcher/settings.json
    Ini game;               // bbport.ini

    // Game folders.
    fs::path game_dir, base_dir, update_dir;
    FolderInfo game_info, base_info, update_info;
    std::atomic<bool> game_checked{false};
    bool game_ok = false;
    std::string check_message;

    // Background work (one at a time).
    std::atomic<Job> job{Job::None};
    std::thread worker;
    std::mutex mutex;  // guards the strings below and log
    std::string job_error;
    std::atomic<size_t> merge_done{0}, merge_total{0};
    std::atomic<bool> job_finished{false};

    // The running game (sacredfire_launcher.py --game in a process group of its own).
    pid_t process = 0;
    std::deque<std::string> log;
    std::string last_status;
    std::atomic<bool> game_running{false};
    int exit_code = 0;
    bool show_exit = false, stopped = false;

    // Monitors and controllers (bb-gpu-capabilities).
    std::vector<std::pair<std::string, std::string>> displays;  // value (number), label
    std::vector<std::pair<std::string, std::string>> gamepads;  // GUID, name
    std::thread probe;
    std::atomic<bool> probe_done{false};

    bool want_close = false;
    bool confirm_clear_cache = false;
    std::string notice;

    // Patches tab.
    std::vector<PatchInfo> patches;
    std::vector<std::string> patch_enabled, patch_disabled;
    // Saves tab.
    std::vector<SaveSlot> saves;
    std::vector<Backup> backups;
    bool saves_dirty = true;
    fs::path pending_path;
    int pending_action = 0, confirm_action = 0;  // 1 restore, 2 delete a backup, 3 remove a DLC
    // DLC tab.
    std::vector<DlcInfo> dlc;
    bool dlc_dirty = true;
};

App g_app;

/// The title ID of the chosen game (the merged folder, else the update or the base game): the
/// saves, the shader cache, the languages and the merged folder's name follow it.
std::string GameTitleId() {
    for (const FolderInfo* info : {&g_app.game_info, &g_app.update_info, &g_app.base_info})
        if (IsCtrTitle(info->title)) return info->title;
    return kTitleId;
}
std::wstring GameTitle() { return Widen(GameTitleId()); }
bool EuEdition() { return GameTitle() == L"CUSA14876"; }

std::string EffectiveRenderRes();
std::string GameLayout();

fs::path LauncherIni() {
    const char* xdg = std::getenv("XDG_CONFIG_HOME");
    const fs::path config = xdg && *xdg ? fs::path(xdg) : fs::path(HomeDir()) / ".config";
    return config / "sacredfire-launcher" / "settings.json";
}
fs::path GameIni() {
    const char* config = std::getenv("BB_CONFIG");
    return config && *config ? fs::path(config) : g_app.data / "bbport.ini";
}

void SaveLauncher() {
    App& a = g_app;
    a.launcher.Set("game_dir", PathText(a.game_dir));
    a.launcher.Set("base_dir", PathText(a.base_dir));
    a.launcher.Set("update_dir", PathText(a.update_dir));
    if (!a.launcher.Save(LauncherIni(), nullptr)) {
        a.notice = T("No se pudieron guardar los ajustes del launcher.", "Could not save the launcher's settings.");
    }
}

void SaveGameIni() {
    if (!g_app.game.Save(GameIni(), "# SacredFire / bbport settings (in-game menu: Insert / L3+R3)")) {
        g_app.notice = T("No se pudo guardar bbport.ini.", "Could not save bbport.ini.");
    }
}

void AddLog(const std::string& line) {
    std::scoped_lock lock{g_app.mutex};
    g_app.log.push_back(line);
    while (g_app.log.size() > 2000) g_app.log.pop_front();
    if (!line.empty()) g_app.last_status = line;
}

void JoinWorker() {
    if (g_app.worker.joinable()) g_app.worker.join();
}

void StartCheck() {
    App& a = g_app;
    a.game_info = Analyze(a.game_dir);
    a.game_checked = false;
    a.game_ok = false;
    a.check_message.clear();
    if (a.game_info.kind != Kind::Complete || !a.python) return;
    if (a.game_info.split) {  // Image0 + Sc0: PlayTab offers to prepare it
        a.game_checked = true;
        return;
    }
    JoinWorker();
    a.job = Job::Check;
    a.job_finished = false;
    const fs::path dir = a.game_dir;
    a.worker = std::thread([dir] {
        App& app = g_app;
        const Captured r = RunCaptured({app.python->exe, "-X", "utf8", (app.root / "scripts" / "game_check.py").string(),
                                        dir.string()},
                                       app.root, 120000);
        {
            std::scoped_lock lock{app.mutex};
            app.game_ok = r.started && r.code == 0;
            app.check_message = r.started ? Trim(r.output) : "Python could not be started.";
        }
        app.game_checked = true;
        app.job_finished = true;
    });
}

fs::path MergeTarget(const fs::path& near_dir) {
    App& a = g_app;
    const fs::path parent = near_dir.parent_path();
    fs::path target = parent / (GameTitleId() + "-1.21");
    if (target == near_dir || target == a.base_dir || target == a.update_dir || target == a.game_dir) {
        target = parent / (GameTitleId() + "-1.21-merged");
    }
    return target;
}

/// Base game + 1.21 update (or one already merged dump in the Image0 + Sc0 layout) -> a new
/// folder of hard links in the PS4 layout (copies across drives).
void StartMerge(std::vector<fs::path> sources) {
    App& a = g_app;
    if (sources.empty()) return;
    const fs::path target = MergeTarget(sources.front());
    JoinWorker();
    a.job = Job::Merge;
    a.job_finished = false;
    a.merge_done = 0;
    a.merge_total = 0;
    {
        std::scoped_lock lock{a.mutex};
        a.job_error.clear();
    }
    a.worker = std::thread([sources, target] {
        App& app = g_app;
        std::vector<std::pair<fs::path, fs::path>> files;  // source, relative
        std::error_code ec;
        for (const fs::path& from : sources) {
            auto more = LayoutFiles(from, ec);
            if (ec) break;
            files.insert(files.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
        }
        std::string error;
        if (ec) error = "Cannot read the game folders: " + ec.message();
        app.merge_total = files.size();
        size_t copied = 0;
        for (size_t i = 0; error.empty() && i < files.size(); ++i) {
            const fs::path dest = target / files[i].second;
            std::error_code e2;
            fs::create_directories(dest.parent_path(), e2);
            if (fs::exists(dest, e2)) fs::remove(dest, e2);
            if (link(files[i].first.c_str(), dest.c_str()) != 0) {
                fs::copy_file(files[i].first, dest, fs::copy_options::overwrite_existing, e2);
                if (e2) {
                    error = "Cannot create " + PathText(dest) + " (" + e2.message() + ")";
                    break;
                }
                ++copied;
            }
            app.merge_done = i + 1;
        }
        {
            std::scoped_lock lock{app.mutex};
            app.job_error = error;
            if (error.empty()) {
                app.game_dir = target;
                app.log.push_back("Merged " + std::to_string(files.size()) + " files into " + PathText(target) +
                                  (copied ? " (" + std::to_string(copied) + " copied: different drive)" : ""));
            }
        }
        app.job_finished = true;
    });
}

const char* DefaultGameLanguage() {
    if (EuEdition()) return g_spanish ? "3" : "18";
    return g_spanish ? "20" : "1";
}

/// The game language to start with: the saved one when the build has it, else the default (older
/// launchers saved codes this build lacks, such as 3 for Spain's Spanish).
std::string GameLanguage() {
    const std::string saved = g_app.launcher.Get("language");
    if (EuEdition()) {
        for (const auto& c : kGameLanguagesEu)
            if (saved == c.value) return saved;
        if (saved == "20") return "3"; // Spanish: Spain's, the one this build has
        if (saved == "1") return "18"; // English: the UK one
        return DefaultGameLanguage();
    }
    for (const auto& c : kGameLanguages)
        if (saved == c.value) return saved;
    if (saved == "3") return "20"; // Spanish: the Latin American one this build has
    return DefaultGameLanguage();
}

bool g_hidden_for_game = false;  // the window was hidden while the game runs (Steam Deck)
bool OnSteamDeck();

/// The game: launcher/sacredfire_launcher.py --game reads the settings saved here (game folder,
/// language, monitor, DLC...), starts run.sh and keeps the player's Discord status while it runs.
void StartGame() {
    App& a = g_app;
    if (!a.python || a.game_running) return;
    a.launcher.Set("language", GameLanguage());
    SaveGameIni();
    SaveLauncher();
    JoinWorker();
    {
        std::scoped_lock lock{a.mutex};
        a.log.clear();
        a.last_status.clear();
    }
    Pads::Stop();  // the game gets the controllers alone
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, nullptr, 0);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
    setenv("PYTHONUTF8", "1", 1);
    setenv("PYTHONIOENCODING", "utf-8", 1);
    setenv("BB_MENU_LANGUAGE", g_spanish ? "es" : "en", 1);  // the in-game menu follows the launcher
    int fd = -1;
    const pid_t pid = Spawn({a.python->exe, "-X", "utf8", (a.root / "launcher" / "sacredfire_launcher.py").string(), "--game"},
                            a.root, &fd);
    if (pid <= 0) {
        AddLog(std::string("Could not start Python: ") + std::strerror(errno));
        a.exit_code = 1;
        a.show_exit = true;
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoFirst);
        return;
    }
    a.process = pid;
    a.game_running = true;
    a.show_exit = false;
    a.stopped = false;
    a.job = Job::Play;
    a.job_finished = false;
    AddLog("Starting the game...");
    a.worker = std::thread([fd, pid] {
        App& app = g_app;
        std::string pending;
        char buf[4096];
        for (ssize_t n; (n = read(fd, buf, sizeof buf)) > 0;) {
            pending.append(buf, size_t(n));
            size_t nl;
            while ((nl = pending.find('\n')) != std::string::npos) {
                AddLog(Trim(pending.substr(0, nl)));
                pending.erase(0, nl + 1);
            }
        }
        if (!pending.empty()) AddLog(Trim(pending));
        close(fd);
        int status = 0;
        waitpid(pid, &status, 0);
        app.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
        app.job_finished = true;
    });
    // The game takes the screen: on the Steam Deck the launcher steps aside completely.
    if (OnSteamDeck()) {
        SDL_HideWindow(a.hwnd);
        g_hidden_for_game = true;
    } else {
        SDL_MinimizeWindow(a.hwnd);
    }
}

void StopGame() {
    g_app.stopped = true;
    if (g_app.process > 0) kill(-g_app.process, SIGTERM);  // the script and the game it started
}

/// Called every frame: picks up finished background work.
void PollJobs() {
    App& a = g_app;
    if (a.probe_done && a.probe.joinable()) a.probe.join();
    if (!a.job_finished) return;
    const Job finished = a.job.exchange(Job::None);
    a.job_finished = false;
    JoinWorker();
    if (finished == Job::Merge) {
        std::string error;
        {
            std::scoped_lock lock{a.mutex};
            error = a.job_error;
        }
        if (error.empty()) {
            SaveLauncher();
            StartCheck();
        } else {
            a.notice = error;
        }
    } else if (finished == Job::Play) {
        a.process = 0;
        a.game_running = false;
        a.show_exit = true;
        a.game.Load(GameIni());  // the in-game menu may have changed settings
        a.saves_dirty = true;
        SDL_InitSubSystem(SDL_INIT_GAMEPAD);
        ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_AutoFirst);
        if (g_hidden_for_game) SDL_ShowWindow(a.hwnd);
        g_hidden_for_game = false;
        SDL_RestoreWindow(a.hwnd);
        SDL_RaiseWindow(a.hwnd);
    }
}

void StartProbe() {
    App& a = g_app;
    a.probe_done = false;
    a.probe = std::thread([] {
        App& app = g_app;
        const fs::path tool = app.bin / "bb-gpu-capabilities";
        std::vector<std::pair<std::string, std::string>> displays, pads;
        const Captured d = RunCaptured({tool.string(), "--displays"}, app.bin, 10000);
        std::stringstream ds(d.output);
        std::string line;
        int number = 0;
        while (std::getline(ds, line)) {
            line = Trim(line);
            const auto t1 = line.find('\t');
            if (t1 == std::string::npos) continue;
            const auto t2 = line.find('\t', t1 + 1);
            ++number;
            std::string label = std::to_string(number) + ": " + line.substr(0, t1) + " (" +
                                line.substr(t1 + 1, t2 == std::string::npos ? std::string::npos : t2 - t1 - 1) + ")";
            if (t2 != std::string::npos && line.substr(t2 + 1) == "1") label += g_spanish ? " - principal" : " - primary";
            displays.emplace_back(std::to_string(number), label);
        }
        const Captured p = RunCaptured({tool.string(), "--gamepads"}, app.bin, 10000);
        std::stringstream ps(p.output);
        while (std::getline(ps, line)) {
            line = Trim(line);
            const auto t = line.find('\t');
            if (t != std::string::npos) pads.emplace_back(line.substr(0, t), line.substr(t + 1));
        }
        std::scoped_lock lock{app.mutex};
        app.displays = std::move(displays);
        app.gamepads = std::move(pads);
        app.probe_done = true;
    });
}

void ChooseFolder(fs::path& slot, FolderInfo& info, const char* title) {
    App& a = g_app;
    fs::path start = slot.empty() ? (a.base_dir.empty() ? a.root : a.base_dir.parent_path()) : slot.parent_path();
    auto picked = PickFolder(a.hwnd, title, start);
    if (!picked) return;
    *picked = GameRoot(*picked);
    const FolderInfo found = Analyze(*picked);
    // Whatever was picked goes where it belongs.
    switch (found.kind) {
    case Kind::Complete:
        a.game_dir = *picked;
        a.game_info = found;
        SaveLauncher();
        if (found.split) {
            StartMerge({*picked});  // Image0 + Sc0 -> the PS4 layout, next to it
        } else {
            StartCheck();
        }
        return;
    case Kind::Base:
        a.base_dir = *picked;
        a.base_info = found;
        break;
    case Kind::Update:
        a.update_dir = *picked;
        a.update_info = found;
        break;
    default:
        slot = *picked;
        info = found;
        break;
    }
    SaveLauncher();
}

// ---------------------------------------------------------------------------------------------
// Saves: <data>/user/savedata/<user>/CUSA13795/<dir> holds the game's files and <dir>.sce_sys its
// param.bin (title, subtitle, detail) and icon0.png, as src/runtime_savedata.c keeps them.






std::string FileTimeText(const fs::path& p) {
    struct stat st{};
    if (stat(p.c_str(), &st) != 0) return {};
    std::tm local{};
    localtime_r(&st.st_mtime, &local);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", &local);
    return buf;
}

std::string Stamp() {
    const std::time_t now = std::time(nullptr);
    std::tm local{};
    localtime_r(&now, &local);
    char buf[64];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &local);
    return buf;
}

std::string Fixed(const char* s, size_t n) { return std::string(s, strnlen(s, n)); }

std::string FormatSize(std::uintmax_t bytes) {
    char buf[64];
    if (bytes >= (1ull << 20)) std::snprintf(buf, sizeof buf, "%.1f MB", double(bytes) / double(1ull << 20));
    else std::snprintf(buf, sizeof buf, "%.0f KB", double(bytes) / 1024.0);
    return buf;
}

/// The game's user folder (saves, shader cache, trophies): BB_USER_DIR, the launcher's
/// user_dir, else <data>/user, as run.sh.
fs::path UserDir() {
    if (const char* env = std::getenv("BB_USER_DIR"); env && *env) return env;
    const std::string chosen = g_app.launcher.Get("user_dir");
    return chosen.empty() ? g_app.data / "user" : TextPath(chosen);
}

fs::path SaveRoot() {
    const fs::path base = UserDir() / L"savedata";
    std::error_code ec;
    for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (fs::is_directory(it->path() / GameTitle(), e2)) return it->path() / GameTitle();
    }
    return base / L"1" / GameTitle();
}

fs::path BackupRoot() { return g_app.data / L"backups"; }

bool IsMetaName(const std::wstring& name) {
    auto ends = [&](const wchar_t* suffix) {
        const size_t n = wcslen(suffix);
        return name.size() > n && name.compare(name.size() - n, n, suffix) == 0;
    };
    return ends(L".sce_sys") || ends(L".memory");
}

std::vector<SaveSlot> ListSaves() {
    std::vector<SaveSlot> slots;
    const fs::path root = SaveRoot();
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_directory(e2) || IsMetaName(it->path().filename().wstring())) continue;
        SaveSlot s;
        s.dir = PathText(it->path().filename());
        s.bytes = FolderBytes(it->path());
        fs::path meta = it->path();
        meta += L".sce_sys";
        const std::string raw = ReadText(meta / L"param.bin");
        if (raw.size() >= sizeof(SaveParam)) {
            SaveParam p;
            std::memcpy(&p, raw.data(), sizeof p);
            s.title = Fixed(p.title, sizeof p.title);
            s.subtitle = Fixed(p.subtitle, sizeof p.subtitle);
            s.detail = Fixed(p.detail, sizeof p.detail);
        }
        s.modified = FileTimeText(it->path());
        for (fs::recursive_directory_iterator f(it->path(), e2), fend; !e2 && f != fend; f.increment(e2)) {
            std::error_code e3;
            if (f->is_regular_file(e3)) {
                const std::string t = FileTimeText(f->path());
                if (t > s.modified) s.modified = t;
            }
        }
        slots.push_back(std::move(s));
    }
    std::sort(slots.begin(), slots.end(), [](const SaveSlot& a, const SaveSlot& b) { return a.dir < b.dir; });
    return slots;
}

std::vector<Backup> ListBackups() {
    std::vector<Backup> list;
    std::error_code ec;
    for (fs::directory_iterator it(BackupRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const std::string name = PathText(it->path().filename());
        if (!it->is_directory(e2) || name.rfind("saves-", 0) != 0) continue;
        list.push_back({it->path(), name, FileTimeText(it->path()), FolderBytes(it->path())});
    }
    std::sort(list.begin(), list.end(), [](const Backup& a, const Backup& b) { return a.name > b.name; });
    return list;
}

bool HasSaves() {
    std::error_code ec;
    return fs::is_directory(SaveRoot(), ec) && !fs::is_empty(SaveRoot(), ec);
}

std::string CopyTree(const fs::path& from, const fs::path& to) {
    std::error_code ec;
    fs::create_directories(to, ec);
    fs::copy(from, to, fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
    return ec ? PathText(from) + " -> " + PathText(to) + ": " + ec.message() : std::string();
}

/// A copy of all saves in backups\saves-<date>[-label]; empty error when there is nothing to copy.
std::string BackupSaves(const char* label, fs::path* made = nullptr) {
    if (!HasSaves()) return {};
    fs::path target = BackupRoot() / Widen("saves-" + Stamp() + (label ? std::string("-") + label : ""));
    const std::string error = CopyTree(SaveRoot(), target);
    if (error.empty() && made) *made = target;
    return error;
}

std::string RestoreBackup(const fs::path& backup) {
    std::string error = BackupSaves("auto");
    if (!error.empty()) return error;
    std::error_code ec;
    fs::remove_all(SaveRoot(), ec);
    if (ec) return PathText(SaveRoot()) + ": " + ec.message();
    return CopyTree(backup, SaveRoot());
}

/// A save folder from elsewhere: this launcher's backup or another PC's CUSA13795 folder (copied
/// as it is), or a decrypted PS4 save (Apollo Save Tool, Save Wizard: its files plus
/// sce_sys\param.sfo), converted to the runtime's layout.
std::string ImportSaves(const fs::path& from) {
    std::error_code ec;
    const fs::path sfo_path = from / L"sce_sys" / L"param.sfo";
    if (fs::is_regular_file(sfo_path, ec)) {
        const auto sfo = ReadSfo(sfo_path);
        auto get = [&](const char* k) {
            const auto it = sfo.find(k);
            return it == sfo.end() ? std::string() : it->second;
        };
        std::string dir = get("SAVEDATA_DIRECTORY");
        if (dir.empty() || dir.size() >= 32 || dir.find_first_of("\\/:") != std::string::npos) {
            return T("param.sfo no dice en qué ranura va la partida (SAVEDATA_DIRECTORY).",
                     "param.sfo does not name the save slot (SAVEDATA_DIRECTORY).");
        }
        std::string error = BackupSaves("antes-de-importar");
        if (!error.empty()) return error;
        const fs::path target = SaveRoot() / Widen(dir);
        fs::path meta = target;
        meta += L".sce_sys";
        fs::remove_all(target, ec);
        fs::create_directories(target, ec);
        fs::create_directories(meta, ec);
        for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().filename() == L"sce_sys") continue;
            std::error_code e2;
            fs::copy(it->path(), target / it->path().filename(),
                     fs::copy_options::recursive | fs::copy_options::overwrite_existing, e2);
            if (e2) return PathText(it->path()) + ": " + e2.message();
        }
        SaveParam p{};
        std::snprintf(p.title, sizeof p.title, "%s", get("MAINTITLE").c_str());
        std::snprintf(p.subtitle, sizeof p.subtitle, "%s", get("SUBTITLE").c_str());
        std::snprintf(p.detail, sizeof p.detail, "%s", get("DETAIL").c_str());
        p.mtime = int64_t(time(nullptr));
        if (!WriteTextAtomic(meta / L"param.bin", std::string(reinterpret_cast<const char*>(&p), sizeof p))) {
            return T("No se pudo escribir param.bin.", "Could not write param.bin.");
        }
        fs::copy_file(from / L"sce_sys" / L"icon0.png", meta / L"icon0.png", fs::copy_options::overwrite_existing, ec);
        return {};
    }
    // The runtime's layout: <dir> next to <dir>.sce_sys.
    bool ours = false;
    for (fs::directory_iterator it(from, ec), end; !ec && it != end; it.increment(ec)) {
        fs::path meta = it->path();
        meta += L".sce_sys";
        std::error_code e2;
        if (!IsMetaName(it->path().filename().wstring()) && fs::is_directory(meta, e2)) ours = true;
    }
    if (!ours) {
        return T("No reconozco esa carpeta: elige una copia de seguridad, la carpeta CUSA13795 de las partidas de "
                 "otro PC o una partida de PS4 descifrada (con sce_sys/param.sfo).",
                 "This folder is not recognized: choose a backup, the CUSA13795 saves folder from another PC or a "
                 "decrypted PS4 save (with sce_sys/param.sfo).");
    }
    std::string error = BackupSaves("antes-de-importar");
    if (!error.empty()) return error;
    return CopyTree(from, SaveRoot());
}

// ---------------------------------------------------------------------------------------------
// Patches: every eboot patch for the game's title (CUSA13795 or CUSA14876) 01.21 in patches\*.xml (shadPS4/GoldHEN XML format).
// The three community patches keep their bbport.ini keys (shared with the in-game menu); others
// are switched in patches.json ({"enabled": [...], "disabled": [...]}), as scripts/patches.py reads.



constexpr std::pair<const char*, const char*> kIniPatches[] = {
    {"CrashTeamRacing.xml/60 FPS Unlock", "patch_60fps"},
    {"CrashTeamRacing.xml/Disable Motion Blur", "patch_no_motion_blur"},
    {"CrashTeamRacing.xml/Resolution Patch (1080p for PS4 Pro)", "patch_pro_1080p"},
};

std::string XmlUnescape(std::string s) {
    const std::pair<const char*, const char*> entities[] = {
        {"&lt;", "<"}, {"&gt;", ">"}, {"&quot;", "\""}, {"&apos;", "'"}, {"&amp;", "&"}};
    for (const auto& [from, to] : entities) {
        for (size_t at = 0; (at = s.find(from, at)) != std::string::npos; at += std::strlen(to))
            s.replace(at, std::strlen(from), to);
    }
    return s;
}

std::map<std::string, std::string> XmlAttributes(const std::string& tag) {
    std::map<std::string, std::string> out;
    size_t i = 0;
    while (i < tag.size()) {
        const size_t eq = tag.find('=', i);
        if (eq == std::string::npos) break;
        size_t name_end = eq;
        while (name_end > i && (tag[name_end - 1] == ' ' || tag[name_end - 1] == '\n' || tag[name_end - 1] == '\r' || tag[name_end - 1] == '\t')) --name_end;
        size_t name_start = name_end;
        while (name_start > i && !std::isspace(static_cast<unsigned char>(tag[name_start - 1])) && tag[name_start - 1] != '<') --name_start;
        size_t q = eq + 1;
        while (q < tag.size() && std::isspace(static_cast<unsigned char>(tag[q]))) ++q;
        if (q >= tag.size() || (tag[q] != '"' && tag[q] != '\'')) break;
        const char quote = tag[q];
        const size_t close = tag.find(quote, q + 1);
        if (close == std::string::npos) break;
        out[tag.substr(name_start, name_end - name_start)] = XmlUnescape(tag.substr(q + 1, close - q - 1));
        i = close + 1;
    }
    return out;
}

std::vector<PatchInfo> LoadPatches() {
    std::vector<PatchInfo> list;
    std::error_code ec;
    std::vector<fs::path> files;
    // The bundled patches and the player's own (the data folder's patches/, as run.sh reads them).
    for (const fs::path& dir : {g_app.root / "patches", g_app.data / "patches"}) {
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            if (it->path().extension() == L".xml") files.push_back(it->path());
        }
        ec.clear();
        if (g_app.data == g_app.root) break;
    }
    std::sort(files.begin(), files.end());
    for (const auto& file : files) {
        const std::string xml = ReadText(file);
        // Files naming other games are skipped; a file without <ID> applies to any.
        bool any_id = false, ours = false;
        for (size_t at = 0; (at = xml.find("<ID>", at)) != std::string::npos; at += 4) {
            const size_t close = xml.find("</ID>", at);
            if (close == std::string::npos) break;
            any_id = true;
            if (Trim(xml.substr(at + 4, close - at - 4)) == GameTitleId()) ours = true;
        }
        if (any_id && !ours) continue;
        for (size_t at = 0; (at = xml.find("<Metadata", at)) != std::string::npos; ++at) {
            const size_t close = xml.find('>', at);
            if (close == std::string::npos) break;
            const auto attr = XmlAttributes(xml.substr(at + 9, close - at - 9));
            auto get = [&](const char* k) {
                const auto it = attr.find(k);
                return it == attr.end() ? std::string() : it->second;
            };
            const std::string elf = get("AppElf");
            if (get("AppVer") != kUpdateVersion || !(elf.empty() || elf == "eboot.bin")) continue;
            PatchInfo p;
            p.name = get("Name");
            // The EU edition's bundled file shares CrashTeamRacing.xml's keys (scripts/patches.py
            // selection_key), so the in-game menu and patches.json switch the same patches.
            const std::string file_name = PathText(file.filename());
            p.key = (file_name == "CrashTeamRacingEU.xml" ? std::string("CrashTeamRacing.xml") : file_name) + "/" + p.name;
            p.author = get("Author");
            p.note = get("Note");
            p.version = get("PatchVer");
            std::string on = get("isEnabled");
            std::transform(on.begin(), on.end(), on.begin(), [](unsigned char c) { return char(std::tolower(c)); });
            p.default_on = on == "true";
            for (const auto& [key, ini] : kIniPatches)
                if (p.key == key) p.ini_key = ini;
            list.push_back(std::move(p));
        }
    }
    return list;
}

/// patches.json's "enabled" and "disabled" string lists (the file only holds those).
void ReadPatchConfig(std::vector<std::string>& enabled, std::vector<std::string>& disabled) {
    enabled.clear();
    disabled.clear();
    const std::string json = ReadText(g_app.data / L"patches.json");
    auto read_list = [&](const char* name, std::vector<std::string>& out) {
        size_t at = json.find(std::string("\"") + name + "\"");
        if (at == std::string::npos) return;
        at = json.find('[', at);
        const size_t end = at == std::string::npos ? at : json.find(']', at);
        if (at == std::string::npos || end == std::string::npos) return;
        for (size_t i = at + 1; i < end; ++i) {
            if (json[i] != '"') continue;
            std::string value;
            for (++i; i < end && json[i] != '"'; ++i) {
                if (json[i] == '\\' && i + 1 < end) ++i;
                value += json[i];
            }
            out.push_back(value);
        }
    };
    read_list("enabled", enabled);
    read_list("disabled", disabled);
}

void WritePatchConfig(const std::vector<std::string>& enabled, const std::vector<std::string>& disabled) {
    auto list = [](const std::vector<std::string>& items) {
        std::string out = "[";
        for (size_t i = 0; i < items.size(); ++i) {
            out += i ? ", \"" : "\"";
            for (const char c : items[i]) {
                if (c == '"' || c == '\\') out += '\\';
                out += c;
            }
            out += '"';
        }
        return out + "]";
    };
    WriteTextAtomic(g_app.data / L"patches.json",
                    "{\n  \"enabled\": " + list(enabled) + ",\n  \"disabled\": " + list(disabled) + "\n}\n");
}

// ---------------------------------------------------------------------------------------------
// UI

// GetLostIn's v0.4 look: a dark theme with the Nitro orange as accent (ApplyTheme); on Linux Noto
// Sans or DejaVu Sans for text, the CTR menu font for titles and DejaVu Sans' symbols as icons.
const ImVec4 kGreen{0.42f, 0.86f, 0.52f, 1.0f};
const ImVec4 kRed{1.0f, 0.42f, 0.40f, 1.0f};
const ImVec4 kYellow{1.0f, 0.78f, 0.30f, 1.0f};
const ImVec4 kGrey{0.58f, 0.61f, 0.70f, 1.0f};
const ImVec4 kText{0.92f, 0.93f, 0.96f, 1.0f};
const ImVec4 kAccent{1.0f, 0.54f, 0.12f, 1.0f};        // #FF8A1F
const ImVec4 kAccentHover{1.0f, 0.63f, 0.26f, 1.0f};
const ImVec4 kAccentActive{0.91f, 0.45f, 0.06f, 1.0f};
const ImVec4 kBlue{0.30f, 0.64f, 1.0f, 1.0f};
const ImVec4 kBg{0.067f, 0.075f, 0.102f, 1.0f};         // #11131A
const ImVec4 kPanel{0.098f, 0.110f, 0.149f, 1.0f};      // #191C26
const ImVec4 kCard{0.118f, 0.133f, 0.188f, 1.0f};       // #1E2230
const ImVec4 kFrame{0.149f, 0.169f, 0.231f, 1.0f};      // #262B3B
const ImVec4 kFrameHover{0.184f, 0.208f, 0.314f, 1.0f};
const ImVec4 kFrameActive{0.220f, 0.251f, 0.388f, 1.0f};
const ImVec4 kBorder{0.173f, 0.192f, 0.259f, 1.0f};

// Icons: symbols DejaVu Sans has (merged into every font by LoadFonts), as UTF-8. The Windows
// launcher uses Segoe Fluent Icons there.
#define ICON_PLAY "\xE2\x96\xB6"       // U+25B6 black right-pointing triangle
#define ICON_SETTINGS "\xE2\x9A\x99"   // U+2699 gear
#define ICON_GAMEPAD "\xE2\x9C\x9C"    // U+271C heavy open centre cross
#define ICON_KEYBOARD "\xE2\x8C\xA8"   // U+2328 keyboard
#define ICON_PUZZLE "\xE2\x9C\x9A"     // U+271A heavy Greek cross
#define ICON_SAVE "\xE2\x96\xA3"       // U+25A3 square with a small square
#define ICON_PACKAGE "\xE2\x9D\x92"    // U+2752 shadowed square
#define ICON_TOOLS "\xE2\x9A\x92"      // U+2692 hammer and pick
#define ICON_FOLDER "\xE2\x9D\x90"     // U+2750 shadowed white square
#define ICON_REFRESH "\xE2\x86\xBB"    // U+21BB clockwise open circle arrow
#define ICON_CHECK "\xE2\x9C\x94"      // U+2714 heavy check mark
#define ICON_WARN "\xE2\x9A\xA0"       // U+26A0 warning sign
#define ICON_INFO "\xE2\x84\xB9"       // U+2139 information source
#define ICON_STOP "\xE2\x96\xA0"       // U+25A0 black square
#define ICON_GLOBE "\xE2\x8A\x95"      // U+2295 circled plus
#define ICON_BLUETOOTH "\xE1\x9B\x92"  // U+16D2 runic berkanan (the Bluetooth logo's rune)
#define ICON_USB "\xE2\x87\x8C"        // U+21CC rightwards harpoon over leftwards
#define ICON_CANCEL "\xE2\x9C\x96"     // U+2716 heavy multiplication x
#define ICON_UNDO "\xE2\x86\xBA"       // U+21BA anticlockwise open circle arrow
#define ICON_VIBRATE "\xE2\x89\x8B"    // U+224B triple tilde
#define ICON_PEOPLE "\xE2\x98\xBA"     // U+263A white smiling face
#define ICON_DISPLAY "\xE2\x96\xAD"    // U+25AD white rectangle
#define ICON_DOWNLOAD "\xE2\x87\xA9"   // U+21E9 downwards white arrow
#define ICON_STAR "\xE2\x98\x85"       // U+2605 black star

ImFont* g_font_heading = nullptr;  // Segoe UI Semibold (section titles)
ImFont* g_font_title = nullptr;    // Bahnschrift (the header, the Play button)
float g_ui_scale = 1.0f;
SDL_Texture* g_logo = nullptr;  // the app icon (LoadLogo)
const char* g_goto_tab = nullptr;             // a tab to bring to front on the next frame
void PlaySummary();
std::string EditionLabel();

/// A section title: the semibold font in the accent colour over a separator line.
void Section(const char* label) {
    ImGui::Spacing();
    if (g_font_heading) ImGui::PushFont(g_font_heading, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, kAccent);
    ImGui::SeparatorText(label);
    ImGui::PopStyleColor();
    if (g_font_heading) ImGui::PopFont();
}

/// A rounded panel that grows with its content.
void BeginCard(const char* id) {
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kCard);
    ImGui::PushStyleColor(ImGuiCol_Border, kBorder);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 10.0f * g_ui_scale);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(16.0f * g_ui_scale, 12.0f * g_ui_scale));
    ImGui::BeginChild(id, ImVec2(0, 0),
                      ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding | ImGuiChildFlags_Borders);
}
void EndCard() {
    ImGui::EndChild();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(2);
}

void ApplyTheme(float scale) {
    ImGuiStyle& st = ImGui::GetStyle();
    st.WindowPadding = ImVec2(18 * scale, 14 * scale);
    st.FramePadding = ImVec2(10 * scale, 6 * scale);
    st.ItemSpacing = ImVec2(10 * scale, 8 * scale);
    st.ItemInnerSpacing = ImVec2(8 * scale, 6 * scale);
    st.CellPadding = ImVec2(8 * scale, 5 * scale);
    st.ScrollbarSize = 12 * scale;
    st.GrabMinSize = 12 * scale;
    st.WindowRounding = 0;
    st.ChildRounding = 8 * scale;
    st.FrameRounding = 6 * scale;
    st.PopupRounding = 8 * scale;
    st.ScrollbarRounding = 8 * scale;
    st.GrabRounding = 6 * scale;
    st.TabRounding = 6 * scale;
    st.WindowBorderSize = 0;
    st.ChildBorderSize = 1;
    st.PopupBorderSize = 1;
    st.FrameBorderSize = 0;
    st.TabBorderSize = 0;
    st.TabBarBorderSize = 1;
    st.TabBarOverlineSize = 2 * scale;
    st.SeparatorTextBorderSize = 2 * scale;
    st.SeparatorTextPadding = ImVec2(14 * scale, 4 * scale);
    ImVec4* c = st.Colors;
    c[ImGuiCol_Text] = kText;
    c[ImGuiCol_TextDisabled] = ImVec4(0.45f, 0.48f, 0.56f, 1.0f);
    c[ImGuiCol_WindowBg] = kBg;
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_PopupBg] = kPanel;
    c[ImGuiCol_Border] = kBorder;
    c[ImGuiCol_FrameBg] = kFrame;
    c[ImGuiCol_FrameBgHovered] = kFrameHover;
    c[ImGuiCol_FrameBgActive] = kFrameActive;
    c[ImGuiCol_TitleBg] = kPanel;
    c[ImGuiCol_TitleBgActive] = kPanel;
    c[ImGuiCol_MenuBarBg] = kPanel;
    c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_ScrollbarGrab] = kFrameHover;
    c[ImGuiCol_ScrollbarGrabHovered] = kFrameActive;
    c[ImGuiCol_ScrollbarGrabActive] = kAccentActive;
    c[ImGuiCol_CheckMark] = kAccent;
    c[ImGuiCol_SliderGrab] = kAccent;
    c[ImGuiCol_SliderGrabActive] = kAccentHover;
    c[ImGuiCol_Button] = kFrame;
    c[ImGuiCol_ButtonHovered] = kFrameHover;
    c[ImGuiCol_ButtonActive] = kFrameActive;
    c[ImGuiCol_Header] = ImVec4(1.0f, 0.54f, 0.12f, 0.20f);
    c[ImGuiCol_HeaderHovered] = ImVec4(1.0f, 0.54f, 0.12f, 0.30f);
    c[ImGuiCol_HeaderActive] = ImVec4(1.0f, 0.54f, 0.12f, 0.42f);
    c[ImGuiCol_Separator] = kBorder;
    c[ImGuiCol_SeparatorHovered] = kAccentHover;
    c[ImGuiCol_SeparatorActive] = kAccent;
    c[ImGuiCol_ResizeGrip] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_Tab] = kPanel;
    c[ImGuiCol_TabHovered] = kFrameHover;
    c[ImGuiCol_TabSelected] = kCard;
    c[ImGuiCol_TabSelectedOverline] = kAccent;
    c[ImGuiCol_TabDimmed] = kPanel;
    c[ImGuiCol_TabDimmedSelected] = kCard;
    c[ImGuiCol_TabDimmedSelectedOverline] = kAccent;
    c[ImGuiCol_PlotHistogram] = kAccent;
    c[ImGuiCol_PlotHistogramHovered] = kAccentHover;
    c[ImGuiCol_TableHeaderBg] = kFrame;
    c[ImGuiCol_TableBorderStrong] = kBorder;
    c[ImGuiCol_TableBorderLight] = ImVec4(0.15f, 0.17f, 0.23f, 1.0f);
    c[ImGuiCol_TableRowBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1, 1, 1, 0.025f);
    c[ImGuiCol_TextLink] = kBlue;
    c[ImGuiCol_TextSelectedBg] = ImVec4(1.0f, 0.54f, 0.12f, 0.35f);
    c[ImGuiCol_NavCursor] = kAccent;
    c[ImGuiCol_ModalWindowDimBg] = ImVec4(0, 0, 0, 0.55f);
}

std::string Describe(const FolderInfo& f) {
    switch (f.kind) {
    case Kind::Empty: return T("sin elegir", "not chosen");
    case Kind::Missing: return T("la carpeta no existe", "the folder does not exist");
    case Kind::NotGame:
        return T("no tiene eboot.bin y sce_sys\\param.sfo (ni Image0 y Sc0)", "no eboot.bin and sce_sys\\param.sfo (nor Image0 and Sc0)");
    case Kind::OtherTitle: return std::string(T("es otro juego o región: ", "another game or region: ")) + f.title;
    case Kind::OtherVersion: return std::string(T("actualización ", "update ")) + f.version + T(" (se necesita la 01.21)", " (01.21 is needed)");
    case Kind::Base: return std::string(T("juego base ", "base game ")) + f.title + " v" + f.version;
    case Kind::Update: return std::string(T("actualización ", "update ")) + f.version + " (" + FormatBytes(f.bytes) + ")";
    case Kind::Complete: return std::string(f.title) + " v" + f.version + " (" + FormatBytes(f.bytes) + ")";
    }
    return {};
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void GameFlag(const char* key, const char* label, bool fallback, const char* help = nullptr) {
    bool v = g_app.game.Flag(key, fallback);
    if (ImGui::Checkbox(label, &v)) {
        g_app.game.Set(key, v ? "1" : "0");
        SaveGameIni();
    }
    if (help) HelpMarker(help);
}

void LauncherFlag(const char* key, const char* label, bool fallback, const char* help = nullptr) {
    bool v = g_app.launcher.Flag(key, fallback);
    if (ImGui::Checkbox(label, &v)) {
        g_app.launcher.Set(key, v ? "1" : "0");
        SaveLauncher();
    }
    if (help) HelpMarker(help);
}

template <size_t N>
void ChoiceCombo(const char* label, const char* key, const Choice (&choices)[N], const char* fallback) {
    const std::string current = g_app.launcher.Get(key, fallback);
    const Choice* selected = &choices[0];
    for (const auto& c : choices)
        if (current == c.value) selected = &c;
    if (ImGui::BeginCombo(label, T(selected->es, selected->en))) {
        for (const auto& c : choices) {
            if (ImGui::Selectable(T(c.es, c.en), &c == selected)) {
                g_app.launcher.Set(key, c.value);
                SaveLauncher();
            }
        }
        ImGui::EndCombo();
    }
}

void PlayTab() {
    App& a = g_app;
    const bool busy = a.job != Job::None;
    BeginCard("game");
    if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.15f);
    ImGui::TextUnformatted((std::string(ICON_FOLDER "  ") + T("Tu juego", "Your game")).c_str());
    if (g_font_heading) ImGui::PopFont();

    const bool ready = a.game_info.kind == Kind::Complete && a.game_checked && a.game_ok;
    if (!a.python) {
        ImGui::TextColored(kRed, "%s", T("Falta Python 3: no se encontró python\\python.exe ni Python instalado.",
                                         "Python 3 is missing: no python\\python.exe and no installed Python."));
    }
    if (a.game_info.kind == Kind::Complete) {
        if (a.job == Job::Merge) {
            const size_t total = a.merge_total, done = a.merge_done;
            char overlay[64];
            std::snprintf(overlay, sizeof overlay, "%zu / %zu", done, total);
            ImGui::TextColored(kYellow, "%s", T("Preparando el juego...", "Preparing the game..."));
            ImGui::ProgressBar(total ? float(done) / float(total) : 0.0f, ImVec2(-FLT_MIN, 0), overlay);
        } else if (a.game_info.split) {
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextColored(kYellow, "%s",
                               T("Esta carpeta tiene el formato de PS4 PKG Tool (Image0 y Sc0). Hay que prepararla: "
                                 "se crea una carpeta nueva con enlaces duros, sin tocar la tuya.",
                                 "This folder has the PS4 PKG Tool layout (Image0 and Sc0). It needs preparing: a "
                                 "new folder of hard links is created, yours is not changed."));
            ImGui::PopTextWrapPos();
            ImGui::BeginDisabled(busy);
            if (ImGui::Button(T("Preparar el juego", "Prepare the game"))) StartMerge({a.game_dir});
            ImGui::EndDisabled();
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s %s", T("se crea", "creates"), PathText(MergeTarget(a.game_dir)).c_str());
        } else if (!a.game_checked) {
            ImGui::TextColored(kYellow, "%s", T("Comprobando el juego...", "Checking the game..."));
        } else if (a.game_ok) {
            ImGui::TextColored(kGreen, "%s  %s %s", ICON_CHECK, T("Listo:", "Ready:"), Describe(a.game_info).c_str());
            const std::string edition = EditionLabel();
            if (!edition.empty()) {
                ImGui::SameLine();
                ImGui::TextColored(kBlue, "·  %s", edition.c_str());
            }
        } else {
            ImGui::TextColored(kRed, "%s", T("Estos archivos no sirven:", "These files do not work:"));
            ImGui::PushTextWrapPos(0.0f);
            ImGui::TextUnformatted(a.check_message.c_str());
            ImGui::PopTextWrapPos();
        }
        ImGui::TextColored(kGrey, "%s", PathText(a.game_dir).c_str());
        if (ImGui::Button((std::string(ICON_FOLDER "  ") + T("Cambiar carpeta del juego...", "Change game folder...")).c_str()) &&
            !busy && !a.game_running) {
            ChooseFolder(a.game_dir, a.game_info, "CUSA13795 + 1.21");
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Abrir carpeta", "Open folder"))) OpenInShell(a.game_dir);
    } else {
        if (a.game_info.kind == Kind::Update) {
            ImGui::TextColored(kRed, "%s: %s", PathText(a.game_dir).c_str(),
                               T("solo tiene la actualización 1.21, falta el juego base",
                                 "only has the 1.21 update, the base game is missing"));
        } else if (a.game_info.kind == Kind::Base) {
            ImGui::TextColored(kRed, "%s: %s", PathText(a.game_dir).c_str(),
                               T("es el juego base sin la actualización 1.21", "is the base game without the 1.21 update"));
        } else if (a.game_info.kind != Kind::Empty) {
            ImGui::TextColored(kRed, "%s: %s", PathText(a.game_dir).c_str(), Describe(a.game_info).c_str());
        }
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextUnformatted(T("Necesitas tu propio dump de PS4: el juego base (CUSA13795) y la actualización 1.21 "
                                 "(CUSA13795-patch; o la edición europea CUSA14876 con su 1.21). Si ya los tienes juntos en una carpeta, elige esa.",
                                 "You need your own PS4 dump: the base game (CUSA13795) and the 1.21 update "
                                 "(CUSA13795-patch; or the European edition CUSA14876 with its 1.21). If they are already merged in one folder, "
                                 "choose that one."));
        ImGui::PopTextWrapPos();
        ImGui::Spacing();
        if (ImGui::BeginTable("setup", 3, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("what", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("button", ImGuiTableColumnFlags_WidthFixed);
            auto row = [&](const char* what, fs::path& slot, FolderInfo& info, Kind good, const char* title, const char* id) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(what);
                ImGui::TableNextColumn();
                const bool ok = info.kind == good;
                ImGui::TextColored(ok ? kGreen : (info.kind == Kind::Empty ? kGrey : kRed), "%s", Describe(info).c_str());
                if (!slot.empty()) ImGui::TextColored(kGrey, "%s", PathText(slot).c_str());
                ImGui::TableNextColumn();
                ImGui::PushID(id);
                if (ImGui::Button(T("Elegir...", "Choose...")) && !busy) ChooseFolder(slot, info, title);
                ImGui::PopID();
            };
            row(T("Juego base", "Base game"), a.base_dir, a.base_info, Kind::Base, "CUSA13795", "base");
            row(T("Actualización 1.21", "Update 1.21"), a.update_dir, a.update_info, Kind::Update, "CUSA13795-patch", "update");
            ImGui::EndTable();
        }
        const bool can_merge = a.base_info.kind == Kind::Base && a.update_info.kind == Kind::Update;
        if (a.job == Job::Merge) {
            const size_t total = a.merge_total, done = a.merge_done;
            const float fraction = total ? float(done) / float(total) : 0.0f;
            char overlay[64];
            std::snprintf(overlay, sizeof overlay, "%zu / %zu", done, total);
            ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), overlay);
        } else {
            ImGui::BeginDisabled(!can_merge || busy);
            if (ImGui::Button(T("Preparar el juego (base + 1.21)", "Prepare the game (base + 1.21)"))) {
                StartMerge({a.base_dir, a.update_dir});
            }
            ImGui::EndDisabled();
            if (can_merge) {
                ImGui::SameLine();
                ImGui::TextColored(kGrey, "%s %s", T("se crea", "creates"),
                                   PathText(MergeTarget(a.base_dir)).c_str());
            }
            HelpMarker(T("Une la actualización con el juego base en una carpeta nueva usando enlaces duros: no ocupa "
                         "espacio extra ni cambia tus carpetas originales (si están en discos distintos, se copia).",
                         "Merges the update over the base game into a new folder with hard links: no extra disk "
                         "space and your original folders are not changed (copied if they are on different drives)."));
        }
        ImGui::Spacing();
        if (ImGui::Button(T("Ya tengo el juego preparado...", "I already have the merged game...")) && !busy) {
            ChooseFolder(a.game_dir, a.game_info, "CUSA13795-1.21");
        }
    }

    EndCard();

    if (ready && !a.game_running) {
        ImGui::Spacing();
        BeginCard("summary");
        if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.15f);
        ImGui::TextUnformatted((std::string(ICON_DISPLAY "  ") + T("Así arrancará", "It will start with")).c_str());
        if (g_font_heading) ImGui::PopFont();
        PlaySummary();
        EndCard();
    }

    ImGui::Spacing();
    const float play_height = ImGui::GetFontSize() * 3.2f;
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 12.0f * g_ui_scale);
    ImGui::PushFont(g_font_title, ImGui::GetStyle().FontSizeBase * 1.7f);
    if (a.game_running) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.62f, 0.20f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.74f, 0.26f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(0.52f, 0.16f, 0.14f, 1.0f));
        if (ImGui::Button((std::string(ICON_STOP "  ") + T("CERRAR EL JUEGO", "STOP THE GAME")).c_str(),
                          ImVec2(-FLT_MIN, play_height)))
            StopGame();
        ImGui::PopStyleColor(3);
    } else {
        ImGui::BeginDisabled(!ready || busy || !a.python);
        ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kAccentHover);
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, kAccentActive);
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.06f, 0.04f, 1.0f));
        if (ImGui::Button((std::string(ICON_PLAY "  ") + T("JUGAR", "PLAY")).c_str(), ImVec2(-FLT_MIN, play_height)))
            StartGame();
        ImGui::PopStyleColor(4);
        ImGui::EndDisabled();
    }
    ImGui::PopFont();
    ImGui::PopStyleVar();

    std::string status;
    {
        std::scoped_lock lock{a.mutex};
        status = a.last_status;
    }
    if (a.game_running) {
        ImGui::TextColored(kYellow, "%s", T("El juego está abierto. El primer arranque compila shaders y tarda un poco.",
                                            "The game is running. The first start compiles shaders and takes a while."));
    } else if (a.show_exit) {
        if (a.exit_code == 0 || a.stopped) ImGui::TextColored(kGrey, "%s", T("El juego se cerró.", "The game was closed."));
        else ImGui::TextColored(kRed, T("El juego se cerró con un error (código %lu). Mira el registro abajo o en last_run.log.",
                                        "The game ended with an error (code %lu). See the log below or last_run.log."),
                                (unsigned long)a.exit_code);
    }
    if (!status.empty()) ImGui::TextColored(kGrey, "%s", status.c_str());

    ImGui::Spacing();
    if (ImGui::CollapsingHeader(T("Registro", "Log"), a.game_running || a.show_exit ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
        ImGui::BeginChild("log", ImVec2(0, std::max(ImGui::GetContentRegionAvail().y, ImGui::GetFontSize() * 12.0f)),
                          ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        ImGui::PopStyleColor();
        {
            std::scoped_lock lock{a.mutex};
            ImGuiListClipper clipper;
            clipper.Begin(int(a.log.size()));
            while (clipper.Step())
                for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) ImGui::TextUnformatted(a.log[size_t(i)].c_str());
        }
        if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 4.0f) ImGui::SetScrollHereY(1.0f);
        ImGui::EndChild();
    }
}

// bbport.ini render_res: the scene size (scripts/patches.py, run.py; docs/sacredfire/13-resolution.md).
struct RenderRes {
    const char* value;
    const char* es;
    const char* en;
};
constexpr RenderRes kRenderRes[] = {
    {"480", "480p (854 x 480, PCs muy modestos)", "480p (854 x 480, very low-end PCs)"},
    {"720", "720p (1280 x 720, PCs modestos)", "720p (1280 x 720, low-end PCs)"},
    {"1080", "1080p (1920 x 1080)", "1080p (1920 x 1080)"},
    {"1440", "1440p (2560 x 1440, PS4 Pro)", "1440p (2560 x 1440, PS4 Pro)"},
    {"1620", "1620p (2880 x 1620)", "1620p (2880 x 1620)"},
    {"2160", "4K (3840 x 2160)", "4K (3840 x 2160)"},
};

// The render_res in effect: the setting, or for older settings the Pro mode and 1080p patch.
std::string EffectiveRenderRes() {
    const std::string v = g_app.game.Get("render_res");
    for (const auto& r : kRenderRes)
        if (v == r.value) return v;
    return "1080";
}

/// bbport.ini temporal (the upscaler), with the keys of v0.3 test builds before it: fsr3=1, fsr1=0.
std::string GameTemporal() {
    const Ini& game = g_app.game;
    const std::string value = game.Get("temporal");
    for (const char* known : {"off", "fsr1", "fsr3", "dlss", "fsr4"})
        if (value == known) return value;
    if (game.Flag("fsr3", false)) return "fsr3";
    return game.Flag("fsr1", true) ? "fsr1" : "off";
}

/// The corrected 21:9 / 32:9 layout (scripts/widescreen.py, by tcosi): bbport.ini widescreen_layout
/// "21x9" or "32x9", "off" for 16:9; widescreen_output the screen size.
constexpr const char* kLayouts[][3] = {{"off", "16:9", "16:9"}, {"21x9", "21:9", "21:9"}, {"32x9", "32:9", "32:9"}};
constexpr const char* kLayoutOutputs21[] = {"3440x1440", "2560x1080"};
constexpr const char* kLayoutOutputs32[] = {"5120x1440", "3840x1080", "2560x720", "7680x2160"};

std::string GameLayout() {
    const std::string v = g_app.game.Get("widescreen_layout", "off");
    if (v == "21x9" || v == "32x9") return v;
    const std::string old = g_app.game.Get("aspect_ratio");  // v0.3's single selector
    if (old == "21:9" || old == "21x9") return "21x9";
    if (old == "32:9" || old == "32x9") return "32x9";
    return "off";
}

std::string GameLayoutOutput() {
    const std::string layout = GameLayout(), v = g_app.game.Get("widescreen_output");
    if (layout == "21x9") {
        for (const char* o : kLayoutOutputs21)
            if (v == o) return v;
        return kLayoutOutputs21[0];
    }
    if (layout == "32x9") {
        for (const char* o : kLayoutOutputs32)
            if (v == o) return v;
        return kLayoutOutputs32[0];
    }
    return {};
}

std::string LayoutLabel(const std::string& layout) {
    for (const auto& l : kLayouts)
        if (layout == l[0]) return l[1];
    return "16:9";
}

/// A combo over a bbport.ini key; true when the player picked another value (already set).
template <size_t N>
bool GameChoiceCombo(const char* label, const char* key, const Choice (&choices)[N], const char* fallback,
                     float width_em) {
    App& a = g_app;
    const std::string current = a.game.Get(key, fallback);
    const Choice* selected = &choices[0];
    for (const auto& c : choices)
        if (current == c.value) selected = &c;
    bool changed = false;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * width_em);
    if (ImGui::BeginCombo(label, T(selected->es, selected->en))) {
        for (const auto& c : choices) {
            if (ImGui::Selectable(T(c.es, c.en), &c == selected) && &c != selected) {
                a.game.Set(key, c.value);
                changed = true;
            }
        }
        ImGui::EndCombo();
    }
    return changed;
}

void RenderResCombo() {
    App& a = g_app;
    const std::string current = EffectiveRenderRes();
    const RenderRes* selected = &kRenderRes[0];
    for (const auto& r : kRenderRes)
        if (current == r.value) selected = &r;
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
    if (ImGui::BeginCombo(T("Resolución interna", "Internal resolution"), T(selected->es, selected->en))) {
        for (const auto& r : kRenderRes) {
            if (ImGui::Selectable(T(r.es, r.en), &r == selected)) {
                a.game.Set("render_res", r.value);
                SaveGameIni();
            }
        }
        ImGui::EndCombo();
    }
    HelpMarker(T("El tamaño al que el juego dibuja la escena 3D. Por encima de 1080p usa el modo PS4 Pro del juego "
                 "y la imagen sale en 4K: en una pantalla 1080p eso quita los dientes de sierra. "
                 "4K necesita una GPU potente (una RTX 3070 va a unos 100 FPS); 720p y 480p alivian PCs modestos "
                 "(la imagen sale a 1080p).",
                 "The size the game draws its 3D scene at. Above 1080p it uses the game's PS4 Pro path (turned "
                 "on automatically) and the picture is output at 4K: on a 1080p screen that removes jagged "
                 "edges. 4K needs a strong GPU (an RTX 3070 runs it at about 100 FPS); 720p and 480p help low-end "
                 "PCs (the picture is output at 1080p)."));
}

void SettingsTab() {
    App& a = g_app;
    ImGui::TextColored(kGrey, "%s", T("Se aplican al pulsar Jugar. El menú del juego (Insert o L3+R3) cambia los mismos ajustes.",
                                      "Applied when you press Play. The in-game menu (Insert or L3+R3) changes the same settings."));

    Section(T("Rendimiento", "Performance"));
    {
        int limit = a.game.Int("fps_limit", 60);
        auto name = [](int v) -> std::string {
            if (v == 0) return T("Sin límite", "No limit");
            return std::to_string(v) + " FPS";
        };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::BeginCombo(T("Límite de FPS", "FPS limit"), name(limit).c_str())) {
            for (int v : kFpsLimits) {
                if (ImGui::Selectable(name(v).c_str(), v == limit)) {
                    a.game.Set("fps_limit", std::to_string(v));
                    SaveGameIni();
                }
            }
            ImGui::EndCombo();
        }
        HelpMarker(T("Con el parche de 60 FPS el juego va tan rápido como permita tu PC. En una pantalla de 144 Hz sin "
                     "G-Sync/FreeSync, 72 reparte los fotogramas de forma pareja.",
                     "With the 60 FPS patch the game runs as fast as your PC allows. On a 144 Hz screen without "
                     "G-Sync/FreeSync, 72 divides the refresh evenly."));
    }
    GameFlag("async_pipelines", T("Compilar shaders en segundo plano (experimental)", "Compile shaders in the background (experimental)"), false,
             T("Menos tirones la primera vez que aparece un efecto; a veces un efecto tarda un fotograma en verse.",
               "Fewer hitches when an effect is first seen; an effect may show up a frame late."));

    Section(T("Pantalla", "Display"));
    {
        // Corrected 21:9 and 32:9 layouts (scripts/widescreen.py, tcosi; the European edition
        // through GetLostIn's port): the aspect ratio, then the screen size, as the in-game menu.
        const std::string layout = GameLayout();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::BeginCombo(T("Relación de aspecto", "Aspect ratio"), LayoutLabel(layout).c_str())) {
            for (const auto& l : kLayouts) {
                if (ImGui::Selectable(l[1], layout == l[0]) && layout != l[0]) {
                    a.game.Set("widescreen_layout", l[0]);
                    a.game.Erase("aspect_ratio");
                    if (std::strcmp(l[0], "off") == 0) a.game.Erase("widescreen_output");
                    else a.game.Set("widescreen_output", std::strcmp(l[0], "21x9") == 0 ? kLayoutOutputs21[0] : kLayoutOutputs32[0]);
                    SaveGameIni();
                }
            }
            ImGui::EndCombo();
        }
        HelpMarker(T("21:9 y 32:9 con menús, fondos y un HUD centrado corregidos, hechos por tcosi. Para la actualización 1.21 "
                     "de las ediciones de EE. UU. y europea; por ahora sin online (con el online activado, el juego arranca en "
                     "16:9) y sin FSR ni DLSS. Tus archivos del juego no cambian (se preparan unos 9 MB una vez). La opción "
                     "Ultrawide del menú del juego sigue como estaba.",
                     "21:9 and 32:9 with corrected menus, backgrounds and a centered HUD, made by tcosi. For the 1.21 update "
                     "of the USA and European editions; offline only for now (with online on, the game starts at 16:9) and "
                     "no FSR or DLSS. Your game files are not changed (about 9 MB are prepared once). The in-game menu's "
                     "Ultrawide option stays as it was."));
        if (layout != "off") {
            const std::string output = GameLayoutOutput();
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
            if (ImGui::BeginCombo(T("Tamaño de pantalla", "Screen size"), output.c_str())) {
                auto option = [&](const char* o) {
                    if (ImGui::Selectable(o, output == o)) {
                        a.game.Set("widescreen_output", o);
                        SaveGameIni();
                    }
                };
                if (layout == "21x9") for (const char* o : kLayoutOutputs21) option(o);
                else for (const char* o : kLayoutOutputs32) option(o);
                ImGui::EndCombo();
            }
            if (a.game.Flag("online", false))
                ImGui::TextColored(kYellow, "%s", T("Con el online activado el juego arranca en 16:9.",
                                                    "With online on, the game starts at 16:9."));
        }
    }
    GameFlag("msaa", T("MSAA del juego (2x)", "Game MSAA (2x)"), false,
             T("El antialiasing 2x de la PS4. Desactivado por defecto: en PC dibuja líneas punteadas en el "
               "cielo y en los rayos de luz. A resoluciones altas o con FSR/DLSS casi no se nota. "
               "Requiere reiniciar.",
               "The PS4's 2x anti-aliasing. Off by default: on PC it draws dotted lines in the sky and "
               "in light shafts. At high internal resolutions or with FSR/DLSS it is barely missed. "
               "Needs a restart."));
    const std::string temporal = GameTemporal();
    const bool uw_on = GameLayout() != "off";
    const bool dlss_sized = temporal == "dlss" && a.game.Get("dlss_quality", "auto") != "auto";
    ImGui::BeginDisabled(dlss_sized);
    RenderResCombo();
    ImGui::EndDisabled();
    if (dlss_sized) {
        ImGui::SameLine();
        ImGui::TextColored(kGrey, "%s", T("(la decide la calidad de DLSS)", "(set by the DLSS quality)"));
    }
    {
        // temporal=off|fsr1|fsr3|dlss|fsr4 (bbport.ini): one upscaler at a time.
        // Off with a 21:9 / 32:9 layout (scripts/widescreen.py turns it off).
        static constexpr Choice kTemporal[] = {{"Desactivado", "Off", "off"},
                                               {"AMD FSR 1", "AMD FSR 1", "fsr1"},
                                               {"AMD FSR 3", "AMD FSR 3", "fsr3"},
                                               {"AMD FSR 4 (experimental)", "AMD FSR 4 (experimental)", "fsr4"},
                                               {"NVIDIA DLSS", "NVIDIA DLSS", "dlss"}};
        ImGui::BeginDisabled(uw_on);
        if (GameChoiceCombo(T("Escalado", "Upscaling"), "temporal", kTemporal, temporal.c_str(), 16.0f)) {
            a.game.Erase("fsr1");
            a.game.Erase("fsr3");
            SaveGameIni();
        }
        ImGui::EndDisabled();
        if (uw_on) {
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s", T("(desactivado con 21:9 / 32:9)", "(off with 21:9 / 32:9)"));
        }
        HelpMarker(T("FSR 1 agranda una escena más pequeña con bordes más nítidos que el estirado simple del juego. "
                     "FSR 3, FSR 4 y DLSS reconstruyen el detalle a lo largo de varios cuadros con la profundidad y el movimiento "
                     "del juego, y a la misma resolución también suavizan los bordes. DLSS necesita una tarjeta NVIDIA "
                     "RTX (si no, se usa FSR 3); FSR 4 necesita soporte INT8 y sus modelos (si no, se usa FSR 3). "
                     "FSR 4 es experimental. Si ves estelas o parpadeos, cambia a FSR 1.",
                     "FSR 1 enlarges a smaller scene with sharper edges than the game's plain stretch. FSR 3, FSR 4 and DLSS "
                     "rebuild detail over several frames from the game's depth and motion, and smooth edges even at the "
                     "same resolution. DLSS needs an NVIDIA RTX card (FSR 3 runs otherwise); FSR 4 needs INT8 support "
                     "and its model files (FSR 3 runs otherwise). FSR 4 is experimental. Switch to "
                     "FSR 1 if you see trails or shimmering."));
    }
    if (temporal == "dlss") {
        static constexpr Choice kQuality[] = {
            {"Automática (resolución interna)", "Auto (internal resolution)", "auto"},
            {"DLAA (nativa)", "DLAA (native)", "dlaa"},
            {"Calidad", "Quality", "quality"},
            {"Equilibrado", "Balanced", "balanced"},
            {"Rendimiento", "Performance", "performance"},
            {"Ultra rendimiento", "Ultra Performance", "ultra_performance"}};
        if (GameChoiceCombo(T("Calidad de DLSS", "DLSS quality"), "dlss_quality", kQuality, "auto", 16.0f)) SaveGameIni();
        HelpMarker(T("Automática usa la resolución interna de arriba. Las demás dibujan la escena a la proporción de NVIDIA "
                     "respecto a la salida de DLSS: Calidad 67 %, Equilibrado 58 %, Rendimiento 50 %, Ultra rendimiento "
                     "33 %, DLAA 100 %.",
                     "Auto uses the internal resolution above. The others draw the scene at NVIDIA's share of the DLSS "
                     "output: Quality 67%, Balanced 58%, Performance 50%, Ultra Performance 33%, DLAA 100%."));
        if (a.game.Get("dlss_quality", "auto") != "auto") {
            static constexpr Choice kOutput[] = {{"1920 x 1080", "1920 x 1080", "1080"},
                                                 {"3840 x 2160 (4K)", "3840 x 2160 (4K)", "2160"}};
            if (GameChoiceCombo(T("Salida de DLSS", "DLSS output"), "dlss_output", kOutput, "1080", 16.0f)) SaveGameIni();
        }
        static constexpr Choice kModel[] = {{"Transformer (recomendado)", "Transformer (recommended)", "transformer"},
                                            {"CNN", "CNN", "cnn"}};
        if (GameChoiceCombo(T("Modelo de DLSS", "DLSS model"), "dlss_model", kModel, "transformer", 16.0f)) SaveGameIni();
        HelpMarker(T("Transformer: el modelo actual de NVIDIA (DLSS 4), más nítido y estable, algo más pesado. CNN: el "
                     "modelo anterior (preset E), más ligero; NVIDIA lo marca como obsoleto.",
                     "Transformer: NVIDIA's current model (DLSS 4), sharper and steadier, a little heavier. CNN: the "
                     "older model (preset E), lighter; NVIDIA marks it deprecated."));
    }
    if (temporal == "fsr3") {
        float sharp = float(std::atof(a.game.Get("fsr3_sharpness", "0.30").c_str()));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderFloat(T("Nitidez de FSR 3", "FSR 3 sharpness"), &sharp, 0.0f, 1.0f, "%.2f")) {
            char text[16];
            std::snprintf(text, sizeof text, "%.2f", sharp);
            a.game.Set("fsr3_sharpness", text);
        }
        if (ImGui::IsItemDeactivatedAfterEdit()) SaveGameIni();
    }
    {
        // bbport.ini fullscreen (the in-game menu's switch) and the launcher's own, which
        // sacredfire_launcher.py passes as BB_FULLSCREEN.
        bool full = a.game.Flag("fullscreen", false);
        if (ImGui::Checkbox(T("Pantalla completa", "Fullscreen"), &full)) {
            a.game.Set("fullscreen", full ? "1" : "0");
            a.launcher.SetFlag("fullscreen", full);
            SaveGameIni();
            SaveLauncher();
        }
    }
    GameFlag("show_fps", T("Contador de FPS en la esquina", "FPS counter in the corner"), false);
    {
        std::scoped_lock lock{a.mutex};
        const std::string current = a.launcher.Get("display");
        std::string preview = T("Principal", "Primary");
        for (const auto& [value, label] : a.displays)
            if (value == current) preview = label;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        if (ImGui::BeginCombo(T("Monitor", "Monitor"), preview.c_str())) {
            if (ImGui::Selectable(T("Principal", "Primary"), current.empty())) {
                a.launcher.Set("display", "");
                SaveLauncher();
            }
            for (const auto& [value, label] : a.displays) {
                if (ImGui::Selectable(label.c_str(), value == current)) {
                    a.launcher.Set("display", value);
                    SaveLauncher();
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
    ChoiceCombo(T("Presentación", "Presentation"), "present_mode", kPresentModes, "Mailbox");
    {
        // framegen=off|fsr3 (bbport.ini): AMD FSR 3 frame generation, one interpolated
        // frame between each pair of game frames. Live in game (Insert menu) too.
        static constexpr Choice kFramegen[] = {{"Desactivada", "Off", "off"},
                                               {"AMD FSR 3 (experimental)", "AMD FSR 3 (experimental)", "fsr3"}};
        std::string current = g_app.game.Get("framegen", "off");
        if (current == "fsr" || current == "on" || current == "1") current = "fsr3"; // test builds
        const Choice* selected = &kFramegen[0];
        for (const auto& c : kFramegen)
            if (current == c.value) selected = &c;
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
        if (ImGui::BeginCombo(T("Generación de cuadros", "Frame generation"), T(selected->es, selected->en))) {
            for (const auto& c : kFramegen) {
                if (ImGui::Selectable(T(c.es, c.en), &c == selected)) {
                    a.game.Set("framegen", c.value);
                    SaveGameIni();
                }
            }
            ImGui::EndCombo();
        }
        HelpMarker(T("Muestra un cuadro interpolado entre cada par de cuadros del juego: casi el doble en pantalla. "
                     "El juego, su límite de FPS y su ritmo no cambian. Ideal en monitores de 120 Hz o más con el juego "
                     "a la mitad de la frecuencia (p. ej. límite 72 en 144 Hz). Funciona con cualquier escalado; FSR 3/4 "
                     "y DLSS le dan además el movimiento del juego. Experimental: el HUD rápido puede vibrar.",
                     "Shows an interpolated frame between each pair of game frames: about twice the frames on screen. "
                     "The game, its FPS limit and its timing do not change. Best on 120 Hz+ screens with the game at "
                     "half the refresh rate (e.g. a 72 limit at 144 Hz). Works with any upscaler; FSR 3/4 and DLSS "
                     "also give it the game's motion. Experimental: fast HUD elements may shimmer."));
        if (current != "off") {
            static constexpr Choice kMotion[] = {
                {"Flujo óptico (recomendado)", "Optical flow (recommended)", "flow"},
                {"Movimiento del juego + flujo óptico", "Game motion + optical flow", "game"}};
            const std::string motion = g_app.game.Get("framegen_motion", "flow");
            const Choice* chosen = &kMotion[0];
            for (const auto& c : kMotion)
                if (motion == c.value) chosen = &c;
            ImGui::SetNextItemWidth(ImGui::GetFontSize() * 14.0f);
            if (ImGui::BeginCombo(T("Movimiento para generar", "Frame generation motion"),
                                  T(chosen->es, chosen->en))) {
                for (const auto& c : kMotion) {
                    if (ImGui::Selectable(T(c.es, c.en), &c == chosen)) {
                        a.game.Set("framegen_motion", c.value);
                        SaveGameIni();
                    }
                }
                ImGui::EndCombo();
            }
            HelpMarker(T("El flujo óptico sigue lo que se mueve en pantalla, karts incluidos. El movimiento del juego "
                         "(con FSR 3/4 o DLSS) mueve los karts con la cámara y puede emborronarlos.",
                         "Optical flow follows what moves on screen, karts included. The game's motion (with FSR 3/4 "
                         "or DLSS) moves karts with the camera and can smear them."));
        }
    }

    Section(T("Mando", "Controller"));
    ImGui::TextColored(kGrey, "%s  %s", ICON_GAMEPAD,
                       T("Los mandos, los jugadores y los botones están en la pestaña Controles.",
                         "Controllers, players and buttons are in the Controls tab."));
    ImGui::SameLine();
    if (ImGui::SmallButton(T("Ir a Controles", "Open Controls"))) g_goto_tab = "controls";

    Section(T("Online (RyuNET)", "Online (RyuNET)"));
    {
        static char nick[32] = "";
        static bool nick_loaded = false;
        if (!nick_loaded) {
            std::snprintf(nick, sizeof nick, "%s", a.game.Get("online_name").c_str());
            nick_loaded = true;
        }
        GameFlag("online", T("Jugar online", "Play online"), false,
                 T("Carreras online, salas con amigos y el Pit Stop en el servidor RyuNET. Con el online activado, "
                   "21:9 y 32:9 arrancan en 16:9 por ahora.",
                   "Online races, rooms with friends and the Pit Stop on the RyuNET server. With online on, 21:9 and 32:9 "
                   "start at 16:9 for now."));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        if (ImGui::InputText(T("Apodo", "Nickname"), nick, sizeof nick)) {
            a.game.Set("online_name", nick);
            SaveGameIni();
        }
        HelpMarker(T("De 3 a 16 letras, cifras, _ o -. Vincularlo con Discord (para recuperarlo en otro PC): menú del juego "
                     "(Insert o L3+R3), Online.",
                     "3 to 16 letters, digits, _ or -. Linking it with Discord (to get it back on another PC): the in-game "
                     "menu (Insert or L3+R3), Online."));
    }

    Section(T("Launcher", "Launcher"));
    GameFlag("launcher_at_start", T("Abrir el launcher al iniciar", "Open the launcher at start"), false,
             T("Desactivado: el AppImage y el acceso de Steam arrancan el juego directamente (--launcher abre esta ventana "
               "igualmente). También en el menú del juego (Insert o L3+R3), General.",
               "Off: the AppImage and the Steam shortcut start the game directly (--launcher still opens this window). "
               "Also in the in-game menu (Insert or L3+R3), General."));

    Section(T("Consola", "Console"));
    if (a.launcher.Get("language") != GameLanguage()) a.launcher.Set("language", GameLanguage());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    if (EuEdition()) {
        ChoiceCombo(T("Idioma del juego", "Game language"), "language", kGameLanguagesEu, DefaultGameLanguage());
        HelpMarker(T("Edición europea (CUSA14876): los idiomas de la versión PAL. Si el juego no trae uno, sale en "
                     "inglés. Se aplica al pulsar Jugar.",
                     "European edition (CUSA14876): the PAL release's languages. A language the game lacks shows "
                     "English. Applied when you press Play."));
    } else {
        ChoiceCombo(T("Idioma del juego", "Game language"), "language", kGameLanguages, DefaultGameLanguage());
        HelpMarker(T("Los idiomas que trae esta versión del juego (CUSA13795, América): inglés, español "
                     "latinoamericano, francés y portugués de Brasil. El español de España no está en ella. Se aplica "
                     "al pulsar Jugar.",
                     "The languages this build of the game has (CUSA13795, the Americas): English, Latin American "
                     "Spanish, French and Brazilian Portuguese. Spain's Spanish is not in it. Applied when you press "
                     "Play."));
    }
}

void PatchesTab() {
    App& a = g_app;
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", T("Se aplican al iniciar el juego (o con \"Apply and restart\" del menú del juego).",
                                      "Applied when the game starts (or with the in-game menu's \"Apply and restart\")."));
    ImGui::TextColored(kGrey, "%s", T("21:9 y 32:9 están en Ajustes > Pantalla > Relación de aspecto.",
                                      "21:9 and 32:9 are in Settings > Display > Aspect ratio."));
    ImGui::PopTextWrapPos();
    const bool neo = a.launcher.Flag("neo", false);
    if (a.patches.empty()) {
        ImGui::TextColored(kYellow, "%s", T("No hay parches en la carpeta patches.", "No patches in the patches folder."));
    }
    for (const auto& p : a.patches) {
        ImGui::PushID(p.key.c_str());
        bool on;
        if (p.ini_key) {
            on = a.game.Flag(p.ini_key, false);
        } else {
            const bool listed_on = std::find(a.patch_enabled.begin(), a.patch_enabled.end(), p.key) != a.patch_enabled.end();
            const bool listed_off = std::find(a.patch_disabled.begin(), a.patch_disabled.end(), p.key) != a.patch_disabled.end();
            on = listed_on || (p.default_on && !listed_off);
        }
        const bool needs_neo = p.ini_key && std::strcmp(p.ini_key, "patch_pro_1080p") == 0;
        // The online key patch follows the online switch; the 1080p Pro patch, the resolution.
        const bool hidden = p.key.find("CrashTeamRacingOnline") == 0;
        if (needs_neo || hidden) {  // replaced by Settings
            ImGui::PopID();
            continue;
        }
        // render_res decides the 1080p Pro patch (Settings > Internal resolution).
        if (needs_neo) on = EffectiveRenderRes() == "1080" && neo;
        ImGui::BeginDisabled(needs_neo);
        if (ImGui::Checkbox(p.name.c_str(), &on)) {
            if (p.ini_key) {
                a.game.Set(p.ini_key, on ? "1" : "0");
                SaveGameIni();
            } else {
                auto drop = [&](std::vector<std::string>& v) { v.erase(std::remove(v.begin(), v.end(), p.key), v.end()); };
                drop(a.patch_enabled);
                drop(a.patch_disabled);
                if (on) a.patch_enabled.push_back(p.key);
                else if (p.default_on) a.patch_disabled.push_back(p.key);
                WritePatchConfig(a.patch_enabled, a.patch_disabled);
            }
        }
        ImGui::EndDisabled();
        if (!p.author.empty()) {
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s %s%s%s", T("de", "by"), p.author.c_str(), p.version.empty() ? "" : "  v",
                               p.version.c_str());
        }
        ImGui::Indent();
        ImGui::PushTextWrapPos(0.0f);
        if (needs_neo) ImGui::TextColored(kGrey, "%s", T("Lo controla Ajustes > Resolución interna (1080p con el modo PS4 Pro).",
                                                          "Controlled by Settings > Internal resolution (1080p with PS4 Pro mode)."));
        if (!p.note.empty()) ImGui::TextColored(kGrey, "%s", p.note.c_str());
        ImGui::TextColored(kGrey, "%s", p.key.c_str());
        ImGui::PopTextWrapPos();
        ImGui::Unindent();
        ImGui::PopID();
    }

    Section(T("Añadir parches", "Add patches"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(T("Copia archivos .xml de parches en formato shadPS4 (como los de PS-Game-Patch o GoldHEN) en la "
                             "carpeta patches y pulsa Recargar. Solo aparecen los que son para CUSA13795 versión 01.21.",
                             "Copy patch .xml files in shadPS4 format (like PS-Game-Patch's or GoldHEN's) into the patches "
                             "folder and press Reload. Only those for CUSA13795 version 01.21 are listed."));
    ImGui::TextColored(kGrey, "%s", T("Por ahora no existen trucos públicos para CTR Nitro-Fueled (como desbloquear todos los "
                                      "personajes). Para tener el juego desbloqueado puedes importar una partida en la pestaña "
                                      "Guardados.",
                                      "There are no public cheats for CTR Nitro-Fueled yet (like unlocking every character). To "
                                      "have the game unlocked you can import a save in the Saves tab."));
    ImGui::PopTextWrapPos();
    if (ImGui::Button(T("Abrir la carpeta de parches", "Open the patches folder"))) {
        std::error_code ec;
        fs::create_directories(a.root / L"patches", ec);
        OpenInShell(a.root / L"patches");
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Recargar", "Reload"))) {
        a.patches = LoadPatches();
        ReadPatchConfig(a.patch_enabled, a.patch_disabled);
    }
}

void SavesTab() {
    App& a = g_app;
    if (a.saves_dirty) {
        a.saves = ListSaves();
        a.backups = ListBackups();
        a.saves_dirty = false;
    }
    const bool locked = a.game_running;
    if (locked) {
        ImGui::TextColored(kYellow, "%s", T("Cierra el juego para copiar, importar o restaurar partidas.",
                                            "Close the game to back up, import or restore saves."));
    }
    Section(T("Partidas", "Saves"));
    ImGui::TextColored(kGrey, "%s", PathText(SaveRoot()).c_str());
    if (a.saves.empty()) {
        ImGui::TextUnformatted(T("Todavía no hay partidas guardadas.", "No saves yet."));
    } else if (ImGui::BeginTable("saves", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Ranura", "Slot"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(T("Descripción", "Description"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Tamaño", "Size"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn(T("Modificada", "Modified"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableHeadersRow();
        for (const auto& s : a.saves) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.dir.c_str());
            ImGui::TableNextColumn();
            std::string text = s.title;
            if (!s.subtitle.empty()) text += (text.empty() ? "" : " - ") + s.subtitle;
            if (!s.detail.empty()) text += (text.empty() ? "" : " - ") + s.detail;
            ImGui::TextUnformatted(text.empty() ? "-" : text.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatSize(s.bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(s.modified.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(locked);
    ImGui::BeginDisabled(a.saves.empty());
    if (ImGui::Button(T("Hacer copia de seguridad", "Back up now"))) {
        fs::path made;
        const std::string error = BackupSaves(nullptr, &made);
        a.notice = error.empty() ? std::string(T("Copia creada: ", "Backup created: ")) + PathText(made.filename()) : error;
        a.saves_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Exportar...", "Export..."))) {
        if (const auto dest = PickFolder(a.hwnd, "Export saves", fs::path())) {
            const fs::path target = *dest / Widen("CTR-saves-" + Stamp());
            const std::string error = CopyTree(SaveRoot(), target);
            a.notice = error.empty() ? std::string(T("Partidas exportadas a ", "Saves exported to ")) + PathText(target) : error;
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Importar...", "Import..."))) {
        if (const auto from = PickFolder(a.hwnd, "Import save", fs::path())) {
            const std::string error = ImportSaves(*from);
            a.notice = error.empty() ? std::string(T("Partida importada. La anterior quedó en una copia de seguridad.",
                                                     "Save imported. The previous one was backed up."))
                                     : error;
            a.saves_dirty = true;
        }
    }
    HelpMarker(T("Acepta una copia de seguridad de este launcher, la carpeta CUSA13795 de las partidas de otro PC, o una "
                 "partida de PS4 descifrada (Apollo Save Tool, Save Wizard: una carpeta con sce_sys\\param.sfo). Antes de "
                 "importar se hace una copia de seguridad de tus partidas.",
                 "Takes a backup made by this launcher, the CUSA13795 saves folder from another PC, or a decrypted PS4 "
                 "save (Apollo Save Tool, Save Wizard: a folder with sce_sys\\param.sfo). Your saves are backed up "
                 "before importing."));
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir carpeta", "Open folder"))) {
        std::error_code ec;
        fs::create_directories(SaveRoot(), ec);
        OpenInShell(SaveRoot());
    }

    Section(T("Copias de seguridad", "Backups"));
    if (a.backups.empty()) {
        ImGui::TextColored(kGrey, "%s", T("Todavía no hay copias.", "No backups yet."));
    } else if (ImGui::BeginTable("backups", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Copia", "Backup"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(T("Tamaño", "Size"), ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed);
        for (const auto& b : a.backups) {
            ImGui::PushID(b.name.c_str());
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(b.name.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(FormatSize(b.bytes).c_str());
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(locked);
            if (ImGui::SmallButton(T("Restaurar", "Restore"))) {
                a.pending_path = b.path;
                a.pending_action = 1;
            }
            ImGui::EndDisabled();
            ImGui::TableNextColumn();
            if (ImGui::SmallButton(T("Borrar", "Delete"))) {
                a.pending_path = b.path;
                a.pending_action = 2;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button(T("Abrir carpeta de copias", "Open backups folder"))) {
        std::error_code ec;
        fs::create_directories(BackupRoot(), ec);
        OpenInShell(BackupRoot());
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Actualizar", "Refresh"))) a.saves_dirty = true;
}

// ---------------------------------------------------------------------------------------------
// DLC (optional): the player's add-on packages in <data>/dlc (BB_DLC_DIR), as .pkg files or folders.
// CTR Nitro-Fueled's DLC hold only the license; scripts/content_profile.py reads the same folder
// and the runtime reports each one installed (BB_DLC, BB_DLC_DISABLED from launcher.ini).

fs::path DlcRoot() {
    const char* env = std::getenv("BB_DLC_DIR");
    return env && *env ? fs::path(env) : g_app.data / "dlc";
}

uint32_t Be32(const char* p) {
    return uint32_t(uint8_t(p[0])) << 24 | uint32_t(uint8_t(p[1])) << 16 | uint32_t(uint8_t(p[2])) << 8 | uint32_t(uint8_t(p[3]));
}

/// param.sfo of a PS4 package (entry 0x1000), when it is stored in the clear.
std::map<std::string, std::string> ReadPkgSfo(const fs::path& pkg, std::string& error) {
    std::ifstream f(pkg, std::ios::binary);
    char header[0x20];
    if (!f.read(header, sizeof header) || std::memcmp(header, "\x7f" "CNT", 4) != 0) {
        error = T("no es un paquete de PS4", "not a PS4 package");
        return {};
    }
    const uint32_t count = Be32(header + 0x10), table = Be32(header + 0x18);
    if (count == 0 || count > 4096) {
        error = T("paquete dañado", "damaged package");
        return {};
    }
    std::string entries(size_t(count) * 32, '\0');
    f.seekg(table);
    if (!f.read(entries.data(), std::streamsize(entries.size()))) {
        error = T("paquete dañado", "damaged package");
        return {};
    }
    for (uint32_t i = 0; i < count; ++i) {
        const char* e = entries.data() + size_t(i) * 32;
        if (Be32(e) != 0x1000) continue;
        const uint32_t flags = Be32(e + 8), offset = Be32(e + 16), size = Be32(e + 20);
        if (flags & 0x80000000u) {
            error = T("param.sfo está cifrado", "param.sfo is encrypted");
            return {};
        }
        if (size > (1u << 20)) break;
        std::string data(size, '\0');
        f.seekg(offset);
        if (!f.read(data.data(), size)) break;
        return ParseSfo(data);
    }
    error = T("no tiene param.sfo", "no param.sfo");
    return {};
}

DlcInfo DescribeDlc(const fs::path& path) {
    DlcInfo d;
    d.path = path;
    d.file = PathText(path.filename());
    std::error_code ec;
    std::map<std::string, std::string> sfo;
    if (fs::is_directory(path, ec)) {
        if (!fs::is_regular_file(path / L"sce_sys" / L"param.sfo", ec)) {
            d.problem = T("la carpeta no tiene sce_sys\\param.sfo", "the folder has no sce_sys\\param.sfo");
            return d;
        }
        sfo = ReadSfo(path / L"sce_sys" / L"param.sfo");
    } else {
        sfo = ReadPkgSfo(path, d.problem);
        if (!d.problem.empty()) return d;
    }
    auto get = [&](const char* k) {
        const auto it = sfo.find(k);
        return it == sfo.end() ? std::string() : it->second;
    };
    const std::string content = get("CONTENT_ID");
    const size_t dash = content.rfind('-');
    d.label = dash == std::string::npos ? std::string() : content.substr(dash + 1);
    d.title = get("TITLE");
    if (!IsCtrTitle(get("TITLE_ID")) || get("CATEGORY") != "ac" || d.label.size() != 16) {
        d.problem = T("no es un DLC de CTR Nitro-Fueled (CUSA13795 / CUSA14876)",
                      "not a CTR Nitro-Fueled (CUSA13795 / CUSA14876) DLC");
        return d;
    }
    if (d.title.empty()) d.title = d.label;
    d.ok = true;
    return d;
}

std::vector<DlcInfo> ScanDlc() {
    std::vector<DlcInfo> list;
    std::error_code ec;
    for (fs::directory_iterator it(DlcRoot(), ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        const bool pkg = it->is_regular_file(e2) && (it->path().extension() == L".pkg" || it->path().extension() == L".PKG");
        if (pkg || it->is_directory(e2)) list.push_back(DescribeDlc(it->path()));
    }
    std::sort(list.begin(), list.end(), [](const DlcInfo& a, const DlcInfo& b) { return a.title < b.title; });
    return list;
}

std::vector<std::string> SplitList(const std::string& text) {
    std::vector<std::string> out;
    std::stringstream ss(text);
    std::string item;
    while (std::getline(ss, item, ','))
        if (!Trim(item).empty()) out.push_back(Trim(item));
    return out;
}

std::string JoinList(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) out += (out.empty() ? "" : ",") + item;
    return out;
}

void AddDlcPackages() {
    App& a = g_app;
    const auto files = PickFiles(a.hwnd, "DLC (.pkg)", "PS4 DLC (*.pkg)", "pkg;PKG");
    if (files.empty()) return;
    std::error_code ec;
    fs::create_directories(DlcRoot(), ec);
    int added = 0;
    std::string problems;
    for (const auto& file : files) {
        const DlcInfo d = DescribeDlc(file);
        if (!d.ok) {
            problems += PathText(file.filename()) + ": " + d.problem + ". ";
            continue;
        }
        const fs::path target = DlcRoot() / file.filename();
        std::error_code e2;
        fs::copy_file(file, target, fs::copy_options::overwrite_existing, e2);
        if (e2) {
            problems += PathText(file.filename()) + ": " + e2.message() + ". ";
            continue;
        }
        ++added;
    }
    a.notice = std::string(T("DLC añadidos: ", "DLC added: ")) + std::to_string(added) + (problems.empty() ? "" : ". " + problems);
    a.dlc_dirty = true;
}

void DlcTab() {
    App& a = g_app;
    if (a.dlc_dirty) {
        a.dlc = ScanDlc();
        a.dlc_dirty = false;
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(T("Opcional: el juego funciona igual sin DLC. Los DLC de CTR Nitro-Fueled solo traen la licencia "
                             "(el contenido ya está en el juego); el port los marca como instalados al iniciar.",
                             "Optional: the game runs the same without DLC. CTR Nitro-Fueled's DLC only carry the license "
                             "(the content is already in the game); the port reports them installed at start."));
    ImGui::TextColored(kGrey, "%s", T("Usa tus propios paquetes DLC de PS4 (.pkg) de CUSA13795. Se copian a la carpeta dlc del "
                                      "port; tus archivos originales no cambian.",
                                      "Use your own PS4 DLC packages (.pkg) for CUSA13795. They are copied into the port's "
                                      "dlc folder; your original files are not changed."));
    ImGui::PopTextWrapPos();
    ImGui::Spacing();
    LauncherFlag("dlc", T("Activar los DLC", "Enable DLC"), true);
    const bool enabled = a.launcher.Flag("dlc", true);
    std::vector<std::string> disabled = SplitList(a.launcher.Get("dlc_disabled"));

    Section(T("DLC encontrados", "DLC found"));
    if (a.dlc.empty()) ImGui::TextColored(kGrey, "%s", T("No hay DLC en la carpeta dlc.", "No DLC in the dlc folder."));
    ImGui::BeginDisabled(!enabled);
    for (const auto& d : a.dlc) {
        ImGui::PushID(d.file.c_str());
        if (d.ok) {
            bool on = std::find(disabled.begin(), disabled.end(), d.label) == disabled.end();
            if (ImGui::Checkbox(d.title.c_str(), &on)) {
                disabled.erase(std::remove(disabled.begin(), disabled.end(), d.label), disabled.end());
                if (!on) disabled.push_back(d.label);
                a.launcher.Set("dlc_disabled", JoinList(disabled));
                SaveLauncher();
            }
            ImGui::SameLine();
            ImGui::TextColored(kGrey, "%s", d.label.c_str());
        } else {
            ImGui::TextColored(kRed, "%s: %s", d.file.c_str(), d.problem.c_str());
        }
        ImGui::SameLine();
        ImGui::BeginDisabled(a.game_running);
        if (ImGui::SmallButton(T("Quitar", "Remove"))) {
            a.pending_path = d.path;
            a.pending_action = 3;
        }
        ImGui::EndDisabled();
        ImGui::PopID();
    }
    ImGui::EndDisabled();
    ImGui::Spacing();
    if (ImGui::Button(T("Añadir DLC (.pkg)...", "Add DLC (.pkg)..."))) AddDlcPackages();
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir la carpeta dlc", "Open the dlc folder"))) {
        std::error_code ec;
        fs::create_directories(DlcRoot(), ec);
        OpenInShell(DlcRoot());
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Actualizar##dlc", "Refresh##dlc"))) a.dlc_dirty = true;
    ImGui::TextColored(kGrey, "%s", T("Los cambios se aplican la próxima vez que pulses Jugar.", "Changes apply the next time you press Play."));
}

void SaveConfirmations() {
    App& a = g_app;
    if (a.pending_action) {
        ImGui::OpenPopup("save_confirm");
        a.confirm_action = a.pending_action;
        a.pending_action = 0;
    }
    if (ImGui::BeginPopupModal("save_confirm", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        const std::string name = PathText(a.pending_path.filename());
        if (a.confirm_action == 1) {
            ImGui::Text(T("¿Restaurar %s?", "Restore %s?"), name.c_str());
            ImGui::TextColored(kGrey, "%s", T("Tus partidas actuales se guardan antes en una copia automática.",
                                              "Your current saves are backed up automatically first."));
        } else if (a.confirm_action == 3) {
            ImGui::Text(T("¿Quitar %s de la carpeta dlc?", "Remove %s from the dlc folder?"), name.c_str());
            ImGui::TextColored(kGrey, "%s", T("Solo se borra la copia del port.", "Only the port's copy is deleted."));
        } else {
            ImGui::Text(T("¿Borrar la copia %s? No se puede deshacer.", "Delete the backup %s? This cannot be undone."), name.c_str());
        }
        if (ImGui::Button(a.confirm_action == 1 ? T("Restaurar", "Restore") : a.confirm_action == 3 ? T("Quitar", "Remove") : T("Borrar", "Delete"))) {
            std::string error;
            if (a.confirm_action == 1) {
                error = RestoreBackup(a.pending_path);
                if (error.empty()) a.notice = std::string(T("Restaurada: ", "Restored: ")) + name;
            } else {
                std::error_code ec;
                fs::remove_all(a.pending_path, ec);
                if (ec) error = name + ": " + ec.message();
            }
            if (!error.empty()) a.notice = error;
            a.saves_dirty = true;
            a.dlc_dirty = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// ---------------------------------------------------------------------------------------------
// Keyboard tab: bbport.ini key.<input>=<SDL key names> (src/runtime_pad.c reads them at start;
// a missing line keeps the runtime's default). Keys are captured by their scan code (the key's
// place on the keyboard, as SDL's scancodes), so the binding holds whatever the layout.

struct KeyInput {
    const char* id;      // runtime_pad.c input name
    const char* es;
    const char* en;
    const char* fallback;  // runtime_pad.c bind_defaults()
};
constexpr KeyInput kKeyInputs[] = {
    {"move_up", "Stick izquierdo arriba", "Left stick up", "W"},
    {"move_down", "Stick izquierdo abajo", "Left stick down", "S"},
    {"move_left", "Stick izquierdo izquierda (girar)", "Left stick left (steer)", "A"},
    {"move_right", "Stick izquierdo derecha (girar)", "Left stick right (steer)", "D"},
    {"cross", "X (acelerar)", "Cross (accelerate)", "Space"},
    {"square", "Cuadrado (frenar)", "Square (brake)", "E"},
    {"circle", "Círculo", "Circle", "Left Shift"},
    {"triangle", "Triángulo", "Triangle", "Q"},
    {"r1", "R1", "R1", "3"},
    {"l1", "L1", "L1", "1"},
    {"r2", "R2", "R2", "F"},
    {"l2", "L2", "L2", "R"},
    {"l3", "L3", "L3", "Z"},
    {"r3", "R3", "R3", "C"},
    {"options", "Options (pausa)", "Options (pause)", "Return"},
    {"touchpad", "Panel táctil", "Touchpad", "Tab"},
    {"touchpad_right", "Panel táctil (derecha)", "Touchpad (right)", "Backspace"},
    {"up", "Cruceta arriba", "D-pad up", "I"},
    {"down", "Cruceta abajo", "D-pad down", "K"},
    {"left", "Cruceta izquierda", "D-pad left", "J"},
    {"right", "Cruceta derecha", "D-pad right", "L"},
    {"look_up", "Stick derecho arriba", "Right stick up", "Up"},
    {"look_down", "Stick derecho abajo", "Right stick down", "Down"},
    {"look_left", "Stick derecho izquierda", "Right stick left", "Left"},
    {"look_right", "Stick derecho derecha", "Right stick right", "Right"},
};

// An SDL scancode's name, as src/runtime_pad.c matches key.<input>= ("Left Shift", "Space"...).
const char* SdlKeyName(unsigned code) {
    const char* name = SDL_GetScancodeName(SDL_Scancode(code));
    return name && *name ? name : nullptr;
}

// How a key name reads in the launcher.
std::string KeyLabel(const std::string& name) {
    static const std::map<std::string, std::pair<const char*, const char*>> labels = {
        {"Space", {"Espacio", "Space"}}, {"Return", {"Enter", "Enter"}},
        {"Backspace", {"Retroceso", "Backspace"}}, {"Left Shift", {"Mayús izq.", "Left Shift"}},
        {"Right Shift", {"Mayús der.", "Right Shift"}}, {"Left Ctrl", {"Ctrl izq.", "Left Ctrl"}},
        {"Right Ctrl", {"Ctrl der.", "Right Ctrl"}}, {"Left Alt", {"Alt", "Left Alt"}},
        {"Right Alt", {"Alt Gr", "Right Alt"}}, {"Up", {"Flecha arriba", "Up arrow"}},
        {"Down", {"Flecha abajo", "Down arrow"}}, {"Left", {"Flecha izquierda", "Left arrow"}},
        {"Right", {"Flecha derecha", "Right arrow"}}, {"CapsLock", {"Bloq Mayús", "Caps Lock"}},
        {"Delete", {"Supr", "Delete"}}, {"Home", {"Inicio", "Home"}}, {"End", {"Fin", "End"}},
        {"PageUp", {"Re Pág", "Page Up"}}, {"PageDown", {"Av Pág", "Page Down"}},
        {"`", {"Tecla a la izquierda del 1", "` key"}},
        {"NonUSBackslash", {"Tecla < >", "< > key"}}, {"Keypad Enter", {"Enter (teclado num.)", "Keypad Enter"}},
    };
    const auto it = labels.find(name);
    if (it != labels.end()) return T(it->second.first, it->second.second);
    if (name.rfind("Keypad ", 0) == 0) return std::string(T("Num ", "Num ")) + name.substr(7);
    return name;
}

std::atomic<int> g_key_capture{-1};      // the input waiting for a key, -1 none
std::atomic<unsigned> g_key_captured{0};  // SDL scancode + 1 (0: nothing yet)
bool g_key_replace = false;                 // the captured key replaces the default binding

std::vector<std::string> SplitKeys(const std::string& value) {
    std::vector<std::string> keys;
    std::stringstream ss(value);
    std::string key;
    while (std::getline(ss, key, ',')) {
        key = Trim(key);
        if (!key.empty()) keys.push_back(key);
    }
    return keys;
}

void KeyboardTab() {
    App& a = g_app;
    ImGui::TextWrapped("%s", T("Teclas del jugador 1. El teclado funciona a la vez que el mando. Pulsa \"Cambiar\" y luego la "
                               "tecla; cada acción admite hasta 4 teclas. Se guarda en bbport.ini y vale desde el próximo arranque.",
                               "Player 1's keys. The keyboard works together with the controller. Press \"Change\", then the key; "
                               "each action takes up to 4 keys. Saved in bbport.ini, used from the next start."));
    ImGui::TextColored(kGrey, "%s", T("Insert abre el menú del juego y Esc cancela, por eso no se pueden asignar.",
                                      "Insert opens the in-game menu and Esc cancels, so they cannot be bound."));
    const int capturing = g_key_capture;
    if (capturing >= 0) {
        const unsigned got = g_key_captured.exchange(0);
        if (got) {
            const unsigned code = got - 1;
            if (code == SDL_SCANCODE_ESCAPE) {
                g_key_capture = -1;  // Esc: cancel
            } else if (const char* name = SdlKeyName(code); name && code != SDL_SCANCODE_INSERT) {
                const KeyInput& in = kKeyInputs[capturing];
                const std::string key = std::string("key.") + in.id;
                auto keys = g_key_replace ? std::vector<std::string>{} : SplitKeys(a.game.Get(key));
                if (std::find(keys.begin(), keys.end(), name) == keys.end()) {
                    if (keys.size() >= 4) keys.erase(keys.begin());
                    keys.push_back(name);
                }
                std::string value;
                for (const auto& k : keys) value += (value.empty() ? "" : ",") + k;
                a.game.Set(key, value);
                SaveGameIni();
                g_key_capture = -1;
            } else {
                a.notice = T("Esa tecla no se puede asignar.", "That key cannot be bound.");
                g_key_capture = -1;
            }
        }
    }

    Section(T("Controles", "Controls"));
    if (ImGui::BeginTable("keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Acción", "Action"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(T("Teclas", "Keys"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 14.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(std::size(kKeyInputs)); ++i) {
            const KeyInput& in = kKeyInputs[i];
            const std::string key = std::string("key.") + in.id;
            const bool custom = a.game.values.count(key) > 0;
            const auto keys = SplitKeys(custom ? a.game.Get(key) : in.fallback);
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(in.es, in.en));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (capturing == i) {
                ImGui::TextColored(kYellow, "%s", T("Pulsa una tecla... (Esc cancela)", "Press a key... (Esc cancels)"));
            } else if (keys.empty()) {
                ImGui::TextColored(kGrey, "%s", T("(sin tecla)", "(none)"));
            } else {
                std::string text;
                for (const auto& k : keys) text += (text.empty() ? "" : "  /  ") + KeyLabel(k);
                if (custom) ImGui::TextUnformatted(text.c_str());
                else ImGui::TextColored(kGrey, "%s", text.c_str());
            }
            ImGui::TableNextColumn();
            ImGui::BeginDisabled(capturing >= 0);
            if (ImGui::SmallButton(T("Cambiar", "Change"))) {
                // A key replaces the default binding; on a custom one it is added (up to 4).
                g_key_replace = !custom;
                g_key_captured = 0;
                g_key_capture = i;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton(T("Quitar", "Clear"))) {
                a.game.Set(key, "");
                SaveGameIni();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(!custom);
            if (ImGui::SmallButton(T("Original", "Default"))) {
                a.game.Erase(key);
                SaveGameIni();
            }
            ImGui::EndDisabled();
            ImGui::EndDisabled();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Spacing();
    ImGui::BeginDisabled(capturing >= 0);
    if (ImGui::Button(T("Restaurar todas las teclas", "Restore all keys"))) {
        for (const auto& in : kKeyInputs) a.game.Erase(std::string("key.") + in.id);
        SaveGameIni();
    }
    ImGui::EndDisabled();
    ImGui::TextColored(kGrey, "%s", T("Gris: tecla original. Las teclas van por su posición en el teclado.",
                                      "Grey: default key. Keys go by their place on the keyboard."));
}

// ---------------------------------------------------------------------------------------------
// Controls tab (v0.4). The connected controllers through the game's own SDL3 (gamepads.cpp), so
// names, GUIDs and buttons are what src/runtime_pad.c sees: a live test, the players'
// controllers (bbport.ini port.1-4, as the in-game menu writes them; player 1 also launcher.ini
// gamepad = BB_GAMEPAD), the gamepad buttons (bbport.ini pad.<input>) and the sticks' dead zone
// (launcher.ini pad_deadzone*, passed as BB_PAD_DEADZONE*). The keyboard is its second page.

uint32_t g_pad_selected = 0;
int g_pad_capture = -1;  // kKeyInputs index waiting for a gamepad button, -1 none
bool g_pad_replace = false;
double g_pads_retry = 0.0;

// runtime_pad.c bind_defaults() per kKeyInputs id; nullptr: not a gamepad binding (the sticks).
const char* PadFallback(const std::string& id) {
    static const std::map<std::string, const char*> defaults = {
        {"cross", "a"},           {"circle", "b"},         {"square", "x"},           {"triangle", "y"},
        {"l1", "leftshoulder"},   {"r1", "rightshoulder"}, {"l2", "lefttrigger"},     {"r2", "righttrigger"},
        {"l3", "leftstick"},      {"r3", "rightstick"},    {"options", "start"},      {"touchpad", "back,touchpad"},
        {"touchpad_right", ""},   {"up", "dpup"},          {"down", "dpdown"},        {"left", "dpleft"},
        {"right", "dpright"},
    };
    const auto it = defaults.find(id);
    return it == defaults.end() ? nullptr : it->second;
}

void LeaveControls() {
    if (g_key_capture >= 0) g_key_capture = -1;
    if (g_pad_capture >= 0) {
        g_pad_capture = -1;
        Pads::CancelCapture();
    }
}

/// SDL's gamepads while the launcher shows them; never while the game runs (it has them).
bool EnsurePads() {
    App& a = g_app;
    if (a.game_running) {
        Pads::Stop();
        return false;
    }
    if (Pads::Running()) return true;
    const double now = ImGui::GetTime();
    if (now < g_pads_retry) return false;
    if (!Pads::Start()) {
        g_pads_retry = now + 3.0;
        return false;
    }
    return true;
}

/// The controller of player p (1..4): a GUID, or "" for automatic (p = 1) / nobody.
std::string PlayerPad(int p) {
    std::string v = g_app.game.Get("port." + std::to_string(p));
    if (p == 1 && v.empty()) v = g_app.launcher.Get("gamepad");
    return v;
}

void SetPlayerPad(int p, const Pads::Info* pad) {
    App& a = g_app;
    a.game.Set("port." + std::to_string(p), pad ? pad->guid : "");
    SaveGameIni();
    if (p == 1) {
        a.launcher.Set("gamepad", pad ? pad->guid : "");
        a.launcher.Set("gamepad_name", pad ? pad->name : "");
    }
    a.launcher.Set("port_name." + std::to_string(p), pad ? pad->name : "");
    SaveLauncher();
}

ImU32 Col(const ImVec4& c, float alpha = 1.0f) { return ImGui::GetColorU32(ImVec4(c.x, c.y, c.z, c.w * alpha)); }

/// A drawing of the controller with what is pressed now (the raw input, before any remapping).
void DrawPad(const Pads::Info& info, const Pads::State& st, float dz_inner, float dz_outer) {
    using namespace Pads;
    const bool ps = info.family == Family::PlayStation;
    const float W = std::min(ImGui::GetContentRegionAvail().x, 430.0f * g_ui_scale);
    const float H = W * 0.56f;
    const ImVec2 o = ImGui::GetCursorScreenPos();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    auto P = [&](float x, float y) { return ImVec2(o.x + x * W, o.y + y * W); };
    auto R = [&](float r) { return r * W; };
    const ImU32 body = Col(kFrame), edge = Col(kBorder), idle = Col(ImVec4(0.24f, 0.27f, 0.37f, 1.0f)),
                on = Col(kAccent), text = Col(kText), dim = Col(kGrey);
    auto centered = [&](ImVec2 c, const std::string& label, ImU32 color) {
        const ImVec2 t = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(c.x - t.x * 0.5f, c.y - t.y * 0.5f), color, label.c_str());
    };
    // Body and grips.
    dl->AddCircleFilled(P(0.235f, 0.405f), R(0.125f), body, 48);
    dl->AddCircleFilled(P(0.765f, 0.405f), R(0.125f), body, 48);
    dl->AddRectFilled(P(0.11f, 0.115f), P(0.89f, 0.43f), body, R(0.095f));
    // Triggers (analog fill) and shoulders.
    auto trigger = [&](float x0, float x1, int axis, int button) {
        const float v = std::clamp(st.axes[axis] / 32767.0f, 0.0f, 1.0f);
        dl->AddRectFilled(P(x0, 0.0f), P(x1, 0.04f), idle, R(0.012f));
        if (v > 0.01f) dl->AddRectFilled(P(x0, 0.0f), P(x0 + (x1 - x0) * v, 0.04f), on, R(0.012f));
        centered(P((x0 + x1) * 0.5f, 0.02f), Label(button, info.family, info.type, g_spanish), text);
    };
    trigger(0.14f, 0.34f, AxTriggerLeft, BtnLeftTrigger);
    trigger(0.66f, 0.86f, AxTriggerRight, BtnRightTrigger);
    auto shoulder = [&](float x0, float x1, int button) {
        dl->AddRectFilled(P(x0, 0.055f), P(x1, 0.095f), st.buttons[button] ? on : idle, R(0.012f));
        centered(P((x0 + x1) * 0.5f, 0.075f), Label(button, info.family, info.type, g_spanish), text);
    };
    shoulder(0.14f, 0.34f, BtnLeftShoulder);
    shoulder(0.66f, 0.86f, BtnRightShoulder);
    // D-pad and left stick: PlayStation pads have the d-pad on top, the others the stick.
    const ImVec2 dpad = ps ? ImVec2(0.25f, 0.25f) : ImVec2(0.37f, 0.37f);
    const ImVec2 lstick = ps ? ImVec2(0.37f, 0.37f) : ImVec2(0.25f, 0.25f);
    {
        const float a = 0.027f;
        const struct { float dx, dy; int b; } arms[] = {
            {0, -2 * a, BtnDpadUp}, {0, 2 * a, BtnDpadDown}, {-2 * a, 0, BtnDpadLeft}, {2 * a, 0, BtnDpadRight}};
        dl->AddRectFilled(P(dpad.x - a, dpad.y - a), P(dpad.x + a, dpad.y + a), idle);
        for (const auto& arm : arms)
            dl->AddRectFilled(P(dpad.x + arm.dx - a, dpad.y + arm.dy - a), P(dpad.x + arm.dx + a, dpad.y + arm.dy + a),
                              st.buttons[arm.b] ? on : idle, R(0.006f));
    }
    // Face buttons, with the shapes or letters of this controller.
    {
        const ImVec2 c(0.75f, 0.25f);
        const float d = 0.058f, r = 0.028f;
        const struct { float dx, dy; int b; } face[] = {
            {0, -d, BtnNorth}, {0, d, BtnSouth}, {-d, 0, BtnWest}, {d, 0, BtnEast}};
        for (const auto& f : face) {
            const ImVec2 at = P(c.x + f.dx, c.y + f.dy);
            const bool down = st.buttons[f.b];
            dl->AddCircleFilled(at, R(r), down ? on : idle, 32);
            const float k = R(r) * 0.48f;
            const ImU32 ink = down ? Col(kBg) : text;
            if (ps) {
                const float th = 1.8f * g_ui_scale;
                if (f.b == BtnNorth) {
                    dl->AddTriangle(ImVec2(at.x, at.y - k), ImVec2(at.x - k, at.y + k * 0.75f), ImVec2(at.x + k, at.y + k * 0.75f),
                                    down ? ink : Col(ImVec4(0.35f, 0.88f, 0.66f, 1.0f)), th);
                } else if (f.b == BtnEast) {
                    dl->AddCircle(at, k, down ? ink : Col(ImVec4(1.0f, 0.45f, 0.45f, 1.0f)), 24, th);
                } else if (f.b == BtnSouth) {
                    const ImU32 blue = down ? ink : Col(ImVec4(0.45f, 0.66f, 1.0f, 1.0f));
                    dl->AddLine(ImVec2(at.x - k, at.y - k), ImVec2(at.x + k, at.y + k), blue, th);
                    dl->AddLine(ImVec2(at.x - k, at.y + k), ImVec2(at.x + k, at.y - k), blue, th);
                } else {
                    dl->AddRect(ImVec2(at.x - k * 0.85f, at.y - k * 0.85f), ImVec2(at.x + k * 0.85f, at.y + k * 0.85f),
                                down ? ink : Col(ImVec4(0.96f, 0.60f, 0.88f, 1.0f)), 0.0f, 0, th);
                }
            } else {
                std::string l = Label(f.b, info.family, info.type, g_spanish);
                if (l.size() > 1) l = l.substr(0, 1);
                centered(at, l, ink);
            }
        }
    }
    // Centre buttons: the touchpad on PlayStation pads.
    if (ps) {
        dl->AddRectFilled(P(0.39f, 0.12f), P(0.61f, 0.27f), st.buttons[BtnTouchpad] ? on : idle, R(0.02f));
        dl->AddRectFilled(P(0.335f, 0.13f), P(0.365f, 0.17f), st.buttons[BtnBack] ? on : idle, R(0.01f));
        dl->AddRectFilled(P(0.635f, 0.13f), P(0.665f, 0.17f), st.buttons[BtnStart] ? on : idle, R(0.01f));
        dl->AddCircleFilled(P(0.5f, 0.32f), R(0.02f), st.buttons[BtnGuide] ? on : idle, 24);
    } else {
        dl->AddCircleFilled(P(0.44f, 0.25f), R(0.017f), st.buttons[BtnBack] ? on : idle, 24);
        dl->AddCircleFilled(P(0.56f, 0.25f), R(0.017f), st.buttons[BtnStart] ? on : idle, 24);
        dl->AddCircleFilled(P(0.5f, 0.16f), R(0.028f), st.buttons[BtnGuide] ? on : idle, 32);
    }
    // Sticks: the dead zone (inner ring) and the full-travel ring, the stick's position.
    auto stick = [&](ImVec2 c, int ax, int ay, int button) {
        const ImVec2 at = P(c.x, c.y);
        const float rad = R(0.068f);
        dl->AddCircleFilled(at, rad, Col(kBg), 40);
        dl->AddCircle(at, rad, edge, 40, 1.5f * g_ui_scale);
        if (dz_inner > 0.0f) dl->AddCircle(at, rad * dz_inner, dim, 32, 1.0f * g_ui_scale);
        if (dz_outer < 0.999f) dl->AddCircle(at, rad * dz_outer, Col(kAccent, 0.45f), 40, 1.0f * g_ui_scale);
        const float x = std::clamp(st.axes[ax] / 32767.0f, -1.0f, 1.0f), y = std::clamp(st.axes[ay] / 32767.0f, -1.0f, 1.0f);
        const ImVec2 dot(at.x + x * rad, at.y + y * rad);
        dl->AddLine(at, dot, Col(kAccent, 0.6f), 2.0f * g_ui_scale);
        dl->AddCircleFilled(dot, R(0.026f), st.buttons[button] ? on : text, 24);
    };
    stick(lstick, AxLeftX, AxLeftY, BtnLeftStick);
    stick(ImVec2(0.63f, 0.37f), AxRightX, AxRightY, BtnRightStick);
    ImGui::Dummy(ImVec2(W, H));
}

void ControllersPage() {
    App& a = g_app;
    const bool sdl = EnsurePads();
    if (sdl) Pads::Update();
    const auto& pads = Pads::List();
    // The same model twice: "#2" tells them apart.
    auto display = [&](const Pads::Info& p) {
        int same = 0, index = 0;
        for (const auto& q : pads) {
            if (q.name == p.name) ++same;
            if (q.id == p.id) index = same;
        }
        return same > 1 ? p.name + " #" + std::to_string(index) : p.name;
    };
    auto players_of = [&](const Pads::Info& p) {
        std::string out;
        for (int n = 1; n <= 4; ++n)
            if (PlayerPad(n) == p.guid) out += (out.empty() ? "" : " ") + std::string(T("J", "P")) + std::to_string(n);
        return out;
    };

    Section(T("Mandos conectados", "Connected controllers"));
    if (a.game_running) {
        ImGui::TextColored(kYellow, "%s  %s", ICON_INFO,
                           T("El juego está abierto y usa los mandos. Ciérralo para probarlos aquí; dentro del juego, "
                             "Insert › Controllers asigna los puertos.",
                             "The game is running and has the controllers. Close it to test them here; in the game, "
                             "Insert › Controllers assigns the ports."));
    } else if (!sdl) {
        ImGui::TextColored(kRed, "%s  %s: %s", ICON_WARN, T("No se pueden leer los mandos", "Controllers cannot be read"),
                           Pads::Error().c_str());
    } else if (pads.empty()) {
        BeginCard("nopads");
        ImGui::TextColored(kGrey, "%s", ICON_GAMEPAD);
        ImGui::SameLine();
        ImGui::TextWrapped("%s", T("No hay mandos conectados. Conecta uno por USB o Bluetooth: aparece aquí al momento. "
                                   "El teclado funciona siempre (página Teclado).",
                                   "No controllers connected. Plug one in over USB or pair it over Bluetooth: it shows "
                                   "up here at once. The keyboard always works (Keyboard page)."));
        EndCard();
    } else {
        bool found = false;
        for (const auto& p : pads) found |= p.id == g_pad_selected;
        if (!found) g_pad_selected = pads.front().id;
        if (ImGui::BeginTable("padlayout", 2, ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("list", ImGuiTableColumnFlags_WidthStretch, 0.9f);
            ImGui::TableSetupColumn("test", ImGuiTableColumnFlags_WidthStretch, 1.1f);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            const float row = ImGui::GetTextLineHeightWithSpacing() * 2.4f;
            for (const auto& p : pads) {
                ImGui::PushID(int(p.id));
                const bool selected = p.id == g_pad_selected;
                const ImVec2 at = ImGui::GetCursorScreenPos();
                if (ImGui::Selectable("##pad", selected, 0, ImVec2(0, row))) g_pad_selected = p.id;
                ImDrawList* dl = ImGui::GetWindowDrawList();
                const float pad_x = 10.0f * g_ui_scale, line = ImGui::GetTextLineHeight();
                dl->AddText(ImVec2(at.x + pad_x, at.y + row * 0.5f - line * 0.5f), Col(selected ? kAccent : kGrey),
                            ICON_GAMEPAD);
                const float tx = at.x + pad_x + ImGui::GetFontSize() * 2.0f;
                dl->AddText(g_font_heading ? g_font_heading : ImGui::GetFont(), ImGui::GetFontSize(), ImVec2(tx, at.y + row * 0.12f), Col(kText),
                            display(p).c_str());
                std::string sub = Pads::TypeName(p.type, g_spanish);
                if (p.wireless) sub += std::string(" · ") + ICON_BLUETOOTH + T(" inalámbrico", " wireless");
                else if (p.wired) sub += std::string(" · ") + ICON_USB + " USB";
                if (p.battery >= 0) sub += " · " + std::to_string(p.battery) + "%" + (p.charging ? T(" cargando", " charging") : "");
                dl->AddText(ImVec2(tx, at.y + row * 0.12f + line * 1.15f), Col(kGrey), sub.c_str());
                const std::string who = players_of(p);
                if (!who.empty()) {
                    const ImVec2 t = ImGui::CalcTextSize(who.c_str());
                    const float right = at.x + ImGui::GetContentRegionAvail().x - pad_x;
                    dl->AddRectFilled(ImVec2(right - t.x - 12 * g_ui_scale, at.y + 6 * g_ui_scale),
                                      ImVec2(right, at.y + 6 * g_ui_scale + t.y + 4 * g_ui_scale), Col(kAccent, 0.25f), 99.0f);
                    dl->AddText(ImVec2(right - t.x - 6 * g_ui_scale, at.y + 8 * g_ui_scale), Col(kAccent), who.c_str());
                }
                ImGui::PopID();
            }
            ImGui::TableNextColumn();
            for (const auto& p : pads) {
                if (p.id != g_pad_selected) continue;
                Pads::State st;
                Pads::Read(p.id, st);
                const int inner = std::clamp(a.launcher.Int("pad_deadzone", 5), 0, 126);
                const int outer = std::clamp(a.launcher.Int("pad_deadzone_outer", 127), inner + 1, 127);
                DrawPad(p, st, inner / 127.0f, outer / 127.0f);
                ImGui::TextColored(kGrey, T("Stick izq. %+.2f %+.2f   Stick der. %+.2f %+.2f", "Left %+.2f %+.2f   Right %+.2f %+.2f"),
                                   st.axes[Pads::AxLeftX] / 32767.0f, st.axes[Pads::AxLeftY] / 32767.0f,
                                   st.axes[Pads::AxRightX] / 32767.0f, st.axes[Pads::AxRightY] / 32767.0f);
                if (ImGui::Button((std::string(ICON_VIBRATE "  ") + T("Vibrar", "Rumble")).c_str())) {
                    if (!Pads::Rumble(p.id, 450)) a.notice = T("Este mando no admite vibración.", "This controller has no rumble.");
                }
                ImGui::SameLine();
                ImGui::TextColored(kGrey, "%s", T("Pulsa botones y mueve los sticks para probarlo.",
                                                  "Press buttons and move the sticks to test it."));
            }
            ImGui::EndTable();
        }
    }

    Section(T("Jugadores", "Players"));
    for (int n = 1; n <= 4; ++n) {
        ImGui::PushID(n);
        const std::string current = PlayerPad(n);
        std::string preview = n == 1 ? T("Automático: el último mando que uses", "Automatic: the last controller used")
                                     : T("Nadie", "Nobody");
        if (!current.empty()) {
            preview = a.launcher.Get("port_name." + std::to_string(n));
            if (preview.empty() && n == 1) preview = a.launcher.Get("gamepad_name");
            if (preview.empty()) preview = current;
            bool connected = false;
            for (const auto& p : pads)
                if (p.guid == current && !connected) {
                    preview = display(p);
                    connected = true;
                }
            if (!connected && sdl) preview += T(" (no conectado)", " (not connected)");
        }
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted((std::string(T("Jugador ", "Player ")) + std::to_string(n)).c_str());
        ImGui::SameLine(ImGui::GetFontSize() * 6.0f);
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 22.0f);
        if (ImGui::BeginCombo("##player", preview.c_str())) {
            if (ImGui::Selectable(n == 1 ? T("Automático: el último mando que uses", "Automatic: the last controller used")
                                         : T("Nadie", "Nobody"),
                                  current.empty()))
                SetPlayerPad(n, nullptr);
            for (const auto& p : pads) {
                ImGui::PushID(int(p.id));
                const std::string label = display(p) + "  (" + Pads::TypeName(p.type, g_spanish) + ")";
                if (ImGui::Selectable(label.c_str(), p.guid == current)) SetPlayerPad(n, &p);
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        if (n == 1)
            HelpMarker(T("El jugador 1 también usa el teclado. En automático juega el mando que toques: coge cualquiera y pulsa un botón.",
                         "Player 1 also uses the keyboard. On automatic, the controller you touch plays: pick any and press a button."));
        if (n == 2)
            HelpMarker(T("Jugadores 2 a 4: pantalla dividida. Entran al juego mientras su mando está conectado. También se cambia "
                         "dentro del juego (Insert › Controllers).",
                         "Players 2 to 4: split screen. They join the game while their controller is connected. Also set in "
                         "the game (Insert › Controllers)."));
        ImGui::PopID();
    }

    // Gamepad buttons: pad.<input> in bbport.ini.
    Section(T("Botones del mando", "Controller buttons"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", T("Lo que hace cada botón en el juego, para todos los mandos. Pulsa \"Cambiar\" y luego el botón "
                                      "en el mando; cada acción admite hasta 4. Se guarda en bbport.ini y vale desde el próximo arranque.",
                                      "What each button does in the game, for every controller. Press \"Change\", then the button on "
                                      "the controller; each action takes up to 4. Saved in bbport.ini, used from the next start."));
    ImGui::PopTextWrapPos();
    const Pads::Info* shown = nullptr;
    for (const auto& p : pads)
        if (p.id == g_pad_selected) shown = &p;
    const Pads::Family family = shown ? shown->family : Pads::Family::Xbox;
    const int type = shown ? shown->type : 3;  // SDL_GAMEPAD_TYPE_XBOXONE
    if (g_pad_capture >= 0) {
        std::string name;
        uint32_t from = 0;
        if (Pads::TakeCapture(name, from) && !name.empty()) {
            const KeyInput& in = kKeyInputs[g_pad_capture];
            const std::string key = std::string("pad.") + in.id;
            auto names = g_pad_replace ? std::vector<std::string>{} : SplitKeys(a.game.Get(key));
            if (std::find(names.begin(), names.end(), name) == names.end()) {
                if (names.size() >= 4) names.erase(names.begin());
                names.push_back(name);
            }
            std::string value;
            for (const auto& n : names) value += (value.empty() ? "" : ",") + n;
            a.game.Set(key, value);
            SaveGameIni();
            g_pad_capture = -1;
        } else if (!Pads::Capturing()) {
            g_pad_capture = -1;  // the controllers went away
        }
    }
    if (ImGui::BeginTable("padbuttons", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(T("Acción", "Action"), ImGuiTableColumnFlags_WidthStretch, 1.2f);
        ImGui::TableSetupColumn(T("Botones", "Buttons"), ImGuiTableColumnFlags_WidthStretch, 1.4f);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 14.0f);
        ImGui::TableHeadersRow();
        for (int i = 0; i < int(std::size(kKeyInputs)); ++i) {
            const KeyInput& in = kKeyInputs[i];
            const char* fallback = PadFallback(in.id);
            if (!fallback) continue;
            const std::string key = std::string("pad.") + in.id;
            const bool custom = a.game.values.count(key) > 0;
            const auto names = SplitKeys(custom ? a.game.Get(key) : fallback);
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(T(in.es, in.en));
            ImGui::TableNextColumn();
            ImGui::AlignTextToFramePadding();
            if (g_pad_capture == i) {
                ImGui::TextColored(kYellow, "%s", T("Pulsa un botón del mando...", "Press a controller button..."));
            } else if (names.empty()) {
                ImGui::TextColored(kGrey, "%s", T("(sin botón)", "(none)"));
            } else {
                std::string text;
                for (const auto& n : names) {
                    const int input = Pads::InputFromName(n);
                    text += (text.empty() ? "" : "  /  ") + (input >= 0 ? Pads::Label(input, family, type, g_spanish) : n);
                }
                ImGui::TextColored(custom ? kText : kGrey, "%s", text.c_str());
            }
            ImGui::TableNextColumn();
            if (g_pad_capture == i) {
                if (ImGui::SmallButton(T("Cancelar", "Cancel"))) {
                    g_pad_capture = -1;
                    Pads::CancelCapture();
                }
            } else {
                ImGui::BeginDisabled(g_pad_capture >= 0 || !sdl || pads.empty());
                if (ImGui::SmallButton(T("Cambiar", "Change"))) {
                    g_pad_replace = !custom;
                    g_pad_capture = i;
                    Pads::ArmCapture();
                }
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(g_pad_capture >= 0);
                if (ImGui::SmallButton(T("Quitar", "Clear"))) {
                    a.game.Set(key, "");
                    SaveGameIni();
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(!custom);
                if (ImGui::SmallButton(T("Original", "Default"))) {
                    a.game.Erase(key);
                    SaveGameIni();
                }
                ImGui::EndDisabled();
                ImGui::EndDisabled();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(g_pad_capture >= 0);
    if (ImGui::Button((std::string(ICON_UNDO "  ") + T("Restaurar todos los botones", "Restore all buttons")).c_str())) {
        for (const auto& in : kKeyInputs)
            if (PadFallback(in.id)) a.game.Erase(std::string("pad.") + in.id);
        SaveGameIni();
    }
    ImGui::EndDisabled();
    ImGui::TextColored(kGrey, "%s", T("Gris: botón original. Los nombres siguen al mando elegido arriba.",
                                      "Grey: default button. Names follow the controller chosen above."));

    Section(T("Sticks", "Sticks"));
    {
        int inner = std::clamp(a.launcher.Int("pad_deadzone", 5), 0, 126);
        int outer = std::clamp(a.launcher.Int("pad_deadzone_outer", 127), inner + 1, 127);
        int inner_pct = int(std::lround(inner * 100.0 / 127.0)), outer_pct = int(std::lround(outer * 100.0 / 127.0));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderInt(T("Zona muerta", "Dead zone"), &inner_pct, 0, 40, "%d %%")) {
            a.launcher.Set("pad_deadzone", std::to_string(int(std::lround(inner_pct * 127.0 / 100.0))));
            SaveLauncher();
        }
        HelpMarker(T("Cuánto hay que mover el stick antes de que el juego lo note. Súbela si el kart gira solo (stick con deriva). "
                     "Por defecto 4 %. Se ve como el anillo gris en la prueba de arriba.",
                     "How far the stick moves before the game notices. Raise it if the kart steers by itself (stick drift). "
                     "Default 4 %. It is the grey ring in the test above."));
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 16.0f);
        if (ImGui::SliderInt(T("Giro completo al", "Full turn at"), &outer_pct, 60, 100, "%d %%")) {
            a.launcher.Set("pad_deadzone_outer", std::to_string(int(std::lround(outer_pct * 127.0 / 100.0))));
            SaveLauncher();
        }
        HelpMarker(T("Con menos del 100 % el giro llega al máximo antes del tope del stick: útil con sticks gastados o mandos "
                     "que no llegan al borde. Es el anillo naranja.",
                     "Below 100 % the turn reaches its maximum before the stick's edge: useful for worn sticks or "
                     "controllers that do not reach the edge. It is the orange ring."));
        LauncherFlag("pad_center_cal", T("Corregir el centro de mandos genéricos", "Correct the centre of generic controllers"), true,
                     T("Algunos mandos clónicos dicen estar inclinados con el stick suelto; el port mide su centro al conectarlos. "
                       "Desactívalo solo si un mando original se comporta raro.",
                       "Some clone controllers report a tilt with the stick at rest; the port measures their centre when they "
                       "connect. Turn it off only if a genuine controller behaves oddly."));
        if (ImGui::Button((std::string(ICON_UNDO "  ") + T("Valores por defecto", "Defaults")).c_str())) {
            a.launcher.Erase("pad_deadzone");
            a.launcher.Erase("pad_deadzone_outer");
            a.launcher.Erase("pad_center_cal");
            SaveLauncher();
        }
    }
}

void ControlsTab() {
    if (ImGui::BeginTabBar("controls")) {
        if (ImGui::BeginTabItem((std::string(ICON_GAMEPAD "  ") + T("Mandos", "Controllers") + "###pads").c_str())) {
            if (g_key_capture >= 0) g_key_capture = -1;
            ImGui::BeginChild("pads");
            ControllersPage();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem((std::string(ICON_KEYBOARD "  ") + T("Teclado", "Keyboard") + "###keys").c_str())) {
            if (g_pad_capture >= 0) {
                g_pad_capture = -1;
                Pads::CancelCapture();
            }
            ImGui::BeginChild("keyboard");
            KeyboardTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// ---------------------------------------------------------------------------------------------
// Achievements tab (v0.4): the game's trophies (sce_sys/trophy/trophy00.trp), with the ones
// unlocked while playing (<user>/trophies/<TITLE_ID>/unlocked.txt, written by the runtime:
// src/runtime_services.c). Icons are plain PNGs in the TRP. Names, descriptions and grades are
// in its ESFM files, AES-encrypted with the console's trophy key, which is not part of this
// port: with the player's own key (the one shadPS4 uses: entered here or imported from
// shadPS4's keys.json) they are decrypted as shadPS4 does (src/core/file_format/trp.cpp) and
// written to trophies.tsv, which the runtime reads for its in-game notice and the platinum.

SDL_Renderer* g_renderer = nullptr;
using Texture = SDL_Texture*;
ImTextureRef TextureRef(Texture t) { return ImTextureRef(ImTextureID(reinterpret_cast<intptr_t>(t))); }

// The 48 trophies' names, grades and what to do (src/ctr_trophies.h, shared with the runtime).
#include "../../src/ctr_trophies.h"
#define kCtrTrophies ctr_trophies


struct Trophy {
    int id = 0;
    char grade = 0;  // 'P' 'G' 'S' 'B', 0 unknown
    bool hidden = false;
    std::string name, detail;
    Texture icon = nullptr;
    uint64_t unlocked = 0;  // unix time, 0 locked
};

struct TrophySet {
    fs::path game;              // the game folder it was read from
    std::string language;       // the game language its names are in
    std::string title_id, np_comm_id, title_name;
    std::vector<Trophy> list;
    Texture icon = nullptr;
    bool names = false;         // names and grades known
    bool builtin = false;       // ... from kCtrTrophies, not the game's own (encrypted) texts
    bool spanish = false;       // the launcher language its built-in texts are in
    std::string error, note;
    fs::file_time_type unlocked_time{};
    double next_poll = 0.0;
};
TrophySet g_trophies;
bool g_trophies_reload = true;
int g_trophy_filter = 0;  // 0 all, 1 unlocked, 2 locked
bool g_trophy_reset_confirm = false;
char g_trophy_key_input[64] = "";

fs::path TrophyDir(const std::string& title) { return UserDir() / "trophies" / title; }

void ReleaseTrophies(TrophySet& t) {
    for (auto& tr : t.list)
        if (tr.icon) SDL_DestroyTexture(tr.icon);
    if (t.icon) SDL_DestroyTexture(t.icon);
    t = TrophySet{};
}

/// A PNG to a texture (SDL's own PNG loader).
Texture PngTexture(const std::string& png) {
    if (!g_renderer || png.empty()) return nullptr;
    SDL_Surface* surface = SDL_LoadPNG_IO(SDL_IOFromConstMem(png.data(), png.size()), true);
    if (!surface) return nullptr;
    Texture texture = SDL_CreateTextureFromSurface(g_renderer, surface);
    SDL_DestroySurface(surface);
    if (texture) SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    return texture;
}

/// The 16-byte trophy key from hex ("" when it is not 32 hex digits).
std::string TrophyKeyBytes(const std::string& hex) {
    std::string clean;
    for (char c : hex)
        if (std::isxdigit(static_cast<unsigned char>(c))) clean += c;
    if (clean.size() != 32) return {};
    std::string out;
    for (size_t i = 0; i < 32; i += 2) out += char(std::stoi(clean.substr(i, 2), nullptr, 16));
    return out;
}

// AES-128 (FIPS-197), for the trophy texts: the Windows launcher uses CNG for the same.
namespace aes {
constexpr uint8_t kSbox[256] = {
    0x63,0x7c,0x77,0x7b,0xf2,0x6b,0x6f,0xc5,0x30,0x01,0x67,0x2b,0xfe,0xd7,0xab,0x76,0xca,0x82,0xc9,0x7d,0xfa,0x59,0x47,0xf0,
    0xad,0xd4,0xa2,0xaf,0x9c,0xa4,0x72,0xc0,0xb7,0xfd,0x93,0x26,0x36,0x3f,0xf7,0xcc,0x34,0xa5,0xe5,0xf1,0x71,0xd8,0x31,0x15,
    0x04,0xc7,0x23,0xc3,0x18,0x96,0x05,0x9a,0x07,0x12,0x80,0xe2,0xeb,0x27,0xb2,0x75,0x09,0x83,0x2c,0x1a,0x1b,0x6e,0x5a,0xa0,
    0x52,0x3b,0xd6,0xb3,0x29,0xe3,0x2f,0x84,0x53,0xd1,0x00,0xed,0x20,0xfc,0xb1,0x5b,0x6a,0xcb,0xbe,0x39,0x4a,0x4c,0x58,0xcf,
    0xd0,0xef,0xaa,0xfb,0x43,0x4d,0x33,0x85,0x45,0xf9,0x02,0x7f,0x50,0x3c,0x9f,0xa8,0x51,0xa3,0x40,0x8f,0x92,0x9d,0x38,0xf5,
    0xbc,0xb6,0xda,0x21,0x10,0xff,0xf3,0xd2,0xcd,0x0c,0x13,0xec,0x5f,0x97,0x44,0x17,0xc4,0xa7,0x7e,0x3d,0x64,0x5d,0x19,0x73,
    0x60,0x81,0x4f,0xdc,0x22,0x2a,0x90,0x88,0x46,0xee,0xb8,0x14,0xde,0x5e,0x0b,0xdb,0xe0,0x32,0x3a,0x0a,0x49,0x06,0x24,0x5c,
    0xc2,0xd3,0xac,0x62,0x91,0x95,0xe4,0x79,0xe7,0xc8,0x37,0x6d,0x8d,0xd5,0x4e,0xa9,0x6c,0x56,0xf4,0xea,0x65,0x7a,0xae,0x08,
    0xba,0x78,0x25,0x2e,0x1c,0xa6,0xb4,0xc6,0xe8,0xdd,0x74,0x1f,0x4b,0xbd,0x8b,0x8a,0x70,0x3e,0xb5,0x66,0x48,0x03,0xf6,0x0e,
    0x61,0x35,0x57,0xb9,0x86,0xc1,0x1d,0x9e,0xe1,0xf8,0x98,0x11,0x69,0xd9,0x8e,0x94,0x9b,0x1e,0x87,0xe9,0xce,0x55,0x28,0xdf,
    0x8c,0xa1,0x89,0x0d,0xbf,0xe6,0x42,0x68,0x41,0x99,0x2d,0x0f,0xb0,0x54,0xbb,0x16};
uint8_t inv_sbox[256];
uint8_t xt(uint8_t x) { return uint8_t(x << 1 ^ (x & 0x80 ? 0x1b : 0)); }
uint8_t mul(uint8_t a, uint8_t b) {
    uint8_t r = 0;
    for (; b; b >>= 1, a = xt(a))
        if (b & 1) r ^= a;
    return r;
}
void expand(const uint8_t key[16], uint8_t rk[176]) {
    std::memcpy(rk, key, 16);
    uint8_t rcon = 1;
    for (int i = 16; i < 176; i += 4) {
        uint8_t t[4] = {rk[i - 4], rk[i - 3], rk[i - 2], rk[i - 1]};
        if (i % 16 == 0) {
            const uint8_t first = t[0];
            t[0] = uint8_t(kSbox[t[1]] ^ rcon), t[1] = kSbox[t[2]], t[2] = kSbox[t[3]], t[3] = kSbox[first];
            rcon = xt(rcon);
        }
        for (int k = 0; k < 4; ++k) rk[i + k] = rk[i - 16 + k] ^ t[k];
    }
}
void encrypt(const uint8_t rk[176], uint8_t s[16]) {
    for (int i = 0; i < 16; ++i) s[i] ^= rk[i];
    for (int round = 1; round <= 10; ++round) {
        uint8_t t[16];
        for (int i = 0; i < 16; ++i) t[i] = kSbox[s[(i + 4 * (i % 4)) % 16]];  // SubBytes + ShiftRows
        for (int c = 0; c < 4 && round < 10; ++c) {
            uint8_t* col = t + 4 * c;
            const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
            col[0] = uint8_t(xt(a0) ^ xt(a1) ^ a1 ^ a2 ^ a3);
            col[1] = uint8_t(a0 ^ xt(a1) ^ xt(a2) ^ a2 ^ a3);
            col[2] = uint8_t(a0 ^ a1 ^ xt(a2) ^ xt(a3) ^ a3);
            col[3] = uint8_t(xt(a0) ^ a0 ^ a1 ^ a2 ^ xt(a3));
        }
        for (int i = 0; i < 16; ++i) s[i] = t[i] ^ rk[16 * round + i];
    }
}
void decrypt(const uint8_t rk[176], uint8_t s[16]) {
    if (!inv_sbox[kSbox[1]])
        for (int i = 0; i < 256; ++i) inv_sbox[kSbox[i]] = uint8_t(i);
    for (int i = 0; i < 16; ++i) s[i] ^= rk[160 + i];
    for (int round = 9; round >= 0; --round) {
        uint8_t t[16];
        for (int i = 0; i < 16; ++i) t[(i + 4 * (i % 4)) % 16] = inv_sbox[s[i]];  // InvShiftRows + InvSubBytes
        for (int i = 0; i < 16; ++i) t[i] ^= rk[16 * round + i];
        for (int c = 0; c < 4 && round > 0; ++c) {
            uint8_t* col = t + 4 * c;
            const uint8_t a0 = col[0], a1 = col[1], a2 = col[2], a3 = col[3];
            col[0] = uint8_t(mul(a0, 14) ^ mul(a1, 11) ^ mul(a2, 13) ^ mul(a3, 9));
            col[1] = uint8_t(mul(a0, 9) ^ mul(a1, 14) ^ mul(a2, 11) ^ mul(a3, 13));
            col[2] = uint8_t(mul(a0, 13) ^ mul(a1, 9) ^ mul(a2, 14) ^ mul(a3, 11));
            col[3] = uint8_t(mul(a0, 11) ^ mul(a1, 13) ^ mul(a2, 9) ^ mul(a3, 14));
        }
        std::memcpy(s, t, 16);
    }
}
}  // namespace aes

/// AES-128-CBC without padding. encrypt: true to encrypt.
bool AesCbc(const std::string& key, const unsigned char iv_in[16], const unsigned char* in, size_t size, unsigned char* out,
            bool encrypt) {
    if (key.size() != 16 || size % 16) return false;
    uint8_t rk[176], iv[16];
    aes::expand(reinterpret_cast<const uint8_t*>(key.data()), rk);
    std::memcpy(iv, iv_in, 16);
    for (size_t at = 0; at < size; at += 16) {
        uint8_t block[16];
        std::memcpy(block, in + at, 16);
        if (encrypt) {
            for (int i = 0; i < 16; ++i) block[i] ^= iv[i];
            aes::encrypt(rk, block);
            std::memcpy(iv, block, 16);
        } else {
            uint8_t next[16];
            std::memcpy(next, block, 16);
            aes::decrypt(rk, block);
            for (int i = 0; i < 16; ++i) block[i] ^= iv[i];
            std::memcpy(iv, next, 16);
        }
        std::memcpy(out + at, block, 16);
    }
    return true;
}

/// One ESFM entry decrypted to its XML (shadPS4's DecryptEFSM): the key for the file is the
/// NP communication ID (16 bytes, zero-padded) encrypted with the trophy key (CBC, zero IV); the
/// entry is IV (16 bytes) + data, CBC-decrypted with that key and IV.
std::string DecryptEsfm(const std::string& trophy_key, const std::string& np_comm_id, const std::string& entry) {
    if (entry.size() <= 32 || (entry.size() - 16) % 16) return {};
    unsigned char comm[16]{}, zero[16]{}, file_key[16];
    std::memcpy(comm, np_comm_id.data(), std::min<size_t>(np_comm_id.size(), 16));
    if (!AesCbc(trophy_key, zero, comm, 16, file_key, true)) return {};
    std::string xml(entry.size() - 16, '\0');
    if (!AesCbc(std::string(reinterpret_cast<char*>(file_key), 16), reinterpret_cast<const unsigned char*>(entry.data()),
                reinterpret_cast<const unsigned char*>(entry.data()) + 16, entry.size() - 16,
                reinterpret_cast<unsigned char*>(xml.data()), false))
        return {};
    const size_t end = xml.rfind('>');
    if (end == std::string::npos) return {};
    xml.resize(end + 1);
    return xml.rfind("<?xml", 0) == 0 || xml.find("<trophyconf") != std::string::npos ? xml : std::string{};
}

std::string XmlTag(const std::string& xml, const std::string& tag, size_t from = 0, size_t until = std::string::npos) {
    const size_t a = xml.find("<" + tag + ">", from);
    if (a == std::string::npos || a >= until) return {};
    const size_t b = xml.find("</" + tag + ">", a);
    if (b == std::string::npos || b > until) return {};
    return XmlUnescape(xml.substr(a + tag.size() + 2, b - a - tag.size() - 2));
}

/// The trophies in a decrypted TROP*.XML: id -> (grade, hidden, name, detail).
void ParseTrophyXml(const std::string& xml, TrophySet& t) {
    t.title_name = XmlTag(xml, "title-name");
    for (size_t at = 0; (at = xml.find("<trophy ", at)) != std::string::npos;) {
        const size_t close = xml.find('>', at), end = xml.find("</trophy>", at);
        if (close == std::string::npos || end == std::string::npos) break;
        const auto attr = XmlAttributes(xml.substr(at + 8, close - at - 8));
        auto get = [&](const char* k) {
            const auto it = attr.find(k);
            return it == attr.end() ? std::string() : it->second;
        };
        const int id = std::atoi(get("id").c_str());
        for (auto& tr : t.list) {
            if (tr.id != id) continue;
            const std::string type = get("ttype");
            tr.grade = type.empty() ? 0 : type[0];
            tr.hidden = get("hidden") == "yes";
            tr.name = XmlTag(xml, "name", close, end);
            tr.detail = XmlTag(xml, "detail", close, end);
        }
        at = end;
    }
}

std::string Sanitize(std::string s) {
    for (char& c : s)
        if (c == '\t' || c == '\r' || c == '\n') c = ' ';
    return s;
}

/// trophies.tsv: "#source<TAB>key|builtin", "#lang<TAB>code", then id, grade, hidden, name,
/// detail per line. True for the game's own texts (decrypted with the key) in this language.
bool ReadTrophyTsv(const fs::path& file, const std::string& language, TrophySet& t, std::string* source = nullptr) {
    std::stringstream ss(ReadText(file));
    std::string line;
    bool right_language = false, any = false;
    while (std::getline(ss, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::vector<std::string> f;
        std::stringstream ls(line);
        std::string field;
        while (std::getline(ls, field, '\t')) f.push_back(field);
        if (f.size() >= 2 && f[0] == "#lang") right_language = f[1] == language;
        if (f.size() >= 2 && f[0] == "#source" && source) *source = f[1];
        if (f.size() >= 2 && f[0] == "#title") t.title_name = f[1];
        if (f.size() < 5 || f[0].empty() || !std::isdigit(static_cast<unsigned char>(f[0][0]))) continue;
        for (auto& tr : t.list)
            if (tr.id == std::atoi(f[0].c_str())) {
                tr.grade = f[1].empty() ? 0 : f[1][0];
                tr.hidden = f[2] == "1";
                tr.name = f[3];
                tr.detail = f[4];
                any = true;
            }
    }
    return any && right_language && (!source || *source != "builtin");
}

void WriteTrophyTsv(const TrophySet& t) {
    std::string out = std::string("#source\t") + (t.builtin ? "builtin" : "key") + "\n#lang\t" + t.language + "\n#title\t" +
                      Sanitize(t.title_name) + "\n";
    for (const auto& tr : t.list)
        out += std::to_string(tr.id) + "\t" + std::string(1, tr.grade ? tr.grade : '?') + "\t" + (tr.hidden ? "1" : "0") + "\t" +
               Sanitize(tr.name) + "\t" + Sanitize(tr.detail) + "\n";
    std::error_code ec;
    fs::create_directories(TrophyDir(t.title_id), ec);
    WriteTextAtomic(TrophyDir(t.title_id) / L"trophies.tsv", out);
}

void ReadUnlocked(TrophySet& t) {
    for (auto& tr : t.list) tr.unlocked = 0;
    std::stringstream ss(ReadText(TrophyDir(t.title_id) / L"unlocked.txt"));
    std::string line;
    while (std::getline(ss, line)) {
        int id = -1;
        unsigned long long at = 0;
        if (std::sscanf(line.c_str(), "%d %llu", &id, &at) != 2) continue;
        for (auto& tr : t.list)
            if (tr.id == id && !tr.unlocked) tr.unlocked = at ? at : 1;
    }
    std::error_code ec;
    t.unlocked_time = fs::last_write_time(TrophyDir(t.title_id) / L"unlocked.txt", ec);
}

/// The trophy key the launcher keeps, else shadPS4's (keys.json "ReleaseTrophyKey").
std::string ShadPs4TrophyKey() {
    // shadPS4's user folder on Linux: $XDG_DATA_HOME/shadPS4 (~/.local/share/shadPS4).
    const char* xdg = std::getenv("XDG_DATA_HOME");
    const fs::path data = xdg && *xdg ? fs::path(xdg) : fs::path(HomeDir()) / ".local" / "share";
    std::string json = ReadText(data / "shadPS4" / "keys.json");
    if (json.empty()) json = ReadText(fs::path(HomeDir()) / ".config" / "shadPS4" / "keys.json");
    const size_t at = json.find("\"ReleaseTrophyKey\"");
    if (at == std::string::npos) return {};
    const size_t colon = json.find(':', at);
    if (colon == std::string::npos) return {};
    size_t i = colon + 1;
    while (i < json.size() && std::isspace(static_cast<unsigned char>(json[i]))) ++i;
    if (i < json.size() && json[i] == '"') {  // a hex string
        const size_t end = json.find('"', i + 1);
        return end == std::string::npos ? std::string() : json.substr(i + 1, end - i - 1);
    }
    if (i < json.size() && json[i] == '[') {  // a byte array
        const size_t end = json.find(']', i);
        std::stringstream ss(json.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1));
        std::string hex, n;
        char buf[3];
        while (std::getline(ss, n, ',')) {
            n = Trim(n);
            if (n.empty()) continue;
            std::snprintf(buf, sizeof buf, "%02x", std::atoi(n.c_str()) & 0xff);
            hex += buf;
        }
        return hex;
    }
    return {};
}

void LoadTrophies() {
    App& a = g_app;
    ReleaseTrophies(g_trophies);
    TrophySet& t = g_trophies;
    t.game = a.game_dir;
    t.language = GameLanguage();
    t.spanish = g_spanish;
    t.title_id = GameTitleId();
    const fs::path trp = a.game_dir / L"sce_sys" / L"trophy" / L"trophy00.trp";
    const std::string data = ReadText(trp);
    auto be32 = [&](size_t o) {
        return uint32_t(uint8_t(data[o])) << 24 | uint32_t(uint8_t(data[o + 1])) << 16 | uint32_t(uint8_t(data[o + 2])) << 8 |
               uint32_t(uint8_t(data[o + 3]));
    };
    auto be64 = [&](size_t o) { return uint64_t(be32(o)) << 32 | be32(o + 4); };
    if (data.size() < 0x60 || be32(0) != 0xDCA24D00) {
        t.error = a.game_dir.empty() ? T("Elige primero el juego en la pestaña Jugar.", "Choose the game in the Play tab first.")
                                     : T("Este juego no trae el archivo de trofeos (sce_sys/trophy/trophy00.trp).",
                                         "This game has no trophy file (sce_sys/trophy/trophy00.trp).");
        return;
    }
    const uint32_t count = be32(0x10), entry_size = be32(0x14);
    std::map<std::string, std::string> entries;
    for (uint32_t i = 0; i < count && i < 1024; ++i) {
        const size_t e = 0x60 + size_t(i) * entry_size;
        if (e + 48 > data.size()) break;
        const std::string name(data.c_str() + e, strnlen(data.c_str() + e, 32));
        const uint64_t pos = be64(e + 32), size = be64(e + 40);
        if (pos + size <= data.size()) entries[name] = data.substr(size_t(pos), size_t(size));
    }
    for (const auto& [name, bytes] : entries) {
        int id = -1;
        if (name.size() == 11 && name.rfind("TROP", 0) == 0 && name.substr(7) == ".PNG" && std::sscanf(name.c_str() + 4, "%3d", &id) == 1) {
            Trophy tr;
            tr.id = id;
            tr.icon = PngTexture(bytes);
            t.list.push_back(std::move(tr));
        }
    }
    std::sort(t.list.begin(), t.list.end(), [](const Trophy& x, const Trophy& y) { return x.id < y.id; });
    if (entries.count("ICON0.PNG")) t.icon = PngTexture(entries["ICON0.PNG"]);
    // The NP communication ID (npbind.dat), needed to decrypt.
    const std::string npbind = ReadText(a.game_dir / L"sce_sys" / L"npbind.dat");
    for (size_t i = 0; i + 12 <= npbind.size(); ++i)
        if (npbind.compare(i, 4, "NPWR") == 0 && npbind[i + 9] == '_') {
            t.np_comm_id = npbind.substr(i, 12);
            break;
        }
    // Names: the game's own texts (trophies.tsv in this language, else decrypted with the key),
    // else the built-in list.
    std::string source;
    TrophySet probe;
    for (const auto& tr : t.list) {
        Trophy copy;
        copy.id = tr.id;
        probe.list.push_back(copy);
    }
    if (ReadTrophyTsv(TrophyDir(t.title_id) / L"trophies.tsv", t.language, probe, &source)) {
        ReadTrophyTsv(TrophyDir(t.title_id) / L"trophies.tsv", t.language, t);
        t.names = true;
    } else {
        const std::string key = TrophyKeyBytes(a.launcher.Get("trophy_key"));
        if (!key.empty() && !t.np_comm_id.empty()) {
            char localized[32];
            std::snprintf(localized, sizeof localized, "TROP_%02d.ESFM", std::atoi(t.language.c_str()));
            const std::string file = entries.count(localized) ? localized : "TROP.ESFM";
            const std::string xml = entries.count(file) ? DecryptEsfm(key, t.np_comm_id, entries[file]) : std::string{};
            if (!xml.empty()) {
                ParseTrophyXml(xml, t);
                t.names = true;
                WriteTrophyTsv(t);
            } else {
                t.note = T("La clave de trofeos no descifra este juego: revisa que sea la de tu PS4 (32 cifras hexadecimales).",
                           "The trophy key does not decrypt this game: check it is your PS4's (32 hex digits).");
            }
        }
    }
    if (!t.names && IsCtrTitle(t.title_id) && t.list.size() == std::size(kCtrTrophies)) {
        for (auto& tr : t.list)
            for (const auto& b : kCtrTrophies)
                if (b.id == tr.id) {
                    tr.grade = b.grade;
                    tr.name = b.name;
                    tr.detail = T(b.es, b.en);
                }
        t.title_name = "Crash Team Racing Nitro-Fueled";
        t.names = t.builtin = true;
        // The runtime's in-game notice reads the names and grades (and finds the platinum) here.
        if (source != "key") WriteTrophyTsv(t);
    }
    ReadUnlocked(t);
}

void TrophiesTab() {
    App& a = g_app;
    const std::string language = GameLanguage();
    if (g_trophies_reload || g_trophies.game != a.game_dir || g_trophies.language != language ||
        (g_trophies.builtin && g_trophies.spanish != g_spanish)) {
        g_trophies_reload = false;
        LoadTrophies();
    }
    TrophySet& t = g_trophies;
    // The game writes unlocked.txt while it runs: re-read it when it changes.
    if (ImGui::GetTime() > t.next_poll && !t.title_id.empty()) {
        t.next_poll = ImGui::GetTime() + 1.0;
        std::error_code ec;
        const auto time = fs::last_write_time(TrophyDir(t.title_id) / L"unlocked.txt", ec);
        if (!ec && time != t.unlocked_time) ReadUnlocked(t);
    }
    if (!t.error.empty()) {
        BeginCard("trophy_error");
        ImGui::TextColored(kYellow, "%s  %s", ICON_INFO, t.error.c_str());
        EndCard();
        return;
    }
    auto points = [](char g) { return g == 'P' ? 180 : g == 'G' ? 90 : g == 'S' ? 30 : g == 'B' ? 15 : 0; };
    int unlocked = 0, earned = 0, total_points = 0;
    int by_grade[4][2]{};  // B S G P: unlocked, total
    for (const auto& tr : t.list) {
        unlocked += tr.unlocked != 0;
        total_points += points(tr.grade);
        if (tr.unlocked) earned += points(tr.grade);
        const int g = tr.grade == 'B' ? 0 : tr.grade == 'S' ? 1 : tr.grade == 'G' ? 2 : tr.grade == 'P' ? 3 : -1;
        if (g >= 0) {
            by_grade[g][1]++;
            if (tr.unlocked) by_grade[g][0]++;
        }
    }
    const int total = int(t.list.size());
    const float progress = total_points ? float(earned) / float(total_points) : total ? float(unlocked) / float(total) : 0.0f;
    static const ImVec4 grade_colors[] = {ImVec4(0.82f, 0.54f, 0.32f, 1.0f), ImVec4(0.80f, 0.83f, 0.88f, 1.0f),
                                          ImVec4(1.0f, 0.80f, 0.29f, 1.0f), ImVec4(0.55f, 0.83f, 1.0f, 1.0f)};
    auto grade_index = [](char g) { return g == 'B' ? 0 : g == 'S' ? 1 : g == 'G' ? 2 : g == 'P' ? 3 : -1; };
    auto grade_name = [](int g) {
        static const char* es[] = {"Bronce", "Plata", "Oro", "Platino"};
        static const char* en[] = {"Bronze", "Silver", "Gold", "Platinum"};
        return g_spanish ? es[g] : en[g];
    };

    // Summary.
    BeginCard("trophy_summary");
    const float big = 72.0f * g_ui_scale;
    if (t.icon) {
        ImGui::Image(TextureRef(t.icon), ImVec2(big * 1.78f, big));
        ImGui::SameLine(0.0f, 16.0f * g_ui_scale);
    }
    ImGui::BeginGroup();
    if (g_font_heading) ImGui::PushFont(g_font_heading, ImGui::GetStyle().FontSizeBase * 1.25f);
    ImGui::TextUnformatted(t.title_name.empty() ? "Crash Team Racing Nitro-Fueled" : t.title_name.c_str());
    if (g_font_heading) ImGui::PopFont();
    ImGui::Text(T("%d de %d logros  ·  %d %%", "%d of %d trophies  ·  %d %%"), unlocked, total, int(progress * 100.0f + 0.5f));
    ImGui::ProgressBar(progress, ImVec2(-FLT_MIN, 8.0f * g_ui_scale), "");
    if (t.names) {
        for (int g = 3; g >= 0; --g) {
            if (!by_grade[g][1]) continue;
            ImGui::TextColored(grade_colors[g], "\xE2\x97\x8F");
            ImGui::SameLine(0.0f, 4.0f * g_ui_scale);
            ImGui::Text("%s %d/%d", grade_name(g), by_grade[g][0], by_grade[g][1]);
            ImGui::SameLine(0.0f, 16.0f * g_ui_scale);
        }
        ImGui::NewLine();
    }
    ImGui::EndGroup();
    EndCard();

    // Names: the player's trophy key (the game's own texts, in its language).
    bool show_key = !t.names;
    if (t.builtin) {
        ImGui::Spacing();
        show_key = ImGui::CollapsingHeader(T("Usar los textos del propio juego (clave de trofeos)",
                                             "Use the game's own texts (trophy key)"));
    }
    if (show_key) {
        ImGui::Spacing();
        BeginCard("trophy_key");
        ImGui::TextColored(kBlue, "%s", ICON_INFO);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(0.0f);
        if (t.builtin)
            ImGui::TextUnformatted(T("Los nombres son los oficiales y las descripciones, un resumen de qué hay que hacer. Los textos "
                                     "del propio juego (en su idioma) vienen cifrados con la clave de trofeos de la PS4, que no se "
                                     "incluye: si la tienes (la misma que usa shadPS4), pégala o impórtala para usarlos.",
                                     "Names are the official ones and descriptions a summary of what to do. The game's own texts "
                                     "(in its language) are encrypted with the PS4's trophy key, which is not included: if you "
                                     "have it (the same shadPS4 uses), paste or import it to use them."));
        else
            ImGui::TextUnformatted(T("Los logros se guardan igual al jugar y aquí se ven con su imagen. Sus nombres y descripciones "
                                     "vienen cifrados en el juego con la clave de trofeos de la PS4, que no se incluye: si la tienes "
                                     "(es la misma que usa shadPS4), pégala o impórtala para verlos.",
                                     "Trophies are recorded while you play and shown here with their pictures. Their names and "
                                     "descriptions are encrypted in the game with the PS4's trophy key, which is not included: if "
                                     "you have it (the same shadPS4 uses), paste or import it to see them."));
        ImGui::PopTextWrapPos();
        if (!t.note.empty()) ImGui::TextColored(kRed, "%s", t.note.c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 18.0f);
        ImGui::InputTextWithHint("##trophykey", T("Clave de trofeos (32 cifras hex)", "Trophy key (32 hex digits)"), g_trophy_key_input,
                                 sizeof g_trophy_key_input, ImGuiInputTextFlags_Password | ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        if (ImGui::Button(T("Guardar", "Save"))) {
            if (TrophyKeyBytes(g_trophy_key_input).empty()) {
                a.notice = T("La clave tiene que tener 32 cifras hexadecimales.", "The key must be 32 hex digits.");
            } else {
                a.launcher.Set("trophy_key", g_trophy_key_input);
                SaveLauncher();
                g_trophies_reload = true;
            }
            std::memset(g_trophy_key_input, 0, sizeof g_trophy_key_input);
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Importar de shadPS4", "Import from shadPS4"))) {
            const std::string key = ShadPs4TrophyKey();
            if (TrophyKeyBytes(key).empty()) {
                a.notice = T("shadPS4 no tiene una clave de trofeos guardada (keys.json).",
                             "shadPS4 has no trophy key saved (keys.json).");
            } else {
                a.launcher.Set("trophy_key", key);
                SaveLauncher();
                g_trophies_reload = true;
            }
        }
        EndCard();
    }

    // Filter and actions.
    ImGui::Spacing();
    ImGui::RadioButton(T("Todos", "All"), &g_trophy_filter, 0);
    ImGui::SameLine();
    ImGui::RadioButton(T("Desbloqueados", "Unlocked"), &g_trophy_filter, 1);
    ImGui::SameLine();
    ImGui::RadioButton(T("Por conseguir", "Locked"), &g_trophy_filter, 2);
    ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 13.0f);
    if (ImGui::Button((std::string(ICON_FOLDER "  ") + T("Carpeta", "Folder")).c_str())) {
        std::error_code ec;
        fs::create_directories(TrophyDir(t.title_id), ec);
        OpenInShell(TrophyDir(t.title_id));
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(!unlocked || a.game_running);
    if (ImGui::Button((std::string(ICON_UNDO "  ") + T("Reiniciar", "Reset")).c_str())) g_trophy_reset_confirm = true;
    ImGui::EndDisabled();

    // The list.
    ImGui::Spacing();
    const float icon = 60.0f * g_ui_scale;
    if (ImGui::BeginTable("trophies", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("icon", ImGuiTableColumnFlags_WidthFixed, icon);
        ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("state", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 9.0f);
        // The latest unlocked first, then the rest in the game's order.
        std::vector<const Trophy*> order;
        for (const auto& tr : t.list) order.push_back(&tr);
        std::stable_sort(order.begin(), order.end(), [](const Trophy* x, const Trophy* y) {
            if (bool(x->unlocked) != bool(y->unlocked)) return bool(x->unlocked);
            return x->unlocked > y->unlocked;
        });
        for (const Trophy* item : order) {
            const Trophy& tr = *item;
            if ((g_trophy_filter == 1 && !tr.unlocked) || (g_trophy_filter == 2 && tr.unlocked)) continue;
            const bool secret = tr.hidden && !tr.unlocked;
            ImGui::TableNextRow(0, icon + 8.0f * g_ui_scale);
            ImGui::TableNextColumn();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            ImDrawList* dl = ImGui::GetWindowDrawList();
            if (tr.icon && !secret) {
                dl->AddImageRounded(TextureRef(tr.icon), p, ImVec2(p.x + icon, p.y + icon),
                                    ImVec2(0, 0), ImVec2(1, 1),
                                    tr.unlocked ? IM_COL32_WHITE : IM_COL32(110, 110, 120, 255), 8.0f * g_ui_scale);
            } else {
                dl->AddRectFilled(p, ImVec2(p.x + icon, p.y + icon), Col(kFrame), 8.0f * g_ui_scale);
                const ImVec2 q = ImGui::CalcTextSize("?");
                dl->AddText(ImVec2(p.x + (icon - q.x) * 0.5f, p.y + (icon - q.y) * 0.5f), Col(kGrey), "?");
            }
            if (!tr.unlocked) dl->AddRect(p, ImVec2(p.x + icon, p.y + icon), Col(kBorder), 8.0f * g_ui_scale);
            ImGui::Dummy(ImVec2(icon, icon));
            ImGui::TableNextColumn();
            if (g_font_heading) ImGui::PushFont(g_font_heading, 0.0f);
            std::string name = secret ? T("Logro oculto", "Hidden trophy")
                               : tr.name.empty() ? std::string(T("Logro #", "Trophy #")) + std::to_string(tr.id)
                                                 : tr.name;
            ImGui::TextColored(tr.unlocked ? kText : kGrey, "%s", name.c_str());
            if (g_font_heading) ImGui::PopFont();
            ImGui::PushTextWrapPos(0.0f);
            if (secret) ImGui::TextColored(kGrey, "%s", T("Sigue jugando para descubrirlo.", "Keep playing to find it."));
            else if (!tr.detail.empty()) ImGui::TextColored(kGrey, "%s", tr.detail.c_str());
            ImGui::PopTextWrapPos();
            ImGui::TableNextColumn();
            const int g = grade_index(tr.grade);
            if (g >= 0) {
                ImGui::TextColored(grade_colors[g], "\xE2\x97\x8F %s", grade_name(g));
            }
            if (tr.unlocked > 1) {
                const std::time_t when = std::time_t(tr.unlocked);
                std::tm local{};
                localtime_r(&when, &local);
                char date[32];
                std::strftime(date, sizeof date, "%d/%m/%Y %H:%M", &local);
                ImGui::TextColored(kGreen, "%s %s", ICON_CHECK, date);
            } else if (tr.unlocked) {
                ImGui::TextColored(kGreen, "%s %s", ICON_CHECK, T("Conseguido", "Unlocked"));
            } else {
                ImGui::TextColored(kGrey, "%s", T("Por conseguir", "Locked"));
            }
        }
        ImGui::EndTable();
    }
    ImGui::TextColored(kGrey, "%s", T("Se desbloquean jugando, como en la PS4; el platino llega solo al tener todos los demás.",
                                      "They unlock as you play, as on a PS4; the platinum comes once all the others are."));

    if (g_trophy_reset_confirm) {
        ImGui::OpenPopup("trophy_reset");
        g_trophy_reset_confirm = false;
    }
    if (ImGui::BeginPopupModal("trophy_reset", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("¿Volver a bloquear todos los logros? Se guarda una copia por si acaso.",
                                 "Lock every trophy again? A copy is kept just in case."));
        if (ImGui::Button(T("Reiniciar", "Reset"))) {
            std::error_code ec;
            const fs::path file = TrophyDir(t.title_id) / L"unlocked.txt";
            fs::path backup = file;
            backup += L"." + std::to_wstring(std::time(nullptr)) + L".bak";
            fs::rename(file, backup, ec);
            if (ec) a.notice = std::string(T("No se pudo reiniciar: ", "Could not reset: ")) + ec.message();
            ReadUnlocked(t);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void ToolsTab() {
    App& a = g_app;
    Section(T("Carpetas y registro", "Folders and log"));
    if (ImGui::Button(T("Abrir los registros", "Open the logs"))) {
        std::error_code ec;
        fs::create_directories(a.data / "logs", ec);
        OpenInShell(a.data / "logs");
    }
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir la carpeta de datos", "Open the data folder"))) OpenInShell(a.data);
    ImGui::SameLine();
    if (ImGui::Button(T("Abrir partidas guardadas", "Open saves"))) {
        std::error_code ec;
        fs::create_directories(UserDir(), ec);
        OpenInShell(UserDir());
    }
    {
        bool save_log = a.launcher.Flag("save_log", false);
        if (ImGui::Checkbox(T("Guardar registro detallado (logs/, con estadísticas de fotogramas)",
                              "Save a detailed log (logs/, with frame statistics)"), &save_log)) {
            a.launcher.SetFlag("save_log", save_log);
            SaveLauncher();
        }
        HelpMarker(T("Para reportar fallos o tirones: adjunta el archivo de logs/ en un issue.",
                     "To report crashes or stutter: attach the file from logs/ to an issue."));
    }

    Section(T("Caché de shaders", "Shader cache"));
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextColored(kGrey, "%s", T("Los shaders se guardan para que los siguientes arranques vayan fluidos. Bórrala solo si "
                                      "actualizaste el driver de la GPU o ves fallos gráficos.",
                                      "Shaders are kept so later sessions run smoothly. Clear it only after a GPU driver "
                                      "update or if you see graphics glitches."));
    ImGui::PopTextWrapPos();
    ImGui::BeginDisabled(a.game_running);
    if (ImGui::Button(T("Borrar la caché de shaders", "Clear the shader cache"))) a.confirm_clear_cache = true;
    ImGui::EndDisabled();

    Section(T("Idioma del launcher", "Launcher language"));
    if (ImGui::RadioButton("Español", g_spanish)) {
        g_spanish = true;
        a.launcher.Set("ui_language", "es");
        SaveLauncher();
    }
    ImGui::SameLine();
    if (ImGui::RadioButton("English", !g_spanish)) {
        g_spanish = false;
        a.launcher.Set("ui_language", "en");
        SaveLauncher();
    }

    Section(T("Acerca de", "About"));
    ImGui::Text("CTR Nitro-Fueled PC %s", a.version.c_str());
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(T("Este launcher es el de GetLostIn (GetLostIn/CTR-NitroFueled-PC v0.4) llevado a Linux y la Steam "
                             "Deck. El port: SacredFire (T3CHNOLOG1C), basado en bbport (deadinside28) y shadPS4; online "
                             "de Ryu7w7 (RyuNET). Parches de illusion y TroyWarez. Sin afiliación con Sony, Activision ni "
                             "Beenox. Licencia GPL v2 o posterior.",
                             "This launcher is GetLostIn's (GetLostIn/CTR-NitroFueled-PC v0.4) brought to Linux and the Steam "
                             "Deck. The port: SacredFire (T3CHNOLOG1C), built on bbport (deadinside28) and shadPS4; online by "
                             "Ryu7w7 (RyuNET). Patches by illusion and TroyWarez. Not affiliated with Sony, Activision or "
                             "Beenox. GPL v2 or later."));
    ImGui::Spacing();

    ImGui::TextUnformatted(T("FSR 3.1, la generación de cuadros FSR 3.1.6 y FSR 4 vienen de AMD FidelityFX a través de "
                             "FireBurn/FSR-Vulkan (MIT). 21:9 y 32:9 de tcosi (edición europea: port de GetLostIn); "
                             "edición europea de martijnhe; CPUs antiguas (MetaHeart) y dumps dañados (Naif Alqurashi).",
                             "FSR 3.1, FSR 3.1.6 frame generation and FSR 4 come from AMD FidelityFX through "
                             "FireBurn/FSR-Vulkan (MIT). 21:9 and 32:9 by tcosi (European edition: GetLostIn's port); "
                             "European edition by martijnhe; old CPUs (MetaHeart) and damaged dumps (Naif Alqurashi)."));
    ImGui::PopTextWrapPos();
    ImGui::TextLinkOpenURL(kRepoUrl, kRepoUrl);
    if (a.python) ImGui::TextColored(kGrey, "Python: %s", a.python->description.c_str());
}

void ClearShaderCache() {
    std::error_code ec;
    const fs::path cache = UserDir() / "cache" / GameTitleId();
    const auto removed = fs::remove_all(cache, ec);
    g_app.notice = ec ? std::string(T("No se pudo borrar la caché: ", "Could not clear the cache: ")) + ec.message()
                      : std::string(T("Caché borrada (", "Cache cleared (")) + std::to_string(removed) +
                            T(" archivos).", " files).");
}

// ---------------------------------------------------------------------------------------------
// v0.4 header: the icon, the title, the version and the edition of the chosen game.

/// "América (USA) · CUSA13795", "Europa (PAL) · CUSA14876" or "" without a game.
std::string EditionLabel() {
    const std::string title = GameTitleId();
    bool known = false;
    for (const FolderInfo* info : {&g_app.game_info, &g_app.update_info, &g_app.base_info}) known |= IsCtrTitle(info->title);
    if (!known) return {};
    return std::string(title == "CUSA14876" ? T("Europa (PAL)", "Europe (PAL)") : T("América (USA)", "Americas (USA)")) +
           " · " + title;
}

void Header() {
    App& a = g_app;
    const float s = g_ui_scale;
    const ImVec2 win = ImGui::GetWindowPos();
    const float width = ImGui::GetWindowSize().x;
    const float height = 74.0f * s;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 left = IM_COL32(0x2A, 0x1B, 0x3E, 0xFF), right = IM_COL32(0x15, 0x19, 0x2A, 0xFF);
    dl->AddRectFilledMultiColor(win, ImVec2(win.x + width, win.y + height), left, right, right, left);
    dl->AddRectFilledMultiColor(ImVec2(win.x, win.y + height - 3 * s), ImVec2(win.x + width, win.y + height),
                                IM_COL32(0xFF, 0x7A, 0x18, 0xFF), IM_COL32(0xFF, 0xD2, 0x3F, 0xFF),
                                IM_COL32(0xFF, 0xD2, 0x3F, 0xFF), IM_COL32(0xFF, 0x7A, 0x18, 0xFF));
    const float pad = 18.0f * s, logo = 50.0f * s;
    float x = win.x + pad;
    if (g_logo) {
        const float y = win.y + (height - logo) * 0.5f;
        dl->AddImage(TextureRef(g_logo), ImVec2(x, y), ImVec2(x + logo, y + logo));
        x += logo + 14.0f * s;
    }
    ImFont* title_font = g_font_title ? g_font_title : ImGui::GetFont();
    const float title_size = 30.0f * s;
    dl->AddText(title_font, title_size, ImVec2(x, win.y + 10.0f * s), IM_COL32(0xFF, 0xFF, 0xFF, 0xFF), "CTR Nitro-Fueled PC");
    std::string version = a.version;
    const std::string subtitle =
        std::string(T("Port nativo para Linux y Steam Deck", "Native Linux and Steam Deck port")) +
        (version.empty() ? "" : "  ·  " + version);
    dl->AddText(ImGui::GetFont(), ImGui::GetFontSize() * 0.92f, ImVec2(x + 2 * s, win.y + 10.0f * s + title_size + 2 * s),
                Col(kGrey), subtitle.c_str());

    // Right: the game's state and edition.
    std::vector<std::pair<std::string, ImVec4>> pills;
    const std::string edition = EditionLabel();
    if (a.game_running) pills.emplace_back(std::string(ICON_PLAY "  ") + T("En juego", "Playing"), kGreen);
    else if (a.game_info.kind == Kind::Complete && a.game_checked && a.game_ok)
        pills.emplace_back(std::string(ICON_CHECK "  ") + T("Listo para jugar", "Ready to play"), kGreen);
    else pills.emplace_back(std::string(ICON_WARN "  ") + T("Falta preparar el juego", "Game not set up"), kYellow);
    if (!edition.empty()) pills.emplace_back(std::string(ICON_GLOBE "  ") + edition, kBlue);
    float rx = win.x + width - pad;
    const ImVec2 pp(8.0f * s, 4.0f * s);
    const float line = ImGui::GetTextLineHeight();
    for (const auto& [text, color] : pills) {
        const ImVec2 t = ImGui::CalcTextSize(text.c_str());
        const ImVec2 a0(rx - t.x - pp.x * 2, win.y + (height - line - pp.y * 2) * 0.5f);
        const ImVec2 a1(rx, a0.y + line + pp.y * 2);
        dl->AddRectFilled(a0, a1, Col(color, 0.16f), 99.0f);
        dl->AddRect(a0, a1, Col(color, 0.8f), 99.0f, 0, 1.0f * s);
        dl->AddText(ImVec2(a0.x + pp.x, a0.y + pp.y), Col(color), text.c_str());
        rx = a0.x - 8.0f * s;
    }
    ImGui::SetCursorScreenPos(ImVec2(win.x + ImGui::GetStyle().WindowPadding.x, win.y + height + 10.0f * s));
}

// ---------------------------------------------------------------------------------------------
// Play tab: what the game will start with, at a glance.

void PlaySummary() {
    App& a = g_app;
    auto res = [&]() -> std::string {
        const std::string v = EffectiveRenderRes();
        for (const auto& r : kRenderRes)
            if (v == r.value) return T(r.es, r.en);
        return v;
    };
    auto upscaler = [&]() -> std::string {
        const std::string t = GameTemporal();
        if (t == "fsr1") return "AMD FSR 1";
        if (t == "fsr3") return "AMD FSR 3";
        if (t == "fsr4") return T("AMD FSR 4 (experimental)", "AMD FSR 4 (experimental)");
        if (t == "dlss") return "NVIDIA DLSS";
        return T("Desactivado", "Off");
    };
    auto language = [&]() -> std::string {
        const std::string v = GameLanguage();
        if (EuEdition()) {
            for (const auto& c : kGameLanguagesEu)
                if (v == c.value) return T(c.es, c.en);
        } else {
            for (const auto& c : kGameLanguages)
                if (v == c.value) return T(c.es, c.en);
        }
        return v;
    };
    const int fps = a.game.Int("fps_limit", 60);
    const std::string framegen = a.game.Get("framegen", "off");
    const std::string layout = GameLayout();
    const bool online = a.game.Flag("online", false);
    const std::string aspect = layout == "off" ? std::string("16:9")
                               : online ? LayoutLabel(layout) + T(" (16:9 con online)", " (16:9 with online)")
                                        : LayoutLabel(layout) + "  ·  " + GameLayoutOutput();
    const std::pair<std::string, std::string> rows[] = {
        {T("Resolución interna", "Internal resolution"), res()},
        {T("Escalado", "Upscaling"), layout != "off" ? T("(apagado con 21:9 / 32:9)", "(off with 21:9 / 32:9)") : upscaler()},
        {T("Generación de cuadros", "Frame generation"),
         framegen == "fsr3" || framegen == "fsr" ? "AMD FSR 3" : T("Desactivada", "Off")},
        {T("Relación de aspecto", "Aspect ratio"), aspect},
        {"Online", online ? std::string("RyuNET  ·  ") + a.game.Get("online_name") : std::string(T("No", "Off"))},
        {T("Límite de FPS", "FPS limit"), fps == 0 ? T("Sin límite", "No limit") : std::to_string(fps) + " FPS"},
        {T("Idioma del juego", "Game language"), language()},
    };
    if (ImGui::BeginTable("summary", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("k", ImGuiTableColumnFlags_WidthFixed, ImGui::GetFontSize() * 11.0f);
        ImGui::TableSetupColumn("v", ImGuiTableColumnFlags_WidthStretch);
        for (const auto& [k, v] : rows) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextColored(kGrey, "%s", k.c_str());
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(v.c_str());
        }
        ImGui::EndTable();
    }
    if (ImGui::Button((std::string(ICON_SETTINGS "  ") + T("Cambiar ajustes", "Change settings")).c_str())) g_goto_tab = "settings";
    ImGui::SameLine();
    if (ImGui::Button((std::string(ICON_GAMEPAD "  ") + T("Mandos y teclado", "Controllers and keyboard")).c_str()))
        g_goto_tab = "controls";
}

bool g_quit_confirmed = false;

void Frame() {
    App& a = g_app;
    PollJobs();
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::Begin("launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    Header();

    if (!a.notice.empty()) {
        BeginCard("notice");
        ImGui::TextColored(kYellow, ICON_INFO);
        ImGui::SameLine();
        ImGui::PushTextWrapPos(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 3.0f);
        ImGui::TextUnformatted(a.notice.c_str());
        ImGui::PopTextWrapPos();
        ImGui::SameLine(ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 1.5f);
        if (ImGui::SmallButton("OK")) a.notice.clear();
        EndCard();
    }

    bool on_controls = false;
    if (ImGui::BeginTabBar("tabs")) {
        const char* go = g_goto_tab;
        if (ImGui::GetFrameCount() > 2) g_goto_tab = nullptr;  // a new tab bar picks its first tab once
        auto tab = [go](const char* icon, const char* label, const char* id) {
            const std::string text = std::string(icon) + "  " + label + "###" + id;
            const bool select = go && std::strcmp(go, id) == 0;
            return ImGui::BeginTabItem(text.c_str(), nullptr, select ? ImGuiTabItemFlags_SetSelected : 0);
        };
        if (tab(ICON_PLAY, T("Jugar", "Play"), "play")) {
            ImGui::BeginChild("play");
            PlayTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_SETTINGS, T("Ajustes", "Settings"), "settings")) {
            ImGui::BeginChild("settings");
            SettingsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_GAMEPAD, T("Controles", "Controls"), "controls")) {
            on_controls = true;
            ControlsTab();
            ImGui::EndTabItem();
        }
        if (tab(ICON_STAR, T("Logros", "Achievements"), "achievements")) {
            ImGui::BeginChild("achievements");
            TrophiesTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_PUZZLE, T("Parches", "Patches"), "patches")) {
            ImGui::BeginChild("patches");
            PatchesTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_SAVE, T("Guardados", "Saves"), "saves")) {
            ImGui::BeginChild("saves");
            SavesTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_PACKAGE, "DLC", "dlc")) {
            ImGui::BeginChild("dlc");
            DlcTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (tab(ICON_TOOLS, T("Herramientas", "Tools"), "tools")) {
            ImGui::BeginChild("tools");
            ToolsTab();
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    if (!on_controls) LeaveControls();

    SaveConfirmations();
    if (a.confirm_clear_cache) {
        ImGui::OpenPopup("clear_cache");
        a.confirm_clear_cache = false;
    }
    if (ImGui::BeginPopupModal("clear_cache", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("¿Borrar la caché de shaders? El próximo arranque volverá a compilarlos.",
                                 "Clear the shader cache? The next start compiles them again."));
        if (ImGui::Button(T("Borrar", "Clear"))) {
            ClearShaderCache();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (a.want_close) {
        ImGui::OpenPopup("close");
        a.want_close = false;
    }
    if (ImGui::BeginPopupModal("close", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
        ImGui::TextUnformatted(T("El juego sigue abierto.", "The game is still running."));
        if (ImGui::Button(T("Cerrar el juego y el launcher", "Stop the game and quit"))) {
            StopGame();
            SDL_Event quit{};
            quit.type = SDL_EVENT_QUIT;
            SDL_PushEvent(&quit);
            g_quit_confirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(T("Cancelar", "Cancel"))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
    ImGui::End();
}

void LoadState() {
    App& a = g_app;
    a.root = FindRoot();
    std::error_code ec;
    a.bin = fs::is_directory(a.root / "bin", ec) ? a.root / "bin" : a.root / "out";
    const char* data = std::getenv("BB_DATA_DIR");
    a.data = data && *data ? fs::path(data) : a.root;
    fs::create_directories(a.data, ec);
    for (const fs::path& v : {a.root / "VERSION", a.bin / "VERSION", a.root / "out" / "VERSION"}) {
        if (fs::is_regular_file(v, ec)) {
            a.version = Trim(ReadText(v));
            break;
        }
    }
    a.python = FindPython(a.root);
    a.launcher.Load(LauncherIni());
    a.game.Load(GameIni());
    const std::string lang = a.launcher.Get("ui_language");
    const char* env_lang = std::getenv("LANG");
    g_spanish = lang.empty() ? env_lang && std::strncmp(env_lang, "es", 2) == 0 : lang == "es";
    a.game_dir = TextPath(a.launcher.Get("game_dir"));
    a.base_dir = TextPath(a.launcher.Get("base_dir"));
    a.update_dir = TextPath(a.launcher.Get("update_dir"));
    // A merged folder next to the AppImage (or in the data folder) is picked up, as
    // sacredfire_launcher.py does.
    if (Analyze(a.game_dir).kind != Kind::Complete) {
        std::vector<fs::path> places;
        if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) places.push_back(fs::path(appimage).parent_path());
        places.push_back(a.data);
        places.push_back(a.root);
        places.push_back(a.root.parent_path());  // a checkout next to the game folders
        for (const fs::path& place : places) {
            for (const char* name : {"CUSA14876-1.21", "CUSA13795-1.21", "CUSA14876", "CUSA13795"}) {
                if (Analyze(place / name).kind == Kind::Complete) {
                    a.game_dir = place / name;
                    break;
                }
            }
            if (Analyze(a.game_dir).kind == Kind::Complete) break;
            for (const char* title : {"CUSA13795", "CUSA14876"}) {
                const std::string base = title, update = base + "-patch";
                if (a.base_dir.empty() && Analyze(place / base).kind == Kind::Base) a.base_dir = place / base;
                if (a.update_dir.empty() && Analyze(place / update).kind == Kind::Update) a.update_dir = place / update;
            }
        }
    }
    a.base_info = Analyze(a.base_dir);
    a.update_info = Analyze(a.update_dir);
    a.patches = LoadPatches();
    ReadPatchConfig(a.patch_enabled, a.patch_disabled);
    StartCheck();
    StartProbe();
}

/// The Steam Deck (SteamOS sets SteamDeck=1; gamescope's game mode SteamGamepadUI).
bool OnSteamDeck() {
    const char* deck = std::getenv("SteamDeck");
    const char* gamepad_ui = std::getenv("SteamGamepadUI");
    return (deck && deck[0] == '1') || (gamepad_ui && gamepad_ui[0] == '1');
}

/// A font file: $BB_FONT_DIR, the port's fonts/, then fontconfig's choice for the pattern.
std::string FindFont(std::initializer_list<const char*> files, const char* pattern) {
    std::error_code ec;
    std::vector<fs::path> dirs;
    if (const char* env = std::getenv("BB_FONT_DIR"); env && *env) dirs.emplace_back(env);
    dirs.push_back(g_app.root / "fonts");
    for (const char* dir : {"/usr/share/fonts/TTF", "/usr/share/fonts/truetype/dejavu", "/usr/share/fonts/dejavu",
                            "/usr/share/fonts/noto", "/usr/share/fonts/truetype/noto", "/usr/share/fonts/google-noto"})
        dirs.emplace_back(dir);
    for (const fs::path& dir : dirs)
        for (const char* file : files)
            if (fs::is_regular_file(dir / file, ec)) return (dir / file).string();
    if (!pattern) return {};
    const Captured r = RunCaptured({"fc-match", "-f", "%{file}", pattern}, {}, 3000);
    const std::string path = Trim(r.output);
    return r.started && r.code == 0 && fs::is_regular_file(path, ec) ? path : std::string();
}

// Text in Noto Sans or DejaVu Sans (fontconfig's sans-serif otherwise), with DejaVu Sans merged in
// for the PlayStation shapes and the icons; section titles in the bold face; the header and the
// Play button in the launcher's menu font (art/ctr-menu.ttf, Lilita One).
void LoadFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const std::string body = FindFont({"NotoSans-Regular.ttf", "DejaVuSans.ttf"}, "sans-serif");
    const std::string bold = FindFont({"NotoSans-SemiBold.ttf", "NotoSans-Bold.ttf", "DejaVuSans-Bold.ttf"}, "sans-serif:bold");
    const std::string symbols = FindFont({"DejaVuSans.ttf"}, "DejaVu Sans");
    std::error_code ec;
    std::string title = (g_app.root / "art" / "ctr-menu.ttf").string();
    if (!fs::is_regular_file(title, ec)) title = bold;
    static const ImWchar kSymbolRanges[] = {0x16A0, 0x16FF, 0x2000, 0x2BFF, 0};
    auto merge = [&] {
        if (symbols.empty()) return;
        ImFontConfig cfg;
        cfg.MergeMode = true;
        cfg.GlyphRanges = kSymbolRanges;
        io.Fonts->AddFontFromFileTTF(symbols.c_str(), 0.0f, &cfg);
    };
    if (body.empty()) {
        io.Fonts->AddFontDefault();
        return;
    }
    io.Fonts->AddFontFromFileTTF(body.c_str());
    merge();
    g_font_heading = io.Fonts->AddFontFromFileTTF((bold.empty() ? body : bold).c_str());
    merge();
    g_font_title = io.Fonts->AddFontFromFileTTF((title.empty() ? body : title).c_str());
    merge();
}

// The application icon for the header (and the window).
void LoadLogo() {
    const fs::path icon = g_app.root / "launcher" / "io.github.sacredfire.Launcher.png";
    const std::string png = ReadText(icon);
    g_logo = PngTexture(png);
    if (SDL_Surface* surface = png.empty() ? nullptr : SDL_LoadPNG_IO(SDL_IOFromConstMem(png.data(), png.size()), true)) {
        SDL_SetWindowIcon(g_app.hwnd, surface);
        SDL_DestroySurface(surface);
    }
}

} // namespace

int main(int argc, char** argv) {
#ifdef LAUNCHER_SELFTEST
    // Test build only: --aes (FIPS-197 C.1 through AesCbc), --patches <port>.
    if (argc >= 2 && std::string(argv[1]) == "--aes") {
        std::string key;
        unsigned char plain[16], out[16], back[16], zero[16]{};
        for (int i = 0; i < 16; ++i) {
            key += char(i);
            plain[i] = (unsigned char)(i * 0x11);
        }
        AesCbc(key, zero, plain, 16, out, true);
        AesCbc(key, zero, out, 16, back, false);
        char hex[40]{};
        for (int i = 0; i < 16; ++i) std::snprintf(hex + i * 2, 3, "%02x", out[i]);
        const bool ok = std::string(hex) == "69c4e0d86a7b0430d8cdb78070b4c55a" && std::memcmp(back, plain, 16) == 0;
        std::printf("aes %s %s\n", hex, ok ? "ok" : "BAD");
        return ok ? 0 : 1;
    }
    if (argc >= 3 && std::string(argv[1]) == "--settings") {
        g_app.launcher.Load(argv[2]);
        g_app.launcher.SetFlag("pad_center_cal", false);
        g_app.launcher.Set("game_dir", "/games/CTR \"EU\"");
        g_app.launcher.Save(std::string(argv[2]) + ".out", nullptr);
        return 0;
    }
#endif
    // --tab <play|settings|controls|achievements|patches|saves|dlc|tools>: the tab to open first.
    static std::string first_tab;
    for (int i = 1; i + 1 < argc; ++i)
        if (std::string(argv[i]) == "--tab") first_tab = argv[i + 1];
    if (!first_tab.empty()) g_goto_tab = first_tab.c_str();

    SDL_SetHint(SDL_HINT_APP_ID, "io.github.sacredfire.Launcher");
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD)) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    const bool deck = OnSteamDeck();
    float scale = SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    if (scale <= 0.0f) scale = 1.0f;
    if (deck) scale = std::max(scale, 1.25f);  // 1280 x 800 at arm's length
    SDL_WindowFlags flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY;
    if (deck) flags |= SDL_WINDOW_FULLSCREEN;
    SDL_Window* window = SDL_CreateWindow("CTR Nitro-Fueled PC", int(940 * scale), int(800 * scale), flags);
    if (!window) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    g_window = window;
    g_app.hwnd = window;
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) renderer = SDL_CreateRenderer(window, SDL_SOFTWARE_RENDERER);
    if (!renderer) {
        std::fprintf(stderr, "SDL: %s\n", SDL_GetError());
        return 1;
    }
    g_renderer = renderer;
    SDL_SetRenderVSync(renderer, 1);
    SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    SDL_ShowWindow(window);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.FontSizeBase = 17.0f;
    style.ScaleAllSizes(scale);
    style.FontScaleDpi = scale;
    g_ui_scale = scale;
    ApplyTheme(scale);

    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
    LoadState();
    LoadFonts();
    LoadLogo();

    bool done = false;
    while (!done) {
        const SDL_WindowFlags state = SDL_GetWindowFlags(window);
        const bool idle = (state & (SDL_WINDOW_MINIMIZED | SDL_WINDOW_HIDDEN)) != 0;
        SDL_Event event;
        // Idle while minimized or hidden for the game: wait for events or 100 ms (background work).
        bool have = idle ? SDL_WaitEventTimeout(&event, 100) : SDL_PollEvent(&event);
        while (have) {
            // Keyboard page: the next key's scan code, before ImGui sees it.
            if (event.type == SDL_EVENT_KEY_DOWN && g_key_capture >= 0 && !event.key.repeat) {
                g_key_captured = unsigned(event.key.scancode) + 1;
            } else {
                ImGui_ImplSDL3_ProcessEvent(&event);
            }
            if (event.type == SDL_EVENT_QUIT ||
                (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED && event.window.windowID == SDL_GetWindowID(window))) {
                if (g_app.game_running && !g_quit_confirmed) {
                    g_app.want_close = true;
                    if (g_hidden_for_game) SDL_ShowWindow(window);
                    SDL_RestoreWindow(window);
                } else {
                    done = true;
                }
            }
            have = SDL_PollEvent(&event);
        }
        if (done) break;
        if (idle) {
            PollJobs();
            continue;
        }
        ImGui_ImplSDLRenderer3_NewFrame();
        ImGui_ImplSDL3_NewFrame();
        ImGui::NewFrame();
        Frame();
        ImGui::Render();
        SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
        SDL_SetRenderDrawColorFloat(renderer, kBg.x, kBg.y, kBg.z, 1.0f);
        SDL_RenderClear(renderer);
        ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
        SDL_RenderPresent(renderer);
    }

    // Background work finishes on its own; a running game keeps running.
    if (g_app.worker.joinable()) {
        if (g_app.job == Job::Play) g_app.worker.detach();
        else g_app.worker.join();
    }
    if (g_app.probe.joinable()) g_app.probe.join();
    Pads::Stop();
    ReleaseTrophies(g_trophies);
    if (g_logo) SDL_DestroyTexture(g_logo);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}
