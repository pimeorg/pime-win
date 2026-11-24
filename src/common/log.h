#pragma once

#include <string>

namespace pimewin {

enum class LogLevel { Error = 0, Warn = 1, Info = 2, Debug = 3 };

// Opens <data-dir>\pime.log once. Safe to call repeatedly.
void InitLog(const std::wstring& data_dir);
void WriteLog(LogLevel level, const char* tag, const std::string& message);
void WriteLog(LogLevel level, const char* tag, const std::wstring& message);

}  // namespace pimewin
