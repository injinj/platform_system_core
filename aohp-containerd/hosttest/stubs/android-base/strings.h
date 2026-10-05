#pragma once
#include <string>
namespace android { namespace base {
inline std::string Trim(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n"), e = s.find_last_not_of(" \t\r\n");
    return b == std::string::npos ? "" : s.substr(b, e - b + 1);
}
inline bool StartsWith(const std::string& s, const char* p) { return s.compare(0, strlen(p), p) == 0; }
inline bool StartsWith(const std::string& s, const std::string& p) { return s.compare(0, p.size(), p) == 0; }
}}
#include <cstring>
