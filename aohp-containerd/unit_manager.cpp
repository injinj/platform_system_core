/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#define LOG_TAG "aohp-containerd"

#include "unit_manager.h"

#include "container_manager.h"
#include "protocol.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
extern char** environ;

#include <android-base/logging.h>

namespace aohp {

namespace {

constexpr off_t kUnitLogRotateBytes = 1 * 1024 * 1024;
constexpr const char* kSystemDirRel = "/etc/aohp/system";
constexpr const char* kWantsDirRel = "/etc/aohp/system/aohp.target.wants";
constexpr const char* kEnvNameRel = "/etc/aohp/env-name";

std::string readFileStr(const std::string& path, bool* ok = nullptr) {
    std::ifstream in(path.c_str(), std::ios::binary);
    if (!in) {
        if (ok) *ok = false;
        return "";
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (ok) *ok = true;
    return ss.str();
}

bool writeFileStr(const std::string& path, const std::string& data, mode_t mode = 0644) {
    int fd = open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, mode);
    if (fd < 0) return false;
    ssize_t n = write(fd, data.data(), data.size());
    close(fd);
    return n == static_cast<ssize_t>(data.size());
}

bool mkdirP(const std::string& path, mode_t mode) {
    size_t pos = 0;
    while ((pos = path.find('/', pos + 1)) != std::string::npos) {
        std::string partial = path.substr(0, pos);
        if (!partial.empty() && mkdir(partial.c_str(), mode) != 0 && errno != EEXIST) return false;
    }
    if (mkdir(path.c_str(), mode) != 0 && errno != EEXIST) return false;
    return true;
}

bool pathExists(const std::string& p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

std::string withSuffix(const std::string& n, const char* dflt) {
    if (n.size() > 8 && n.compare(n.size() - 8, 8, ".service") == 0) return n;
    if (n.size() > 6 && n.compare(n.size() - 6, 6, ".timer") == 0) return n;
    return n + dflt;
}

bool validUnitName(const std::string& n) {
    if (n.empty() || n.size() > 96) return false;
    for (char c : n) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' ||
              c == '_' || c == '.' || c == '@' || c == ':')) {
            return false;
        }
    }
    return n.find("..") == std::string::npos && n[0] != '.';
}

const char* signalName(int sig) {
    switch (sig) {
        case SIGHUP: return "HUP";
        case SIGINT: return "INT";
        case SIGQUIT: return "QUIT";
        case SIGABRT: return "ABRT";
        case SIGKILL: return "KILL";
        case SIGSEGV: return "SEGV";
        case SIGPIPE: return "PIPE";
        case SIGTERM: return "TERM";
        case SIGUSR1: return "USR1";
        case SIGUSR2: return "USR2";
        default: return "";
    }
}

}  // namespace

double monoNow() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<double>(ts.tv_sec) + ts.tv_nsec / 1e9;
}

UnitManager::UnitManager(ContainerManager& cm) : mCm_(cm) {}
UnitManager::~UnitManager() {}

void UnitManager::start() {
    std::lock_guard<std::mutex> lk(mEnvsMu_);
    if (mSchedulerStarted_) return;
    mSchedulerStarted_ = true;
    std::thread([this] { schedulerLoop(); }).detach();
}

std::string UnitManager::envDir(const std::string& env) { return std::string(CONTAINER_BASE_DIR) + "/" + env; }
std::string UnitManager::systemDir(const std::string& env) { return envDir(env) + "/rootfs" + kSystemDirRel; }
std::string UnitManager::wantsDir(const std::string& env) { return envDir(env) + "/rootfs" + kWantsDirRel; }
std::string UnitManager::stampPath(const std::string& env, const std::string& unit) {
    return envDir(env) + "/.aohp/timers/" + unit + ".stamp";
}
std::string UnitManager::unitLogPath(const std::string& envDir, const std::string& unitName) {
    std::string base = unitName;
    size_t p = base.rfind('.');
    if (p != std::string::npos && (base.substr(p) == ".service" || base.substr(p) == ".timer")) base = base.substr(0, p);
    return envDir + "/.aohp/log/" + base + ".log";
}

std::shared_ptr<EnvUnits> UnitManager::envUnits(const std::string& env, bool create) {
    std::lock_guard<std::mutex> lk(mEnvsMu_);
    auto it = mEnvs_.find(env);
    if (it != mEnvs_.end()) return it->second;
    if (!create) return nullptr;
    auto e = std::make_shared<EnvUnits>();
    e->env = env;
    mEnvs_[env] = e;
    return e;
}

void UnitManager::forgetEnv(const std::string& env) {
    std::shared_ptr<EnvUnits> e;
    {
        std::lock_guard<std::mutex> lk(mEnvsMu_);
        auto it = mEnvs_.find(env);
        if (it == mEnvs_.end()) return;
        e = it->second;
        mEnvs_.erase(it);
    }
    std::lock_guard<std::mutex> lk(e->st);
    for (auto& kv : e->units) {
        kv.second.stopRequested = true;
        kv.second.restartAt = 0;
        kv.second.nextElapse = 0;
        if (kv.second.mainPid > 0) killGroup(kv.second, kv.second.mainPid, SIGKILL);
    }
    e->units.clear();
}

void UnitManager::writeEnvName(const std::string& env) {
    std::string rootfs = envDir(env) + "/rootfs";
    if (!pathExists(rootfs)) return;
    mkdirP(rootfs + "/etc/aohp", 0755);
    writeFileStr(rootfs + kEnvNameRel, env + "\n");
}

// ---------------------------------------------------------------------------------------------
// loading

void UnitManager::loadUnits(EnvUnits& e) {
    std::string sdir = systemDir(e.env);
    std::string wdir = wantsDir(e.env);
    std::map<std::string, UnitDef> fresh;
    std::vector<std::string> warnings;

    DIR* d = opendir(sdir.c_str());
    if (d) {
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string fn(ent->d_name);
            if (fn.empty() || fn[0] == '.') continue;
            bool svc = fn.size() > 8 && fn.compare(fn.size() - 8, 8, ".service") == 0;
            bool tmr = fn.size() > 6 && fn.compare(fn.size() - 6, 6, ".timer") == 0;
            if (!svc && !tmr) continue;
            if (!validUnitName(fn)) {
                warnings.push_back(fn + ": invalid unit name, skipped");
                continue;
            }
            std::string path = sdir + "/" + fn;
            struct stat st;
            if (stat(path.c_str(), &st) != 0 || !S_ISREG(st.st_mode)) continue;
            bool ok;
            std::string text = readFileStr(path, &ok);
            UnitDef def = parseUnitText(fn, text);
            def.path = path;
            if (!ok) def.loadError = "cannot read unit file";
            fresh[fn] = def;
        }
        closedir(d);
    }

    std::lock_guard<std::mutex> lk(e.st);
    // drop units that vanished (keep transient and anything still running)
    for (auto it = e.units.begin(); it != e.units.end();) {
        UnitState& u = it->second;
        if (u.def.transient || fresh.count(it->first)) {
            ++it;
            continue;
        }
        bool busy = u.mainPid > 0 || u.active == "activating" || u.active == "deactivating";
        if (busy) {
            u.fileMissing = true;
            warnings.push_back(it->first + ": unit file removed while active (kept until it stops)");
            ++it;
        } else {
            it = e.units.erase(it);
        }
    }
    for (auto& kv : fresh) {
        UnitState& u = e.units[kv.first];
        bool wasTransient = u.def.transient;
        u.def = kv.second;
        u.fileMissing = false;
        if (wasTransient && u.mainPid > 0) warnings.push_back(kv.first + ": unit file now shadows a transient unit");
        u.enabled = pathExists(wdir + "/" + kv.first);
        for (auto& w : u.def.warnings) warnings.push_back(kv.first + ": " + w);
        if (!u.def.loadError.empty()) warnings.push_back(kv.first + ": " + u.def.loadError);
    }
    // ordering cycle detection on the whole set
    std::set<std::string> all;
    for (auto& kv : e.units) all.insert(kv.first);
    std::vector<std::string> order;
    std::string err;
    if (!orderUnits(e, all, &order, &err)) warnings.push_back(err);
    // timers referencing missing services
    for (auto& kv : e.units) {
        UnitState& u = kv.second;
        if (u.def.kind == UnitKind::Timer && u.def.loadError.empty() && !e.units.count(u.def.timerUnit)) {
            u.def.loadError = "timer unit " + u.def.timerUnit + " not found";
            warnings.push_back(kv.first + ": " + u.def.loadError);
        }
    }
    e.loadWarnings = warnings;
}

bool UnitManager::orderUnits(EnvUnits& e, const std::set<std::string>& names, std::vector<std::string>* out,
                             std::string* err) {
    // Kahn's algorithm; edges a->b mean "a before b". Deterministic (alphabetical) tie-break.
    std::map<std::string, std::set<std::string>> succ;
    std::map<std::string, int> indeg;
    for (const auto& n : names) indeg[n] = 0;
    auto addEdge = [&](const std::string& a, const std::string& b) {
        if (!names.count(a) || !names.count(b) || a == b) return;
        if (succ[a].insert(b).second) indeg[b]++;
    };
    for (const auto& n : names) {
        auto it = e.units.find(n);
        if (it == e.units.end()) continue;
        const UnitDef& d = it->second.def;
        for (const auto& a : d.after) addEdge(withSuffix(a, ".service"), n);
        for (const auto& b : d.before) addEdge(n, withSuffix(b, ".service"));
        // implicit: a timer is ordered after its service's dependencies only; a service's
        // Requires/Wants do not imply ordering in systemd either.
    }
    std::set<std::string> ready;
    for (const auto& kv : indeg) if (kv.second == 0) ready.insert(kv.first);
    out->clear();
    while (!ready.empty()) {
        std::string n = *ready.begin();
        ready.erase(ready.begin());
        out->push_back(n);
        for (const auto& s : succ[n]) {
            if (--indeg[s] == 0) ready.insert(s);
        }
    }
    if (out->size() != names.size()) {
        std::string cyc;
        for (const auto& kv : indeg) if (kv.second > 0) cyc += (cyc.empty() ? "" : " ") + kv.first;
        if (err) *err = "ordering cycle (After=/Before=) between: " + cyc;
        for (const auto& kv : indeg) {
            if (kv.second > 0) {
                auto it = e.units.find(kv.first);
                if (it != e.units.end() && it->second.def.loadError.empty()) it->second.def.loadError = "ordering cycle";
            }
        }
        return false;
    }
    return true;
}

std::set<std::string> UnitManager::closure(EnvUnits& e, const std::set<std::string>& roots) {
    std::set<std::string> seen;
    std::vector<std::string> work(roots.begin(), roots.end());
    while (!work.empty()) {
        std::string n = work.back();
        work.pop_back();
        if (!seen.insert(n).second) continue;
        auto it = e.units.find(n);
        if (it == e.units.end()) continue;
        for (const auto& r : it->second.def.requiresUnits) work.push_back(withSuffix(r, ".service"));
        for (const auto& w : it->second.def.wants) work.push_back(withSuffix(w, ".service"));
    }
    return seen;
}

bool UnitManager::resolveName(EnvUnits& e, const std::string& in, std::string* out) {
    if (in.empty()) return false;
    std::string n = in;
    if (e.units.count(n)) { *out = n; return true; }
    if (e.units.count(n + ".service")) { *out = n + ".service"; return true; }
    if (e.units.count(n + ".timer")) { *out = n + ".timer"; return true; }
    return false;
}

// ---------------------------------------------------------------------------------------------
// process helpers

void UnitManager::killGroup(const UnitState& u, pid_t pid, int sig) {
    if (pid <= 1) return;
    if (u.def.killMode == KillMode::Process) {
        kill(pid, sig);
        return;
    }
    if (killpg(pid, sig) != 0) kill(pid, sig);
}

void UnitManager::rotateLog(const std::string& path) {
    struct stat st;
    if (stat(path.c_str(), &st) == 0 && st.st_size > kUnitLogRotateBytes) {
        std::string old = path + ".1";
        unlink(old.c_str());
        rename(path.c_str(), old.c_str());
    }
}

std::vector<std::string> UnitManager::buildEnv(EnvUnits& e, const UnitState& u, std::vector<std::string>* warnings) {
    std::map<std::string, std::string> env;
    std::string rootfs = envDir(e.env) + "/rootfs";
    if (u.def.hostExec) {
        // Android host process: start from the daemon's own environment (init's exports — BOOTCLASSPATH,
        // ANDROID_*_ROOT, DEX2OATBOOTCLASSPATH, ... — which app_process/ART need), then overlay.
        for (char** ep = environ; ep && *ep; ++ep) {
            const char* eq = strchr(*ep, '=');
            if (!eq) continue;
            std::string k(*ep, eq - *ep);
            if (k.rfind("ANDROID_SOCKET_", 0) == 0) continue;  // the daemon's control socket fd is not inherited
            env[k] = eq + 1;
        }
        env["HOME"] = "/data/local/tmp";
        if (env.find("PATH") == env.end()) env["PATH"] = "/system/bin:/system/xbin:/vendor/bin";
        env["AOHP_ROOTFS"] = rootfs;  // the env's rootfs is not mounted at /, so point the helper at it
    } else {
        env["HOME"] = "/root";
        env["PATH"] = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    }
    env["TERM"] = "xterm-256color";
    env["LANG"] = "C.UTF-8";
    env["NODE_NO_WARNINGS"] = "1";
    env["AOHP_ENV"] = e.env;
    env["AOHP_UNIT"] = u.def.name;
    for (const auto& ef : u.def.environmentFiles) {
        bool ok;
        // EnvironmentFile= is a path inside the env for normal units, a host path for HostExec= units.
        std::string text = readFileStr((u.def.hostExec ? std::string() : rootfs) + ef.first, &ok);
        if (!ok) {
            if (!ef.second && warnings) warnings->push_back("EnvironmentFile " + ef.first + " missing");
            continue;
        }
        std::istringstream iss(text);
        std::string line;
        while (std::getline(iss, line)) {
            size_t b = line.find_first_not_of(" \t");
            if (b == std::string::npos) continue;
            line = line.substr(b);
            if (line[0] == '#') continue;
            if (line.compare(0, 7, "export ") == 0) line = line.substr(7);
            size_t eq = line.find('=');
            if (eq == std::string::npos || eq == 0) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            while (!v.empty() && (v.back() == '\r' || v.back() == ' ' || v.back() == '\t')) v.pop_back();
            if (v.size() >= 2 && ((v.front() == '"' && v.back() == '"') || (v.front() == '\'' && v.back() == '\'')))
                v = v.substr(1, v.size() - 2);
            env[k] = v;
        }
    }
    for (const auto& kv : u.def.environment) env[kv.first] = kv.second;
    std::vector<std::string> out;
    for (const auto& kv : env) out.push_back(kv.first + "=" + kv.second);
    return out;
}

pid_t UnitManager::spawn(EnvUnits& e, const UnitState& u, const std::string& command,
                         const std::vector<std::string>& extraEnv, std::string* err) {
    std::vector<std::string> warnings;
    std::vector<std::string> env = buildEnv(e, u, &warnings);
    for (const auto& x : extraEnv) env.push_back(x);
    std::string logPath = unitLogPath(envDir(e.env), u.def.name);
    mkdirP(envDir(e.env) + "/.aohp/log", 0755);
    SpawnSpec spec;
    spec.host = u.def.hostExec;
    spec.argv = {spec.host ? "/system/bin/sh" : "/bin/sh", "-c", command};
    spec.env = env;
    spec.workDir = u.def.workingDirectory.empty() ? "/" : u.def.workingDirectory;
    spec.workDirOptional = u.def.workingDirectoryOptional || u.def.workingDirectory.empty();
    spec.logPath = logPath;
    std::string serr;
    pid_t pid = mCm_.spawnInContainer(e.env, spec, &serr);
    if (pid <= 0 && err) *err = serr.empty() ? "spawn failed" : serr;
    return pid;
}

int UnitManager::runSync(EnvUnits& e, UnitState& u, const ExecLine& line, double timeoutSec,
                         const std::vector<std::string>& extraEnv, std::string* err) {
    pid_t pid = spawn(e, u, line.command, extraEnv, err);
    if (pid <= 0) return -1;
    double deadline = monoNow() + (timeoutSec > 0 ? timeoutSec : 90.0);
    int status = 0;
    bool killed = false;
    while (true) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) break;
        if (r < 0 && errno != EINTR) {
            if (err) *err = std::string("waitpid: ") + strerror(errno);
            return -1;
        }
        if (monoNow() > deadline) {
            if (!killed) {
                killGroup(u, pid, SIGTERM);
                killed = true;
                deadline = monoNow() + 5.0;
            } else {
                killGroup(u, pid, SIGKILL);
                waitpid(pid, &status, 0);
                if (err) *err = "timeout";
                return -2;
            }
        }
        usleep(50000);
    }
    // reap anything left in the group
    killGroup(u, pid, SIGKILL);
    if (killed) {
        if (err) *err = "timeout";
        return -2;
    }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

bool UnitManager::isCleanExit(const UnitDef& d, int code, int sig) {
    if (sig > 0) {
        if (sig == SIGHUP || sig == SIGINT || sig == SIGTERM || sig == SIGPIPE) return true;
        for (int s : d.successExitSignals) if (s == sig) return true;
        return false;
    }
    if (code == 0) return true;
    for (int c : d.successExitStatus) if (c == code) return true;
    return false;
}

bool UnitManager::conditionsHold(EnvUnits& e, const UnitState& u) {
    std::string rootfs = envDir(e.env) + "/rootfs";
    for (const auto& c : u.def.conditionPathExists) {
        bool ex = pathExists(rootfs + c.first);
        if (c.second ? ex : !ex) return false;
    }
    return true;
}

void UnitManager::watch(std::shared_ptr<EnvUnits> e, const std::string& name, pid_t pid, int generation) {
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return;
    }
    int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
    bool propagate = false;
    {
        std::lock_guard<std::mutex> lk(e->st);
        auto it = e->units.find(name);
        if (it == e->units.end()) return;
        UnitState& u = it->second;
        if (u.generation != generation || u.mainPid != pid) return;  // stale
        // control-group semantics: the main process is gone, take the rest of its group with it
        killGroup(u, pid, SIGTERM);
        u.mainPid = 0;
        u.exitCode = code;
        u.exitSignal = sig;
        time_t now = time(nullptr);
        bool clean = isCleanExit(u.def, code, sig);
        LOG(INFO) << "unit " << e->env << "/" << name << " main process " << pid << " exited code=" << code
                  << " signal=" << sig << (clean ? " (clean)" : " (failure)") << (u.stopRequested ? " [stop requested]" : "");
        if (u.stopRequested || u.active == "deactivating") {
            u.active = "inactive";
            u.sub = "dead";
            u.result = "success";
            u.inactiveEnterTime = now;
        } else {
            bool restart = false;
            switch (u.def.restart) {
                case RestartPolicy::No: restart = false; break;
                case RestartPolicy::Always: restart = true; break;
                case RestartPolicy::OnFailure: restart = !clean; break;
                case RestartPolicy::OnSuccess: restart = clean; break;
                case RestartPolicy::OnAbnormal: restart = (sig > 0 && !clean); break;
            }
            if (u.def.type == ServiceType::Oneshot) restart = false;
            u.result = clean ? "success" : (sig > 0 ? "signal" : "exit-code");
            if (restart) {
                // start limit
                time_t cutoff = now - static_cast<time_t>(u.def.startLimitIntervalSec);
                int recent = 0;
                for (time_t t : u.startTimes) if (t >= cutoff) ++recent;
                if (recent >= u.def.startLimitBurst) {
                    u.active = "failed";
                    u.sub = "failed";
                    u.result = "start-limit-hit";
                    u.inactiveEnterTime = now;
                    propagate = true;
                    LOG(WARNING) << "unit " << e->env << "/" << name << ": start limit hit (" << recent << " starts in "
                                 << u.def.startLimitIntervalSec << "s)";
                } else {
                    u.active = "activating";
                    u.sub = "auto-restart";
                    u.restartAt = monoNow() + u.def.restartSec;
                }
            } else if (clean) {
                u.active = "inactive";
                u.sub = "dead";
                u.inactiveEnterTime = now;
                propagate = true;
            } else {
                u.active = "failed";
                u.sub = "failed";
                u.inactiveEnterTime = now;
                propagate = true;
            }
        }
        e->cv.notify_all();
    }
    // a few hundred ms later make sure the group is really gone
    std::thread([pid] { usleep(500000); killpg(pid, SIGKILL); }).detach();
    if (propagate) propagateRequiresFailure(e, name);
}

void UnitManager::propagateRequiresFailure(std::shared_ptr<EnvUnits> e, const std::string& name) {
    std::vector<std::string> dependents;
    {
        std::lock_guard<std::mutex> lk(e->st);
        for (auto& kv : e->units) {
            if (kv.second.active != "active" && kv.second.active != "activating") continue;
            for (const auto& r : kv.second.def.requiresUnits) {
                if (withSuffix(r, ".service") == name) dependents.push_back(kv.first);
            }
        }
    }
    if (dependents.empty()) return;
    std::thread([this, e, dependents, name] {
        std::lock_guard<std::mutex> ops(e->ops);
        for (const auto& d : dependents) {
            LOG(INFO) << "unit " << e->env << "/" << d << " stopped: required unit " << name << " is gone";
            std::string err;
            stopSingle(*e, d, &err);
            std::lock_guard<std::mutex> lk(e->st);
            auto it = e->units.find(d);
            if (it != e->units.end()) {
                it->second.active = "inactive";
                it->second.result = "dependency";
            }
        }
    }).detach();
}

// ---------------------------------------------------------------------------------------------
// start / stop

bool UnitManager::startSingle(EnvUnits& e, const std::string& name, StartFlags f, std::string* err) {
    std::shared_ptr<EnvUnits> eptr = envUnits(e.env, false);
    UnitDef def;
    {
        std::lock_guard<std::mutex> lk(e.st);
        auto it = e.units.find(name);
        if (it == e.units.end()) {
            if (err) *err = "unit " + name + " not found";
            return false;
        }
        UnitState& u = it->second;
        if (!u.def.loadError.empty()) {
            if (err) *err = name + ": " + u.def.loadError;
            return false;
        }
        if (u.active == "active" || (u.active == "activating" && u.sub != "auto-restart")) return true;
        if (u.active == "deactivating") {
            if (err) *err = name + " is deactivating";
            return false;
        }
        time_t now = time(nullptr);
        if (f.resetLimit) u.startTimes.clear();
        if (f.checkLimit && !f.isRestart) {
            time_t cutoff = now - static_cast<time_t>(u.def.startLimitIntervalSec);
            int recent = 0;
            for (time_t t : u.startTimes) if (t >= cutoff) ++recent;
            if (recent >= u.def.startLimitBurst) {
                u.active = "failed";
                u.sub = "failed";
                u.result = "start-limit-hit";
                if (err) *err = name + ": start limit hit";
                return false;
            }
        }
        u.stopRequested = false;
        u.restartAt = 0;
        if (f.isRestart) u.nRestarts++;
        def = u.def;
        if (!conditionsHold(e, u)) {
            u.active = "inactive";
            u.sub = "dead";
            u.result = "condition";
            u.inactiveEnterTime = now;
            LOG(INFO) << "unit " << e.env << "/" << name << ": condition failed, skipped";
            return true;
        }
        u.active = "activating";
        u.sub = def.kind == UnitKind::Timer ? "waiting" : (def.execStartPre.empty() ? "start" : "start-pre");
        u.result = "success";
        u.exitCode = -1;
        u.exitSignal = 0;
        u.startTimes.push_back(now);
        while (u.startTimes.size() > 64) u.startTimes.pop_front();
    }

    if (def.kind == UnitKind::Timer) {
        std::lock_guard<std::mutex> lk(e.st);
        UnitState& u = e.units[name];
        return startTimer(e, u, err);
    }

    auto fail = [&](const std::string& result, const std::string& msg) {
        std::lock_guard<std::mutex> lk(e.st);
        UnitState& u = e.units[name];
        u.active = "failed";
        u.sub = "failed";
        u.result = result;
        u.inactiveEnterTime = time(nullptr);
        if (err) *err = name + ": " + msg;
        LOG(WARNING) << "unit " << e.env << "/" << name << " failed: " << msg;
        return false;
    };
    auto snapshot = [&]() {
        std::lock_guard<std::mutex> lk(e.st);
        return e.units[name];
    };

    rotateLog(unitLogPath(envDir(e.env), name));
    writeEnvName(e.env);

    // ExecStartPre
    for (const auto& pre : def.execStartPre) {
        std::string perr;
        UnitState snap = snapshot();
        int rc = runSync(e, snap, pre, def.timeoutStartSec, {}, &perr);
        {
            std::lock_guard<std::mutex> lk(e.st);
            if (e.units[name].stopRequested) return fail("success", "stopped during ExecStartPre");
        }
        if (rc != 0 && !pre.ignoreFailure) {
            return fail(rc == -2 ? "timeout" : "exit-code", "ExecStartPre failed (" + std::to_string(rc) + (perr.empty() ? "" : ": " + perr) + ")");
        }
    }

    if (def.type == ServiceType::Oneshot) {
        {
            std::lock_guard<std::mutex> lk(e.st);
            e.units[name].sub = "start";
        }
        for (const auto& ex : def.execStart) {
            std::string xerr;
            UnitState snap = snapshot();
            // run with the main pid visible so 'stop' can kill it
            pid_t pid = spawn(e, snap, ex.command, {}, &xerr);
            if (pid <= 0) return fail("resources", "spawn failed: " + xerr);
            {
                std::lock_guard<std::mutex> lk(e.st);
                UnitState& u = e.units[name];
                u.mainPid = pid;
                ++u.generation;
            }
            // wait here (oneshot completes before dependents start)
            double deadline = monoNow() + def.timeoutStartSec;
            int status = 0;
            bool timedOut = false;
            while (true) {
                pid_t r = waitpid(pid, &status, WNOHANG);
                if (r == pid) break;
                if (r < 0 && errno != EINTR) break;
                bool stopReq;
                {
                    std::lock_guard<std::mutex> lk(e.st);
                    stopReq = e.units[name].stopRequested;
                }
                if (stopReq || monoNow() > deadline) {
                    killGroup(snap, pid, SIGTERM);
                    usleep(300000);
                    killGroup(snap, pid, SIGKILL);
                    waitpid(pid, &status, 0);
                    timedOut = !stopReq;
                    break;
                }
                usleep(50000);
            }
            killGroup(snap, pid, SIGKILL);
            int code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
            int sig = WIFSIGNALED(status) ? WTERMSIG(status) : 0;
            {
                std::lock_guard<std::mutex> lk(e.st);
                UnitState& u = e.units[name];
                u.mainPid = 0;
                u.exitCode = code;
                u.exitSignal = sig;
                e.cv.notify_all();
                if (u.stopRequested) {
                    u.active = "inactive";
                    u.sub = "dead";
                    u.inactiveEnterTime = time(nullptr);
                    return true;
                }
            }
            if (timedOut) return fail("timeout", "ExecStart timed out");
            if (!isCleanExit(def, code, sig) && !ex.ignoreFailure)
                return fail(sig > 0 ? "signal" : "exit-code", "ExecStart exited " + std::to_string(code) + (sig ? std::string(" signal ") + signalName(sig) : ""));
        }
        for (const auto& post : def.execStartPost) {
            std::string perr;
            UnitState snap = snapshot();
            int rc = runSync(e, snap, post, def.timeoutStartSec, {}, &perr);
            if (rc != 0 && !post.ignoreFailure) return fail("exit-code", "ExecStartPost failed (" + std::to_string(rc) + ")");
        }
        std::lock_guard<std::mutex> lk(e.st);
        UnitState& u = e.units[name];
        time_t now = time(nullptr);
        if (def.remainAfterExit) {
            u.active = "active";
            u.sub = "exited";
            u.activeEnterTime = now;
        } else {
            u.active = "inactive";
            u.sub = "dead";
            u.activeEnterTime = now;  // last activation (for OnUnitActiveSec)
            u.inactiveEnterTime = now;
        }
        u.result = "success";
        return true;
    }

    // Type=simple
    std::string serr;
    UnitState snap = snapshot();
    pid_t pid = spawn(e, snap, def.execStart[0].command, {}, &serr);
    if (pid <= 0) return fail("resources", "spawn failed: " + serr);
    int gen;
    {
        std::lock_guard<std::mutex> lk(e.st);
        UnitState& u = e.units[name];
        u.mainPid = pid;
        gen = ++u.generation;
        u.active = "active";
        u.sub = "running";
        u.activeEnterTime = time(nullptr);
        u.result = "success";
    }
    LOG(INFO) << "unit " << e.env << "/" << name << " started pid " << pid;
    std::thread([this, eptr, name, pid, gen] { watch(eptr, name, pid, gen); }).detach();

    for (const auto& post : def.execStartPost) {
        {
            std::lock_guard<std::mutex> lk(e.st);
            e.units[name].sub = "start-post";
        }
        std::string perr;
        std::vector<std::string> extra = {"MAINPID=" + std::to_string(pid)};
        UnitState snap2 = snapshot();
        int rc = runSync(e, snap2, post, def.timeoutStartSec, extra, &perr);
        if (rc != 0 && !post.ignoreFailure) {
            std::string serr2;
            stopSingle(e, name, &serr2);
            return fail("exit-code", "ExecStartPost failed (" + std::to_string(rc) + ")");
        }
    }
    {
        std::lock_guard<std::mutex> lk(e.st);
        UnitState& u = e.units[name];
        if (u.mainPid == pid && u.active == "active") u.sub = "running";
    }
    return true;
}

bool UnitManager::stopSingle(EnvUnits& e, const std::string& name, std::string* err) {
    UnitDef def;
    pid_t pid = 0;
    {
        std::lock_guard<std::mutex> lk(e.st);
        auto it = e.units.find(name);
        if (it == e.units.end()) {
            if (err) *err = "unit " + name + " not found";
            return false;
        }
        UnitState& u = it->second;
        u.stopRequested = true;
        u.restartAt = 0;
        def = u.def;
        pid = u.mainPid;
        if (u.def.kind == UnitKind::Timer) {
            u.active = "inactive";
            u.sub = "dead";
            u.nextElapse = 0;
            u.inactiveEnterTime = time(nullptr);
            return true;
        }
        if (u.active == "inactive" || u.active == "failed") {
            if (u.active == "failed") u.result = "success";
            u.active = "inactive";
            u.sub = "dead";
            return true;
        }
        u.active = "deactivating";
        u.sub = def.execStop.empty() ? "stop-sigterm" : "stop";
    }
    stopDependents(e, name);

    std::vector<std::string> extra;
    if (pid > 0) extra.push_back("MAINPID=" + std::to_string(pid));
    for (const auto& es : def.execStop) {
        std::string serr;
        UnitState snap;
        {
            std::lock_guard<std::mutex> lk(e.st);
            snap = e.units[name];
        }
        int rc = runSync(e, snap, es, def.timeoutStopSec, extra, &serr);
        if (rc != 0 && !es.ignoreFailure) LOG(WARNING) << "unit " << e.env << "/" << name << ": ExecStop exited " << rc;
    }
    if (pid > 0) {
        UnitState snap;
        {
            std::lock_guard<std::mutex> lk(e.st);
            snap = e.units[name];
            e.units[name].sub = "stop-sigterm";
        }
        killGroup(snap, pid, SIGTERM);
        std::unique_lock<std::mutex> lk(e.st);
        auto gone = [&] { return e.units[name].mainPid == 0; };
        if (!e.cv.wait_for(lk, std::chrono::milliseconds(static_cast<long>(def.timeoutStopSec * 1000)), gone)) {
            e.units[name].sub = "stop-sigkill";
            lk.unlock();
            LOG(WARNING) << "unit " << e.env << "/" << name << ": SIGTERM timeout, sending SIGKILL";
            killGroup(snap, pid, SIGKILL);
            lk.lock();
            if (!e.cv.wait_for(lk, std::chrono::seconds(10), gone)) {
                if (err) *err = name + ": process " + std::to_string(pid) + " did not die";
                e.units[name].active = "failed";
                e.units[name].sub = "failed";
                e.units[name].result = "timeout";
                return false;
            }
        }
    }
    std::lock_guard<std::mutex> lk(e.st);
    UnitState& u = e.units[name];
    u.active = "inactive";
    u.sub = "dead";
    u.result = "success";
    u.mainPid = 0;
    u.inactiveEnterTime = time(nullptr);
    LOG(INFO) << "unit " << e.env << "/" << name << " stopped";
    return true;
}

void UnitManager::stopDependents(EnvUnits& e, const std::string& name) {
    std::vector<std::string> deps;
    {
        std::lock_guard<std::mutex> lk(e.st);
        for (auto& kv : e.units) {
            if (kv.second.active != "active" && kv.second.active != "activating") continue;
            for (const auto& r : kv.second.def.requiresUnits) {
                if (withSuffix(r, ".service") == name) deps.push_back(kv.first);
            }
        }
    }
    for (const auto& d : deps) {
        std::string err;
        stopSingle(e, d, &err);
    }
}

bool UnitManager::startClosure(EnvUnits& e, const std::set<std::string>& roots, StartFlags f,
                               std::vector<std::string>* started, std::vector<std::string>* failed,
                               std::string* err) {
    std::vector<std::string> order;
    std::set<std::string> all;
    {
        std::lock_guard<std::mutex> lk(e.st);
        all = closure(e, roots);
        std::string oerr;
        if (!orderUnits(e, all, &order, &oerr)) {
            if (err) *err = oerr;
            return false;
        }
    }
    std::set<std::string> failedSet;
    bool ok = true;
    for (const auto& n : order) {
        std::vector<std::string> reqs;
        bool exists;
        {
            std::lock_guard<std::mutex> lk(e.st);
            auto it = e.units.find(n);
            exists = it != e.units.end();
            if (exists) for (const auto& r : it->second.def.requiresUnits) reqs.push_back(withSuffix(r, ".service"));
        }
        if (!exists) {
            failedSet.insert(n);  // a missing Wants is harmless; a missing Requires fails dependents below
            continue;
        }
        bool depFailed = false;
        for (const auto& r : reqs) {
            if (failedSet.count(r)) {
                depFailed = true;
                break;
            }
        }
        if (depFailed) {
            std::lock_guard<std::mutex> lk(e.st);
            UnitState& u = e.units[n];
            u.active = "failed";
            u.sub = "failed";
            u.result = "dependency";
            failedSet.insert(n);
            if (failed) failed->push_back(n);
            if (roots.count(n)) ok = false;
            continue;
        }
        std::string serr;
        if (startSingle(e, n, f, &serr)) {
            if (started) started->push_back(n);
        } else {
            failedSet.insert(n);
            if (failed) failed->push_back(n);
            if (roots.count(n)) {
                ok = false;
                if (err && err->empty()) *err = serr;
            }
        }
    }
    return ok;
}

// ---------------------------------------------------------------------------------------------
// timers

void UnitManager::scheduleNext(EnvUnits& e, UnitState& t, time_t now, bool afterTrigger) {
    const UnitDef& d = t.def;
    time_t next = 0;
    auto consider = [&](time_t c) {
        if (c > 0 && (next == 0 || c < next)) next = c;
    };
    if (!afterTrigger && d.onBootSec >= 0) {
        time_t base = e.booted ? e.bootTime : now;
        time_t c = base + static_cast<time_t>(d.onBootSec);
        if (c < now) c = now;
        consider(c);
    }
    if (d.onUnitActiveSec >= 0) {
        time_t last = 0;
        auto it = e.units.find(d.timerUnit);
        if (it != e.units.end()) last = it->second.activeEnterTime;
        if (afterTrigger) last = std::max(last, now);
        if (last > 0) consider(last + static_cast<time_t>(d.onUnitActiveSec));
        else if (d.persistent && t.lastTrigger > 0) consider(t.lastTrigger + static_cast<time_t>(d.onUnitActiveSec));
    }
    if (d.onCalendar.kind != CalendarSpec::None) {
        time_t c = calendarNext(d.onCalendar, now);
        if (d.persistent && t.lastTrigger > 0 && !afterTrigger) {
            time_t due = calendarNext(d.onCalendar, t.lastTrigger);  // missed while the env was down?
            if (due > 0 && due <= now) c = now;
        }
        consider(c);
    }
    t.nextElapse = next;
    t.sub = next > 0 ? "waiting" : "elapsed";
}

bool UnitManager::startTimer(EnvUnits& e, UnitState& u, std::string* err) {
    time_t now = time(nullptr);
    if (u.def.persistent) {
        bool ok;
        std::string s = readFileStr(stampPath(e.env, u.def.name), &ok);
        if (ok) u.lastTrigger = static_cast<time_t>(strtoll(s.c_str(), nullptr, 10));
    }
    scheduleNext(e, u, now, false);
    u.active = "active";
    u.activeEnterTime = now;
    u.result = "success";
    u.stopRequested = false;
    LOG(INFO) << "timer " << e.env << "/" << u.def.name << " armed, next elapse " << u.nextElapse;
    return true;
}

void UnitManager::fireTimer(std::shared_ptr<EnvUnits> e, const std::string& timerName) {
    std::string svc;
    bool persistent = false;
    {
        std::lock_guard<std::mutex> lk(e->st);
        auto it = e->units.find(timerName);
        if (it == e->units.end()) return;
        UnitState& t = it->second;
        if (t.active != "active") return;
        time_t now = time(nullptr);
        t.lastTrigger = now;
        svc = t.def.timerUnit;
        persistent = t.def.persistent;
        scheduleNext(*e, t, now, true);
    }
    if (persistent) {
        mkdirP(envDir(e->env) + "/.aohp/timers", 0755);
        writeFileStr(stampPath(e->env, timerName), std::to_string(static_cast<long long>(time(nullptr))) + "\n");
    }
    std::lock_guard<std::mutex> ops(e->ops);
    {
        std::lock_guard<std::mutex> lk(e->st);
        auto it = e->units.find(svc);
        if (it == e->units.end()) return;
        if (it->second.active == "active" || it->second.active == "activating") {
            LOG(INFO) << "timer " << e->env << "/" << timerName << ": " << svc << " still running, skipped";
            return;
        }
        if (it->second.active == "failed") it->second.active = "inactive";  // timers retry failed oneshots
    }
    LOG(INFO) << "timer " << e->env << "/" << timerName << " elapsed -> start " << svc;
    StartFlags f;
    f.checkLimit = false;
    std::vector<std::string> started, failed;
    std::string err;
    startClosure(*e, {svc}, f, &started, &failed, &err);
    if (!err.empty()) LOG(WARNING) << "timer " << e->env << "/" << timerName << ": " << err;
}

void UnitManager::schedulerLoop() {
    while (true) {
        usleep(500000);
        std::vector<std::shared_ptr<EnvUnits>> envs;
        {
            std::lock_guard<std::mutex> lk(mEnvsMu_);
            for (auto& kv : mEnvs_) envs.push_back(kv.second);
        }
        double mono = monoNow();
        time_t now = time(nullptr);
        for (auto& e : envs) {
            std::vector<std::string> restarts, timers;
            {
                std::lock_guard<std::mutex> lk(e->st);
                for (auto& kv : e->units) {
                    UnitState& u = kv.second;
                    if (u.restartAt > 0 && u.restartAt <= mono && u.active == "activating" && u.sub == "auto-restart") {
                        u.restartAt = 0;
                        restarts.push_back(kv.first);
                    }
                    if (u.def.kind == UnitKind::Timer && u.active == "active" && u.nextElapse > 0 && u.nextElapse <= now) {
                        u.nextElapse = 0;  // fireTimer reschedules
                        timers.push_back(kv.first);
                    }
                }
            }
            for (const auto& n : restarts) {
                std::thread([this, e, n] {
                    std::lock_guard<std::mutex> ops(e->ops);
                    {
                        std::lock_guard<std::mutex> lk(e->st);
                        auto it = e->units.find(n);
                        if (it == e->units.end() || it->second.stopRequested || it->second.active != "activating") return;
                        it->second.active = "inactive";  // let startSingle proceed
                        it->second.sub = "auto-restart";
                    }
                    StartFlags f;
                    f.isRestart = true;
                    std::string err;
                    LOG(INFO) << "unit " << e->env << "/" << n << ": auto-restart";
                    if (!startSingle(*e, n, f, &err)) LOG(WARNING) << "unit " << e->env << "/" << n << ": restart failed: " << err;
                }).detach();
            }
            for (const auto& n : timers) {
                std::thread([this, e, n] { fireTimer(e, n); }).detach();
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// JSON

std::string UnitManager::unitJson(const UnitState& u, time_t now) {
    const UnitDef& d = u.def;
    std::ostringstream o;
    o << "{\"name\":\"" << jsonEscapeStr(d.name) << "\"";
    o << ",\"type\":\"" << (d.kind == UnitKind::Timer ? "timer" : "service") << "\"";
    o << ",\"serviceType\":\"" << (d.type == ServiceType::Oneshot ? "oneshot" : "simple") << "\"";
    o << ",\"description\":\"" << jsonEscapeStr(d.description) << "\"";
    o << ",\"loadState\":\"" << (d.transient ? "transient" : (d.loadError.empty() ? "loaded" : "error")) << "\"";
    o << ",\"loadError\":\"" << jsonEscapeStr(d.loadError) << "\"";
    o << ",\"loadWarnings\":" << jsonStringArray(d.warnings);
    o << ",\"path\":\"" << jsonEscapeStr(d.path) << "\"";
    o << ",\"fileMissing\":" << (u.fileMissing ? "true" : "false");
    o << ",\"enabled\":" << (u.enabled ? "true" : "false");
    o << ",\"active\":\"" << u.active << "\",\"sub\":\"" << u.sub << "\",\"result\":\"" << u.result << "\"";
    o << ",\"mainPid\":" << u.mainPid << ",\"exitCode\":" << u.exitCode << ",\"exitSignal\":" << u.exitSignal;
    o << ",\"exitSignalName\":\"" << signalName(u.exitSignal) << "\"";
    o << ",\"nRestarts\":" << u.nRestarts;
    o << ",\"activeEnterTime\":" << static_cast<long long>(u.activeEnterTime);
    o << ",\"inactiveEnterTime\":" << static_cast<long long>(u.inactiveEnterTime);
    long long up = (u.active == "active" && u.activeEnterTime > 0) ? static_cast<long long>(now - u.activeEnterTime) : 0;
    o << ",\"uptimeSec\":" << up;
    std::string exec = d.execStart.empty() ? "" : d.execStart[0].command;
    o << ",\"execStart\":\"" << jsonEscapeStr(exec) << "\"";
    const char* rs = "no";
    switch (d.restart) {
        case RestartPolicy::No: rs = "no"; break;
        case RestartPolicy::Always: rs = "always"; break;
        case RestartPolicy::OnFailure: rs = "on-failure"; break;
        case RestartPolicy::OnAbnormal: rs = "on-abnormal"; break;
        case RestartPolicy::OnSuccess: rs = "on-success"; break;
    }
    o << ",\"restart\":\"" << rs << "\",\"restartSec\":" << d.restartSec;
    o << ",\"restartInSec\":" << (u.restartAt > 0 ? std::max(0.0, u.restartAt - monoNow()) : 0.0);
    o << ",\"remainAfterExit\":" << (d.remainAfterExit ? "true" : "false");
    o << ",\"workingDirectory\":\"" << jsonEscapeStr(d.workingDirectory) << "\"";
    o << ",\"after\":" << jsonStringArray(d.after) << ",\"before\":" << jsonStringArray(d.before);
    o << ",\"requires\":" << jsonStringArray(d.requiresUnits) << ",\"wants\":" << jsonStringArray(d.wants);
    std::vector<std::string> conds;
    for (const auto& c : d.conditionPathExists) conds.push_back((c.second ? "!" : "") + c.first);
    o << ",\"conditionPathExists\":" << jsonStringArray(conds);
    if (d.kind == UnitKind::Timer) {
        o << ",\"timer\":{\"unit\":\"" << jsonEscapeStr(d.timerUnit) << "\"";
        o << ",\"nextElapse\":" << static_cast<long long>(u.nextElapse);
        o << ",\"lastTrigger\":" << static_cast<long long>(u.lastTrigger);
        o << ",\"onBootSec\":" << d.onBootSec << ",\"onUnitActiveSec\":" << d.onUnitActiveSec;
        o << ",\"onCalendar\":\"" << jsonEscapeStr(d.onCalendar.text) << "\"";
        o << ",\"persistent\":" << (d.persistent ? "true" : "false") << "}";
    }
    o << "}";
    return o.str();
}

std::string UnitManager::listJson(EnvUnits& e) {
    std::lock_guard<std::mutex> lk(e.st);
    time_t now = time(nullptr);
    std::ostringstream o;
    o << "{\"env\":\"" << jsonEscapeStr(e.env) << "\",\"booted\":" << (e.booted ? "true" : "false")
      << ",\"bootTime\":" << static_cast<long long>(e.bootTime) << ",\"units\":[";
    bool first = true;
    for (const auto& kv : e.units) {
        if (!first) o << ",";
        first = false;
        o << unitJson(kv.second, now);
    }
    o << "],\"warnings\":" << jsonStringArray(e.loadWarnings) << "}";
    return o.str();
}

std::string UnitManager::timersJson(EnvUnits& e) {
    std::lock_guard<std::mutex> lk(e.st);
    time_t now = time(nullptr);
    std::ostringstream o;
    o << "{\"timers\":[";
    bool first = true;
    for (const auto& kv : e.units) {
        if (kv.second.def.kind != UnitKind::Timer) continue;
        if (!first) o << ",";
        first = false;
        o << unitJson(kv.second, now);
    }
    o << "]}";
    return o.str();
}

// ---------------------------------------------------------------------------------------------
// op dispatch

bool UnitManager::op(const std::string& env, const std::string& opName, const std::string& argsJson,
                     std::string* outJson, std::string* err) {
    if (env.empty() || env.find('/') != std::string::npos || env[0] == '.') {
        *err = "invalid env name";
        return false;
    }
    if (!pathExists(envDir(env) + "/rootfs")) {
        *err = "container not found: " + env;
        return false;
    }
    std::shared_ptr<EnvUnits> e = envUnits(env, true);
    std::string unitArg = jsonGetString(argsJson, "unit");
    bool now = jsonGetBool(argsJson, "now", false);

    auto needUnit = [&](std::string* resolved) -> bool {
        if (unitArg.empty()) {
            *err = "missing 'unit'";
            return false;
        }
        std::lock_guard<std::mutex> lk(e->st);
        if (!resolveName(*e, unitArg, resolved)) {
            *err = "unit " + unitArg + " not found";
            return false;
        }
        return true;
    };
    auto statusOf = [&](const std::string& name) {
        std::lock_guard<std::mutex> lk(e->st);
        auto it = e->units.find(name);
        if (it == e->units.end()) return std::string("{}");
        return unitJson(it->second, time(nullptr));
    };

    std::lock_guard<std::mutex> ops(e->ops);
    bool firstLoad;
    {
        std::lock_guard<std::mutex> lk(e->st);
        firstLoad = e->units.empty() && !e->booted;
    }
    if (opName == "daemon-reload" || firstLoad) {
        loadUnits(*e);
        writeEnvName(env);
    }

    if (opName == "list" || opName == "daemon-reload") {
        *outJson = listJson(*e);
        return true;
    }
    if (opName == "timers") {
        *outJson = timersJson(*e);
        return true;
    }
    if (opName == "status" || opName == "is-active") {
        std::string name;
        if (!needUnit(&name)) return false;
        std::string js = statusOf(name);
        if (opName == "status") {
            std::string log = logTail(env, name, 2048);
            js.insert(js.size() - 1, ",\"log\":\"" + jsonEscapeStr(log) + "\"");
        }
        *outJson = js;
        return true;
    }
    if (opName == "cat") {
        std::string name;
        if (!needUnit(&name)) return false;
        std::string path, text;
        {
            std::lock_guard<std::mutex> lk(e->st);
            path = e->units[name].def.path;
            if (path.empty() && !e->units[name].def.execStart.empty())
                text = "# transient unit\n[Service]\nExecStart=" + e->units[name].def.execStart[0].command + "\n";
        }
        if (!path.empty()) text = readFileStr(path);
        *outJson = "{\"path\":\"" + jsonEscapeStr(path) + "\",\"text\":\"" + jsonEscapeStr(text) + "\"}";
        return true;
    }
    if (opName == "log") {
        std::string name;
        if (!needUnit(&name)) return false;
        int tail = static_cast<int>(jsonGetInt(argsJson, "tailBytes", 8192));
        std::string log = logTail(env, name, tail);
        struct stat st;
        long long size = stat(unitLogPath(envDir(env), name).c_str(), &st) == 0 ? st.st_size : 0;
        *outJson = "{\"unit\":\"" + jsonEscapeStr(name) + "\",\"size\":" + std::to_string(size) + ",\"log\":\"" + jsonEscapeStr(log) + "\"}";
        return true;
    }
    if (opName == "start" || opName == "restart") {
        std::string name;
        if (!needUnit(&name)) return false;
        if (opName == "restart") {
            std::string serr;
            stopSingle(*e, name, &serr);
        }
        StartFlags f;
        f.resetLimit = true;
        std::vector<std::string> started, failed;
        std::string serr;
        bool ok = startClosure(*e, {name}, f, &started, &failed, &serr);
        *outJson = statusOf(name);
        if (!ok) {
            *err = serr.empty() ? name + " failed to start" : serr;
            return false;
        }
        return true;
    }
    if (opName == "stop") {
        std::string name;
        if (!needUnit(&name)) return false;
        std::string serr;
        bool ok = stopSingle(*e, name, &serr);
        *outJson = statusOf(name);
        if (!ok) {
            *err = serr;
            return false;
        }
        return true;
    }
    if (opName == "reload") {
        std::string name;
        if (!needUnit(&name)) return false;
        UnitState snap;
        {
            std::lock_guard<std::mutex> lk(e->st);
            snap = e->units[name];
        }
        if (snap.def.execReload.empty()) {
            std::string serr;
            stopSingle(*e, name, &serr);
            StartFlags f;
            f.resetLimit = true;
            std::vector<std::string> a, b;
            startClosure(*e, {name}, f, &a, &b, &serr);
        } else if (snap.active == "active" && snap.mainPid > 0) {
            std::vector<std::string> extra = {"MAINPID=" + std::to_string(snap.mainPid)};
            for (const auto& r : snap.def.execReload) {
                std::string rerr;
                int rc = runSync(*e, snap, r, snap.def.timeoutStartSec, extra, &rerr);
                if (rc != 0 && !r.ignoreFailure) {
                    *err = name + ": ExecReload exited " + std::to_string(rc);
                    *outJson = statusOf(name);
                    return false;
                }
            }
        } else {
            *err = name + " is not active";
            return false;
        }
        *outJson = statusOf(name);
        return true;
    }
    if (opName == "enable" || opName == "disable") {
        std::string name;
        if (!needUnit(&name)) return false;
        bool transient;
        {
            std::lock_guard<std::mutex> lk(e->st);
            transient = e->units[name].def.transient;
        }
        if (transient) {
            *err = name + " is transient and cannot be enabled";
            return false;
        }
        std::string wdir = wantsDir(env);
        std::string link = wdir + "/" + name;
        if (opName == "enable") {
            mkdirP(wdir, 0755);
            unlink(link.c_str());
            if (symlink(("../" + name).c_str(), link.c_str()) != 0) {
                *err = std::string("symlink: ") + strerror(errno);
                return false;
            }
        } else {
            if (unlink(link.c_str()) != 0 && errno != ENOENT) {
                *err = std::string("unlink: ") + strerror(errno);
                return false;
            }
        }
        {
            std::lock_guard<std::mutex> lk(e->st);
            e->units[name].enabled = (opName == "enable");
        }
        bool ok = true;
        std::string serr;
        if (now) {
            if (opName == "enable") {
                StartFlags f;
                f.resetLimit = true;
                std::vector<std::string> a, b;
                ok = startClosure(*e, {name}, f, &a, &b, &serr);
            } else {
                ok = stopSingle(*e, name, &serr);
            }
        }
        *outJson = statusOf(name);
        if (!ok) {
            *err = serr;
            return false;
        }
        return true;
    }
    if (opName == "reset-failed") {
        std::lock_guard<std::mutex> lk(e->st);
        for (auto& kv : e->units) {
            if (!unitArg.empty() && kv.first != unitArg && kv.first != withSuffix(unitArg, ".service")) continue;
            if (kv.second.active == "failed") {
                kv.second.active = "inactive";
                kv.second.sub = "dead";
                kv.second.result = "success";
            }
            kv.second.startTimes.clear();
        }
        time_t t = time(nullptr);
        std::ostringstream o;
        o << "{\"units\":[";
        bool first = true;
        for (const auto& kv : e->units) {
            if (!first) o << ",";
            first = false;
            o << unitJson(kv.second, t);
        }
        o << "]}";
        *outJson = o.str();
        return true;
    }
    if (opName == "env-start") {
        loadUnits(*e);
        writeEnvName(env);
        std::set<std::string> roots;
        {
            std::lock_guard<std::mutex> lk(e->st);
            if (!e->booted) {
                e->booted = true;
                e->bootTime = time(nullptr);
            }
            for (const auto& kv : e->units) if (kv.second.enabled && kv.second.def.loadError.empty()) roots.insert(kv.first);
        }
        StartFlags f;
        f.resetLimit = true;
        std::vector<std::string> started, failed;
        std::string serr;
        startClosure(*e, roots, f, &started, &failed, &serr);
        std::string js = listJson(*e);
        js.insert(js.size() - 1, ",\"started\":" + jsonStringArray(started) + ",\"failed\":" + jsonStringArray(failed) +
                                 ",\"error\":\"" + jsonEscapeStr(serr) + "\"");
        *outJson = js;
        return true;
    }
    if (opName == "env-stop") {
        std::vector<std::string> order;
        std::set<std::string> active;
        {
            std::lock_guard<std::mutex> lk(e->st);
            for (const auto& kv : e->units)
                if (kv.second.active != "inactive" && kv.second.active != "failed") active.insert(kv.first);
            std::string oerr;
            if (!orderUnits(*e, active, &order, &oerr)) order.assign(active.begin(), active.end());
        }
        std::reverse(order.begin(), order.end());
        std::vector<std::string> stopped, failed;
        for (const auto& n : order) {
            std::string serr;
            if (stopSingle(*e, n, &serr)) stopped.push_back(n); else failed.push_back(n);
        }
        {
            std::lock_guard<std::mutex> lk(e->st);
            e->booted = false;
        }
        std::string js = listJson(*e);
        js.insert(js.size() - 1, ",\"stopped\":" + jsonStringArray(stopped) + ",\"failed\":" + jsonStringArray(failed));
        *outJson = js;
        return true;
    }
    *err = "unknown unit op: " + opName;
    return false;
}

// ---------------------------------------------------------------------------------------------
// legacy bridge

long UnitManager::legacyStartService(const std::string& env, const std::string& serviceId,
                                     const std::string& command, std::string* err) {
    if (!pathExists(envDir(env) + "/rootfs")) {
        *err = "Container not found";
        return -1;
    }
    std::shared_ptr<EnvUnits> e = envUnits(env, true);
    std::lock_guard<std::mutex> ops(e->ops);
    std::string name = serviceId + ".service";
    {
        bool firstLoad;
        {
            std::lock_guard<std::mutex> lk(e->st);
            firstLoad = e->units.empty() && !e->booted;
        }
        if (firstLoad) {
            loadUnits(*e);
            writeEnvName(env);
        }
        std::lock_guard<std::mutex> lk(e->st);
        auto it = e->units.find(name);
        if (it == e->units.end() || it->second.def.transient) {
            // (re)create the transient unit, like systemd-run
            UnitState& u = e->units[name];
            if (u.mainPid > 0) {
                *err = "service already running";
                return -1;
            }
            UnitDef d;
            d.name = name;
            d.kind = UnitKind::Service;
            d.transient = true;
            d.description = "transient unit (startService)";
            ExecLine ex;
            ex.command = command;
            d.execStart = {ex};
            d.restart = RestartPolicy::No;
            int keepRestarts = u.nRestarts;
            u = UnitState();
            u.def = d;
            u.nRestarts = keepRestarts;
        } else if (!it->second.def.loadError.empty()) {
            *err = name + ": " + it->second.def.loadError;
            return -1;
        }
    }
    StartFlags f;
    f.resetLimit = true;
    std::vector<std::string> a, b;
    std::string serr;
    if (!startClosure(*e, {name}, f, &a, &b, &serr)) {
        *err = serr.empty() ? "start failed" : serr;
        return -1;
    }
    std::lock_guard<std::mutex> lk(e->st);
    UnitState& u = e->units[name];
    if (u.mainPid > 0) return u.mainPid;
    if (u.active == "active" || u.active == "inactive") return 1;  // oneshot done / condition skipped
    *err = "start failed (" + u.result + ")";
    return -1;
}

bool UnitManager::legacyStopService(const std::string& env, const std::string& serviceId) {
    std::shared_ptr<EnvUnits> e = envUnits(env, false);
    if (!e) return false;
    std::lock_guard<std::mutex> ops(e->ops);
    std::string name;
    {
        std::lock_guard<std::mutex> lk(e->st);
        if (!resolveName(*e, serviceId, &name)) return false;
    }
    std::string err;
    return stopSingle(*e, name, &err);
}

std::string UnitManager::legacyListJson(const std::string& env) {
    std::shared_ptr<EnvUnits> e = envUnits(env, false);
    if (!e) return "[]";
    std::lock_guard<std::mutex> lk(e->st);
    time_t now = time(nullptr);
    std::ostringstream o;
    o << "[";
    bool first = true;
    for (const auto& kv : e->units) {
        const UnitState& u = kv.second;
        if (u.def.kind != UnitKind::Service) continue;
        bool alive = u.active == "active" || u.active == "activating";
        long long up = alive && u.activeEnterTime > 0 ? static_cast<long long>(now - u.activeEnterTime) : 0;
        if (!first) o << ",";
        first = false;
        o << "{\"serviceId\":\"" << jsonEscapeStr(u.def.baseName()) << "\",\"pid\":" << (u.mainPid > 0 ? u.mainPid : -1)
          << ",\"alive\":" << (alive ? "true" : "false") << ",\"startTime\":" << static_cast<long long>(u.activeEnterTime)
          << ",\"uptimeSec\":" << up << ",\"command\":\"" << jsonEscapeStr(u.def.execStart.empty() ? "" : u.def.execStart[0].command)
          << "\",\"unit\":\"" << jsonEscapeStr(u.def.name) << "\",\"active\":\"" << u.active << "\",\"sub\":\"" << u.sub
          << "\",\"transient\":" << (u.def.transient ? "true" : "false") << "}";
    }
    o << "]";
    return o.str();
}

std::string UnitManager::logTail(const std::string& env, const std::string& unitOrId, int tailBytes) {
    if (tailBytes <= 0 || tailBytes > 4 * 1024 * 1024) tailBytes = 65536;
    std::string path = unitLogPath(envDir(env), unitOrId);
    int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return "";
    struct stat st;
    if (fstat(fd, &st) != 0) {
        close(fd);
        return "";
    }
    off_t off = st.st_size > tailBytes ? st.st_size - tailBytes : 0;
    if (lseek(fd, off, SEEK_SET) < 0) {
        close(fd);
        return "";
    }
    std::string out;
    out.resize(static_cast<size_t>(st.st_size - off));
    ssize_t n = read(fd, &out[0], out.size());
    close(fd);
    if (n < 0) return "";
    out.resize(static_cast<size_t>(n));
    return out;
}

}  // namespace aohp
