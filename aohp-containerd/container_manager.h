/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "cgroup_controller.h"

namespace aohp {

struct ExecResult {
    int exitCode;
    std::string stdoutStr;
    std::string stderrStr;
};

class ContainerManager {
public:
    ContainerManager();

    std::vector<std::string> listContainers();
    bool createContainer(const std::string& name, const std::string& templateName);
    bool destroyContainer(const std::string& name);
    bool resetContainer(const std::string& name);
    ExecResult execSync(const std::string& name, const std::string& command, int timeoutMs);

    int openShell(const std::string& name);

    const std::string& getLastError() const { return mLastError_; }

    std::string templateInfo(const std::string& name);

    long startService(const std::string& name, const std::string& serviceId, const std::string& command);
    bool stopService(const std::string& name, const std::string& serviceId);
    std::string listServicesJson(const std::string& name);
    std::string serviceLogTail(const std::string& name, const std::string& serviceId, int tailBytes);
    std::string getUsageJson(const std::string& name);
    std::string diagnose(const std::string& name);

    /** Remove stale .pid files when the child died externally; call at daemon startup. */
    void adoptOrphanServicePids();

    CgroupController& cgroup() { return mCgroup_; }

private:
    std::string mLastError_;
    std::mutex mWorkDirMutex_;
    std::map<std::string, std::string> mWorkDir_;

    CgroupController mCgroup_;

    std::string rootfsPath(const std::string& name);
    std::string envPath(const std::string& name);
    std::string templatePath(const std::string& templateName);
    std::string templateRecordPath(const std::string& name);
    std::string servicesDirPath(const std::string& name);

    bool extractTemplate(const std::string& templateTarGz, const std::string& destDir);
    bool setupBindMounts(const std::string& rootfs);
    bool teardownBindMounts(const std::string& rootfs);
    void killContainerProcesses(const std::string& rootfs);
    int forkIntoContainer(const std::string& containerName, const std::string& rootfs,
                          const char* const argv[], bool usePty);

    bool writeTemplateRecord(const std::string& name, const std::string& templateName);
    std::string readTemplateRecord(const std::string& name);
    std::string firstTemplateTarInTemplateDir();

    static bool isValidServiceId(const std::string& id);
};

}  // namespace aohp
