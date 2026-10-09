/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#include "unit_file.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include <signal.h>

namespace aohp {

namespace {

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::vector<std::string> splitWs(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream iss(s);
    std::string t;
    while (iss >> t) out.push_back(t);
    return out;
}

// Split on whitespace honouring double quotes: A=b "C=d e" -> [A=b, C=d e]
std::vector<std::string> splitQuoted(const std::string& s) {
    std::vector<std::string> out;
    std::string cur;
    bool inq = false, any = false;
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c == '"') {
            inq = !inq;
            any = true;
            continue;
        }
        if (!inq && isspace(static_cast<unsigned char>(c))) {
            if (any || !cur.empty()) out.push_back(cur);
            cur.clear();
            any = false;
            continue;
        }
        cur += c;
        any = true;
    }
    if (any || !cur.empty()) out.push_back(cur);
    return out;
}

bool parseBool(const std::string& v, bool* out) {
    std::string l = lower(trim(v));
    if (l == "yes" || l == "true" || l == "1" || l == "on") { *out = true; return true; }
    if (l == "no" || l == "false" || l == "0" || l == "off") { *out = false; return true; }
    return false;
}

int signalByName(const std::string& n) {
    static const struct { const char* name; int sig; } kSigs[] = {
            {"SIGHUP", SIGHUP},   {"HUP", SIGHUP},   {"SIGINT", SIGINT},   {"INT", SIGINT},
            {"SIGTERM", SIGTERM}, {"TERM", SIGTERM}, {"SIGPIPE", SIGPIPE}, {"PIPE", SIGPIPE},
            {"SIGKILL", SIGKILL}, {"KILL", SIGKILL}, {"SIGUSR1", SIGUSR1}, {"USR1", SIGUSR1},
            {"SIGUSR2", SIGUSR2}, {"USR2", SIGUSR2}, {"SIGQUIT", SIGQUIT}, {"QUIT", SIGQUIT},
            {"SIGABRT", SIGABRT}, {"ABRT", SIGABRT},
    };
    for (const auto& s : kSigs) {
        if (n == s.name) return s.sig;
    }
    return -1;
}

void addExec(std::vector<ExecLine>* v, const std::string& value) {
    std::string t = trim(value);
    ExecLine e;
    if (!t.empty() && t[0] == '-') {
        e.ignoreFailure = true;
        t = trim(t.substr(1));
    }
    e.command = t;
    v->push_back(e);
}

}  // namespace

std::string UnitDef::baseName() const {
    size_t p = name.rfind('.');
    return p == std::string::npos ? name : name.substr(0, p);
}

bool parseDurationSec(const std::string& in, double* out) {
    std::string s = trim(in);
    if (s.empty()) return false;
    if (s == "infinity") { *out = 1e9; return true; }
    double total = 0;
    size_t i = 0;
    bool any = false;
    while (i < s.size()) {
        while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) break;
        char* endp = nullptr;
        double v = strtod(s.c_str() + i, &endp);
        if (endp == s.c_str() + i) return false;
        i = static_cast<size_t>(endp - s.c_str());
        while (i < s.size() && isspace(static_cast<unsigned char>(s[i]))) ++i;
        size_t j = i;
        while (j < s.size() && isalpha(static_cast<unsigned char>(s[j]))) ++j;
        std::string unit = s.substr(i, j - i);
        i = j;
        double mult = 1;
        if (unit.empty() || unit == "s" || unit == "sec" || unit == "second" || unit == "seconds") mult = 1;
        else if (unit == "ms" || unit == "msec") mult = 0.001;
        else if (unit == "us" || unit == "usec") mult = 0.000001;
        else if (unit == "m" || unit == "min" || unit == "minute" || unit == "minutes") mult = 60;
        else if (unit == "h" || unit == "hr" || unit == "hour" || unit == "hours") mult = 3600;
        else if (unit == "d" || unit == "day" || unit == "days") mult = 86400;
        else if (unit == "w" || unit == "week" || unit == "weeks") mult = 7 * 86400;
        else return false;
        total += v * mult;
        any = true;
    }
    if (!any) return false;
    *out = total;
    return true;
}

bool parseCalendar(const std::string& in, CalendarSpec* out) {
    std::string s = trim(in);
    std::string l = lower(s);
    CalendarSpec c;
    c.text = s;
    if (l == "minutely") c.kind = CalendarSpec::Minutely;
    else if (l == "hourly") c.kind = CalendarSpec::Hourly;
    else if (l == "daily") c.kind = CalendarSpec::Daily;
    else if (l == "weekly") c.kind = CalendarSpec::Weekly;
    else {
        // "*-*-* HH:MM:SS" or "*-*-* HH:MM" or "HH:MM[:SS]"
        std::string t = s;
        const std::string pre = "*-*-* ";
        if (t.compare(0, pre.size(), pre) == 0) t = trim(t.substr(pre.size()));
        int h = 0, m = 0, sec = 0;
        int n = sscanf(t.c_str(), "%d:%d:%d", &h, &m, &sec);
        if (n < 2) return false;
        if (n == 2) sec = 0;
        // reject trailing garbage such as "12:00:00 foo" or wildcards in the time part
        for (char ch : t) {
            if (!(isdigit(static_cast<unsigned char>(ch)) || ch == ':')) return false;
        }
        if (h < 0 || h > 23 || m < 0 || m > 59 || sec < 0 || sec > 59) return false;
        c.kind = CalendarSpec::DailyAt;
        c.hour = h;
        c.minute = m;
        c.second = sec;
    }
    *out = c;
    return true;
}

time_t calendarNext(const CalendarSpec& c, time_t after) {
    struct tm tmv;
    localtime_r(&after, &tmv);
    switch (c.kind) {
        case CalendarSpec::Minutely:
            tmv.tm_sec = 0;
            tmv.tm_min += 1;
            break;
        case CalendarSpec::Hourly:
            tmv.tm_sec = 0;
            tmv.tm_min = 0;
            tmv.tm_hour += 1;
            break;
        case CalendarSpec::Daily:
            tmv.tm_sec = 0;
            tmv.tm_min = 0;
            tmv.tm_hour = 0;
            tmv.tm_mday += 1;
            break;
        case CalendarSpec::Weekly: {
            tmv.tm_sec = 0;
            tmv.tm_min = 0;
            tmv.tm_hour = 0;
            int wd = tmv.tm_wday;  // 0 = Sunday; systemd weekly = Monday 00:00
            int add = (8 - wd) % 7;
            if (add == 0) add = 7;
            tmv.tm_mday += add;
            break;
        }
        case CalendarSpec::DailyAt: {
            tmv.tm_hour = c.hour;
            tmv.tm_min = c.minute;
            tmv.tm_sec = c.second;
            tmv.tm_isdst = -1;
            time_t t = mktime(&tmv);
            if (t <= after) {
                tmv.tm_mday += 1;
                tmv.tm_isdst = -1;
                t = mktime(&tmv);
            }
            return t;
        }
        case CalendarSpec::None:
        default:
            return 0;
    }
    tmv.tm_isdst = -1;
    return mktime(&tmv);
}

UnitDef parseUnitText(const std::string& name, const std::string& text) {
    UnitDef u;
    u.name = name;
    if (name.size() > 6 && name.compare(name.size() - 6, 6, ".timer") == 0) u.kind = UnitKind::Timer;
    else if (name.size() > 8 && name.compare(name.size() - 8, 8, ".service") == 0) u.kind = UnitKind::Service;
    else {
        u.loadError = "unit name must end in .service or .timer";
        return u;
    }

    std::string section;
    std::istringstream iss(text);
    std::string rawLine, line;
    int lineNo = 0;
    std::string pending;  // backslash continuation
    auto warn = [&](const std::string& msg) { u.warnings.push_back(msg); };
    auto err = [&](const std::string& msg) {
        if (u.loadError.empty()) u.loadError = msg;
    };

    while (std::getline(iss, rawLine)) {
        ++lineNo;
        line = pending.empty() ? rawLine : pending + trim(rawLine);
        pending.clear();
        std::string t = trim(line);
        if (!t.empty() && t.back() == '\\') {
            pending = trim(t.substr(0, t.size() - 1)) + " ";
            continue;
        }
        if (t.empty() || t[0] == '#' || t[0] == ';') continue;
        if (t.front() == '[' && t.back() == ']') {
            section = t.substr(1, t.size() - 2);
            if (section != "Unit" && section != "Service" && section != "Timer" && section != "Install") {
                warn("line " + std::to_string(lineNo) + ": unknown section [" + section + "] ignored");
            }
            continue;
        }
        size_t eq = t.find('=');
        if (eq == std::string::npos) {
            warn("line " + std::to_string(lineNo) + ": not key=value, ignored");
            continue;
        }
        std::string key = trim(t.substr(0, eq));
        std::string value = trim(t.substr(eq + 1));
        std::string where = "line " + std::to_string(lineNo) + ": ";

        if (section == "Unit") {
            if (key == "Description") u.description = value;
            else if (key == "After") for (auto& x : splitWs(value)) u.after.push_back(x);
            else if (key == "Before") for (auto& x : splitWs(value)) u.before.push_back(x);
            else if (key == "Requires") for (auto& x : splitWs(value)) u.requiresUnits.push_back(x);
            else if (key == "Wants") for (auto& x : splitWs(value)) u.wants.push_back(x);
            else if (key == "ConditionPathExists") {
                bool neg = !value.empty() && value[0] == '!';
                std::string p = neg ? trim(value.substr(1)) : value;
                if (p.empty() || p[0] != '/') err(where + "ConditionPathExists needs an absolute path");
                else u.conditionPathExists.emplace_back(p, neg);
            } else warn(where + "[Unit] " + key + " not supported, ignored");
        } else if (section == "Service") {
            if (u.kind != UnitKind::Service) { warn(where + "[Service] in a timer unit, ignored"); continue; }
            if (key == "Type") {
                std::string l = lower(value);
                if (l == "simple" || l == "exec") u.type = ServiceType::Simple;
                else if (l == "oneshot") u.type = ServiceType::Oneshot;
                else err(where + "Type=" + value + " not supported (simple|oneshot)");
            } else if (key == "ExecStartPre") addExec(&u.execStartPre, value);
            else if (key == "ExecStart") addExec(&u.execStart, value);
            else if (key == "ExecStartPost") addExec(&u.execStartPost, value);
            else if (key == "ExecStop") addExec(&u.execStop, value);
            else if (key == "ExecReload") addExec(&u.execReload, value);
            else if (key == "RemainAfterExit") { if (!parseBool(value, &u.remainAfterExit)) err(where + "bad boolean"); }
            else if (key == "Restart") {
                std::string l = lower(value);
                if (l == "no") u.restart = RestartPolicy::No;
                else if (l == "always") u.restart = RestartPolicy::Always;
                else if (l == "on-failure") u.restart = RestartPolicy::OnFailure;
                else if (l == "on-abnormal") u.restart = RestartPolicy::OnAbnormal;
                else if (l == "on-success") u.restart = RestartPolicy::OnSuccess;
                else if (l == "on-abort" || l == "on-watchdog") { u.restart = RestartPolicy::OnAbnormal; warn(where + "Restart=" + value + " treated as on-abnormal"); }
                else err(where + "Restart=" + value + " unknown");
            } else if (key == "RestartSec") { if (!parseDurationSec(value, &u.restartSec)) err(where + "bad RestartSec"); }
            else if (key == "StartLimitBurst") u.startLimitBurst = atoi(value.c_str());
            else if (key == "StartLimitIntervalSec" || key == "StartLimitInterval") { if (!parseDurationSec(value, &u.startLimitIntervalSec)) err(where + "bad StartLimitIntervalSec"); }
            else if (key == "SuccessExitStatus") {
                for (auto& x : splitWs(value)) {
                    char* endp = nullptr;
                    long v = strtol(x.c_str(), &endp, 10);
                    if (*endp == '\0') u.successExitStatus.push_back(static_cast<int>(v));
                    else {
                        int sig = signalByName(x);
                        if (sig > 0) u.successExitSignals.push_back(sig);
                        else warn(where + "SuccessExitStatus " + x + " ignored");
                    }
                }
            } else if (key == "Environment") {
                for (auto& kv : splitQuoted(value)) {
                    size_t e = kv.find('=');
                    if (e == std::string::npos || e == 0) { warn(where + "Environment entry '" + kv + "' ignored"); continue; }
                    u.environment.emplace_back(kv.substr(0, e), kv.substr(e + 1));
                }
            } else if (key == "EnvironmentFile") {
                bool opt = !value.empty() && value[0] == '-';
                std::string p = opt ? trim(value.substr(1)) : value;
                if (p.empty() || p[0] != '/') err(where + "EnvironmentFile needs an absolute path");
                else u.environmentFiles.emplace_back(p, opt);
            } else if (key == "WorkingDirectory") {
                bool opt = !value.empty() && value[0] == '-';
                std::string p = opt ? trim(value.substr(1)) : value;
                if (p == "~") p = "/root";
                if (p.empty() || p[0] != '/') err(where + "WorkingDirectory needs an absolute path");
                else { u.workingDirectory = p; u.workingDirectoryOptional = opt; }
            } else if (key == "TimeoutStopSec") { if (!parseDurationSec(value, &u.timeoutStopSec)) err(where + "bad TimeoutStopSec"); }
            else if (key == "TimeoutStartSec") { if (!parseDurationSec(value, &u.timeoutStartSec)) err(where + "bad TimeoutStartSec"); }
            else if (key == "TimeoutSec") {
                double d;
                if (!parseDurationSec(value, &d)) err(where + "bad TimeoutSec");
                else { u.timeoutStopSec = d; u.timeoutStartSec = d; }
            } else if (key == "HostExec") { if (!parseBool(value, &u.hostExec)) err(where + "bad boolean"); }
            else if (key == "KillMode") {
                std::string l = lower(value);
                if (l == "control-group" || l == "mixed") u.killMode = KillMode::ControlGroup;
                else if (l == "process") u.killMode = KillMode::Process;
                else err(where + "KillMode=" + value + " unknown");
            } else warn(where + "[Service] " + key + " not supported, ignored");
        } else if (section == "Timer") {
            if (u.kind != UnitKind::Timer) { warn(where + "[Timer] in a service unit, ignored"); continue; }
            if (key == "OnBootSec" || key == "OnStartupSec" || key == "OnActiveSec") { if (!parseDurationSec(value, &u.onBootSec)) err(where + "bad " + key); }
            else if (key == "OnUnitActiveSec" || key == "OnUnitInactiveSec") { if (!parseDurationSec(value, &u.onUnitActiveSec)) err(where + "bad " + key); }
            else if (key == "OnCalendar") { if (!parseCalendar(value, &u.onCalendar)) err(where + "OnCalendar=" + value + " not in the supported subset (minutely|hourly|daily|weekly|*-*-* HH:MM:SS|HH:MM)"); }
            else if (key == "Unit") u.timerUnit = value;
            else if (key == "Persistent") { if (!parseBool(value, &u.persistent)) err(where + "bad boolean"); }
            else warn(where + "[Timer] " + key + " not supported, ignored");
        } else if (section == "Install") {
            if (key == "WantedBy" || key == "RequiredBy") {
                for (auto& x : splitWs(value)) {
                    u.wantedBy.push_back(x);
                    if (x != "aohp.target") warn(where + "WantedBy=" + x + ": only aohp.target is used");
                }
            } else if (key == "Alias" || key == "Also") warn(where + "[Install] " + key + " not supported, ignored");
            else warn(where + "[Install] " + key + " not supported, ignored");
        } else if (section.empty()) {
            warn(where + "key outside a section, ignored");
        }
    }

    if (u.kind == UnitKind::Service) {
        if (u.execStart.empty()) err("ExecStart is required");
        if (u.execStart.size() > 1 && u.type != ServiceType::Oneshot) err("multiple ExecStart only allowed with Type=oneshot");
        if (u.restartSec < 0.1) u.restartSec = 0.1;
        if (u.startLimitBurst <= 0) u.startLimitBurst = 5;
        if (u.timeoutStopSec <= 0) u.timeoutStopSec = 30;
        if (u.timeoutStartSec <= 0) u.timeoutStartSec = 90;
    } else {
        if (u.onBootSec < 0 && u.onUnitActiveSec < 0 && u.onCalendar.kind == CalendarSpec::None)
            err("timer needs OnBootSec, OnUnitActiveSec or OnCalendar");
        if (u.timerUnit.empty()) u.timerUnit = u.baseName() + ".service";
    }
    return u;
}

std::string jsonEscapeStr(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '\\': o += "\\\\"; break;
            case '"': o += "\\\""; break;
            case '\n': o += "\\n"; break;
            case '\r': o += "\\r"; break;
            case '\t': o += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    snprintf(buf, sizeof(buf), "\\u%04x", c);
                    o += buf;
                } else {
                    o += static_cast<char>(c);
                }
        }
    }
    return o;
}

std::string jsonStringArray(const std::vector<std::string>& v) {
    std::string o = "[";
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) o += ",";
        o += "\"" + jsonEscapeStr(v[i]) + "\"";
    }
    return o + "]";
}

namespace {
// Locate the value start for "key": in a flat object; returns npos if missing.
size_t jsonFindValue(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\"";
    size_t p = 0;
    while ((p = j.find(pat, p)) != std::string::npos) {
        size_t q = p + pat.size();
        while (q < j.size() && isspace(static_cast<unsigned char>(j[q]))) ++q;
        if (q < j.size() && j[q] == ':') {
            ++q;
            while (q < j.size() && isspace(static_cast<unsigned char>(j[q]))) ++q;
            return q;
        }
        p = q;
    }
    return std::string::npos;
}
}  // namespace

std::string jsonGetString(const std::string& j, const char* key, const std::string& def) {
    size_t p = jsonFindValue(j, key);
    if (p == std::string::npos || p >= j.size()) return def;
    if (j[p] != '"') {
        // bare number/bool
        size_t e = p;
        while (e < j.size() && j[e] != ',' && j[e] != '}' && !isspace(static_cast<unsigned char>(j[e]))) ++e;
        std::string v = j.substr(p, e - p);
        if (v == "null") return def;
        return v;
    }
    ++p;
    std::string o;
    while (p < j.size()) {
        char c = j[p];
        if (c == '\\' && p + 1 < j.size()) {
            char n = j[p + 1];
            switch (n) {
                case 'n': o += '\n'; break;
                case 't': o += '\t'; break;
                case 'r': o += '\r'; break;
                case 'b': o += '\b'; break;
                case 'f': o += '\f'; break;
                case 'u':
                    if (p + 5 < j.size()) {
                        unsigned code = static_cast<unsigned>(strtoul(j.substr(p + 2, 4).c_str(), nullptr, 16));
                        if (code < 0x80) o += static_cast<char>(code);
                        else if (code < 0x800) { o += static_cast<char>(0xC0 | (code >> 6)); o += static_cast<char>(0x80 | (code & 0x3F)); }
                        else { o += static_cast<char>(0xE0 | (code >> 12)); o += static_cast<char>(0x80 | ((code >> 6) & 0x3F)); o += static_cast<char>(0x80 | (code & 0x3F)); }
                        p += 4;
                    }
                    break;
                default: o += n; break;
            }
            p += 2;
            continue;
        }
        if (c == '"') break;
        o += c;
        ++p;
    }
    return o;
}

long long jsonGetInt(const std::string& j, const char* key, long long def) {
    std::string v = jsonGetString(j, key, "");
    if (v.empty()) return def;
    char* endp = nullptr;
    long long r = strtoll(v.c_str(), &endp, 10);
    if (endp == v.c_str()) return def;
    return r;
}

bool jsonGetBool(const std::string& j, const char* key, bool def) {
    std::string v = lower(jsonGetString(j, key, ""));
    if (v == "true" || v == "1" || v == "yes") return true;
    if (v == "false" || v == "0" || v == "no") return false;
    return def;
}

std::string base64Decode(const std::string& in) {
    static int T[256];
    static bool init = false;
    if (!init) {
        for (int& t : T) t = -1;
        const char* k = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        for (int i = 0; i < 64; ++i) T[static_cast<unsigned char>(k[i])] = i;
        T[static_cast<unsigned char>('-')] = 62;
        T[static_cast<unsigned char>('_')] = 63;
        init = true;
    }
    std::string out;
    int val = 0, valb = -8;
    for (unsigned char c : in) {
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        if (T[c] == -1) break;
        val = (val << 6) + T[c];
        valb += 6;
        if (valb >= 0) {
            out.push_back(static_cast<char>((val >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return out;
}

std::string base64EncodeStr(const std::string& in) {
    static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((in.size() + 2) / 3) * 4);
    int val = 0;
    int valb = -6;
    for (unsigned char c : in) {
        val = (val << 8) + c;
        valb += 8;
        while (valb >= 0) {
            out.push_back(kB64[(val >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) out.push_back(kB64[((val << 8) >> (valb + 8)) & 0x3F]);
    while (out.size() % 4) out.push_back('=');
    return out;
}

}  // namespace aohp
