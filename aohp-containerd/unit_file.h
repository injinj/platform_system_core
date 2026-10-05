/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <map>
#include <string>
#include <vector>

#include <ctime>

namespace aohp {

// Parsed systemd-subset unit file (docs/UNITS.md in aohp-driver). Unknown keys collect in
// warnings; unsupported values of supported keys set loadError.

enum class UnitKind { Service, Timer };
enum class ServiceType { Simple, Oneshot };
enum class RestartPolicy { No, Always, OnFailure, OnAbnormal, OnSuccess };
enum class KillMode { ControlGroup, Process };

struct ExecLine {
    std::string command;
    bool ignoreFailure = false;  // leading '-'
};

struct CalendarSpec {
    enum Kind { None, Minutely, Hourly, Daily, Weekly, DailyAt } kind = None;
    int hour = 0, minute = 0, second = 0;  // DailyAt
    std::string text;
};

struct UnitDef {
    std::string name;  // "foo.service" / "foo.timer"
    UnitKind kind = UnitKind::Service;
    std::string path;  // absolute host path of the file, empty for transient
    bool transient = false;

    // [Unit]
    std::string description;
    std::vector<std::string> after, before, requiresUnits, wants;
    std::vector<std::pair<std::string, bool>> conditionPathExists;  // (path, negated)

    // [Service]
    ServiceType type = ServiceType::Simple;
    std::vector<ExecLine> execStartPre, execStart, execStartPost, execStop, execReload;
    bool remainAfterExit = false;
    RestartPolicy restart = RestartPolicy::No;
    double restartSec = 1.0;
    int startLimitBurst = 5;
    double startLimitIntervalSec = 60.0;
    std::vector<int> successExitStatus;    // codes
    std::vector<int> successExitSignals;   // extra signals counted clean
    std::vector<std::pair<std::string, std::string>> environment;
    std::vector<std::pair<std::string, bool>> environmentFiles;  // (path, optional)
    std::string workingDirectory;
    bool workingDirectoryOptional = false;
    double timeoutStopSec = 30.0;
    double timeoutStartSec = 90.0;
    KillMode killMode = KillMode::ControlGroup;

    // [Timer]
    double onBootSec = -1;
    double onUnitActiveSec = -1;
    CalendarSpec onCalendar;
    std::string timerUnit;  // defaults to <base>.service
    bool persistent = false;

    // [Install]
    std::vector<std::string> wantedBy;

    std::string loadError;
    std::vector<std::string> warnings;

    std::string baseName() const;  // "foo" for "foo.service"
};

// Parse a unit file body. name must carry the .service/.timer suffix.
UnitDef parseUnitText(const std::string& name, const std::string& text);

// "5", "5s", "2min", "500ms", "1h" -> seconds; false on parse error.
bool parseDurationSec(const std::string& s, double* out);
bool parseCalendar(const std::string& s, CalendarSpec* out);
// Next elapse strictly after 'after' (epoch seconds, local time); 0 if none.
time_t calendarNext(const CalendarSpec& c, time_t after);

std::string jsonEscapeStr(const std::string& s);
std::string jsonStringArray(const std::vector<std::string>& v);

// Minimal JSON object field extraction (flat objects, string/int/bool values) for request args.
std::string jsonGetString(const std::string& j, const char* key, const std::string& def = "");
long long jsonGetInt(const std::string& j, const char* key, long long def = 0);
bool jsonGetBool(const std::string& j, const char* key, bool def = false);

std::string base64Decode(const std::string& in);
std::string base64EncodeStr(const std::string& in);

}  // namespace aohp
