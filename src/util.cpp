#include "common.hpp"

#include <cstdarg>
#include <cwctype>
#include <mutex>
#include <share.h>

namespace {
std::mutex g_log_mu;
FILE*      g_log_file = nullptr;
bool       g_console  = false;
}

void log_init(const wchar_t* path) {
    if (AttachConsole(ATTACH_PARENT_PROCESS)) {
        FILE* f = nullptr;
        freopen_s(&f, "CONOUT$", "w", stdout);
        freopen_s(&f, "CONOUT$", "w", stderr);
        SetConsoleOutputCP(CP_UTF8);
        g_console = true;
    }
    if (path) g_log_file = _wfsopen(path, L"w", _SH_DENYWR);
}

void log_msg(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    std::lock_guard lk(g_log_mu);
    OutputDebugStringA(buf);
    OutputDebugStringA("\n");
    if (g_console) {
        std::fputs(buf, stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    if (g_log_file) {
        std::fputs(buf, g_log_file);
        std::fputc('\n', g_log_file);
        std::fflush(g_log_file);
    }
}

void throw_hr(long hr, const char* ctx) {
    char msg[512];
    std::snprintf(msg, sizeof(msg), "HRESULT 0x%08lX dans %s", static_cast<unsigned long>(hr), ctx);
    log_msg("%s", msg);
    throw std::runtime_error(msg);
}

std::string narrow(std::wstring_view w) {
    if (w.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), int(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::wstring widen(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), nullptr, 0);
    std::wstring out(size_t(n), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), int(s.size()), out.data(), n);
    return out;
}

std::wstring to_lower(std::wstring s) {
    for (auto& c : s) c = wchar_t(std::towlower(c));
    return s;
}

std::wstring exe_directory() {
    wchar_t path[MAX_PATH * 2]{};
    DWORD n = GetModuleFileNameW(nullptr, path, DWORD(std::size(path)));
    std::wstring p(path, n);
    auto slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1);
}

double now_seconds() {
    static const double inv_freq = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 1.0 / double(f.QuadPart);
    }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return double(c.QuadPart) * inv_freq;
}
