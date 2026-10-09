// Parser/calendar tests for unit_file.cpp (make test).
#include "unit_file.h"
#include <cassert>
#include <cstdio>
#include <cstring>
using namespace aohp;
static int fails = 0;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); ++fails; } } while (0)
int main() {
    double d;
    CHECK(parseDurationSec("5", &d) && d == 5);
    CHECK(parseDurationSec("5s", &d) && d == 5);
    CHECK(parseDurationSec("2min", &d) && d == 120);
    CHECK(parseDurationSec("500ms", &d) && d == 0.5);
    CHECK(parseDurationSec("1h 30min", &d) && d == 5400);
    CHECK(!parseDurationSec("abc", &d));
    CalendarSpec c;
    CHECK(parseCalendar("daily", &c) && c.kind == CalendarSpec::Daily);
    CHECK(parseCalendar("*-*-* 03:15:00", &c) && c.kind == CalendarSpec::DailyAt && c.hour == 3 && c.minute == 15);
    CHECK(parseCalendar("23:59", &c) && c.hour == 23 && c.minute == 59 && c.second == 0);
    CHECK(!parseCalendar("Mon *-*-* 00:00", &c));
    CHECK(!parseCalendar("*:0/15", &c));
    time_t now = 1000000000;  // 2001-09-09 01:46:40 UTC
    CalendarSpec m; parseCalendar("minutely", &m);
    time_t n1 = calendarNext(m, now);
    CHECK(n1 > now && n1 - now <= 60 && n1 % 60 == 0);
    CalendarSpec h; parseCalendar("hourly", &h);
    CHECK(calendarNext(h, now) % 3600 == 0 && calendarNext(h, now) > now);
    CalendarSpec da; parseCalendar("*-*-* 02:00:00", &da);
    time_t n2 = calendarNext(da, now);
    struct tm t2; localtime_r(&n2, &t2);
    CHECK(t2.tm_hour == 2 && t2.tm_min == 0 && n2 > now && n2 - now < 86400);
    CHECK(calendarNext(da, n2) - n2 == 86400 || calendarNext(da, n2) - n2 == 82800 || calendarNext(da, n2) - n2 == 90000);

    const char* svc =
        "[Unit]\nDescription=OpenClaw gateway\nAfter=wg0.service network.target\nWants=wg0.service\n"
        "ConditionPathExists=!/etc/aohp/no-gateway\n"
        "[Service]\nType=simple\nEnvironment=NODE_ENV=production \"FOO=bar baz\"\nEnvironmentFile=-/root/.openclaw/env.sh\n"
        "WorkingDirectory=/root/.openclaw/workspace\nExecStartPre=-/bin/true\nExecStart=/usr/local/bin/openclaw gateway\n"
        "Restart=on-failure\nRestartSec=5s\nSuccessExitStatus=0 143 SIGTERM\nTimeoutStopSec=30\nKillMode=control-group\n"
        "User=root\nLimitNOFILE=1048576\n[Install]\nWantedBy=aohp.target\n";
    UnitDef u = parseUnitText("openclaw-gateway.service", svc);
    CHECK(u.loadError.empty());
    CHECK(u.description == "OpenClaw gateway");
    CHECK(u.after.size() == 2 && u.after[0] == "wg0.service");
    CHECK(u.wants.size() == 1);
    CHECK(u.conditionPathExists.size() == 1 && u.conditionPathExists[0].second == true);
    CHECK(u.environment.size() == 2 && u.environment[1].first == "FOO" && u.environment[1].second == "bar baz");
    CHECK(u.environmentFiles.size() == 1 && u.environmentFiles[0].second == true);
    CHECK(u.workingDirectory == "/root/.openclaw/workspace");
    CHECK(!u.hostExec);
    {
        UnitDef h = parseUnitText("x11.service", "[Service]\nHostExec=yes\nExecStart=/system/bin/app_process / com.termux.x11.CmdEntryPoint :0\n");
        CHECK(h.loadError.empty() && h.hostExec);
        UnitDef bad = parseUnitText("x.service", "[Service]\nHostExec=maybe\nExecStart=/bin/true\n");
        CHECK(!bad.loadError.empty());
    }
    CHECK(u.execStartPre.size() == 1 && u.execStartPre[0].ignoreFailure);
    CHECK(u.execStart.size() == 1 && u.execStart[0].command == "/usr/local/bin/openclaw gateway");
    CHECK(u.restart == RestartPolicy::OnFailure && u.restartSec == 5);
    CHECK(u.successExitStatus.size() == 2 && u.successExitStatus[1] == 143 && u.successExitSignals.size() == 1);
    CHECK(u.timeoutStopSec == 30);
    CHECK(u.warnings.size() == 2);  // User=, LimitNOFILE=
    CHECK(u.wantedBy.size() == 1 && u.wantedBy[0] == "aohp.target");

    UnitDef bad = parseUnitText("x.service", "[Service]\nType=notify\nExecStart=/bin/true\n");
    CHECK(!bad.loadError.empty());
    UnitDef noexec = parseUnitText("x.service", "[Service]\nType=simple\n");
    CHECK(noexec.loadError == "ExecStart is required");
    UnitDef one = parseUnitText("wg0.service", "[Service]\nType=oneshot\nRemainAfterExit=yes\nExecStart=wg-quick up wg0\nExecStop=wg-quick down wg0\n");
    CHECK(one.loadError.empty() && one.type == ServiceType::Oneshot && one.remainAfterExit && one.execStop.size() == 1);
    UnitDef tm = parseUnitText("net-watchdog.timer", "[Timer]\nOnBootSec=2min\nOnUnitActiveSec=5min\nRandomizedDelaySec=30\n[Install]\nWantedBy=timers.target\n");
    CHECK(tm.loadError.empty() && tm.kind == UnitKind::Timer && tm.onBootSec == 120 && tm.onUnitActiveSec == 300 && tm.timerUnit == "net-watchdog.service");
    CHECK(tm.warnings.size() == 2);
    UnitDef tbad = parseUnitText("t.timer", "[Timer]\nOnCalendar=Mon..Fri 09:00\n");
    CHECK(!tbad.loadError.empty());
    UnitDef tnone = parseUnitText("t.timer", "[Timer]\nUnit=foo.service\n");
    CHECK(!tnone.loadError.empty());
    UnitDef cont = parseUnitText("c.service", "[Service]\nExecStart=/bin/sh -c \\\n  'echo hi'\n");
    CHECK(cont.loadError.empty() && cont.execStart[0].command == "/bin/sh -c 'echo hi'");

    CHECK(jsonGetString("{\"unit\":\"foo.service\",\"now\":true,\"tailBytes\":4096}", "unit") == "foo.service");
    CHECK(jsonGetBool("{\"unit\":\"x\",\"now\":true}", "now") == true);
    CHECK(jsonGetInt("{\"tailBytes\": 4096}", "tailBytes") == 4096);
    CHECK(jsonGetString("{\"a\":\"q\\\"x\\\\y\"}", "a") == "q\"x\\y");
    CHECK(base64Decode(base64EncodeStr("hello {\"unit\":\"a\"}")) == "hello {\"unit\":\"a\"}");
    CHECK(jsonEscapeStr("a\nb\"c") == "a\\nb\\\"c");
    if (fails == 0) printf("unit_file_test: all passed\n");
    return fails ? 1 : 0;
}
