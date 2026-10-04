/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <string>

#include <sys/types.h>

namespace aohp {

/** cgroup v2 resource limits for one AOHP container (env). */
class CgroupController {
public:
    CgroupController();

    /** Load limits from config (defaults if missing). Call once at startup. */
    void loadConfig(const char* path);

    bool isEnabled() const { return mEnabled_; }

    /** Create cgroup dir and write memory.max, cpu.max, pids.max, etc. */
    bool createForContainer(const std::string& containerName);

    /** Remove cgroup directory (processes must already be gone). */
    void destroyForContainer(const std::string& containerName);

    /** Best-effort: move pid into this container's cgroup before chroot (logs on failure; always ok to continue). */
    bool joinContainerCgroup(const std::string& containerName, pid_t pid);

    /** JSON: memoryCurrent, memoryMax, memoryPeak, cpuUsageUsec, pidsCurrent, cgroupPath */
    std::string getUsageJson(const std::string& containerName) const;

    std::string cgroupPathFor(const std::string& containerName) const;

    /** JSON fragment for diagnose: cgroupEnabled, cgroupPath, memoryMaxConfigured */
    std::string diagnoseJson(const std::string& containerName) const;

private:
    bool mEnabled_ = false;
    bool mV2Detected_ = false;
    std::string mCgroupRoot_;
    std::string mMemoryMax;
    std::string mMemorySwapMax;
    std::string mCpuMax;
    std::string mPidsMax;

    static std::string sanitizeName(const std::string& name);
    static bool writeFile(const std::string& path, const std::string& value);
    static std::string readFileTrim(const std::string& path);
    static bool parseSizeToString(const std::string& in, std::string* outBytes);
};

}  // namespace aohp
