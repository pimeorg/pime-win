#include "common/log.h"

#include <windows.h>

#include <cstring>
#include <mutex>
#include <string>

#include "common/win_util.h"

namespace pimewin {
namespace {

std::mutex g_mutex;
HANDLE g_file = INVALID_HANDLE_VALUE;
bool g_opened = false;

const char* LevelName(LogLevel level) {
    switch (level) {
        case LogLevel::Error: return "E";
        case LogLevel::Warn: return "W";
        case LogLevel::Info: return "I";
        case LogLevel::Debug: return "D";
    }
    return "?";
}

}  // namespace

void InitLog(const std::wstring& data_dir) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_opened) return;
    g_opened = true;
    EnsureDirectory(data_dir);
    const std::wstring path =
        PathJoin(data_dir, L"pime-" + std::to_wstring(GetCurrentProcessId()) +
                               L".log");
    // Shared access so a second terminal can tail the file while the host
    // process is running.
    g_file = CreateFileW(path.c_str(), FILE_APPEND_DATA,
                         FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
}

void WriteLog(LogLevel level, const char* tag, const std::string& message) {
    SYSTEMTIME now{};
    GetLocalTime(&now);
    char line[2048];
    const int written = _snprintf_s(
        line, sizeof(line), _TRUNCATE, "[%02u:%02u:%02u.%03u][%s][%s] %s\n",
        now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, LevelName(level),
        tag, message.c_str());
    if (written <= 0) return;
    OutputDebugStringA(line);
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_file != INVALID_HANDLE_VALUE) {
        DWORD count = 0;
        WriteFile(g_file, line, static_cast<DWORD>(std::strlen(line)), &count,
                  nullptr);
    }
}

void WriteLog(LogLevel level, const char* tag, const std::wstring& message) {
    WriteLog(level, tag, ToUtf8(message));
}

}  // namespace pimewin
