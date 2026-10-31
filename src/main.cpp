// ============================================================================
// SyncTool v2.1.0 —— 多文件夹同步 / 还原工具（Windows / Linux / macOS，C++17）
//
// 用法：
//   SyncTool sync <文件夹1> <文件夹2> [...] [--min-level N] [--dry-run]
//                 [--jobs N] [--exclude 模式]... [--force]
//   SyncTool restore <文件夹> [--time <时间戳>] [--dry-run]
//   SyncTool list <文件夹>
//   SyncTool help | --version
//
// v2 相对 v1 的变化：
//   1) 备份目录改为 .backup（Windows 下自动设为隐藏），扫描时排除；
//   2) 同步前先完整扫描并生成"操作计划"，写入用户临时目录的 JSON 文件，
//      并用独占句柄占用该文件（防止被清理、防止并发运行）；
//   3) 预检磁盘空间：目标盘放不下时默认中止，避免写满导致半截文件；
//   4) 总进度 / 当前操作 / 单文件分进度（扫描、预检、执行三阶段）；
//   5) Windows 下同步期间占用各根文件夹（共享读 + 独占写），提示用户不要修改；
//   6) 多线程并行复制/备份（--jobs），zip 改为流式写盘，内存占用大幅降低；
//   7) 跨平台：文件锁、时间戳、控制台、隐藏属性均做平台分支；
//   8) 同一最小单元时间完全相同则跳过，不做无意义复制。
//   9) v2.1：时间相同先比对内容哈希；不同则逐个询问源/跳过，询问期间其他线程继续复制；
//       Ctrl+C 可保留备份（下次继续）或回滚本次更改；新增 clean 清理备份；
//       Windows 长路径未开启时询问并自动开启（必要时提权 + 重启自身）。
// ============================================================================
#define _CRT_SECURE_NO_WARNINGS
#if defined(_WIN32)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <io.h>
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/file.h>
#  include <sys/stat.h>
#  include <signal.h>
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#if defined(_WIN32)
#  include <shellapi.h>
#endif

#include "miniz.h"

namespace fs = std::filesystem;
using std::string;
using std::vector;

static const char* kVersion = "2.1.0";
static const char* kBackupName = ".backup";
static const char* kLegacyBackupName = "backup";
static const char* kJournalName = "journal.jsonl";

// ---------------------------------------------------------------- 全局状态 ---
static std::atomic<bool> g_cancel{false};
static std::atomic<bool> g_lowSpace{false};
static std::atomic<uint64_t> g_scanFiles{0};
static std::atomic<uint64_t> g_doneOps{0};
static std::atomic<uint64_t> g_doneBytes{0};
static std::atomic<uint64_t> g_doneFiles{0};
static std::atomic<uint64_t> g_totalOps{0};
static std::atomic<uint64_t> g_totalBytes{0};
static std::atomic<uint64_t> g_hashDone{0};
static std::atomic<uint64_t> g_hashTotal{0};
static std::atomic<bool> g_promptActive{false};
static std::mutex g_promptMutex;
static bool g_yes = false;
static std::atomic<bool> g_needLongPaths{false};
static std::atomic<uint32_t> g_maxPathLen{0};
static std::vector<string> g_originalArgs;
static std::mutex g_logMutex;
static std::mutex g_curMutex;
static std::mutex g_errMutex;
static std::mutex g_manifestMutex;
static std::string g_current;
static vector<string> g_errors;
static bool g_tty = false;
static std::atomic<int> g_phase{0}; // 0=扫描 1=预检 2=哈希 3=执行 4=完成

static void logLine(const string& s);
static bool askYesNo(const string& question, bool defaultYes);
static void clearHashCaches();

#if defined(_WIN32)
#  pragma comment(lib, "advapi32.lib")
#  pragma comment(lib, "shell32.lib")
#endif

// ---------------------------------------------------------------- 平台工具 ---
static void setupConsole()
{
#if defined(_WIN32)
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleCP(CP_UTF8);
    g_tty = _isatty(_fileno(stdout)) != 0;
#else
    g_tty = isatty(STDOUT_FILENO) != 0;
#endif
    std::cout << std::unitbuf;
}

static void installSignalHandlers()
{
#if defined(_WIN32)
    SetConsoleCtrlHandler([](DWORD type) -> BOOL {
        if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT)
        {
            g_cancel.store(true);
            return TRUE;
        }
        return FALSE;
    }, TRUE);
#else
    struct sigaction sa;
    std::memset(&sa, 0, sizeof(sa));
    sa.sa_handler = [](int) { g_cancel.store(true); };
    sigemptyset(&sa.sa_mask);
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);
#endif
}

static bool g_noLongPaths = false;
static bool g_skipLongPathCheck = false;
static bool g_noHashCache = false;

#if defined(_WIN32)
static std::wstring utf8ToWide(const string& s)
{
    if (s.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int)s.size(), &w[0], n);
    return w;
}

static std::wstring getExePathW()
{
    std::vector<wchar_t> buf(32768);
    DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    return std::wstring(buf.data(), n);
}

static bool isElevated()
{
    HANDLE h = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &h)) return false;
    TOKEN_ELEVATION te{};
    DWORD sz = sizeof(te);
    BOOL ok = GetTokenInformation(h, TokenElevation, &te, sizeof(te), &sz);
    CloseHandle(h);
    return ok && te.TokenIsElevated;
}

static bool longPathsEnabled()
{
    DWORD v = 0, sz = sizeof(v);
    LONG r = RegGetValueW(HKEY_LOCAL_MACHINE,
                          L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                          L"LongPathsEnabled", RRF_RT_REG_DWORD, nullptr, &v, &sz);
    return r == ERROR_SUCCESS && v != 0;
}

static bool setLongPathsEnabled()
{
    HKEY k = nullptr;
    if (RegOpenKeyExW(HKEY_LOCAL_MACHINE, L"SYSTEM\\CurrentControlSet\\Control\\FileSystem",
                      0, KEY_SET_VALUE | KEY_QUERY_VALUE, &k) != ERROR_SUCCESS)
        return false;
    DWORD v = 1;
    LONG r = RegSetValueExW(k, L"LongPathsEnabled", 0, REG_DWORD, (const BYTE*)&v, sizeof(v));
    RegCloseKey(k);
    return r == ERROR_SUCCESS;
}

static std::wstring quoteWinArg(const std::wstring& a)
{
    bool need = a.empty() || a.find_first_of(L" \t\"") != std::wstring::npos;
    if (!need) return a;
    std::wstring out = L"\"";
    for (wchar_t c : a)
    {
        if (c == L'"') out += L"\\\"";
        else out += c;
    }
    out += L"\"";
    return out;
}

static int runElevatedEnableLongPaths()
{
    std::wstring exe = getExePathW();
    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = exe.c_str();
    sei.lpParameters = L"--enable-long-paths";
    sei.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&sei)) return 1;
    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(sei.hProcess, &code);
    CloseHandle(sei.hProcess);
    return (int)code;
}

static int relaunchSelfWithLongPaths()
{
    std::wstring cmd = L"\"" + getExePathW() + L"\"";
    for (size_t i = 1; i < g_originalArgs.size(); ++i)
    {
        if (g_originalArgs[i] == "--long-paths-enabled") continue;
        cmd += L" ";
        cmd += quoteWinArg(utf8ToWide(g_originalArgs[i]));
    }
    cmd += L" --long-paths-enabled";

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(0);
    if (!CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi))
    {
        logLine("[长路径] 重新启动失败，请手动重新运行。");
        return 1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return (int)code;
}

// 返回 true 表示继续；false 表示已经重启（exitCode 为子进程退出码）
static bool ensureLongPaths(int& exitCode)
{
    if (g_skipLongPathCheck || g_noLongPaths) return true;
    if (longPathsEnabled()) return true;

    bool yes = askYesNo("检测到 Windows 长路径支持（LongPathsEnabled）未开启，是否自动开启？", true);
    if (!yes)
    {
        logLine("[长路径] 未开启；路径超过 260 字符时可能失败。");
        return true;
    }

    bool ok = false;
    if (isElevated()) ok = setLongPathsEnabled();
    else
    {
        int rc = runElevatedEnableLongPaths();
        ok = (rc == 0) && longPathsEnabled();
    }
    if (!ok)
    {
        logLine("[长路径] 自动开启失败（可能取消了 UAC 或没有管理员权限）。");
        return true;
    }
    logLine("[长路径] 已开启 LongPathsEnabled=1，正在重新启动程序以生效……");
    exitCode = relaunchSelfWithLongPaths();
    return false;
}
#else
static bool ensureLongPaths(int& exitCode)
{
    (void)exitCode;
    return true;
}
#endif

static std::tm localNowTm()
{
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
#if defined(_WIN32)
    localtime_s(&tmv, &t);
#else
    localtime_r(&t, &tmv);
#endif
    return tmv;
}

// file_time_type <-> time_t（跨平台，按两个时钟的当前偏移换算）
static std::time_t fileTimeToTimeT(const fs::file_time_type& ft)
{
    using namespace std::chrono;
    auto sctp = time_point_cast<system_clock::duration>(
        ft - fs::file_time_type::clock::now() + system_clock::now());
    return system_clock::to_time_t(sctp);
}

static fs::file_time_type timeTToFileTime(std::time_t t)
{
    using namespace std::chrono;
    auto sctp = system_clock::from_time_t(t);
    return time_point_cast<fs::file_time_type::duration>(
        sctp - system_clock::now() + fs::file_time_type::clock::now());
}

static string makeTimestamp()
{
    std::tm tmv = localNowTm();
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d-%02d-%02d-%02d",
                  tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return string(buf);
}

static string trim(const string& s)
{
    size_t off = 0;
    // 去掉 PowerShell 管道/文件可能带上的 UTF-8 BOM
    if (s.size() >= 3 && (unsigned char)s[0] == 0xEF && (unsigned char)s[1] == 0xBB && (unsigned char)s[2] == 0xBF)
        off = 3;
    size_t b = s.find_first_not_of(" \t\r\n", off);
    if (b == string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static bool endsWith(const string& s, const string& suf)
{
    return s.size() >= suf.size() && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
}

static string toLower(string s)
{
    for (auto& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

static string relStr(const fs::path& root, const fs::path& p)
{
    fs::path rel = p.lexically_relative(root);
    string out;
    for (const auto& part : rel)
    {
        if (!out.empty()) out += '/';
        out += part.u8string();
    }
    return out;
}

static uint32_t pathLength(const fs::path& p)
{
#if defined(_WIN32)
    return (uint32_t)p.wstring().size();
#else
    return (uint32_t)p.u8string().size();
#endif
}

static void notePathLength(const fs::path& p)
{
    uint32_t n = pathLength(p);
    uint32_t old = g_maxPathLen.load();
    while (n > old && !g_maxPathLen.compare_exchange_weak(old, n)) {}
#if defined(_WIN32)
    if (n >= 250) g_needLongPaths.store(true);
#else
    if (n >= 3500) g_needLongPaths.store(true);
#endif
}

static void noteLongPathError(const std::error_code& ec)
{
#if defined(_WIN32)
    if (ec.value() == ERROR_FILENAME_EXCED_RANGE || ec.value() == ERROR_PATH_NOT_FOUND)
        g_needLongPaths.store(true);
#else
    if (ec.value() == ENAMETOOLONG) g_needLongPaths.store(true);
#endif
}

static int countComps(const string& s)
{
    if (s.empty()) return 0;
    int n = 1;
    for (char c : s) if (c == '/') ++n;
    return n;
}

static string firstNComps(const string& s, int k)
{
    size_t pos = 0;
    for (int i = 0; i < k - 1; ++i)
    {
        pos = s.find('/', pos);
        if (pos == string::npos) return s;
        ++pos;
    }
    size_t end = s.find('/', pos);
    return (end == string::npos) ? s : s.substr(0, end);
}

static string formatBytes(uint64_t n)
{
    const char* units[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)n;
    int i = 0;
    while (v >= 1024.0 && i < 4) { v /= 1024.0; ++i; }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%.2f %s", v, units[i]);
    return string(buf);
}

static string formatDuration(double sec)
{
    if (sec < 0 || sec > 359999) return "--:--:--";
    int s = (int)sec;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    return string(buf);
}

static string formatNum(uint64_t n)
{
    string s = std::to_string(n);
    string out;
    int c = 0;
    for (int i = (int)s.size() - 1; i >= 0; --i)
    {
        out.insert(out.begin(), s[i]);
        if (++c % 3 == 0 && i > 0) out.insert(out.begin(), ',');
    }
    return out;
}

static string jsonEscape(const string& s)
{
    string out;
    out.reserve(s.size() + 16);
    for (unsigned char c : s)
    {
        switch (c)
        {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20)
            {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                out += buf;
            }
            else out += (char)c;
        }
    }
    return out;
}

static void setHidden(const fs::path& p)
{
#if defined(_WIN32)
    DWORD a = GetFileAttributesW(p.c_str());
    if (a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_HIDDEN))
        SetFileAttributesW(p.c_str(), a | FILE_ATTRIBUTE_HIDDEN);
#else
    (void)p;
#endif
}

static bool freeSpaceOf(const fs::path& p, uint64_t& out)
{
    std::error_code ec;
    fs::space_info si = fs::space(p, ec);
    if (ec) return false;
    out = si.available;
    return true;
}

static uint64_t fnv1a64(const string& s)
{
    uint64_t h = 1469598103934665603ULL;
    for (unsigned char c : s)
    {
        h ^= c;
        h *= 1099511628211ULL;
    }
    return h;
}

// ---------------------------------------------------------------- 内容哈希 ---
// 128 位非加密哈希（两个不同乘子的 FNV 变体），用于“时间相同但内容可能不同”的比对。
struct H128
{
    uint64_t a = 1469598103934665603ULL;
    uint64_t b = 1099511628211ULL;
};

static inline uint64_t rotl64(uint64_t x, int r)
{
    return (x << r) | (x >> (64 - r));
}

static inline void h128MixWord(H128& h, uint64_t x)
{
    h.a ^= x;
    h.a *= 1099511628211ULL;
    h.a = rotl64(h.a, 13);
    h.b += x;
    h.b *= 14029467366897019727ULL;
    h.b = rotl64(h.b, 31);
}

static inline void h128MixByte(H128& h, unsigned char c)
{
    h.a = (h.a ^ c) * 1099511628211ULL;
    h.b = (h.b ^ c) * 14029467366897019727ULL;
}

static void h128MixData(H128& h, const void* data, size_t n)
{
    const unsigned char* p = (const unsigned char*)data;
    size_t i = 0;
    for (; i + 8 <= n; i += 8)
    {
        uint64_t x;
        std::memcpy(&x, p + i, 8);
        h128MixWord(h, x);
    }
    for (; i < n; ++i) h128MixByte(h, p[i]);
}

static string h128Finish(H128 h, uint64_t total)
{
    h.a ^= total;
    h.a *= 1099511628211ULL;
    h.b ^= total;
    h.b *= 14029467366897019727ULL;
    char buf[40];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx",
                  (unsigned long long)h.a, (unsigned long long)h.b);
    return string(buf);
}

static string hexEncode(const string& s)
{
    static const char* d = "0123456789abcdef";
    string out;
    out.reserve(s.size() * 2);
    for (unsigned char c : s)
    {
        out += d[c >> 4];
        out += d[c & 15];
    }
    return out;
}

static string hexDecode(const string& s)
{
    auto val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    string out;
    out.reserve(s.size() / 2);
    for (size_t i = 0; i + 1 < s.size(); i += 2)
    {
        int hi = val(s[i]), lo = val(s[i + 1]);
        if (hi < 0 || lo < 0) return string();
        out += (char)((hi << 4) | lo);
    }
    return out;
}

struct HashCacheEntry
{
    uint64_t size = 0;
    int64_t mtime = 0;
    string hash;
};

class HashCache
{
public:
    ~HashCache() { save(); }

    void open(const fs::path& p)
    {
        enabled_ = true;
        path_ = p;
        std::ifstream f(path_, std::ios::binary);
        if (!f) return;
        string line;
        while (std::getline(f, line))
        {
            size_t t1 = line.find('\t');
            if (t1 == string::npos) continue;
            size_t t2 = line.find('\t', t1 + 1);
            if (t2 == string::npos) continue;
            size_t t3 = line.find('\t', t2 + 1);
            if (t3 == string::npos) continue;
            string path = hexDecode(line.substr(0, t1));
            if (path.empty()) continue;
            HashCacheEntry e;
            e.size = (uint64_t)std::strtoull(line.substr(t1 + 1, t2 - t1 - 1).c_str(), nullptr, 10);
            e.mtime = (int64_t)std::strtoll(line.substr(t2 + 1, t3 - t2 - 1).c_str(), nullptr, 10);
            e.hash = line.substr(t3 + 1);
            if (!e.hash.empty()) map_[path] = e;
        }
    }

    bool get(const fs::path& p, uint64_t size, int64_t mtime, string& out) const
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!enabled_) return false;
        auto it = map_.find(p.u8string());
        if (it == map_.end()) return false;
        if (it->second.size != size || it->second.mtime != mtime) return false;
        out = it->second.hash;
        return true;
    }

    void put(const fs::path& p, uint64_t size, int64_t mtime, const string& hash)
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!enabled_) return;
        HashCacheEntry e;
        e.size = size;
        e.mtime = mtime;
        e.hash = hash;
        map_[p.u8string()] = e;
        dirty_ = true;
    }

    void erase(const fs::path& p)
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!enabled_) return;
        auto it = map_.find(p.u8string());
        if (it != map_.end()) { map_.erase(it); dirty_ = true; }
    }

    void eraseTree(const fs::path& p)
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!enabled_) return;
        string prefix = p.u8string();
        if (!prefix.empty() && prefix.back() != '/' && prefix.back() != '\\')
            prefix += fs::path::preferred_separator;
        for (auto it = map_.begin(); it != map_.end();)
        {
            if (it->first == p.u8string() || it->first.compare(0, prefix.size(), prefix) == 0)
            {
                it = map_.erase(it);
                dirty_ = true;
            }
            else ++it;
        }
    }

    void save()
    {
        std::lock_guard<std::mutex> lk(m_);
        if (!enabled_ || !dirty_ || path_.empty()) return;
        std::error_code ec;
        fs::create_directories(path_.parent_path(), ec);
        fs::path tmp = path_;
        tmp += ".tmp";
        {
            std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
            if (!f) return;
            for (const auto& kv : map_)
            {
                f << hexEncode(kv.first) << '\t' << kv.second.size << '\t'
                  << kv.second.mtime << '\t' << kv.second.hash << '\n';
            }
        }
        fs::rename(tmp, path_, ec);
        if (ec)
        {
            // Windows 上目标已存在时 rename 可能失败；先删旧文件再试一次
            std::error_code rec;
            fs::remove(path_, rec);
            fs::rename(tmp, path_, ec);
        }
        if (ec) { std::error_code e2; fs::remove(tmp, e2); }
        dirty_ = false;
    }

private:
    fs::path path_;
    std::unordered_map<string, HashCacheEntry> map_;
    mutable std::mutex m_;
    bool dirty_ = false;
    bool enabled_ = false;
};

static int64_t mtimeKey(const fs::file_time_type& t)
{
    return (int64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(t.time_since_epoch()).count();
}

static bool hashFileContents(const fs::path& p, uint64_t size, int64_t mtime,
                             HashCache* cache, string& out)
{
    if (cache && cache->get(p, size, mtime, out)) return true;
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    H128 h;
    vector<char> buf(1 << 20);
    uint64_t total = 0;
    while (f)
    {
        f.read(buf.data(), (std::streamsize)buf.size());
        std::streamsize n = f.gcount();
        if (n > 0)
        {
            h128MixData(h, buf.data(), (size_t)n);
            total += (uint64_t)n;
        }
    }
    if (f.bad()) return false;
    out = h128Finish(h, total);
    if (cache) cache->put(p, size, mtime, out);
    return true;
}

// ---------------------------------------------------------------- 日志/进度 ---
static size_t g_progressLen = 0;

static void clearProgressLine()
{
    if (!g_tty || g_progressLen == 0) return;
    std::cout << "\r" << string(g_progressLen, ' ') << "\r";
    g_progressLen = 0;
    std::cout.flush();
}

static void logLine(const string& s)
{
    std::lock_guard<std::mutex> lk(g_logMutex);
    clearProgressLine();
    std::cout << s << "\n";
    std::cout.flush();
}

static void addError(const string& s)
{
    std::lock_guard<std::mutex> lk(g_errMutex);
    g_errors.push_back(s);
    if (g_errors.size() <= 200) logLine("[错误] " + s);
    else if (g_errors.size() == 201) logLine("[错误] 错误过多，后续仅计数……");
}

// 交互输入：暂停进度刷新，避免提示被进度行覆盖
static string askLine(const string& prompt)
{
    std::lock_guard<std::mutex> lk(g_promptMutex);
    g_promptActive.store(true);
    {
        std::lock_guard<std::mutex> lg(g_logMutex);
        clearProgressLine();
        std::cout << prompt;
        std::cout.flush();
    }
    string line;
    if (!std::getline(std::cin, line)) line.clear();
    if (!g_tty) std::cout << "\n";
    g_promptActive.store(false);
    return trim(line);
}

static bool askYesNo(const string& question, bool defaultYes)
{
    if (g_yes) return true; // --yes：确认类询问一律继续
    string a = askLine(question + (defaultYes ? " [Y/n] " : " [y/N] "));
    if (a.empty()) return defaultYes;
    return a[0] == 'y' || a[0] == 'Y';
}

// 写入过程中的兜底：剩余空间低于 256MB 立即停止，避免把系统盘写满
static void checkLowSpace(const fs::path& p)
{
    if (g_lowSpace.load()) return;
    uint64_t a = 0;
    if (freeSpaceOf(p, a) && a < (512ull << 20))
    {
        g_lowSpace.store(true);
        g_cancel.store(true);
        addError("目标磁盘剩余空间不足 512 MB，已停止写入: " + p.u8string());
    }
}

static void setCurrent(const string& s)
{
    std::lock_guard<std::mutex> lk(g_curMutex);
    g_current = s;
}

static string getCurrent()
{
    std::lock_guard<std::mutex> lk(g_curMutex);
    return g_current;
}

class ProgressUI
{
public:
    void start()
    {
        stop_.store(false);
        th_ = std::thread([this] { loop(); });
    }
    void stop()
    {
        if (stopped_) return;
        stopped_ = true;
        stop_.store(true);
        if (th_.joinable()) th_.join();
        render(true);
    }
    ~ProgressUI() { stop(); }

private:
    void loop()
    {
        auto last = std::chrono::steady_clock::now();
        while (!stop_.load())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
            if (g_promptActive.load()) continue;
            auto now = std::chrono::steady_clock::now();
            if (!g_tty && std::chrono::duration_cast<std::chrono::seconds>(now - last).count() < 5)
                continue;
            last = now;
            render(false);
        }
    }

    void render(bool final)
    {
        std::lock_guard<std::mutex> lk(g_logMutex);
        if (g_promptActive.load() && !final) return;
        int ph = g_phase.load();
        string cur = getCurrent();
        if (cur.size() > 70) cur = "..." + cur.substr(cur.size() - 67);

        std::ostringstream os;
        if (ph == 0)
        {
            os << "[扫描] 已扫描文件 " << formatNum(g_scanFiles.load());
            if (!cur.empty()) os << " · " << cur;
        }
        else if (ph == 1)
        {
            os << "[预检] 正在生成操作计划";
            if (!cur.empty()) os << " · " << cur;
        }
        else if (ph == 2)
        {
            uint64_t done = g_hashDone.load(), total = g_hashTotal.load();
            os << "[哈希] 已比对 " << formatNum(done) << "/" << formatNum(total) << " 个版本";
            if (!cur.empty()) os << " · " << cur;
        }
        else
        {
            uint64_t done = g_doneOps.load(), total = g_totalOps.load();
            double pct = total ? (double)done * 100.0 / (double)total : 0.0;
            static auto t0 = std::chrono::steady_clock::now();
            static uint64_t lastBytes = 0;
            static auto lastT = t0;
            auto now = std::chrono::steady_clock::now();
            double dt = std::chrono::duration<double>(now - lastT).count();
            double speed = 0;
            if (dt > 0.5)
            {
                uint64_t b = g_doneBytes.load();
                speed = (double)(b - lastBytes) / dt;
                lastBytes = b;
                lastT = now;
            }
            double eta = speed > 1 ? (double)(g_totalBytes.load() - g_doneBytes.load()) / speed : -1;
            os << "[执行 " << std::fixed << std::setprecision(1) << pct << "%] "
               << formatNum(done) << "/" << formatNum(total) << " 项 · "
               << formatBytes(g_doneBytes.load()) << "/" << formatBytes(g_totalBytes.load())
               << " · " << formatBytes((uint64_t)speed) << "/s · 剩余 " << formatDuration(eta);
            if (!cur.empty()) os << " · " << cur;
        }

        string line = os.str();
        if (g_tty)
        {
            if (line.size() < g_progressLen) line += string(g_progressLen - line.size(), ' ');
            g_progressLen = line.size();
            std::cout << "\r" << line;
        }
        else
        {
            std::cout << line << "\n";
        }
        if (final) std::cout << "\n";
        std::cout.flush();
    }

    std::atomic<bool> stop_{false};
    bool stopped_ = false;
    std::thread th_;
};

// ---------------------------------------------------------------- 临时计划 ---
class TempPlan
{
public:
    ~TempPlan() { cleanup(); }

    bool open(const string& hash, string& err)
    {
        std::error_code ec;
        fs::path dir = fs::temp_directory_path(ec);
        if (ec) { err = "无法获取临时目录: " + ec.message(); return false; }
        dir /= "SyncTool";
        fs::create_directories(dir, ec);
        if (ec) { err = "无法创建临时目录: " + ec.message(); return false; }
        path_ = dir / ("plan-" + hash + ".json");

#if defined(_WIN32)
        handle_ = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
        {
            err = "无法占用临时计划文件（可能有另一个同步正在运行）: " + path_.u8string();
            return false;
        }
#else
        fd_ = ::open(path_.c_str(), O_RDWR | O_CREAT | O_TRUNC, 0600);
        if (fd_ < 0) { err = "无法创建临时计划文件: " + path_.u8string(); return false; }
        if (flock(fd_, LOCK_EX | LOCK_NB) != 0)
        {
            ::close(fd_);
            fd_ = -1;
            err = "无法锁定临时计划文件（可能有另一个同步正在运行）: " + path_.u8string();
            return false;
        }
#endif
        return true;
    }

    bool write(const string& content, string& err)
    {
#if defined(_WIN32)
        if (handle_ == INVALID_HANDLE_VALUE) { err = "计划文件未打开"; return false; }
        LARGE_INTEGER zero{};
        SetFilePointerEx(handle_, zero, nullptr, FILE_BEGIN);
        SetEndOfFile(handle_);
        DWORD written = 0;
        const char* p = content.data();
        size_t left = content.size();
        while (left > 0)
        {
            DWORD chunk = (DWORD)std::min<size_t>(left, 1u << 20);
            if (!WriteFile(handle_, p, chunk, &written, nullptr) || written == 0)
            { err = "写入临时计划文件失败"; return false; }
            p += written;
            left -= written;
        }
        FlushFileBuffers(handle_);
#else
        if (fd_ < 0) { err = "计划文件未打开"; return false; }
        if (ftruncate(fd_, 0) != 0) { err = "截断临时计划文件失败"; return false; }
        if (lseek(fd_, 0, SEEK_SET) < 0) { err = "定位临时计划文件失败"; return false; }
        const char* p = content.data();
        size_t left = content.size();
        while (left > 0)
        {
            ssize_t n = ::write(fd_, p, left);
            if (n <= 0) { err = "写入临时计划文件失败"; return false; }
            p += n;
            left -= (size_t)n;
        }
        fsync(fd_);
#endif
        return true;
    }

    const fs::path& path() const { return path_; }

    void cleanup()
    {
#if defined(_WIN32)
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            DeleteFileW(path_.c_str());
        }
#else
        if (fd_ >= 0)
        {
            ::close(fd_);
            fd_ = -1;
            ::unlink(path_.c_str());
        }
#endif
    }

private:
#if defined(_WIN32)
    HANDLE handle_ = INVALID_HANDLE_VALUE;
#else
    int fd_ = -1;
#endif
    fs::path path_;
};

// ---------------------------------------------------------------- 文件夹占用 ---
// Windows：以“共享读、不共享写/删”的方式打开根目录，持续占用到同步结束，
// 期间其他程序无法重命名/删除该目录，Explorer 也无法进入；子文件的写入
// 仍可能发生（Windows 没有内建强制目录锁），因此仅作为“正在使用”的保护。
class FolderLock
{
public:
    ~FolderLock() { release(); }

    bool acquire(const fs::path& dir, string& err)
    {
#if defined(_WIN32)
        HANDLE h = CreateFileW(dir.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
        if (h == INVALID_HANDLE_VALUE)
        {
            err = "无法占用文件夹（可能被其他程序独占）: " + dir.u8string();
            return false;
        }
        handles_.push_back(h);
#else
        int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY);
        if (fd < 0) { err = "无法打开文件夹: " + dir.u8string(); return false; }
        if (flock(fd, LOCK_EX | LOCK_NB) != 0)
        {
            ::close(fd);
            err = "无法锁定文件夹（可能被其他程序占用）: " + dir.u8string();
            return false;
        }
        fds_.push_back(fd);
#endif
        return true;
    }

    void release()
    {
#if defined(_WIN32)
        for (HANDLE h : handles_) CloseHandle(h);
        handles_.clear();
#else
        for (int fd : fds_) ::close(fd);
        fds_.clear();
#endif
    }

private:
#if defined(_WIN32)
    vector<HANDLE> handles_;
#else
    vector<int> fds_;
#endif
};

// ---------------------------------------------------------------- 排除规则 ---
class Excludes
{
public:
    void add(const string& p)
    {
        if (!p.empty()) patterns_.push_back(normalize(p));
    }
    bool empty() const { return patterns_.empty(); }

    bool match(const string& rel, bool isDir) const
    {
        if (rel.empty()) return false;
        string r = normalize(rel);
        string base = r;
        size_t pos = base.find_last_of('/');
        if (pos != string::npos) base = base.substr(pos + 1);
        for (const auto& pat : patterns_)
        {
            if (wildcard(pat, r)) return true;
            if (wildcard(pat, base)) return true;
        }
        (void)isDir;
        return false;
    }

private:
    static string normalize(const string& s)
    {
        string r = s;
        for (auto& c : r) if (c == '\\') c = '/';
#if defined(_WIN32)
        r = toLower(r);
#endif
        return r;
    }

    static bool wildcard(const string& pat, const string& text)
    {
        size_t p = 0, t = 0, star = string::npos, mark = 0;
        while (t < text.size())
        {
            if (p < pat.size() && (pat[p] == '?' || pat[p] == text[t])) { ++p; ++t; }
            else if (p < pat.size() && pat[p] == '*') { star = p++; mark = t; }
            else if (star != string::npos) { p = star + 1; t = ++mark; }
            else return false;
        }
        while (p < pat.size() && pat[p] == '*') ++p;
        return p == pat.size();
    }

    vector<string> patterns_;
};

// ---------------------------------------------------------------- 文件操作 ---
static bool readFileBytes(const fs::path& p, vector<unsigned char>& out, string& err)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) { err = "无法打开文件"; return false; }
    f.seekg(0, std::ios::end);
    std::streamoff sz = f.tellg();
    if (sz < 0) { err = "读取文件大小失败"; return false; }
    f.seekg(0, std::ios::beg);
    out.resize((size_t)sz);
    if (sz > 0)
    {
        f.read((char*)out.data(), sz);
        if ((size_t)f.gcount() != out.size()) { err = "读取文件内容不完整"; return false; }
    }
    return true;
}

static bool writeFileBytes(const fs::path& p, const void* data, size_t n, string& err)
{
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) { err = "无法创建文件"; return false; }
    if (n > 0) f.write((const char*)data, (std::streamsize)n);
    if (!f) { err = "写入文件失败"; return false; }
    return true;
}

static bool readText(const fs::path& p, string& out, string& err)
{
    vector<unsigned char> b;
    if (!readFileBytes(p, b, err)) return false;
    out.assign(b.begin(), b.end());
    return true;
}

static bool writeText(const fs::path& p, const string& s, string& err)
{
    return writeFileBytes(p, s.data(), s.size(), err);
}

static bool copyFileWithTime(const fs::path& src, const fs::path& dst, string& err)
{
    std::error_code ec;
    fs::file_time_type t = fs::last_write_time(src, ec);
    if (ec) { err = "读取源文件时间失败"; return false; }
    fs::copy_file(src, dst, fs::copy_options::overwrite_existing, ec);
    if (ec) { err = "复制失败: " + ec.message(); return false; }
    fs::last_write_time(dst, t, ec);
    if (ec) { err = "设置目标文件时间失败: " + ec.message(); return false; }
    return true;
}

static bool copyDirTree(const fs::path& src, const fs::path& dst)
{
    std::error_code ec;
    fs::create_directories(dst, ec);
    if (ec) { addError("创建目录失败: " + dst.u8string()); return false; }
    bool ok = true;
    for (auto it = fs::recursive_directory_iterator(src, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        if (g_cancel.load()) { addError("已中断: " + dst.u8string()); return false; }
        std::error_code ec2;
        if (it->is_directory(ec2))
        {
            fs::path rel = it->path().lexically_relative(src);
            fs::create_directories(dst / rel, ec2);
            if (ec2) { addError("创建目录失败: " + (dst / rel).u8string()); ok = false; }
        }
        else if (it->is_regular_file(ec2))
        {
            fs::path rel = it->path().lexically_relative(src);
            uintmax_t sz = it->file_size(ec2);
            string err;
            if (!copyFileWithTime(it->path(), dst / rel, err))
            {
                addError("复制失败 " + it->path().u8string() + " : " + err);
                ok = false;
            }
            else
            {
                g_doneBytes.fetch_add(ec2 ? 0 : (uint64_t)sz);
                g_doneFiles.fetch_add(1);
            }
        }
        if ((g_doneFiles.load() & 0xFF) == 0)
        {
            checkLowSpace(dst);
            setCurrent(dst.u8string());
        }
    }
    return ok;
}

// ---------------------------------------------------------------- ZIP (miniz) ---
static bool safeEntryName(const string& raw, string& err)
{
    string s = raw;
    for (auto& c : s) if (c == '\\') c = '/';
    while (!s.empty() && s.front() == '/') s.erase(s.begin());
    if (s.empty()) { err = "zip 条目名为空"; return false; }
    if (s.find(':') != string::npos) { err = "zip 条目含盘符，拒绝: " + s; return false; }
    std::stringstream ss(s);
    string comp;
    while (std::getline(ss, comp, '/'))
        if (comp == "..") { err = "zip 条目含 '..'，拒绝: " + s; return false; }
    return true;
}

// 流式把目录打包为 zip（level=1），内存占用与目录大小无关
static bool zipDirToFile(const fs::path& srcDir, const fs::path& zipPath, uint64_t totalBytes, string& err)
{
    mz_zip_archive za;
    std::memset(&za, 0, sizeof(za));
    string zpath = zipPath.u8string();
    if (!mz_zip_writer_init_file(&za, zpath.c_str(), 0))
    { err = "创建 zip 文件失败"; return false; }

    std::error_code ec;
    vector<fs::path> entries;
    for (auto it = fs::recursive_directory_iterator(srcDir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
        entries.push_back(it->path());
    if (ec)
    {
        mz_zip_writer_end(&za);
        std::error_code rmec;
        fs::remove(zipPath, rmec);
        err = "扫描目录失败: " + ec.message();
        return false;
    }
    std::sort(entries.begin(), entries.end());

    bool ok = true;
    string lastErr;
    uint64_t done = 0;
    for (const auto& p : entries)
    {
        if (g_cancel.load()) { ok = false; lastErr = "已中断"; break; }
        string rel = relStr(srcDir, p);
        std::error_code ec2;
        if (fs::is_directory(p, ec2))
        {
            if (!mz_zip_writer_add_mem(&za, (rel + "/").c_str(), "", 0, 0))
            { ok = false; lastErr = "写入目录条目失败: " + rel; break; }
        }
        else if (fs::is_regular_file(p, ec2))
        {
            string sp = p.u8string();
            uintmax_t sz = fs::file_size(p, ec2);
            if (!mz_zip_writer_add_file(&za, rel.c_str(), sp.c_str(), nullptr, 0, MZ_BEST_SPEED))
            { ok = false; lastErr = "写入文件条目失败: " + rel; break; }
            done += ec2 ? 0 : (uint64_t)sz;
            g_doneBytes.fetch_add(ec2 ? 0 : (uint64_t)sz);
            g_doneFiles.fetch_add(1);
            if ((g_doneFiles.load() & 0x7F) == 0)
            {
                checkLowSpace(zipPath.parent_path());
                setCurrent(zipPath.filename().u8string() + " ← " + rel);
            }
        }
    }
    (void)totalBytes; (void)done;
    if (ok && !mz_zip_writer_finalize_archive(&za))
    { ok = false; lastErr = "完成 zip 失败"; }
    mz_zip_writer_end(&za);
    if (!ok)
    {
        std::error_code rmec;
        fs::remove(zipPath, rmec);
        err = lastErr;
        return false;
    }
    return true;
}

// 流式解压 zip 到 dstDir
static bool unzipFileToDir(const fs::path& zipPath, const fs::path& dstDir, string& err)
{
    mz_zip_archive za;
    std::memset(&za, 0, sizeof(za));
    string zpath = zipPath.u8string();
    if (!mz_zip_reader_init_file(&za, zpath.c_str(), 0))
    { err = "打开 zip 失败（文件可能损坏）"; return false; }

    mz_uint n = mz_zip_reader_get_num_files(&za);
    bool ok = true;
    string lastErr;
    for (mz_uint i = 0; i < n; ++i)
    {
        if (g_cancel.load()) { ok = false; lastErr = "已中断"; break; }
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&za, i, &st))
        { ok = false; lastErr = "读取 zip 条目信息失败"; break; }
        string name(st.m_filename);
        if (!safeEntryName(name, lastErr)) { ok = false; break; }

        std::error_code ec;
        fs::path target = dstDir / fs::u8path(name);
        if (st.m_is_directory)
        {
            fs::create_directories(target, ec);
            if (ec) { ok = false; lastErr = "创建目录失败: " + name; break; }
            continue;
        }
        fs::create_directories(target.parent_path(), ec);
        if (ec) { ok = false; lastErr = "创建目录失败: " + target.parent_path().u8string(); break; }

        string tp = target.u8string();
        if (!mz_zip_reader_extract_to_file(&za, i, tp.c_str(), 0))
        { ok = false; lastErr = "解压失败: " + name; break; }
        fs::last_write_time(target, timeTToFileTime(st.m_time), ec);
        g_doneBytes.fetch_add(st.m_uncomp_size);
        g_doneFiles.fetch_add(1);
        if ((g_doneFiles.load() & 0x7F) == 0) setCurrent(target.u8string());
    }
    mz_zip_reader_end(&za);
    if (!ok) { err = lastErr; return false; }
    return true;
}

// ---------------------------------------------------------------- 扫描 ---------
struct Version
{
    int folder = -1;
    fs::path abs;
    bool isDir = false;
    fs::file_time_type t{};
    uint64_t size = 0;
    uint64_t files = 0;
};

// 单元内容哈希：文件直接哈希；目录把“路径+大小+文件哈希”排序后汇总
static bool hashUnit(const Version& v, HashCache* cache, string& out)
{
    if (!v.isDir) return hashFileContents(v.abs, v.size, mtimeKey(v.t), cache, out);

    vector<string> lines;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(v.abs, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        std::error_code ec2;
        if (it->is_symlink(ec2)) continue;
        string rel = relStr(v.abs, it->path());
        if (it->is_directory(ec2))
        {
            lines.push_back("D:" + rel);
        }
        else if (it->is_regular_file(ec2))
        {
            uintmax_t sz = it->file_size(ec2);
            string fh;
            if (!hashFileContents(it->path(), ec2 ? 0 : (uint64_t)sz, mtimeKey(it->last_write_time(ec2)), cache, fh))
                return false;
            lines.push_back("F:" + rel + ":" + std::to_string((uint64_t)sz) + ":" + fh);
        }
    }
    if (ec) return false;
    std::sort(lines.begin(), lines.end());
    H128 h;
    for (const auto& s : lines)
    {
        h128MixData(h, s.data(), s.size());
        h128MixByte(h, '\n');
    }
    out = h128Finish(h, (uint64_t)lines.size());
    return true;
}

struct Unit
{
    vector<Version> versions;
};
using UnitMap = std::map<string, Unit>;

static bool isRootBackup(const string& rel, int depth)
{
    if (depth != 1) return false;
    return rel == kBackupName || rel == kLegacyBackupName;
}

static void scanFolder(int idx, const fs::path& root, int minLevel, const Excludes& excludes,
                       std::map<string, Version>& local, vector<string>& errors)
{
    std::function<void(const fs::path&, int)> walk = [&](const fs::path& dir, int depth)
    {
        if (g_cancel.load()) return;
        std::error_code ec;
        for (auto it = fs::directory_iterator(dir, fs::directory_options::skip_permission_denied, ec);
             !ec && it != fs::directory_iterator(); it.increment(ec))
        {
            if (g_cancel.load()) return;
            std::error_code ec2;
            const fs::directory_entry& e = *it;
            fs::file_status st = e.symlink_status(ec2);
            if (ec2) { errors.push_back("无法访问: " + e.path().u8string()); continue; }
            if (fs::is_symlink(st)) continue; // 不跟随符号链接/目录联接，避免循环

            string rel = relStr(root, e.path());
            notePathLength(e.path());
            if (excludes.match(rel, fs::is_directory(st))) continue;

            if (fs::is_directory(st))
            {
                int d = depth + 1;
                if (isRootBackup(rel, d)) continue;
                if (minLevel >= 0 && d == minLevel + 1)
                {
                    fs::file_time_type t = fs::last_write_time(e.path(), ec2);
                    if (ec2) t = fs::file_time_type::min();
                    auto it2 = local.find(rel);
                    if (it2 == local.end()) local[rel] = Version{ idx, e.path(), true, t, 0, 0 };
                    else if (t > it2->second.t) it2->second.t = t;
                }
                walk(e.path(), d);
            }
            else
            {
                int parentDepth = countComps(rel) - 1;
                string key;
                bool fileUnit = false;
                if (minLevel < 0 || parentDepth <= minLevel) { key = rel; fileUnit = true; }
                else { key = firstNComps(rel, minLevel + 1); }

                fs::file_time_type t = fs::last_write_time(e.path(), ec2);
                if (ec2) { errors.push_back("读取修改时间失败: " + e.path().u8string()); continue; }
                uintmax_t sz = fs::file_size(e.path(), ec2);
                uint64_t size = ec2 ? 0 : (uint64_t)sz;

                auto it2 = local.find(key);
                if (it2 == local.end())
                    local[key] = Version{ idx, fileUnit ? e.path() : (root / fs::u8path(key)), !fileUnit, t, size, 1 };
                else
                {
                    if (t > it2->second.t) it2->second.t = t;
                    it2->second.size += size;
                    it2->second.files += 1;
                }
                g_scanFiles.fetch_add(1);
                g_doneBytes.fetch_add(0);
                if ((g_scanFiles.load() & 0x3FF) == 0) setCurrent(e.path().u8string());
            }
        }
        if (ec) { noteLongPathError(ec); errors.push_back("遍历目录失败: " + dir.u8string() + " : " + ec.message()); }
    };
    walk(root, 0);
}

static bool isStrictlyWithin(const fs::path& p, const fs::path& root)
{
    if (p == root) return false;
    fs::path rel = p.lexically_relative(root);
    if (rel.empty()) return false;
    for (const auto& c : rel)
        if (c == fs::path("..")) return false;
    return true;
}

// ---------------------------------------------------------------- 计划 ---------
struct Op
{
    string key;
    int target = 0;
    bool replace = false;
    bool oldIsDir = false;
    bool winIsDir = false;
    const Version* win = nullptr;
    const Version* old = nullptr;
    uint64_t copyBytes = 0;
    uint64_t backupBytes = 0;
};

struct EqualCandidate
{
    string key;
    vector<const Version*> versions;
};

struct ConflictVersion
{
    int folder = -1;
    const Version* v = nullptr;
    string hash;
};

struct Conflict
{
    string key;
    vector<ConflictVersion> versions;
};

struct Plan
{
    UnitMap units;
    vector<Op> ops;
    vector<EqualCandidate> equalCandidates;
    vector<Conflict> conflicts;
    vector<uint64_t> required; // 每个目标文件夹预计新增占用
    vector<uint64_t> copyPer;
    vector<uint64_t> backupPer;
    uint64_t copyTotal = 0;
    uint64_t backupTotal = 0;
    uint64_t unitCount = 0;
    uint64_t skipped = 0;
    uint64_t conflictCopies = 0;
};

static void buildPlan(Plan& plan, size_t folderCount)
{
    plan.required.assign(folderCount, 0);
    plan.copyPer.assign(folderCount, 0);
    plan.backupPer.assign(folderCount, 0);
    for (auto& kv : plan.units)
    {
        Unit& u = kv.second;
        if (u.versions.empty()) continue;
        ++plan.unitCount;

        const Version* win = nullptr;
        for (auto& v : u.versions)
        {
            if (!win) { win = &v; continue; }
            if (v.t > win->t) win = &v;
            else if (v.t == win->t && v.isDir && !win->isDir) win = &v;
        }

        bool anyOp = false;
        uint64_t equalTargets = 0;
        for (size_t f = 0; f < folderCount; ++f)
        {
            if (win->folder == (int)f) continue;
            const Version* v = nullptr;
            for (auto& x : u.versions) if (x.folder == (int)f) { v = &x; break; }

            bool need = false;
            if (!v) need = true;
            else if (v->isDir != win->isDir) need = true;
            else if (v->t < win->t) need = true;
            if (!need) { if (v) ++equalTargets; continue; }
            anyOp = true;

            Op op;
            op.key = kv.first;
            op.target = (int)f;
            op.replace = (v != nullptr);
            op.oldIsDir = v ? v->isDir : false;
            op.winIsDir = win->isDir;
            op.win = win;
            op.old = v;
            op.copyBytes = win->size;
            op.backupBytes = v ? v->size : 0;
            plan.copyTotal += op.copyBytes;
            plan.backupTotal += op.backupBytes;
            plan.copyPer[f] += op.copyBytes;
            plan.backupPer[f] += op.backupBytes;
            plan.required[f] += op.copyBytes + op.backupBytes;
            plan.ops.push_back(op);
        }

        if (!anyOp && u.versions.size() >= 2)
        {
            EqualCandidate c;
            c.key = kv.first;
            for (auto& v : u.versions) c.versions.push_back(&v);
            plan.equalCandidates.push_back(c);
        }
        else
        {
            plan.skipped += equalTargets;
        }
    }
}

static fs::path hashCachePath(const vector<fs::path>& folders)
{
    std::error_code ec;
    fs::path tmp = fs::temp_directory_path(ec);
    if (ec) return fs::path();
    string key;
    for (auto& f : folders) key += f.u8string() + "|";
    char hx[32];
    std::snprintf(hx, sizeof(hx), "%016llx", (unsigned long long)fnv1a64(key));
    return tmp / "SyncTool" / (string("hashcache-v2-") + hx + ".tsv");
}

static void hashEqualCandidates(Plan& plan, const vector<fs::path>& folders, int jobs, HashCache& cache)
{
    if (plan.equalCandidates.empty()) return;

    g_phase.store(2);
    uint64_t total = 0;
    for (auto& c : plan.equalCandidates) total += c.versions.size();
    g_hashTotal.store(total);
    g_hashDone.store(0);
    setCurrent("比对同时间单元内容");

    vector<std::pair<size_t, size_t>> tasks;
    for (size_t ci = 0; ci < plan.equalCandidates.size(); ++ci)
        for (size_t vi = 0; vi < plan.equalCandidates[ci].versions.size(); ++vi)
            tasks.emplace_back(ci, vi);
    vector<vector<string>> hashes(plan.equalCandidates.size());
    for (size_t i = 0; i < hashes.size(); ++i) hashes[i].assign(plan.equalCandidates[i].versions.size(), string());

    std::atomic<size_t> next{0};
    auto worker = [&]() {
        while (!g_cancel.load())
        {
            size_t i = next.fetch_add(1);
            if (i >= tasks.size()) break;
            size_t ci = tasks[i].first, vi = tasks[i].second;
            const Version& v = *plan.equalCandidates[ci].versions[vi];
            string h;
            if (hashUnit(v, &cache, h)) hashes[ci][vi] = h;
            else addError("计算内容哈希失败: " + v.abs.u8string());
            g_hashDone.fetch_add(1);
            setCurrent(v.abs.u8string());
        }
    };
    vector<std::thread> pool;
    int n = std::max(1, std::min<int>(jobs, (int)tasks.size()));
    for (int i = 0; i < n; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
    cache.save();
    if (g_cancel.load()) return;

    for (size_t ci = 0; ci < plan.equalCandidates.size(); ++ci)
    {
        auto& cand = plan.equalCandidates[ci];
        std::set<string> distinct;
        bool bad = false;
        for (auto& h : hashes[ci])
        {
            if (h.empty()) { bad = true; break; }
            distinct.insert(h);
        }
        if (bad || distinct.size() <= 1)
        {
            plan.skipped += cand.versions.size();
            continue;
        }
        Conflict c;
        c.key = cand.key;
        for (size_t vi = 0; vi < cand.versions.size(); ++vi)
        {
            ConflictVersion cv;
            cv.folder = cand.versions[vi]->folder;
            cv.v = cand.versions[vi];
            cv.hash = hashes[ci][vi];
            c.versions.push_back(cv);
        }
        plan.conflicts.push_back(c);
    }
}

static string buildPlanJson(const Plan& plan, const vector<fs::path>& folders, int minLevel, int jobs)
{
    std::ostringstream os;
    os << "{\n";
    os << "  \"tool\": \"SyncTool\",\n  \"version\": \"" << kVersion << "\",\n";
    os << "  \"created\": \"" << makeTimestamp() << "\",\n";
    os << "  \"minLevel\": " << minLevel << ",\n  \"jobs\": " << jobs << ",\n";
    os << "  \"folders\": [";
    for (size_t i = 0; i < folders.size(); ++i)
        os << (i ? ", " : "") << "\"" << jsonEscape(folders[i].u8string()) << "\"";
    os << "],\n";
    os << "  \"summary\": {\"units\": " << plan.unitCount
       << ", \"operations\": " << plan.ops.size()
       << ", \"copyBytes\": " << plan.copyTotal
       << ", \"backupBytes\": " << plan.backupTotal
       << ", \"skippedEqual\": " << plan.skipped << "},\n";
    os << "  \"operations\": [\n";
    for (size_t i = 0; i < plan.ops.size(); ++i)
    {
        const Op& op = plan.ops[i];
        os << "    {\"key\": \"" << jsonEscape(op.key)
           << "\", \"target\": \"" << jsonEscape(folders[op.target].u8string())
           << "\", \"action\": \"" << (op.replace ? "replace" : "add")
           << "\", \"oldIsDir\": " << (op.oldIsDir ? "true" : "false")
           << ", \"newIsDir\": " << (op.winIsDir ? "true" : "false")
           << ", \"winner\": \"" << jsonEscape(folders[op.win->folder].u8string())
           << "\", \"copyBytes\": " << op.copyBytes
           << ", \"backupBytes\": " << op.backupBytes << "}";
        os << (i + 1 == plan.ops.size() ? "\n" : ",\n");
    }
    os << "  ]\n}\n";
    return os.str();
}

// ---------------------------------------------------------------- 执行 ---------
struct JournalEntry
{
    uint64_t seq = 0;
    string key;
    int target = -1;
    string action;   // add / replace
    bool oldIsDir = false;
    bool winIsDir = false;
    string backupRel; // 相对 backupRoot 的备份路径；add 为空
    string state;     // started / backedUp / done / failed
};

struct ApplyContext
{
    const vector<fs::path>* roots = nullptr;
    string runTs;
    vector<fs::path> backupRoots;
    vector<vector<string>> manifests;
    std::mutex backupDirMutex;
    vector<bool> backupDirReady;
    std::mutex rootMutex;
    vector<std::ofstream> journals;
    vector<vector<JournalEntry>> journalEntries;
    std::mutex journalMutex;
    std::atomic<uint64_t> seq{0};
    HashCache* hashCache = nullptr;
};

static bool ensureRunBackupRoot(ApplyContext& ctx, int f, string& err)
{
    (void)err;
    std::lock_guard<std::mutex> lk(ctx.rootMutex);
    if (!ctx.backupRoots[f].empty()) return true;
    std::error_code ec;
    fs::path base = (*ctx.roots)[f] / kBackupName;
    fs::path p = base / fs::u8path(ctx.runTs);
    int k = 2;
    while (fs::exists(p, ec)) p = base / fs::u8path(ctx.runTs + "-" + std::to_string(k++));
    ctx.backupRoots[f] = p;
    return true;
}

static bool ensureBackupDir(ApplyContext& ctx, int f, string& err)
{
    if (!ensureRunBackupRoot(ctx, f, err)) return false;
    std::lock_guard<std::mutex> lk(ctx.backupDirMutex);
    if (ctx.backupDirReady[f]) return true;
    std::error_code ec;
    fs::create_directories(ctx.backupRoots[f], ec);
    if (ec) { err = "无法创建备份目录: " + ec.message(); return false; }
    fs::path base = ctx.backupRoots[f].parent_path();
    setHidden(base);
    setHidden(ctx.backupRoots[f]);
    ctx.backupDirReady[f] = true;
    return true;
}

static void journalAppend(ApplyContext& ctx, const JournalEntry& e)
{
    if (e.target < 0) return;
    string rerr;
    if (!ensureRunBackupRoot(ctx, e.target, rerr)) return; // 失败则只保留内存记录
    std::lock_guard<std::mutex> lk(ctx.journalMutex);
    if (ctx.journals.size() <= (size_t)e.target)
    {
        ctx.journals.resize(ctx.backupRoots.size());
        ctx.journalEntries.resize(ctx.backupRoots.size());
    }
    if (!ctx.journals[e.target].is_open())
    {
        std::error_code ec;
        fs::create_directories(ctx.backupRoots[e.target], ec);
        ctx.journals[e.target].open(ctx.backupRoots[e.target] / kJournalName,
                                    std::ios::binary | std::ios::app);
    }
    std::ostringstream os;
    os << "{\"seq\":" << e.seq
       << ",\"key\":\"" << jsonEscape(e.key)
       << "\",\"target\":" << e.target
       << ",\"action\":\"" << e.action
       << "\",\"oldIsDir\":" << (e.oldIsDir ? "true" : "false")
       << ",\"winIsDir\":" << (e.winIsDir ? "true" : "false")
       << ",\"backup\":\"" << jsonEscape(e.backupRel)
       << "\",\"state\":\"" << e.state << "\"}";
    if (ctx.journals[e.target].is_open())
    {
        ctx.journals[e.target] << os.str() << "\n";
    }
    ctx.journalEntries[e.target].push_back(e);
}

static bool backupOld(const Op& op, ApplyContext& ctx, string& err)
{
    if (!op.old) return true;
    if (!ensureBackupDir(ctx, op.target, err)) return false;
    std::error_code ec;
    if (op.old->isDir)
    {
        fs::path zip = ctx.backupRoots[op.target] / fs::u8path(op.key + ".zip");
        fs::create_directories(zip.parent_path(), ec);
        if (!zipDirToFile(op.old->abs, zip, op.old->size, err)) return false;
        std::lock_guard<std::mutex> lk(g_manifestMutex);
        ctx.manifests[op.target].push_back(op.key + ".zip");
    }
    else
    {
        fs::path dst = ctx.backupRoots[op.target] / fs::u8path(op.key);
        fs::create_directories(dst.parent_path(), ec);
        if (!copyFileWithTime(op.old->abs, dst, err)) return false;
        g_doneBytes.fetch_add(op.old->size);
        g_doneFiles.fetch_add(1);
    }
    return true;
}

static void applyOp(const Op& op, ApplyContext& ctx)
{
    string err;
    std::error_code ec;
    checkLowSpace((*ctx.roots)[op.target]);
    if (g_cancel.load()) { g_doneOps.fetch_add(1); return; }

    JournalEntry je;
    je.seq = ctx.seq.fetch_add(1);
    je.key = op.key;
    je.target = op.target;
    je.action = op.replace ? "replace" : "add";
    je.oldIsDir = op.oldIsDir;
    je.winIsDir = op.winIsDir;
    je.backupRel = op.replace ? (op.oldIsDir ? op.key + ".zip" : op.key) : "";
    je.state = "started";
    // 只写最终状态，避免每个单元写 2-3 行、在大同步里产生几十万行 journal
    if (op.replace)
    {
        if (!backupOld(op, ctx, err))
        {
            addError("备份失败（跳过该单元）: " + op.key + " : " + err);
            je.state = "failed";
            journalAppend(ctx, je);
            g_doneOps.fetch_add(1);
            return;
        }

        fs::path oldPath = op.old->abs;
        if (op.oldIsDir) fs::remove_all(oldPath, ec);
        else fs::remove(oldPath, ec);
        if (ec)
        {
            addError("删除旧版本失败: " + oldPath.u8string() + " : " + ec.message());
            je.state = "backedUp";
            journalAppend(ctx, je);
            g_doneOps.fetch_add(1);
            return;
        }
    }

    fs::path dst = (*ctx.roots)[op.target] / fs::u8path(op.key);
    fs::create_directories(dst.parent_path(), ec);
    bool copyOk = true;
    if (op.winIsDir)
    {
        if (fs::exists(dst, ec)) fs::remove_all(dst, ec);
        if (!copyDirTree(op.win->abs, dst))
        {
            addError("复制目录失败: " + op.key);
            copyOk = false;
        }
        else
        {
            // 目录单元的根目录也要保留修改时间，否则空目录会在下次同步时被误判为更新
            std::error_code tec;
            fs::file_time_type wt = fs::last_write_time(op.win->abs, tec);
            if (!tec) fs::last_write_time(dst, wt, tec);
        }
    }
    else
    {
        string cerr;
        if (!copyFileWithTime(op.win->abs, dst, cerr))
        {
            addError("复制失败: " + op.key + " : " + cerr);
            copyOk = false;
        }
        else
        {
            g_doneBytes.fetch_add(op.win->size);
            g_doneFiles.fetch_add(1);
        }
    }
    if (!copyOk)
    {
        // 替换失败时旧版本已备份，标记为 backedUp 以便回滚；新增失败标记 failed
        je.state = op.replace ? "backedUp" : "failed";
        journalAppend(ctx, je);
        g_doneOps.fetch_add(1);
        return;
    }
    if (ctx.hashCache)
    {
        if (op.winIsDir) ctx.hashCache->eraseTree(dst);
        else ctx.hashCache->erase(dst);
    }
    je.state = "done";
    journalAppend(ctx, je);
    g_doneOps.fetch_add(1);
}

static void runWorkers(ApplyContext& ctx, const Plan& plan, int jobs)
{
    std::atomic<size_t> next{0};
    auto worker = [&]() {
        while (!g_cancel.load())
        {
            size_t i = next.fetch_add(1);
            if (i >= plan.ops.size()) break;
            applyOp(plan.ops[i], ctx);
        }
    };
    vector<std::thread> pool;
    int n = std::max(1, std::min<int>(jobs, (int)plan.ops.size()));
    for (int i = 0; i < n; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
}

// 同时间不同内容：在独立的解析线程里逐个询问；工作线程继续复制普通单元
static void resolveConflicts(const Plan& plan, const vector<fs::path>& folders, ApplyContext& ctx)
{
    if (plan.conflicts.empty()) return;
    if (g_yes)
    {
        logLine("[冲突] 检测到 " + std::to_string(plan.conflicts.size()) + " 个同时间不同内容的单元，--yes 模式全部跳过。");
        return;
    }

    logLine("\n=== 同时间内容冲突 ===\n将逐个询问使用哪个文件夹的版本作为源；其他线程会继续复制。");
    for (size_t i = 0; i < plan.conflicts.size(); ++i)
    {
        if (g_cancel.load()) break;
        const Conflict& c = plan.conflicts[i];
        std::ostringstream os;
        os << "\n[冲突 " << (i + 1) << "/" << plan.conflicts.size() << "] " << c.key
           << "  （修改时间相同，内容不同）\n";
        for (size_t j = 0; j < c.versions.size(); ++j)
        {
            const ConflictVersion& cv = c.versions[j];
            os << "  " << (j + 1) << ") " << folders[cv.folder].u8string()
               << "  " << formatBytes(cv.v->size)
               << "  hash " << cv.hash.substr(0, 12) << "\n";
        }
        os << "  0) 跳过（保持各自现状）\n";

        g_promptActive.store(true);
        {
            std::lock_guard<std::mutex> lk(g_logMutex);
            clearProgressLine();
            std::cout << os.str();
            std::cout.flush();
        }

        int pick = -1;
        while (pick < 0)
        {
            string ans = askLine("请选择作为源的序号（1-" + std::to_string(c.versions.size()) + "，0=跳过）: ");
            if (ans.empty()) { pick = 0; break; }
            bool digits = std::all_of(ans.begin(), ans.end(), [](char ch) { return std::isdigit((unsigned char)ch); });
            if (!digits) { std::cout << "  输入无效，请重新输入。\n"; continue; }
            int n = std::atoi(ans.c_str());
            if (n == 0) { pick = 0; break; }
            if (n >= 1 && n <= (int)c.versions.size()) { pick = n; break; }
            std::cout << "  序号超出范围，请重新输入。\n";
        }
        if (pick == 0)
        {
            logLine("  已跳过: " + c.key);
            continue;
        }

        const ConflictVersion& src = c.versions[pick - 1];
        for (const auto& cv : c.versions)
        {
            if (cv.folder == src.folder || cv.hash == src.hash) continue;
            if (g_cancel.load()) break;
            Op op;
            op.key = c.key;
            op.target = cv.folder;
            op.replace = true;
            op.oldIsDir = cv.v->isDir;
            op.winIsDir = src.v->isDir;
            op.win = src.v;
            op.old = cv.v;
            op.copyBytes = src.v->size;
            op.backupBytes = cv.v->size;
            g_totalOps.fetch_add(1);
            g_totalBytes.fetch_add(op.copyBytes + op.backupBytes);
            logLine("  使用 " + folders[src.folder].u8string() + " 作为源，更新 " + folders[cv.folder].u8string() + " : " + c.key);
            applyOp(op, ctx);
        }
    }
}

static int rollbackRun(ApplyContext& ctx, const vector<fs::path>& folders)
{
    // 回滚期间忽略取消标志，否则 Ctrl+C 会让解压/复制刚开头就退出
    bool prevCancel = g_cancel.exchange(false);
    int restored = 0, removed = 0, errors = 0;
    for (size_t f = 0; f < folders.size(); ++f)
    {
        if (f >= ctx.backupRoots.size() || ctx.backupRoots[f].empty()) continue;
        auto& entries = ctx.journalEntries[f];
        std::map<uint64_t, JournalEntry> latest;
        for (const auto& e : entries) latest[e.seq] = e;
        for (auto rit = latest.rbegin(); rit != latest.rend(); ++rit)
        {
            const JournalEntry& e = rit->second;
            if (e.state == "started" && e.action == "replace") continue;
            if (e.state == "failed" && e.action == "replace") continue;

            fs::path target = folders[f] / fs::u8path(e.key);
            std::error_code ec;
            if (e.action == "add")
            {
                if (fs::exists(target, ec))
                {
                    fs::remove_all(target, ec);
                    if (ec) { logLine("[回滚] 删除失败: " + target.u8string() + " : " + ec.message()); ++errors; }
                    else ++removed;
                }
                if (ctx.hashCache)
                {
                    if (e.winIsDir) ctx.hashCache->eraseTree(target);
                    else ctx.hashCache->erase(target);
                }
                continue;
            }

            // replace: 先删除当前，再从备份恢复旧版本
            if (fs::exists(target, ec)) fs::remove_all(target, ec);
            if (ec) { logLine("[回滚] 删除失败: " + target.u8string() + " : " + ec.message()); ++errors; continue; }
            bool ok = false;
            if (e.oldIsDir)
            {
                string zerr;
                fs::create_directories(target, ec);
                ok = unzipFileToDir(ctx.backupRoots[f] / fs::u8path(e.key + ".zip"), target, zerr);
                if (!ok) logLine("[回滚] 解压失败: " + e.key + " : " + zerr);
            }
            else
            {
                string cerr;
                fs::create_directories(target.parent_path(), ec);
                ok = copyFileWithTime(ctx.backupRoots[f] / fs::u8path(e.key), target, cerr);
                if (!ok) logLine("[回滚] 恢复失败: " + e.key + " : " + cerr);
            }
            if (ok) ++restored;
            else ++errors;
            if (ctx.hashCache)
            {
                if (e.oldIsDir) ctx.hashCache->eraseTree(target);
                else ctx.hashCache->erase(target);
            }
        }
    }
    logLine("[回滚] 恢复 " + std::to_string(restored) + " 项，删除新增 " + std::to_string(removed) + " 项，错误 " + std::to_string(errors) + " 项。");
    g_cancel.store(prevCancel);
    return errors;
}

// ---------------------------------------------------------------- 同步入口 ---
static int runSync(const vector<string>& args)
{
    vector<fs::path> folderArgs;
    int minLevel = -1;
    int jobs = (int)std::max(2u, std::min(8u, std::thread::hardware_concurrency()));
    bool dryRun = false;
    bool force = false;
    Excludes excludes;

    for (size_t i = 0; i < args.size(); ++i)
    {
        const string& a = args[i];
        auto needValue = [&](const char* name) -> string {
            if (i + 1 >= args.size()) { std::cout << "[错误] " << name << " 需要一个参数\n"; std::exit(1); }
            return args[++i];
        };
        if (a == "--min-level") minLevel = std::atoi(needValue("--min-level").c_str());
        else if (a.rfind("--min-level=", 0) == 0) minLevel = std::atoi(a.substr(12).c_str());
        else if (a == "--jobs") jobs = std::atoi(needValue("--jobs").c_str());
        else if (a.rfind("--jobs=", 0) == 0) jobs = std::atoi(a.substr(7).c_str());
        else if (a == "--exclude") excludes.add(needValue("--exclude"));
        else if (a.rfind("--exclude=", 0) == 0) excludes.add(a.substr(10));
        else if (a == "--dry-run") dryRun = true;
        else if (a == "--force") force = true;
        else if (a == "--yes" || a == "-y") g_yes = true;
        else if (a == "--long-paths-enabled") g_skipLongPathCheck = true;
        else if (a == "--no-long-paths") g_noLongPaths = true;
        else if (a == "--no-hash-cache") g_noHashCache = true;
        else if (a.rfind("--", 0) == 0) { std::cout << "[错误] 未知参数: " << a << "\n"; return 1; }
        else folderArgs.push_back(fs::path(a));
    }
    if (minLevel < -1) { std::cout << "[错误] --min-level 必须 >= 0\n"; return 1; }
    jobs = std::max(1, std::min(jobs, 32));
    if (folderArgs.size() < 2) { std::cout << "[错误] sync 至少需要 2 个文件夹\n"; return 1; }
    if (g_noHashCache) clearHashCaches();

    // 解析并校验文件夹
    vector<fs::path> folders;
    std::error_code ec;
    for (const auto& fa : folderArgs)
    {
        fs::path abs = fs::absolute(fa, ec);
        if (ec) { std::cout << "[错误] 无法解析路径: " << fa.u8string() << "\n"; return 1; }
        fs::path can = fs::weakly_canonical(abs, ec);
        if (ec || !fs::is_directory(can, ec))
        { std::cout << "[错误] 文件夹不存在或不可访问: " << fa.u8string() << "\n"; return 1; }
        folders.push_back(can);
    }
    for (size_t i = 0; i < folders.size(); ++i)
        for (size_t j = i + 1; j < folders.size(); ++j)
        {
            if (folders[i] == folders[j])
            { std::cout << "[错误] 文件夹重复: " << folders[i].u8string() << "\n"; return 1; }
            if (isStrictlyWithin(folders[i], folders[j]) || isStrictlyWithin(folders[j], folders[i]))
            { std::cout << "[错误] 文件夹不能互相包含: " << folders[i].u8string() << " 与 " << folders[j].u8string() << "\n"; return 1; }
        }

    // 长路径支持：未开启时询问并自动开启；开启后重启自身以让 manifest 生效
    if (!dryRun)
    {
        int lpExit = 0;
        if (!ensureLongPaths(lpExit)) return lpExit;
    }
#if defined(_WIN32)
    else if (!longPathsEnabled())
    {
        logLine("[长路径] 当前未开启 LongPathsEnabled；预览阶段不会修改注册表，超长路径可能读取失败。");
    }
#endif

    // 兼容 v1：把旧的 backup 目录迁移为 .backup 并隐藏（dry-run 只提示）
    for (auto& f : folders)
    {
        std::error_code mec;
        fs::path legacy = f / kLegacyBackupName;
        fs::path modern = f / kBackupName;
        if (fs::is_directory(modern, mec)) continue;
        if (!fs::is_directory(legacy, mec)) continue;
        if (dryRun)
        {
            std::cout << "[预览] 将把旧备份目录迁移为隐藏目录: " << legacy.u8string() << " -> " << modern.u8string() << "\n";
            continue;
        }
        std::error_code rec;
        fs::rename(legacy, modern, rec);
        if (rec) std::cout << "[警告] 旧备份目录迁移失败: " << legacy.u8string() << " : " << rec.message() << "\n";
        else
        {
            setHidden(modern);
            std::cout << "[迁移] " << legacy.u8string() << " -> " << modern.u8string() << "（已隐藏）\n";
        }
    }

    // 临时计划文件（独占，防并发、防清理）
    string hashInput;
    for (auto& f : folders) hashInput += f.u8string() + "|";
    char hex[32];
    std::snprintf(hex, sizeof(hex), "%016llx", (unsigned long long)fnv1a64(hashInput));
    TempPlan tempPlan;
    string terr;
    if (!tempPlan.open(hex, terr)) { std::cout << "[错误] " << terr << "\n"; return 1; }

    // 占用根文件夹，提示不要修改
    vector<FolderLock*> locks;
    bool lockDisabled = std::getenv("SYNCTOOL_NO_FOLDER_LOCK") != nullptr;
    std::cout << "\n⚠ 同步期间请勿修改被同步的文件夹，否则可能造成数据损坏。\n";
    if (!dryRun && !lockDisabled)
    {
        for (auto& f : folders)
        {
            FolderLock* lk = new FolderLock();
            string lerr;
            if (lk->acquire(f, lerr)) { locks.push_back(lk); }
            else { delete lk; std::cout << "[警告] " << lerr << "（继续，但请勿手动修改）\n"; }
        }
        if (!locks.empty()) std::cout << "[保护] 已占用 " << locks.size() << " 个根文件夹（共享读/独占写）\n";
    }

    std::cout << "开始同步 " << folders.size() << " 个文件夹";
    if (minLevel < 0) std::cout << "（逐文件比较，深度无限）";
    else std::cout << "（最小保留层数 " << minLevel << "，深度 " << (minLevel + 1) << " 的目录为整体单元）";
    std::cout << (dryRun ? "（预览模式）\n" : "\n");
    std::cout << "临时计划: " << tempPlan.path().u8string() << "\n";

    ProgressUI ui;
    g_phase.store(0);
    ui.start();

    // 阶段一：并行扫描
    vector<vector<string>> scanErrors(folders.size());
    vector<std::map<string, Version>> locals(folders.size());
    vector<std::thread> scanners;
    for (size_t i = 0; i < folders.size(); ++i)
        scanners.emplace_back([&, i] { scanFolder((int)i, folders[i], minLevel, excludes, locals[i], scanErrors[i]); });
    for (auto& t : scanners) t.join();

    vector<string> errors;
    for (auto& v : scanErrors)
        for (auto& e : v)
            errors.push_back(e);
    for (auto& e : errors) addError("扫描: " + e);

    if (g_cancel.load())
    {
        ui.stop();
        std::cout << "\n[中断] 已取消（未修改任何文件）\n";
        for (auto* l : locks) { l->release(); delete l; }
        return 130;
    }

    UnitMap units;
    for (auto& loc : locals)
        for (auto& kv : loc)
            units[kv.first].versions.push_back(kv.second);

    g_phase.store(1);
    setCurrent("统计操作与磁盘空间");
    Plan plan;
    plan.units = std::move(units);
    buildPlan(plan, folders.size());

    string planJson = buildPlanJson(plan, folders, minLevel, jobs);
    string perr;
    if (!tempPlan.write(planJson, perr)) addError(perr);

    // 预检磁盘空间（按卷聚合，C: 上 TEMP 与 Documents\Code 共用同一块盘）
    std::cout << "\n=== 预检 ===\n";
    std::cout << "单元 " << formatNum(plan.unitCount) << " 个，待执行操作 " << formatNum(plan.ops.size()) << " 个";
    if (!plan.equalCandidates.empty())
        std::cout << "，待比对同时间单元 " << formatNum(plan.equalCandidates.size()) << " 个";
    std::cout << "\n";
    std::cout << "预计复制 " << formatBytes(plan.copyTotal) << "，备份 " << formatBytes(plan.backupTotal)
              << "（备份为未压缩上限估算）\n";
    bool spaceFail = false;
    std::map<string, uint64_t> volCopy, volBackup, volFree;
    for (size_t i = 0; i < folders.size(); ++i)
    {
        uint64_t avail = 0;
        bool ok = freeSpaceOf(folders[i], avail);
        std::cout << "  " << folders[i].u8string() << "\n"
                  << "    复制 " << formatBytes(plan.copyPer[i])
                  << "，备份 " << formatBytes(plan.backupPer[i])
                  << "，合计 " << formatBytes(plan.required[i]);
        if (ok) std::cout << "，可用 " << formatBytes(avail) << "\n";
        else std::cout << "，[警告] 无法读取可用空间\n";

        std::string vol;
#if defined(_WIN32)
        vol = toLower(folders[i].root_name().u8string());
#else
        vol = folders[i].root_path().u8string();
#endif
        volCopy[vol] += plan.copyPer[i];
        volBackup[vol] += plan.backupPer[i];
        if (ok) volFree[vol] = avail;
    }
    for (auto& kv : volCopy)
    {
        auto it = volFree.find(kv.first);
        if (it == volFree.end()) continue;
        uint64_t backups = volBackup[kv.first];
        uint64_t estBackup = backups * 85 / 100; // zip 快速压缩通常能到 50%-80%，按 85% 保守估算
        uint64_t need = kv.second + estBackup + (1024ull << 20); // 1GB 余量
        std::cout << "  [卷 " << kv.first << "] 复制 " << formatBytes(kv.second)
                  << " + 备份估算 " << formatBytes(estBackup)
                  << " + 余量 1GB = " << formatBytes(need)
                  << "，可用 " << formatBytes(it->second);
        if (need > it->second) { std::cout << "  ← 空间不足！\n"; spaceFail = true; }
        else std::cout << "\n";
    }
    if (spaceFail && !dryRun && !force)
    {
        ui.stop();
        std::cout << "\n[中止] 目标磁盘空间不足，继续同步会写满磁盘并产生半截文件。\n"
                  << "       请清理空间，或用 --exclude 排除缓存/构建目录，或加 --force 强制继续。\n";
        for (auto* l : locks) { l->release(); delete l; }
        return 2;
    }

    // 修改时间相同的单元：比对内容哈希；不同则进入冲突队列
    HashCache hashCache;
    fs::path hcp = hashCachePath(folders);
    if (!hcp.empty() && !g_noHashCache) hashCache.open(hcp);
    if (!plan.equalCandidates.empty() && !g_cancel.load())
    {
        std::cout << "\n=== 同时间内容比对 ===\n";
        hashEqualCandidates(plan, folders, jobs, hashCache);
        if (g_cancel.load())
        {
            ui.stop();
            std::cout << "\n[中断] 已取消（未修改任何文件）\n";
            for (auto* l : locks) { l->release(); delete l; }
            return 130;
        }
        std::cout << "同时间内容一致跳过 " << formatNum(plan.skipped)
                  << " 个版本，内容冲突 " << formatNum(plan.conflicts.size()) << " 个单元\n";
    }

    if (dryRun)
    {
        ui.stop();
        if (!plan.conflicts.empty())
            std::cout << "\n[预览] 有 " << plan.conflicts.size()
                      << " 个同时间不同内容的单元；实际同步时会逐个询问使用哪个为源。\n";
        std::cout << "\n[预览] 不修改任何文件。\n";
        for (auto* l : locks) { l->release(); delete l; }
        return 0;
    }
    if (plan.ops.empty() && plan.conflicts.empty())
    {
        ui.stop();
        std::cout << "\n[完成] 所有单元已一致，无需同步。\n";
        for (auto* l : locks) { l->release(); delete l; }
        return 0;
    }

    ApplyContext ctx;
    ctx.roots = &folders;
    ctx.runTs = makeTimestamp();
    ctx.hashCache = &hashCache;
    ctx.backupRoots.resize(folders.size());
    ctx.manifests.resize(folders.size());
    ctx.backupDirReady.assign(folders.size(), false);
    ctx.journals.resize(folders.size());
    ctx.journalEntries.resize(folders.size());

    g_phase.store(3);
    setCurrent("");
    g_totalOps.store(plan.ops.size());
    g_totalBytes.store(plan.copyTotal + plan.backupTotal);
    std::cout << "\n=== 执行 ===\n";
    std::cout << "并行线程 " << jobs << "，普通操作 " << formatNum(plan.ops.size()) << " 项";
    if (!plan.conflicts.empty())
        std::cout << "，同时间冲突待询问 " << formatNum(plan.conflicts.size()) << " 个单元";
    std::cout << "\n";

    std::thread resolver([&] { resolveConflicts(plan, folders, ctx); });
    runWorkers(ctx, plan, jobs);
    resolver.join();

    // 写 manifest（即使中断，也把已完成的目录备份登记下来）
    for (size_t i = 0; i < folders.size(); ++i)
    {
        if (ctx.manifests[i].empty()) continue;
        string content;
        for (auto& line : ctx.manifests[i]) content += line + "\n";
        string merr;
        if (!writeText(ctx.backupRoots[i] / "manifest.txt", content, merr))
            addError("写 manifest 失败: " + merr);
    }

    bool canceled = g_cancel.load();
    ui.stop();
    g_phase.store(4);
    std::cout << "\n=== 同步" << (canceled ? "已中断" : "完成") << " ===\n";
    std::cout << "操作 " << formatNum(g_doneOps.load()) << "/" << formatNum(g_totalOps.load())
              << "，处理文件 " << formatNum(g_doneFiles.load())
              << "，数据 " << formatBytes(g_doneBytes.load()) << "\n";
    if (!g_errors.empty()) std::cout << "错误 " << g_errors.size() << " 条（见上方日志）\n";

    if (canceled)
    {
        std::cout << "\n[中断] 已完成 " << formatNum(g_doneOps.load()) << "/" << formatNum(g_totalOps.load()) << " 项。\n";
        bool keep = askYesNo("保留本次备份结果以便下次继续？", true);
        if (!keep)
        {
            bool doRollback = askYesNo("是否还原本次已完成的更改？", true);
            if (doRollback)
            {
                int rberr = rollbackRun(ctx, folders);
                if (rberr == 0)
                {
                    std::lock_guard<std::mutex> lk(ctx.journalMutex);
                    for (auto& j : ctx.journals) if (j.is_open()) j.close();
                    std::error_code rec;
                    for (auto& b : ctx.backupRoots)
                        if (!b.empty()) fs::remove_all(b, rec);
                    std::cout << "[回滚] 本次备份目录已清除。\n";
                }
                else
                {
                    std::cout << "[回滚] 有 " << rberr << " 项失败，保留本次备份目录以便手动处理。\n";
                }
            }
            else
            {
                bool del = askYesNo("保留当前状态，并删除本次备份结果？（旧版本将不可恢复）", false);
                if (del)
                {
                    std::lock_guard<std::mutex> lk(ctx.journalMutex);
                    for (auto& j : ctx.journals) if (j.is_open()) j.close();
                    std::error_code rec;
                    for (auto& b : ctx.backupRoots)
                        if (!b.empty()) fs::remove_all(b, rec);
                    std::cout << "[清理] 本次备份目录已删除。\n";
                }
            }
        }
        else
        {
            std::cout << "[保留] 下次运行会自动跳过已完成的单元，继续处理未完成的部分。\n";
        }
    }
    else
    {
        // 正常完成：关闭并删除运行日志，仅保留 manifest 和备份内容
        std::lock_guard<std::mutex> lk(ctx.journalMutex);
        for (size_t i = 0; i < ctx.journals.size(); ++i)
        {
            if (ctx.journals[i].is_open()) ctx.journals[i].close();
            if (i < ctx.backupRoots.size() && !ctx.backupRoots[i].empty())
            {
                std::error_code rec;
                fs::remove(ctx.backupRoots[i] / kJournalName, rec);
                // 只有 journal、没有实际备份时，删掉空时间点目录，避免留下空 .backup/<ts>
                std::error_code rec2;
                fs::remove(ctx.backupRoots[i], rec2);
            }
        }
    }

    for (size_t i = 0; i < folders.size(); ++i)
        if (!ctx.backupRoots[i].empty())
        {
            std::error_code bec;
            if (fs::exists(ctx.backupRoots[i], bec))
                std::cout << "备份目录: " << ctx.backupRoots[i].u8string() << "\n";
        }

    for (auto* l : locks) { l->release(); delete l; }
    return canceled ? 130 : (g_errors.empty() ? 0 : 1);
}

// ---------------------------------------------------------------- 还原 ---------
static vector<string> listTimeDirs(const fs::path& backupDir, std::error_code& ec)
{
    vector<string> times;
    for (auto it = fs::directory_iterator(backupDir, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        std::error_code ec2;
        if (it->is_directory(ec2)) times.push_back(it->path().filename().u8string());
    }
    std::sort(times.begin(), times.end(), std::greater<string>());
    return times;
}

static fs::path findBackupRoot(const fs::path& folder)
{
    std::error_code ec;
    fs::path b = folder / kBackupName;
    if (fs::is_directory(b, ec)) return b;
    b = folder / kLegacyBackupName;
    if (fs::is_directory(b, ec)) return b;
    return fs::path();
}

static bool isSafeRel(const string& rel)
{
    if (rel.empty()) return false;
    if (rel.find(':') != string::npos) return false;
    std::stringstream ss(rel);
    string comp;
    while (std::getline(ss, comp, '/'))
        if (comp == "..") return false;
    return true;
}

static void clearHashCaches()
{
    std::error_code ec;
    fs::path dir = fs::temp_directory_path(ec) / "SyncTool";
    if (ec) return;
    for (auto it = fs::directory_iterator(dir, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec))
    {
        std::error_code ec2;
        if (!it->is_regular_file(ec2)) continue;
        string name = it->path().filename().u8string();
        if (name.rfind("hashcache-", 0) == 0)
        {
            std::error_code rec;
            fs::remove(it->path(), rec);
        }
    }
}

static int runRestore(const vector<string>& args)
{
    string folderArg, timeArg;
    bool dryRun = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const string& a = args[i];
        if (a == "--time") { if (i + 1 >= args.size()) { std::cout << "[错误] --time 需要参数\n"; return 1; } timeArg = args[++i]; }
        else if (a.rfind("--time=", 0) == 0) timeArg = a.substr(7);
        else if (a == "--dry-run") dryRun = true;
        else if (a.rfind("--", 0) == 0) { std::cout << "[错误] 未知参数: " << a << "\n"; return 1; }
        else if (folderArg.empty()) folderArg = a;
        else { std::cout << "[错误] 还原只能选择一个文件夹: " << a << "\n"; return 1; }
    }
    if (folderArg.empty()) { std::cout << "[错误] 用法: SyncTool restore <文件夹> [--time <时间戳>]\n"; return 1; }

    std::error_code ec;
    fs::path folder = fs::weakly_canonical(fs::absolute(fs::path(folderArg), ec), ec);
    if (ec || !fs::is_directory(folder, ec)) { std::cout << "[错误] 文件夹不存在: " << folderArg << "\n"; return 1; }

    fs::path backupDir = findBackupRoot(folder);
    if (backupDir.empty()) { std::cout << "[提示] " << folder.u8string() << " 下没有 .backup/backup 目录\n"; return 1; }

    vector<string> times = listTimeDirs(backupDir, ec);
    if (times.empty()) { std::cout << "[提示] 备份目录下没有任何时间点\n"; return 1; }

    string ts = trim(timeArg);
    if (ts.empty())
    {
        std::cout << "可还原的时间点（新 -> 旧）：\n";
        for (size_t i = 0; i < times.size(); ++i) std::cout << "  " << (i + 1) << ") " << times[i] << "\n";
        std::cout << "请输入序号（1-" << times.size() << "，直接回车 = 最新）: ";
        string line;
        std::getline(std::cin, line);
        if (!g_tty) std::cout << "\n";
        line = trim(line);
        if (line.empty()) ts = times[0];
        else
        {
            bool allDigit = !line.empty() && std::all_of(line.begin(), line.end(), [](char c) { return std::isdigit((unsigned char)c); });
            if (allDigit)
            {
                int idx = std::atoi(line.c_str());
                if (idx < 1 || (size_t)idx > times.size()) { std::cout << "[错误] 序号超出范围\n"; return 1; }
                ts = times[idx - 1];
            }
            else ts = line;
        }
    }
    if (!fs::is_directory(backupDir / fs::u8path(ts), ec))
    { std::cout << "[错误] 时间点无效或不存在: " << ts << "\n"; return 1; }

    fs::path snap = backupDir / fs::u8path(ts);
    std::set<string> folderZips;
    fs::path mf = snap / "manifest.txt";
    if (fs::exists(mf, ec))
    {
        string content, merr;
        if (readText(mf, content, merr))
        {
            std::stringstream ss(content);
            string line;
            while (std::getline(ss, line))
            {
                line = trim(line);
                if (!line.empty()) folderZips.insert(line);
            }
        }
        else std::cout << "[警告] 读取 manifest 失败: " << merr << "\n";
    }

    std::cout << (dryRun ? "[预览] " : "[还原] ") << "文件夹: " << folder.u8string() << "\n"
              << (dryRun ? "[预览] " : "[还原] ") << "时间点:  " << ts << "\n";

    vector<string> errors;
    int nFiles = 0, nDirs = 0;
    std::error_code e2;
    for (auto it = fs::recursive_directory_iterator(snap, fs::directory_options::skip_permission_denied, e2);
         !e2 && it != fs::recursive_directory_iterator(); it.increment(e2))
    {
        if (g_cancel.load()) { errors.push_back("已中断"); break; }
        std::error_code e3;
        if (!it->is_regular_file(e3)) continue;
        string rel = relStr(snap, it->path());
        if (rel == "manifest.txt") continue;

        bool isFolderZip = false;
        string targetRel = rel;
        if (endsWith(rel, ".zip") && folderZips.count(rel) > 0)
        {
            targetRel = rel.substr(0, rel.size() - 4);
            isFolderZip = true;
        }
        if (!isSafeRel(targetRel)) { errors.push_back("跳过不安全路径: " + rel); continue; }

        fs::path target = folder / fs::u8path(targetRel);
        std::cout << (dryRun ? "[预览] " : "  ") << (isFolderZip ? "还原目录 " : "还原文件 ") << targetRel << "\n";
        if (dryRun) { ++(isFolderZip ? nDirs : nFiles); continue; }

        if (fs::exists(target, e3) || fs::is_symlink(target, e3))
        {
            if (fs::is_directory(target, e3)) fs::remove_all(target, e3);
            else fs::remove(target, e3);
            if (e3) { errors.push_back("删除失败: " + targetRel + " (" + e3.message() + ")"); continue; }
        }
        if (isFolderZip)
        {
            fs::create_directories(target, e3);
            string zerr;
            if (!unzipFileToDir(it->path(), target, zerr))
            { errors.push_back("解压失败: " + rel + " (" + zerr + ")"); continue; }
            ++nDirs;
        }
        else
        {
            fs::create_directories(target.parent_path(), e3);
            string cerr;
            if (!copyFileWithTime(it->path(), target, cerr))
            { errors.push_back("复制失败: " + rel + " (" + cerr + ")"); continue; }
            ++nFiles;
        }
    }
    if (e2) errors.push_back("遍历备份目录失败: " + e2.message());

    std::cout << (dryRun ? "[预览] " : "[完成] ") << "文件 " << nFiles << " 个，目录 " << nDirs << " 个";
    if (!errors.empty())
    {
        std::cout << "，错误 " << errors.size() << " 条";
        for (auto& s : errors) std::cout << "\n  - " << s;
    }
    std::cout << "\n";
    if (!dryRun) clearHashCaches();
    return errors.empty() ? 0 : 1;
}

// ---------------------------------------------------------------- 列表/帮助 ---
static int runList(const vector<string>& args)
{
    if (args.empty()) { std::cout << "[错误] 用法: SyncTool list <文件夹>\n"; return 1; }
    std::error_code ec;
    fs::path folder = fs::weakly_canonical(fs::absolute(fs::path(args[0]), ec), ec);
    if (ec || !fs::is_directory(folder, ec)) { std::cout << "[错误] 文件夹不存在: " << args[0] << "\n"; return 1; }
    fs::path backupDir = findBackupRoot(folder);
    if (backupDir.empty()) { std::cout << "[提示] 该文件夹还没有备份\n"; return 0; }
    vector<string> times = listTimeDirs(backupDir, ec);
    if (times.empty()) { std::cout << "[提示] 该文件夹还没有备份\n"; return 0; }
    std::cout << folder.u8string() << " 的备份时间点（新 -> 旧，共 " << times.size() << " 个）：\n";
    for (size_t i = 0; i < times.size(); ++i) std::cout << "  " << (i + 1) << ") " << times[i] << "\n";
    return 0;
}

// ---------------------------------------------------------------- 清理备份 ---
static uint64_t dirSize(const fs::path& p)
{
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::recursive_directory_iterator(p, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
    {
        std::error_code ec2;
        if (it->is_regular_file(ec2)) total += (uint64_t)it->file_size(ec2);
        if (ec2) continue;
    }
    return total;
}

static int runClean(const vector<string>& args)
{
    string folderArg, timeArg;
    bool all = false, dryRun = false;
    for (size_t i = 0; i < args.size(); ++i)
    {
        const string& a = args[i];
        if (a == "--time") { if (i + 1 >= args.size()) { std::cout << "[错误] --time 需要参数\n"; return 1; } timeArg = args[++i]; }
        else if (a.rfind("--time=", 0) == 0) timeArg = a.substr(7);
        else if (a == "--all") all = true;
        else if (a == "--dry-run") dryRun = true;
        else if (a == "--yes" || a == "-y") g_yes = true;
        else if (a.rfind("--", 0) == 0) { std::cout << "[错误] 未知参数: " << a << "\n"; return 1; }
        else if (folderArg.empty()) folderArg = a;
        else { std::cout << "[错误] clean 只能选择一个文件夹: " << a << "\n"; return 1; }
    }
    if (folderArg.empty()) { std::cout << "[错误] 用法: SyncTool clean <文件夹> [--time <时间戳>|--all] [--dry-run]\n"; return 1; }

    std::error_code ec;
    fs::path folder = fs::weakly_canonical(fs::absolute(fs::path(folderArg), ec), ec);
    if (ec || !fs::is_directory(folder, ec)) { std::cout << "[错误] 文件夹不存在: " << folderArg << "\n"; return 1; }
    fs::path backupDir = findBackupRoot(folder);
    if (backupDir.empty()) { std::cout << "[提示] 没有 .backup/backup 目录\n"; return 0; }

    vector<string> times = listTimeDirs(backupDir, ec);
    if (times.empty()) { std::cout << "[提示] 备份目录下没有时间点\n"; return 0; }

    vector<uint64_t> sizes(times.size(), 0);
    for (size_t i = 0; i < times.size(); ++i) sizes[i] = dirSize(backupDir / fs::u8path(times[i]));

    vector<size_t> selected;
    if (all) { for (size_t i = 0; i < times.size(); ++i) selected.push_back(i); }
    else if (!trim(timeArg).empty())
    {
        string ts = trim(timeArg);
        for (size_t i = 0; i < times.size(); ++i) if (times[i] == ts) selected.push_back(i);
        if (selected.empty()) { std::cout << "[错误] 时间点不存在: " << ts << "\n"; return 1; }
    }
    else
    {
        std::cout << folder.u8string() << " 的备份时间点（新 -> 旧）：\n";
        for (size_t i = 0; i < times.size(); ++i)
            std::cout << "  " << (i + 1) << ") " << times[i] << "  " << formatBytes(sizes[i]) << "\n";
        string line = askLine("请输入要清理的序号（可多选，如 1,3；0=全部；回车=取消）: ");
        if (line.empty()) { std::cout << "[取消] 未清理任何备份。\n"; return 0; }
        std::stringstream ss(line);
        string tok;
        bool any = false;
        while (std::getline(ss, tok, ','))
        {
            tok = trim(tok);
            if (tok.empty()) continue;
            bool digits = std::all_of(tok.begin(), tok.end(), [](char c) { return std::isdigit((unsigned char)c); });
            if (!digits) { std::cout << "[错误] 无效序号: " << tok << "\n"; return 1; }
            int n = std::atoi(tok.c_str());
            if (n == 0) { selected.clear(); for (size_t i = 0; i < times.size(); ++i) selected.push_back(i); any = true; break; }
            if (n < 1 || (size_t)n > times.size()) { std::cout << "[错误] 序号超出范围: " << n << "\n"; return 1; }
            selected.push_back((size_t)(n - 1));
            any = true;
        }
        if (!any) { std::cout << "[取消] 未选择任何时间点。\n"; return 0; }
        std::sort(selected.begin(), selected.end());
        selected.erase(std::unique(selected.begin(), selected.end()), selected.end());
    }

    uint64_t total = 0;
    std::cout << (dryRun ? "[预览] 将清理 " : "[清理] 将删除 ") << selected.size() << " 个时间点，共 ";
    for (size_t i : selected) total += sizes[i];
    std::cout << formatBytes(total) << "：\n";
    for (size_t i : selected) std::cout << "  " << times[i] << "  " << formatBytes(sizes[i]) << "\n";
    if (dryRun) { std::cout << "[预览] 不删除任何文件。\n"; return 0; }
    if (!askYesNo("确认删除这些备份？（不可恢复）", false))
    {
        std::cout << "[取消] 未删除任何备份。\n";
        return 0;
    }

    int errors = 0;
    for (size_t i : selected)
    {
        std::error_code rec;
        fs::remove_all(backupDir / fs::u8path(times[i]), rec);
        if (rec) { std::cout << "[错误] 删除失败: " << times[i] << " : " << rec.message() << "\n"; ++errors; }
        else std::cout << "  已删除 " << times[i] << "\n";
    }
    std::cout << (errors ? "[完成] 有错误" : "[完成] 清理完成") << "，共 " << (selected.size() - errors) << " 个时间点\n";
    return errors ? 1 : 0;
}

static void printUsage()
{
    std::cout
        << "SyncTool v" << kVersion << " —— 多文件夹同步 / 还原工具（跨平台, C++17）\n\n"
        << "用法：\n"
        << "  SyncTool sync <文件夹1> <文件夹2> [...] [选项]\n"
        << "      --min-level N    深度 N+1 的目录作为整体替换单位；不设则逐文件比较\n"
        << "      --jobs N         并行复制线程数（默认按 CPU，1-32）\n"
        << "      --exclude 模式   排除路径（可多次；支持 * ?，如 --exclude '*/.git/*'）\n"
        << "      --dry-run        只预检并生成计划，不修改文件（会比对同时间单元的内容哈希）\n"
        << "      --force          磁盘空间不足时仍继续（危险）\n"
        << "      --yes            所有询问取默认值（冲突跳过、中断保留、长路径开启）\n"
        << "      --no-long-paths  不自动开启 Windows 长路径支持\n"
        << "      --no-hash-cache  不使用跨运行哈希缓存（每次都重新读取文件，最准确但更慢）\n\n"
        << "  SyncTool restore <文件夹> [--time <时间戳>] [--dry-run]\n"
        << "  SyncTool list <文件夹>\n"
        << "  SyncTool clean <文件夹> [--time <时间戳>|--all] [--dry-run] [--yes]\n"
        << "  SyncTool help | --version\n\n"
        << "备份结构：\n"
        << "  <文件夹>/.backup/<时间>/<相对路径>       旧文件\n"
        << "  <文件夹>/.backup/<时间>/<相对路径>.zip   旧目录（快速压缩）\n"
        << "  <文件夹>/.backup/<时间>/manifest.txt     记录哪些 zip 是目录备份\n"
        << "  <文件夹>/.backup/<时间>/journal.jsonl    本次运行的变更日志（中断时保留，用于回滚）\n\n"
        << "说明：同步前会完整扫描并生成操作计划，写入系统临时目录并用独占句柄占用；\n"
        << "      修改时间相同的单元会比对内容哈希，不同则逐个询问使用哪个为源（其他线程继续复制）；\n"
        << "      中断（Ctrl+C）时可选择保留备份以便下次继续，或回滚本次已完成的更改；\n"
        << "      时间点选择统一使用序号（1 开始）；Windows 长路径未开启时会询问并自动开启。\n"
        << "      同步期间会占用各根文件夹，请勿手动修改被同步目录。\n";
}

// ---------------------------------------------------------------- 入口 ---------
int main(int argc, char** argv)
{
    setupConsole();
    for (int i = 0; i < argc; ++i) g_originalArgs.push_back(argv[i]);
    installSignalHandlers();

#if defined(_WIN32)
    if (argc > 1 && std::strcmp(argv[1], "--enable-long-paths") == 0)
    {
        bool ok = isElevated() && setLongPathsEnabled();
        std::cout << (ok ? "[长路径] 已开启 LongPathsEnabled=1\n" : "[长路径] 开启失败（需要管理员权限）\n");
        return ok ? 0 : 1;
    }
#endif

    vector<string> args;
    for (int i = 2; i < argc; ++i) args.push_back(argv[i]);
    string cmd = argc > 1 ? argv[1] : "";

    if (cmd.empty() || cmd == "help" || cmd == "--help" || cmd == "-h") { printUsage(); return cmd.empty() ? 1 : 0; }
    if (cmd == "--version" || cmd == "-v") { std::cout << "SyncTool " << kVersion << "\n"; return 0; }
    if (cmd == "sync") return runSync(args);
    if (cmd == "restore") return runRestore(args);
    if (cmd == "list") return runList(args);
    if (cmd == "clean") return runClean(args);

    std::cout << "[错误] 未知命令: " << cmd << "\n";
    printUsage();
    return 1;
}
