/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#define LOG_TAG "aohp-containerd"

#include "container_manager.h"
#include "protocol.h"
#include "tar_gz_extract.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>

#include <algorithm>
#include <vector>

#include <dirent.h>
#include <mntent.h>
#include <fcntl.h>
#include <pty.h>
#include <sched.h>
#include <signal.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <android-base/logging.h>
#include <android-base/strings.h>

namespace aohp {

namespace {

constexpr int64_t kLogRotateBytes = 10 * 1024 * 1024;

bool mkdirParentsAbsolute(const std::string& path, mode_t mode) {
    if (path.empty() || path[0] != '/') return false;
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string partial = path.substr(0, pos);
        if (partial.empty()) continue;
        if (mkdir(partial.c_str(), mode) != 0 && errno != EEXIST) {
            PLOG(ERROR) << "mkdirParentsAbsolute " << partial;
            return false;
        }
    }
    return true;
}

bool ensureDir(const std::string& path, mode_t mode) {
    if (!mkdirParentsAbsolute(path, mode)) return false;
    if (mkdir(path.c_str(), mode) != 0 && errno != EEXIST) {
        PLOG(ERROR) << "ensureDir mkdir " << path;
        return false;
    }
    struct stat sb;
    if (stat(path.c_str(), &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        LOG(ERROR) << "ensureDir not a directory: " << path;
        return false;
    }
    return true;
}

void tryUnshareMountNs() {
    if (unshare(CLONE_NEWNS) != 0) {
        PLOG(WARNING) << "unshare(CLONE_NEWNS) failed, using host mount namespace";
        return;
    }
    // A freshly unshared mount namespace inherits MS_SHARED propagation on most mounts.
    // Without flipping the root to MS_SLAVE (same trick zygote / init's SetUpMountNamespace
    // uses), every bind mount in this child — /proc, /dev, /sys, /sdcard, and especially
    // each `newinstance` devpts — propagates back to the host namespace and survives the
    // child's exit. After ~10 execSyncs the host accumulates dozens of stale mounts plus a
    // new devpts instance each time, which bogs down the whole VM and eventually makes
    // setupBindMounts() fail for all subsequent commands.
    if (mount("rootfs", "/", nullptr, MS_SLAVE | MS_REC, nullptr) != 0) {
        PLOG(WARNING) << "make-rslave / failed; child mounts may leak to host namespace";
    }
}

// Node/V8 allocates RW->RX pages for JIT by default. On some Android / hardened
// kernels those mmaps fail (V8 CHECK: errno 12 ENOMEM). --jitless avoids runtime
// executable mappings; fine for the bundled aohp CLI in AOHP containers.
void setContainerNodeOptionsJitless() {
    const char* existing = getenv("NODE_OPTIONS");
    // --jitless: avoid executable mmap failures on hardened/Android environments.
    // --no-warnings: suppress V8 noise when --jitless disables --expose_wasm (harmless).
    std::string merged = "--jitless --no-warnings";
    if (existing != nullptr && existing[0] != '\0') {
        merged = std::string(existing) + " --jitless --no-warnings";
    }
    setenv("NODE_OPTIONS", merged.c_str(), 1);
}

void setContainerChildCommonEnv() {
    setenv("HOME", "/root", 1);
    setenv("PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin", 1);
    setenv("TERM", "xterm-256color", 1);
    setenv("LANG", "C.UTF-8", 1);
    // Quiets Node/V8 noise (e.g. --jitless vs default --expose_wasm) without hiding user code warnings.
    setenv("NODE_NO_WARNINGS", "1", 0);
    setContainerNodeOptionsJitless();
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    o.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '\\':
                o += "\\\\";
                break;
            case '"':
                o += "\\\"";
                break;
            case '\n':
                o += "\\n";
                break;
            case '\r':
                o += "\\r";
                break;
            case '\t':
                o += "\\t";
                break;
            default:
                o += c;
                break;
        }
    }
    return o;
}

std::string extractJsonStringField(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\":\"";
    size_t p = j.find(pat);
    if (p == std::string::npos) return "";
    p += pat.size();
    std::string o;
    while (p < j.size()) {
        if (j[p] == '\\' && p + 1 < j.size()) {
            o += j[p + 1];
            p += 2;
            continue;
        }
        if (j[p] == '"') break;
        o += j[p++];
    }
    return o;
}

long long extractJsonIntField(const std::string& j, const char* key) {
    std::string pat = std::string("\"") + key + "\":";
    size_t p = j.find(pat);
    if (p == std::string::npos) return 0;
    p += pat.size();
    while (p < j.size() && (j[p] == ' ' || j[p] == '\t')) ++p;
    return strtoll(j.c_str() + p, nullptr, 10);
}

}  // namespace

ContainerManager::ContainerManager() {
    mkdir(CONTAINER_BASE_DIR, 0770);
    mkdir(SHARED_BASE, 0770);
    mkdir(SHARED_NPM_CACHE, 0770);
    mkdir(SHARED_OPENCLAW_DEV, 0770);
    mkdir(SHARED_UDA, 0770);
    mCgroup_.loadConfig(AOHP_CGROUP_CONF);
}

std::string ContainerManager::envPath(const std::string& name) {
    return std::string(CONTAINER_BASE_DIR) + "/" + name;
}

std::string ContainerManager::rootfsPath(const std::string& name) {
    return envPath(name) + "/rootfs";
}

std::string ContainerManager::templatePath(const std::string& templateName) {
    return std::string(TEMPLATE_DIR) + "/" + templateName + ".tar.gz";
}

std::string ContainerManager::templateRecordPath(const std::string& name) {
    return envPath(name) + "/.template";
}

std::string ContainerManager::servicesDirPath(const std::string& name) {
    return envPath(name) + "/services";
}

bool ContainerManager::writeTemplateRecord(const std::string& name, const std::string& templateName) {
    std::string path = templateRecordPath(name);
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) {
        PLOG(ERROR) << "writeTemplateRecord " << path;
        return false;
    }
    write(fd, templateName.data(), templateName.size());
    write(fd, "\n", 1);
    close(fd);
    return true;
}

std::string ContainerManager::readTemplateRecord(const std::string& name) {
    std::string path = templateRecordPath(name);
    std::ifstream in(path.c_str());
    if (!in) return "";
    std::string line;
    if (!std::getline(in, line)) return "";
    return android::base::Trim(line);
}

std::string ContainerManager::firstTemplateTarInTemplateDir() {
    DIR* dir = opendir(TEMPLATE_DIR);
    if (!dir) return "";
    std::string tpl;
    struct dirent* e;
    while ((e = readdir(dir)) != nullptr) {
        std::string fn(e->d_name);
        if (fn.size() > 7 && fn.substr(fn.size() - 7) == ".tar.gz") {
            tpl = std::string(TEMPLATE_DIR) + "/" + fn;
            break;
        }
    }
    closedir(dir);
    return tpl;
}

std::vector<std::string> ContainerManager::listContainers() {
    std::vector<std::string> result;
    DIR* dir = opendir(CONTAINER_BASE_DIR);
    if (!dir) return result;
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_type == DT_DIR && entry->d_name[0] != '.') {
            std::string rootfs = rootfsPath(entry->d_name);
            struct stat st;
            if (stat(rootfs.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                result.emplace_back(entry->d_name);
            }
        }
    }
    closedir(dir);
    return result;
}

bool ContainerManager::extractTemplate(const std::string& templateTarGz, const std::string& destDir) {
    std::string err;
    if (!extractTarGz(templateTarGz, destDir, &err)) {
        mLastError_ = err.empty() ? "template extract failed" : err;
        LOG(ERROR) << mLastError_;
        return false;
    }
    return true;
}

bool ContainerManager::createContainer(const std::string& name, const std::string& templateName) {
    mLastError_.clear();
    std::string env = envPath(name);
    std::string rootfs = rootfsPath(name);

    struct stat st;
    if (stat(rootfs.c_str(), &st) == 0) {
        mLastError_ = "container already exists: " + name;
        LOG(WARNING) << mLastError_;
        return false;
    }

    if (mkdir(env.c_str(), 0755) != 0 && errno != EEXIST) {
        mLastError_ = std::string("mkdir ") + env + ": " + strerror(errno);
        PLOG(ERROR) << mLastError_;
        return false;
    }

    std::string tpl = templatePath(templateName);
    if (stat(tpl.c_str(), &st) != 0) {
        mLastError_ = "template not found: " + tpl;
        LOG(ERROR) << mLastError_;
        return false;
    }

    if (!extractTemplate(tpl, rootfs)) {
        return false;
    }

    mkdir((rootfs + "/proc").c_str(), 0555);
    mkdir((rootfs + "/dev").c_str(), 0755);
    mkdir((rootfs + "/dev/pts").c_str(), 0755);
    mkdir((rootfs + "/sys").c_str(), 0555);
    mkdir((rootfs + "/sdcard").c_str(), 0755);
    mkdir((rootfs + "/tmp").c_str(), 01777);

    std::string resolv = rootfs + "/etc/resolv.conf";
    int fd = open(resolv.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char* dns = "nameserver 8.8.8.8\nnameserver 8.8.4.4\n";
        write(fd, dns, strlen(dns));
        close(fd);
    }

    if (!writeTemplateRecord(name, templateName)) {
        mLastError_ = "failed to write .template record";
        return false;
    }

    if (!mCgroup_.createForContainer(name)) {
        LOG(WARNING) << "cgroup create failed for " << name << " (continuing without limits)";
    }

    LOG(INFO) << "Container " << name << " created from template " << templateName;
    return true;
}

static int removePathRecursive(const std::string& path) {
    struct stat st;
    if (lstat(path.c_str(), &st) != 0) {
        return errno == ENOENT ? 0 : -1;
    }
    if (S_ISDIR(st.st_mode)) {
        DIR* d = opendir(path.c_str());
        if (!d) return -1;
        while (struct dirent* e = readdir(d)) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
            if (removePathRecursive(path + "/" + e->d_name) != 0) {
                closedir(d);
                return -1;
            }
        }
        closedir(d);
        if (rmdir(path.c_str()) != 0 && errno != ENOENT) return -1;
        return 0;
    }
    if (unlink(path.c_str()) != 0 && errno != ENOENT) return -1;
    return 0;
}

static int recursiveRemove(const std::string& path) { return removePathRecursive(path); }

/** Unmount every mount point under {@code rootfs} (deepest first). Needed when fixed-path
 *  teardownBindMounts misses a bind mount or a previous umount failed — otherwise rmdir(rootfs)
 *  fails with EBUSY/ENOTEMPTY. */
static void umountAllUnderRootfs(const std::string& rootfs) {
    if (rootfs.empty()) return;
    FILE* fp = setmntent("/proc/mounts", "r");
    if (!fp) {
        PLOG(WARNING) << "setmntent /proc/mounts";
        return;
    }
    std::vector<std::string> mounts;
    struct mntent* e;
    while ((e = getmntent(fp)) != nullptr) {
        std::string mp(e->mnt_dir);
        if (mp.empty()) continue;
        if (mp == rootfs ||
            (mp.size() > rootfs.size() && mp[rootfs.size()] == '/' &&
             mp.compare(0, rootfs.size(), rootfs) == 0)) {
            mounts.push_back(std::move(mp));
        }
    }
    endmntent(fp);
    std::sort(mounts.begin(), mounts.end(), [](const std::string& a, const std::string& b) {
        return a.size() > b.size();
    });
    for (const auto& mp : mounts) {
        if (umount2(mp.c_str(), MNT_DETACH) != 0 && errno != EINVAL && errno != ENOENT) {
            PLOG(WARNING) << "umount2 " << mp;
        }
    }
}

void ContainerManager::killContainerProcesses(const std::string& rootfs) {
    DIR* proc = opendir("/proc");
    if (!proc) return;
    struct dirent* entry;
    while ((entry = readdir(proc)) != nullptr) {
        if (entry->d_type != DT_DIR) continue;
        char* endp;
        long pid = strtol(entry->d_name, &endp, 10);
        if (*endp != '\0' || pid <= 1) continue;

        char link[PATH_MAX];
        std::string rootLink = std::string("/proc/") + entry->d_name + "/root";
        ssize_t len = readlink(rootLink.c_str(), link, sizeof(link) - 1);
        if (len <= 0) continue;
        link[len] = '\0';
        if (std::string(link).find(rootfs) == 0) {
            kill(static_cast<pid_t>(pid), SIGKILL);
        }
    }
    closedir(proc);
    usleep(100000);
}

bool ContainerManager::teardownBindMounts(const std::string& rootfs) {
    const char* mounts[] = {
            "/etc/resolv.conf",
            "/opt/udagen/workspace",
            "/opt/openclaw-dev",
            "/root/.npm",
            "/proc",
            "/dev/pts",
            "/dev",
            "/sys",
            "/sdcard",
    };
    bool ok = true;
    for (size_t i = 0; i < sizeof(mounts) / sizeof(mounts[0]); ++i) {
        std::string target = rootfs + mounts[i];
        if (umount2(target.c_str(), MNT_DETACH) != 0 && errno != EINVAL && errno != ENOENT) {
            PLOG(WARNING) << "umount " << target;
            ok = false;
        }
    }
    return ok;
}

bool ContainerManager::destroyContainer(const std::string& name) {
    mLastError_.clear();
    {
        std::lock_guard<std::mutex> lock(mWorkDirMutex_);
        mWorkDir_.erase(name);
    }
    std::string rootfs = rootfsPath(name);
    std::string env = envPath(name);

    bool removed = false;
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (attempt > 0) {
            usleep(200000);
        }
        killContainerProcesses(rootfs);
        teardownBindMounts(rootfs);
        umountAllUnderRootfs(rootfs);
        mCgroup_.destroyForContainer(name);

        if (recursiveRemove(env) == 0) {
            removed = true;
            break;
        }
        PLOG(WARNING) << "destroyContainer: recursiveRemove failed attempt " << (attempt + 1) << " for "
                      << env;
    }
    if (!removed) {
        mLastError_ = "Failed to remove " + env + " (busy mount or unreleased file; check logcat aohp-containerd)";
        LOG(ERROR) << mLastError_;
        return false;
    }
    LOG(INFO) << "Container " << name << " destroyed";
    return true;
}

bool ContainerManager::resetContainer(const std::string& name) {
    {
        std::lock_guard<std::mutex> lock(mWorkDirMutex_);
        mWorkDir_.erase(name);
    }
    std::string rootfs = rootfsPath(name);
    std::string env = envPath(name);

    killContainerProcesses(rootfs);
    teardownBindMounts(rootfs);
    umountAllUnderRootfs(rootfs);
    if (recursiveRemove(rootfs) != 0) {
        LOG(ERROR) << "reset: failed to remove rootfs " << rootfs;
        return false;
    }

    std::string tpl;
    std::string recorded = readTemplateRecord(name);
    if (!recorded.empty()) {
        tpl = templatePath(recorded);
        struct stat st;
        if (stat(tpl.c_str(), &st) != 0) {
            LOG(WARNING) << "recorded template missing, falling back: " << tpl;
            tpl.clear();
        }
    }
    if (tpl.empty()) {
        tpl = firstTemplateTarInTemplateDir();
    }
    if (tpl.empty()) {
        LOG(ERROR) << "No template found for reset";
        return false;
    }

    if (!extractTemplate(tpl, rootfs)) {
        return false;
    }

    mkdir((rootfs + "/proc").c_str(), 0555);
    mkdir((rootfs + "/dev").c_str(), 0755);
    mkdir((rootfs + "/dev/pts").c_str(), 0755);
    mkdir((rootfs + "/sys").c_str(), 0555);
    mkdir((rootfs + "/sdcard").c_str(), 0755);
    mkdir((rootfs + "/tmp").c_str(), 01777);

    std::string resolv = rootfs + "/etc/resolv.conf";
    int fd = open(resolv.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        const char* dns = "nameserver 8.8.8.8\nnameserver 8.8.4.4\n";
        write(fd, dns, strlen(dns));
        close(fd);
    }

    mCgroup_.destroyForContainer(name);
    mCgroup_.createForContainer(name);

    LOG(INFO) << "Container " << name << " reset";
    return true;
}

bool ContainerManager::setupBindMounts(const std::string& rootfs) {
    auto doMount = [&](const char* src, const char* rel, unsigned long flags) -> bool {
        std::string target = rootfs + rel;
        if (!ensureDir(target, 0755)) return false;
        if (mount(src, target.c_str(), nullptr, MS_BIND | flags, nullptr) != 0) {
            PLOG(ERROR) << "bind mount " << src << " -> " << target;
            return false;
        }
        return true;
    };

    if (!doMount("/proc", "/proc", 0)) return false;
    if (!doMount("/dev", "/dev", 0)) return false;
    if (!doMount("/sys", "/sys", MS_RDONLY)) return false;

    std::string devpts = rootfs + "/dev/pts";
    if (!ensureDir(devpts, 0755)) return false;
    if (mount("devpts", devpts.c_str(), "devpts", 0, "newinstance,ptmxmode=0666") != 0) {
        PLOG(WARNING) << "devpts mount failed (PTY in container may not work)";
    }

    struct stat st;
    if (stat("/sdcard", &st) == 0) {
        doMount("/sdcard", "/sdcard", 0);
    } else if (stat("/storage/emulated/0", &st) == 0) {
        doMount("/storage/emulated/0", "/sdcard", 0);
    }

    if (stat(SHARED_NPM_CACHE, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (ensureDir(rootfs + "/root", 0755) && ensureDir(rootfs + "/root/.npm", 0755)) {
            if (mount(SHARED_NPM_CACHE, (rootfs + "/root/.npm").c_str(), nullptr, MS_BIND, nullptr) !=
                0) {
                PLOG(WARNING) << "bind mount npm cache failed";
            }
        }
    }
    if (stat(SHARED_OPENCLAW_DEV, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (ensureDir(rootfs + "/opt", 0755) && ensureDir(rootfs + "/opt/openclaw-dev", 0755)) {
            if (mount(SHARED_OPENCLAW_DEV, (rootfs + "/opt/openclaw-dev").c_str(), nullptr,
                      MS_BIND, nullptr) != 0) {
                PLOG(WARNING) << "bind mount openclaw-dev failed";
            }
        }
    }
    if (stat(SHARED_UDA, &st) == 0 && S_ISDIR(st.st_mode)) {
        if (ensureDir(rootfs + "/opt/udagen", 0755) &&
            ensureDir(rootfs + "/opt/udagen/workspace", 0755)) {
            if (mount(SHARED_UDA, (rootfs + "/opt/udagen/workspace").c_str(), nullptr, MS_BIND,
                      nullptr) != 0) {
                PLOG(WARNING) << "bind mount uda workspace failed";
            }
        }
    }
    // Use Android host DNS (netd on 127.0.0.1); template resolv.conf (8.8.8.8) often fails in chroot.
    if (stat("/etc/resolv.conf", &st) == 0 && S_ISREG(st.st_mode)) {
        std::string target = rootfs + "/etc/resolv.conf";
        if (ensureDir(rootfs + "/etc", 0755)) {
            if (mount("/etc/resolv.conf", target.c_str(), nullptr, MS_BIND | MS_RDONLY,
                      nullptr) != 0) {
                PLOG(WARNING) << "bind mount resolv.conf failed";
            }
        }
    }
    return true;
}

int ContainerManager::forkIntoContainer(const std::string& containerName, const std::string& rootfs,
                                       const char* const argv[], bool usePty) {
    int masterFd = -1;
    int pipeFds[2] = {-1, -1};

    if (!usePty) {
        if (pipe(pipeFds) < 0) {
            PLOG(ERROR) << "pipe";
            return -1;
        }
    }

    pid_t pid;
    if (usePty) {
        pid = forkpty(&masterFd, nullptr, nullptr, nullptr);
    } else {
        pid = fork();
    }

    if (pid < 0) {
        PLOG(ERROR) << "fork/forkpty";
        return -1;
    }

    if (pid == 0) {
        tryUnshareMountNs();
        if (!setupBindMounts(rootfs)) {
            _exit(125);
        }
        mCgroup_.joinContainerCgroup(containerName, getpid());

        if (chroot(rootfs.c_str()) != 0) {
            PLOG(ERROR) << "chroot " << rootfs;
            _exit(126);
        }
        chdir("/");

        setContainerChildCommonEnv();

        if (!usePty) {
            close(pipeFds[0]);
            dup2(pipeFds[1], STDOUT_FILENO);
            dup2(pipeFds[1], STDERR_FILENO);
            close(pipeFds[1]);
        }

        execvp(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }

    if (usePty) {
        return masterFd;
    }
    close(pipeFds[1]);
    return pipeFds[0];
}

ExecResult ContainerManager::execSync(const std::string& name, const std::string& command,
                                      int timeoutMs) {
    ExecResult result{-1, "", ""};
    std::string rootfs = rootfsPath(name);

    struct stat st;
    if (stat(rootfs.c_str(), &st) != 0) {
        result.stderrStr = "Container not found: " + name;
        return result;
    }

    std::string workDir = "/";
    {
        std::lock_guard<std::mutex> lock(mWorkDirMutex_);
        auto it = mWorkDir_.find(name);
        if (it != mWorkDir_.end() && !it->second.empty() && it->second[0] == '/') {
            workDir = it->second;
        }
    }

    int stdoutPipe[2], stderrPipe[2];
    if (pipe(stdoutPipe) < 0 || pipe(stderrPipe) < 0) {
        result.stderrStr = "pipe() failed";
        return result;
    }

    pid_t pid = fork();
    if (pid < 0) {
        result.stderrStr = "fork() failed";
        return result;
    }

    if (pid == 0) {
        close(stdoutPipe[0]);
        close(stderrPipe[0]);

        tryUnshareMountNs();
        if (!setupBindMounts(rootfs)) {
            const char* msg = "aohp-containerd: setupBindMounts failed\n";
            write(stderrPipe[1], msg, strlen(msg));
            _exit(125);
        }
        // Best-effort cgroup; failures are logged inside joinContainerCgroup (never stderr).
        mCgroup_.joinContainerCgroup(name, getpid());

        if (chroot(rootfs.c_str()) != 0) {
            _exit(126);
        }
        chdir("/");

        setContainerChildCommonEnv();

        dup2(stdoutPipe[1], STDOUT_FILENO);
        dup2(stderrPipe[1], STDERR_FILENO);
        close(stdoutPipe[1]);
        close(stderrPipe[1]);

        setenv("AOHP_WD", workDir.c_str(), 1);
        setenv("AOHP_CMD", command.c_str(), 1);
        const char* argv[] = {
                "/bin/sh",
                "-c",
                "cd \"${AOHP_WD}\" 2>/dev/null || cd /; "
                "eval \"${AOHP_CMD}\"; ec=$?; "
                "pwd > /tmp/.aohp_lastpwd; "
                "exit $ec",
                nullptr,
        };
        execvp(argv[0], const_cast<char* const*>(argv));
        _exit(127);
    }

    close(stdoutPipe[1]);
    close(stderrPipe[1]);

    auto readAll = [](int fd) -> std::string {
        std::string out;
        char buf[4096];
        ssize_t n;
        while ((n = read(fd, buf, sizeof(buf))) > 0) {
            out.append(buf, n);
        }
        close(fd);
        return out;
    };

    result.stdoutStr = readAll(stdoutPipe[0]);
    result.stderrStr = readAll(stderrPipe[0]);

    int status = 0;
    if (timeoutMs > 0) {
        alarm(timeoutMs / 1000 + 1);
    }
    waitpid(pid, &status, 0);
    alarm(0);

    result.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;

    std::string pwdFile = rootfs + "/tmp/.aohp_lastpwd";
    std::ifstream in(pwdFile.c_str());
    std::string line;
    if (std::getline(in, line)) {
        std::string p = android::base::Trim(line);
        if (!p.empty() && p[0] == '/') {
            std::lock_guard<std::mutex> lock(mWorkDirMutex_);
            mWorkDir_[name] = std::move(p);
        }
    }

    return result;
}

int ContainerManager::openShell(const std::string& name) {
    std::string rootfs = rootfsPath(name);
    struct stat st;
    if (stat(rootfs.c_str(), &st) != 0) {
        LOG(ERROR) << "Container not found: " << name;
        return -1;
    }

    const char* argv[] = {"/bin/sh", "-l", nullptr};
    return forkIntoContainer(name, rootfs, argv, true);
}

std::string ContainerManager::templateInfo(const std::string& name) {
    return readTemplateRecord(name);
}

bool ContainerManager::isValidServiceId(const std::string& id) {
    if (id.empty() || id.size() > 64) return false;
    for (char c : id) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '-' || c == '_')) {
            return false;
        }
    }
    return true;
}

long ContainerManager::startService(const std::string& name, const std::string& serviceId,
                                    const std::string& command) {
    mLastError_.clear();
    if (!isValidServiceId(serviceId)) {
        mLastError_ = "invalid serviceId";
        return -1;
    }
    std::string rootfs = rootfsPath(name);
    struct stat st;
    if (stat(rootfs.c_str(), &st) != 0) {
        mLastError_ = "Container not found";
        return -1;
    }

    std::string sdir = servicesDirPath(name);
    if (!ensureDir(sdir, 0755)) {
        mLastError_ = "mkdir services failed";
        return -1;
    }

    std::string pidPath = sdir + "/" + serviceId + ".pid";
    std::string logPath = sdir + "/" + serviceId + ".log";
    std::string metaPath = sdir + "/" + serviceId + ".meta";

    if (stat(logPath.c_str(), &st) == 0 && st.st_size > kLogRotateBytes) {
        truncate(logPath.c_str(), 0);
    }

    int syncPipe[2];
    if (pipe(syncPipe) != 0) {
        mLastError_ = "pipe failed";
        return -1;
    }

    pid_t pid1 = fork();
    if (pid1 < 0) {
        close(syncPipe[0]);
        close(syncPipe[1]);
        mLastError_ = "fork failed";
        return -1;
    }

    if (pid1 == 0) {
        close(syncPipe[0]);
        if (setsid() < 0) {
            PLOG(ERROR) << "setsid";
            _exit(1);
        }
        pid_t pid2 = fork();
        if (pid2 < 0) {
            _exit(1);
        }
        if (pid2 == 0) {
            close(syncPipe[1]);
            tryUnshareMountNs();
            if (!setupBindMounts(rootfs)) {
                _exit(125);
            }
            mCgroup_.joinContainerCgroup(name, getpid());
            int logfd = open(logPath.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (logfd >= 0) {
                dup2(logfd, STDOUT_FILENO);
                dup2(logfd, STDERR_FILENO);
                if (logfd > STDERR_FILENO) close(logfd);
            }
            if (chroot(rootfs.c_str()) != 0) {
                _exit(126);
            }
            chdir("/");
            setContainerChildCommonEnv();
            const char* argv[] = {"/bin/sh", "-c", command.c_str(), nullptr};
            execvp(argv[0], const_cast<char* const*>(argv));
            _exit(127);
        }

        std::string pidStr = std::to_string(static_cast<int>(pid2));
        int pfd = open(pidPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (pfd >= 0) {
            write(pfd, pidStr.c_str(), pidStr.size());
            write(pfd, "\n", 1);
            close(pfd);
        }
        long long t = static_cast<long long>(time(nullptr));
        std::string meta = std::string("{\"serviceId\":\"") + jsonEscape(serviceId) +
                           "\",\"pid\":" + pidStr + ",\"startTime\":" + std::to_string(t) +
                           ",\"command\":\"" + jsonEscape(command) + "\"}";
        int mfd = open(metaPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (mfd >= 0) {
            write(mfd, meta.c_str(), meta.size());
            write(mfd, "\n", 1);
            close(mfd);
        }
        char one = 1;
        write(syncPipe[1], &one, 1);
        close(syncPipe[1]);
        _exit(0);
    }

    close(syncPipe[1]);
    char buf[1];
    read(syncPipe[0], buf, sizeof(buf));
    close(syncPipe[0]);
    waitpid(pid1, nullptr, 0);

    std::ifstream pin(pidPath.c_str());
    std::string line;
    long outPid = -1;
    if (std::getline(pin, line)) {
        outPid = strtol(line.c_str(), nullptr, 10);
    }
    return outPid;
}

bool ContainerManager::stopService(const std::string& name, const std::string& serviceId) {
    if (!isValidServiceId(serviceId)) return false;
    std::string pidPath = servicesDirPath(name) + "/" + serviceId + ".pid";
    std::ifstream in(pidPath.c_str());
    std::string line;
    if (!std::getline(in, line)) {
        return false;
    }
    pid_t p = static_cast<pid_t>(strtol(line.c_str(), nullptr, 10));
    if (p <= 1) return false;
    kill(p, SIGTERM);
    usleep(500000);
    if (kill(p, 0) == 0) {
        kill(p, SIGKILL);
    }
    unlink(pidPath.c_str());
    return true;
}

std::string ContainerManager::listServicesJson(const std::string& name) {
    std::string sdir = servicesDirPath(name);
    DIR* d = opendir(sdir.c_str());
    if (!d) {
        return "[]";
    }
    std::ostringstream oss;
    oss << "[";
    bool first = true;
    struct dirent* e;
    while ((e = readdir(d)) != nullptr) {
        std::string fn(e->d_name);
        if (fn.size() <= 5 || fn.substr(fn.size() - 5) != ".meta") continue;
        std::string sid = fn.substr(0, fn.size() - 5);
        std::string metaPath = sdir + "/" + fn;
        std::ifstream min(metaPath.c_str());
        std::string metaJson;
        if (std::getline(min, metaJson)) {
            metaJson = android::base::Trim(metaJson);
        }
        std::string pidPath = sdir + "/" + sid + ".pid";
        std::ifstream pin(pidPath.c_str());
        std::string pline;
        int pid = -1;
        if (std::getline(pin, pline)) {
            pid = static_cast<int>(strtol(pline.c_str(), nullptr, 10));
        }
        bool alive = (pid > 0 && kill(static_cast<pid_t>(pid), 0) == 0);
        long long startTime = extractJsonIntField(metaJson, "startTime");
        long long uptime = 0;
        if (alive && startTime > 0) {
            uptime = static_cast<long long>(time(nullptr)) - startTime;
        }
        std::string cmd = extractJsonStringField(metaJson, "command");
        if (!first) oss << ",";
        first = false;
        oss << "{\"serviceId\":\"" << jsonEscape(sid) << "\",\"pid\":" << pid
            << ",\"alive\":" << (alive ? "true" : "false") << ",\"startTime\":" << startTime
            << ",\"uptimeSec\":" << uptime << ",\"command\":\"" << jsonEscape(cmd) << "\"}";
    }
    closedir(d);
    oss << "]";
    return oss.str();
}

std::string ContainerManager::serviceLogTail(const std::string& name, const std::string& serviceId,
                                               int tailBytes) {
    if (!isValidServiceId(serviceId)) return "";
    if (tailBytes <= 0 || tailBytes > 4 * 1024 * 1024) tailBytes = 65536;
    std::string path = servicesDirPath(name) + "/" + serviceId + ".log";
    int fd = open(path.c_str(), O_RDONLY);
    if (fd < 0) return "";
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return "";
    }
    off_t sz = st.st_size;
    off_t off = 0;
    if (sz > tailBytes) {
        off = sz - tailBytes;
    }
    if (lseek(fd, off, SEEK_SET) < 0) {
        close(fd);
        return "";
    }
    std::string out;
    out.resize(static_cast<size_t>(sz - off));
    ssize_t n = read(fd, &out[0], out.size());
    close(fd);
    if (n < 0) return "";
    out.resize(static_cast<size_t>(n));
    return out;
}

std::string ContainerManager::getUsageJson(const std::string& name) {
    return mCgroup_.getUsageJson(name);
}

std::string ContainerManager::diagnose(const std::string& name) {
    std::string tpl = readTemplateRecord(name);
    std::string cg = mCgroup_.diagnoseJson(name);
    struct stat st;
    bool npmHost = stat(SHARED_NPM_CACHE, &st) == 0 && S_ISDIR(st.st_mode);
    bool devHost = stat(SHARED_OPENCLAW_DEV, &st) == 0 && S_ISDIR(st.st_mode);
    std::string rootfs = rootfsPath(name);
    bool rootfsOk = stat(rootfs.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
    std::ostringstream oss;
    oss << "{\"container\":\"" << jsonEscape(name) << "\",\"template\":\"" << jsonEscape(tpl)
        << "\",\"rootfsExists\":" << (rootfsOk ? "true" : "false") << ",\"npmCacheHostDir\":"
        << (npmHost ? "true" : "false") << ",\"openclawDevHostDir\":" << (devHost ? "true" : "false")
        << ",\"cgroup\":" << cg << "}";
    return oss.str();
}

void ContainerManager::adoptOrphanServicePids() {
    std::vector<std::string> envs = listContainers();
    for (const auto& name : envs) {
        std::string sdir = servicesDirPath(name);
        DIR* d = opendir(sdir.c_str());
        if (!d) continue;
        struct dirent* e;
        while ((e = readdir(d)) != nullptr) {
            std::string fn(e->d_name);
            if (fn.size() <= 4 || fn.substr(fn.size() - 4) != ".pid") continue;
            std::string path = sdir + "/" + fn;
            std::ifstream in(path.c_str());
            std::string line;
            if (!std::getline(in, line)) continue;
            int pid = static_cast<int>(strtol(line.c_str(), nullptr, 10));
            if (pid <= 1) {
                unlink(path.c_str());
                continue;
            }
            if (kill(static_cast<pid_t>(pid), 0) != 0) {
                LOG(INFO) << "Removing stale pid file " << path << " for dead pid " << pid;
                unlink(path.c_str());
            }
        }
        closedir(d);
    }
}

}  // namespace aohp
