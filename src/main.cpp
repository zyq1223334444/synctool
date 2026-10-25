// ============================================================================
// SyncTool v2.0.0 —— 多文件夹同步 / 还原工具（Windows / Linux / macOS，C++17）
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
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
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
#include <utility>
#include <vector>

#include "miniz.h"

namespace fs = std::filesystem;
using std::string;
using std::vector;

static const char* kVersion = "2.0.0";
static const char* kBackupName = ".backup";
static const char* kLegacyBackupName = "backup";

// ---------------------------------------------------------------- 全局状态 ---
static std::atomic<bool> g_cancel{false};
static std::atomic<bool> g_lowSpace{false};
static std::atomic<uint64_t> g_scanFiles{0};
static std::atomic<uint64_t> g_doneOps{0};
static std::atomic<uint64_t> g_doneBytes{0};
static std::atomic<uint64_t> g_doneFiles{0};
static uint64_t g_totalOps = 0;
static uint64_t g_totalBytes = 0;
static std::mutex g_logMutex;
static std::mutex g_curMutex;
static std::mutex g_errMutex;
static std::mutex g_manifestMutex;
static std::string g_current;
static vector<string> g_errors;
static bool g_tty = false;
static std::atomic<int> g_phase{0}; // 0=扫描 1=预检 2=执行

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
    size_t b = s.find_first_not_of(" \t\r\n");
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
        else
        {
            uint64_t done = g_doneOps.load(), total = g_totalOps;
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
            double eta = speed > 1 ? (double)(g_totalBytes - g_doneBytes.load()) / speed : -1;
            os << "[执行 " << std::fixed << std::setprecision(1) << pct << "%] "
               << formatNum(done) << "/" << formatNum(total) << " 项 · "
               << formatBytes(g_doneBytes.load()) << "/" << formatBytes(g_totalBytes)
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
        if (ec) errors.push_back("遍历目录失败: " + dir.u8string() + " : " + ec.message());
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

struct Plan
{
    UnitMap units;
    vector<Op> ops;
    vector<uint64_t> required; // 每个目标文件夹预计新增占用
    vector<uint64_t> copyPer;
    vector<uint64_t> backupPer;
    uint64_t copyTotal = 0;
    uint64_t backupTotal = 0;
    uint64_t unitCount = 0;
    uint64_t skipped = 0;
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

        for (size_t f = 0; f < folderCount; ++f)
        {
            if (win->folder == (int)f) continue;
            const Version* v = nullptr;
            for (auto& x : u.versions) if (x.folder == (int)f) { v = &x; break; }

            bool need = false;
            if (!v) need = true;
            else if (v->isDir != win->isDir) need = true;
            else if (v->t < win->t) need = true;
            if (!need) { if (v) ++plan.skipped; continue; }

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
struct ApplyContext
{
    const vector<fs::path>* roots = nullptr;
    vector<fs::path> backupRoots;
    vector<vector<string>> manifests;
    std::mutex backupDirMutex;
    vector<bool> backupDirReady;
};

static bool ensureBackupDir(ApplyContext& ctx, int f, string& err)
{
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

    if (op.replace)
    {
        if (!backupOld(op, ctx, err))
        {
            addError("备份失败（跳过该单元）: " + op.key + " : " + err);
            g_doneOps.fetch_add(1);
            return;
        }
        fs::path oldPath = op.old->abs;
        if (op.oldIsDir) fs::remove_all(oldPath, ec);
        else fs::remove(oldPath, ec);
        if (ec)
        {
            addError("删除旧版本失败: " + oldPath.u8string() + " : " + ec.message());
            g_doneOps.fetch_add(1);
            return;
        }
    }

    fs::path dst = (*ctx.roots)[op.target] / fs::u8path(op.key);
    fs::create_directories(dst.parent_path(), ec);
    if (op.winIsDir)
    {
        if (fs::exists(dst, ec)) fs::remove_all(dst, ec);
        if (!copyDirTree(op.win->abs, dst))
        {
            addError("复制目录失败: " + op.key);
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
            addError("复制失败: " + op.key + " : " + cerr);
        else
        {
            g_doneBytes.fetch_add(op.win->size);
            g_doneFiles.fetch_add(1);
        }
    }
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
        else if (a.rfind("--", 0) == 0) { std::cout << "[错误] 未知参数: " << a << "\n"; return 1; }
        else folderArgs.push_back(fs::path(a));
    }
    if (minLevel < -1) { std::cout << "[错误] --min-level 必须 >= 0\n"; return 1; }
    jobs = std::max(1, std::min(jobs, 32));
    if (folderArgs.size() < 2) { std::cout << "[错误] sync 至少需要 2 个文件夹\n"; return 1; }

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
    std::cout << "单元 " << formatNum(plan.unitCount) << " 个，待执行操作 " << formatNum(plan.ops.size())
              << " 个（时间相同跳过 " << formatNum(plan.skipped) << " 个）\n";
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
    if (dryRun)
    {
        ui.stop();
        std::cout << "\n[预览] 不修改任何文件。\n";
        for (auto* l : locks) { l->release(); delete l; }
        return 0;
    }
    if (plan.ops.empty())
    {
        ui.stop();
        std::cout << "\n[完成] 所有单元已一致，无需同步。\n";
        for (auto* l : locks) { l->release(); delete l; }
        return 0;
    }

    // 备份根目录
    ApplyContext ctx;
    ctx.roots = &folders;
    ctx.backupRoots.resize(folders.size());
    ctx.manifests.resize(folders.size());
    ctx.backupDirReady.assign(folders.size(), false);
    string ts = makeTimestamp();
    for (size_t i = 0; i < folders.size(); ++i)
    {
        bool needBackup = false;
        for (auto& op : plan.ops) if (op.target == (int)i && op.replace) { needBackup = true; break; }
        if (!needBackup) continue;
        fs::path base = folders[i] / kBackupName;
        fs::path p = base / fs::u8path(ts);
        int k = 2;
        while (fs::exists(p, ec)) p = base / fs::u8path(ts + "-" + std::to_string(k++));
        ctx.backupRoots[i] = p;
    }

    g_phase.store(2);
    setCurrent("");
    g_totalOps = plan.ops.size();
    g_totalBytes = plan.copyTotal + plan.backupTotal;
    std::cout << "\n=== 执行 ===\n";
    std::cout << "并行线程 " << jobs << "，总进度 " << formatNum(g_totalOps) << " 项 / "
              << formatBytes(g_totalBytes) << "\n";

    runWorkers(ctx, plan, jobs);

    // 写 manifest
    for (size_t i = 0; i < folders.size(); ++i)
    {
        if (ctx.manifests[i].empty()) continue;
        string content;
        for (auto& line : ctx.manifests[i]) content += line + "\n";
        string merr;
        if (!writeText(ctx.backupRoots[i] / "manifest.txt", content, merr))
            addError("写 manifest 失败: " + merr);
    }

    ui.stop();
    g_phase.store(3);
    std::cout << "\n=== 同步" << (g_cancel.load() ? "已中断" : "完成") << " ===\n";
    std::cout << "操作 " << formatNum(g_doneOps.load()) << "/" << formatNum(g_totalOps)
              << "，处理文件 " << formatNum(g_doneFiles.load())
              << "，数据 " << formatBytes(g_doneBytes.load()) << "\n";
    if (!g_errors.empty()) std::cout << "错误 " << g_errors.size() << " 条（见上方日志）\n";
    for (size_t i = 0; i < folders.size(); ++i)
        if (!ctx.backupRoots[i].empty())
            std::cout << "备份目录: " << ctx.backupRoots[i].u8string() << "\n";

    for (auto* l : locks) { l->release(); delete l; }
    return g_cancel.load() ? 130 : (g_errors.empty() ? 0 : 1);
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
        for (size_t i = 0; i < times.size(); ++i) std::cout << "  " << i << ") " << times[i] << "\n";
        std::cout << "请输入序号或时间戳（直接回车 = 最新）: ";
        string line;
        std::getline(std::cin, line);
        line = trim(line);
        if (line.empty()) ts = times[0];
        else
        {
            bool allDigit = !line.empty() && std::all_of(line.begin(), line.end(), [](char c) { return std::isdigit((unsigned char)c); });
            if (allDigit)
            {
                int idx = std::atoi(line.c_str());
                if (idx < 0 || (size_t)idx >= times.size()) { std::cout << "[错误] 序号超出范围\n"; return 1; }
                ts = times[idx];
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
    for (size_t i = 0; i < times.size(); ++i) std::cout << "  " << i << ") " << times[i] << "\n";
    return 0;
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
        << "      --dry-run        只预检并生成计划，不修改文件\n"
        << "      --force          磁盘空间不足时仍继续（危险）\n\n"
        << "  SyncTool restore <文件夹> [--time <时间戳>] [--dry-run]\n"
        << "  SyncTool list <文件夹>\n"
        << "  SyncTool help | --version\n\n"
        << "备份结构：\n"
        << "  <文件夹>/.backup/<时间>/<相对路径>       旧文件\n"
        << "  <文件夹>/.backup/<时间>/<相对路径>.zip   旧目录（快速压缩）\n"
        << "  <文件夹>/.backup/<时间>/manifest.txt     记录哪些 zip 是目录备份\n\n"
        << "说明：同步前会完整扫描并生成操作计划，写入系统临时目录并用独占句柄占用；\n"
        << "      同步期间会占用各根文件夹，请勿手动修改被同步目录。\n";
}

// ---------------------------------------------------------------- 入口 ---------
int main(int argc, char** argv)
{
    setupConsole();
    installSignalHandlers();
    vector<string> args;
    for (int i = 2; i < argc; ++i) args.push_back(argv[i]);
    string cmd = argc > 1 ? argv[1] : "";

    if (cmd.empty() || cmd == "help" || cmd == "--help" || cmd == "-h") { printUsage(); return cmd.empty() ? 1 : 0; }
    if (cmd == "--version" || cmd == "-v") { std::cout << "SyncTool " << kVersion << "\n"; return 0; }
    if (cmd == "sync") return runSync(args);
    if (cmd == "restore") return runRestore(args);
    if (cmd == "list") return runList(args);

    std::cout << "[错误] 未知命令: " << cmd << "\n";
    printUsage();
    return 1;
}
