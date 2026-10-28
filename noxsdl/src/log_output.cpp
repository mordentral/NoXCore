#include "log_output.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace noxsdl {

namespace fs = std::filesystem;

namespace {

std::ofstream g_file;
std::size_t g_written = 0; ///< bytes of this run in the file: at most 4 MB (hours of bots' kill notices)
constexpr std::size_t kMaxBytes = 4u << 20;

#ifdef _WIN32
HANDLE g_stderr = nullptr;  ///< stderr as our starter redirected it (a file or a pipe), else null
HANDLE g_console = nullptr; ///< -cmd's console (its screen buffer), else null

/// Whether `h` is a file or a pipe someone gave us (not missing, not a console).
bool redirected(HANDLE h)
{
    if (h == nullptr || h == INVALID_HANDLE_VALUE) {
        return false;
    }
    const DWORD type = GetFileType(h);
    return type == FILE_TYPE_DISK || type == FILE_TYPE_PIPE;
}

/// The C library's `stream` (descriptor `fd`) writing to `h`, if it isn't already (a GUI
/// program's may not have taken its starter's handles).
void streamTo(FILE* stream, int fd, HANDLE h)
{
    if (reinterpret_cast<HANDLE>(_get_osfhandle(fd)) == h) {
        return;
    }
    HANDLE copy = nullptr;
    if (!DuplicateHandle(GetCurrentProcess(), h, GetCurrentProcess(), &copy, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
        return;
    }
    const int tmp = _open_osfhandle(reinterpret_cast<intptr_t>(copy), _O_TEXT | _O_WRONLY);
    if (tmp < 0) {
        CloseHandle(copy);
        return;
    }
    std::fflush(stream);
    _dup2(tmp, fd);
    _close(tmp);
}

/// SDL's own prefixes for a line's priority (its default log output's).
const char* priorityPrefix(SDL_LogPriority priority)
{
    switch (priority) {
    case SDL_LOG_PRIORITY_WARN:
        return "WARNING: ";
    case SDL_LOG_PRIORITY_ERROR:
    case SDL_LOG_PRIORITY_CRITICAL:
        return "ERROR: ";
    default:
        return "";
    }
}

/// -cmd: a console, the terminal we were started from or else a new one; stdout and stderr go
/// to it unless they were redirected (those stay where they were sent).
void openConsole()
{
    const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const bool outRedirected = redirected(out);
    if (!AttachConsole(ATTACH_PARENT_PROCESS) && GetLastError() != ERROR_ACCESS_DENIED && !AllocConsole()) {
        return;
    }
    // (Attaching can give the standard handles the console's: the redirected ones are kept.)
    if (outRedirected) {
        SetStdHandle(STD_OUTPUT_HANDLE, out);
    }
    if (g_stderr) {
        SetStdHandle(STD_ERROR_HANDLE, g_stderr);
    }
    const HANDLE console = CreateFileW(L"CONOUT$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                       nullptr, OPEN_EXISTING, 0, nullptr);
    if (console == INVALID_HANDLE_VALUE) {
        return;
    }
    g_console = console;
    FILE* reopened = nullptr;
    if (!outRedirected) {
        freopen_s(&reopened, "CONOUT$", "w", stdout);
        SetStdHandle(STD_OUTPUT_HANDLE, console);
    }
    if (!g_stderr) {
        freopen_s(&reopened, "CONOUT$", "w", stderr);
        SetStdHandle(STD_ERROR_HANDLE, console);
    }
}
#else
SDL_LogOutputFunction g_previous = nullptr; ///< SDL's own output: stderr
void* g_previousData = nullptr;
#endif

void SDLCALL output([[maybe_unused]] void* data, [[maybe_unused]] int category, [[maybe_unused]] SDL_LogPriority priority,
                    const char* message)
{
    if (g_file && g_written < kMaxBytes) {
        g_written += std::strlen(message) + 1;
        g_file << message << '\n';
        if (g_written >= kMaxBytes) {
            g_file << "(the log is full: 4 MB this run)\n";
        }
        g_file.flush();
    }
#ifdef _WIN32
    // As SDL's own output writes it (its priority's prefix, CR LF), without its attaching to the
    // parent's console: to the redirected stderr, -cmd's console, and a debugger.
    const bool debugger = IsDebuggerPresent() != 0;
    if (!g_stderr && !g_console && !debugger) {
        return;
    }
    std::string line = priorityPrefix(priority);
    line += message;
    line += "\r\n";
    if (g_stderr) {
        DWORD written = 0;
        WriteFile(g_stderr, line.data(), static_cast<DWORD>(line.size()), &written, nullptr);
    }
    if (g_console || debugger) {
        std::wstring wide(static_cast<std::size_t>(MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), nullptr, 0)), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, line.data(), static_cast<int>(line.size()), wide.data(), static_cast<int>(wide.size()));
        if (g_console) {
            DWORD written = 0;
            WriteConsoleW(g_console, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
        }
        if (debugger) {
            OutputDebugStringW(wide.c_str());
        }
    }
#else
    if (g_previous) {
        g_previous(data, category, priority, message);
    }
#endif
}

} // namespace

bool isConsoleFlag(std::string_view arg)
{
    return arg == "-cmd" || arg == "--cmd";
}

void startLog([[maybe_unused]] int argc, [[maybe_unused]] char* argv[])
{
#ifdef _WIN32
    if (const HANDLE err = GetStdHandle(STD_ERROR_HANDLE); redirected(err)) {
        g_stderr = err;
        streamTo(stderr, 2, err);
    }
    if (const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE); redirected(out)) {
        streamTo(stdout, 1, out);
    }
    for (int i = 1; i < argc; ++i) {
        if (isConsoleFlag(argv[i])) {
            openConsole();
            break;
        }
    }
#else
    SDL_GetLogOutputFunction(&g_previous, &g_previousData);
#endif
    const char* base = SDL_GetBasePath();
    fs::path path = fs::path(base ? base : "") / "noxcore-log.txt";
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string_view(argv[i]) == "--log") {
            path = fs::path(reinterpret_cast<const char8_t*>(argv[i + 1])); // (a second noxcore here: a session's private test)
        }
    }
    g_file.open(path, std::ios::trunc);
    if (!g_file) {
        if (char* pref = SDL_GetPrefPath("noxcpp", "noxsdl")) {
            path = fs::path(pref) / "noxcore-log.txt";
            SDL_free(pref);
            g_file.open(path, std::ios::trunc);
        }
    }
#ifdef _WIN32
    SDL_SetLogOutputFunction(output, nullptr);
#else
    if (g_file) {
        SDL_SetLogOutputFunction(output, g_previousData);
    }
#endif
}

} // namespace noxsdl
