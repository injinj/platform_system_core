/*
 * Copyright (C) 2026 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 */

#pragma once

#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include <sys/types.h>

#include "unit_file.h"

namespace aohp {

class ContainerManager;

/** Runtime state of one unit (service or timer) in one env. Protected by EnvUnits::st. */
struct UnitState {
    UnitDef def;
    bool enabled = false;
    std::string active = "inactive";  // active|activating|deactivating|inactive|failed
    std::string sub = "dead";         // running|exited|dead|auto-restart|start-pre|start-post|stop-sigterm|stop-sigkill|condition|waiting|elapsed
    std::string result = "success";   // success|exit-code|signal|timeout|start-limit-hit|dependency|condition|resources
    pid_t mainPid = 0;
    int exitCode = -1;
    int exitSignal = 0;
    int nRestarts = 0;
    time_t activeEnterTime = 0;
    time_t inactiveEnterTime = 0;
    bool stopRequested = false;
    int generation = 0;               // bumped on every spawn; stale watchers ignore their exit
    std::deque<time_t> startTimes;    // for StartLimitBurst/IntervalSec
    double restartAt = 0;             // CLOCK_MONOTONIC seconds; 0 = no restart pending
    bool fileMissing = false;         // unit file disappeared since load (kept while active)
    // timers
    time_t nextElapse = 0;
    time_t lastTrigger = 0;
};

struct EnvUnits {
    std::string env;
    std::mutex ops;                   // serializes start/stop/reload/env-start per env
    std::mutex st;                    // protects everything below
    std::condition_variable cv;       // notified when a main process exits
    std::map<std::string, UnitState> units;
    bool booted = false;
    time_t bootTime = 0;
    std::vector<std::string> loadWarnings;
};

/**
 * systemd-subset unit supervisor for container envs (see aohp-driver docs/UNITS.md).
 * Unit files: <rootfs>/etc/aohp/system/NAME.service and NAME.timer; enabled = symlink in
 * <rootfs>/etc/aohp/system/aohp.target.wants/. State is rebuilt from the files at every
 * daemon start / daemon-reload; nothing but Persistent= timer stamps is written to disk.
 */
class UnitManager {
public:
    explicit UnitManager(ContainerManager& cm);
    ~UnitManager();

    /** Start the scheduler thread (restarts, timers). Call once after construction. */
    void start();

    /**
     * Dispatch one protocol op. Returns true with *outJson on success, false with *err.
     * ops: list status start stop restart reload enable disable daemon-reload log cat timers
     *      env-start env-stop reset-failed is-active
     */
    bool op(const std::string& env, const std::string& opName, const std::string& argsJson,
            std::string* outJson, std::string* err);

    // Legacy START_SVC/STOP_SVC/LIST_SVC/SVC_LOG mapped onto units.
    long legacyStartService(const std::string& env, const std::string& serviceId,
                            const std::string& command, std::string* err);
    bool legacyStopService(const std::string& env, const std::string& serviceId);
    std::string legacyListJson(const std::string& env);
    std::string logTail(const std::string& env, const std::string& unitOrId, int tailBytes);

    /** Forget an env's units (container destroyed/reset). Running processes are killed by the caller. */
    void forgetEnv(const std::string& env);

    /** Write <rootfs>/etc/aohp/env-name so the systemctl shim can find its env. */
    void writeEnvName(const std::string& env);

    static std::string unitLogPath(const std::string& envDir, const std::string& unitName);

private:
    struct StartFlags {
        bool checkLimit = true;
        bool resetLimit = false;
        bool isRestart = false;
    };

    ContainerManager& mCm_;
    std::mutex mEnvsMu_;
    std::map<std::string, std::shared_ptr<EnvUnits>> mEnvs_;
    bool mSchedulerStarted_ = false;

    std::shared_ptr<EnvUnits> envUnits(const std::string& env, bool create);

    std::string systemDir(const std::string& env);
    std::string wantsDir(const std::string& env);
    std::string envDir(const std::string& env);
    std::string stampPath(const std::string& env, const std::string& unit);

    void loadUnits(EnvUnits& e);  // ops held
    bool orderUnits(EnvUnits& e, const std::set<std::string>& names, std::vector<std::string>* out,
                    std::string* err);  // st held
    std::set<std::string> closure(EnvUnits& e, const std::set<std::string>& roots);  // st held

    bool startClosure(EnvUnits& e, const std::set<std::string>& roots, StartFlags f,
                      std::vector<std::string>* started, std::vector<std::string>* failed,
                      std::string* err);  // ops held
    bool startSingle(EnvUnits& e, const std::string& name, StartFlags f, std::string* err);  // ops held
    bool stopSingle(EnvUnits& e, const std::string& name, std::string* err);                 // ops held
    void stopDependents(EnvUnits& e, const std::string& name);                               // ops held
    void propagateRequiresFailure(std::shared_ptr<EnvUnits> e, const std::string& name);

    bool startTimer(EnvUnits& e, UnitState& u, std::string* err);  // ops held, st not held
    void fireTimer(std::shared_ptr<EnvUnits> e, const std::string& timerName);
    void scheduleNext(EnvUnits& e, UnitState& t, time_t now, bool afterTrigger);  // st held

    pid_t spawn(EnvUnits& e, const UnitState& u, const std::string& command,
                const std::vector<std::string>& extraEnv, std::string* err);
    int runSync(EnvUnits& e, UnitState& u, const ExecLine& line, double timeoutSec,
                const std::vector<std::string>& extraEnv, std::string* err);
    std::vector<std::string> buildEnv(EnvUnits& e, const UnitState& u, std::vector<std::string>* warnings);
    void watch(std::shared_ptr<EnvUnits> e, const std::string& name, pid_t pid, int generation);
    void schedulerLoop();
    void killGroup(const UnitState& u, pid_t pgid, int sig);
    void rotateLog(const std::string& path);
    bool conditionsHold(EnvUnits& e, const UnitState& u);
    bool isCleanExit(const UnitDef& d, int code, int sig);

    std::string unitJson(const UnitState& u, time_t now);
    std::string listJson(EnvUnits& e);
    std::string timersJson(EnvUnits& e);
    bool resolveName(EnvUnits& e, const std::string& in, std::string* out);
};

double monoNow();

}  // namespace aohp
