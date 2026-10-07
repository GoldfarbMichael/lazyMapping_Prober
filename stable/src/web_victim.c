// web_victim.c -- see web_victim.h for the contract and the three load-bearing subtleties.

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <grp.h>
#include <sys/wait.h>
#endif

#include "web_victim.h"
#include "fp_ctl.h"

// Teardown command, built once at launch so the signal-handler path never formats a string.
static char g_pkill_cmd[1024] = "";

// ---------------------------------------------------------------------------
// Site list
// ---------------------------------------------------------------------------

// A slug becomes a DIRECTORY NAME under the data root and a label_map key in the .h5, so
// restrict it to what is safe on both sides. A '/' would make os.path.join() build a nested
// path that csv_to_h5.py's one-level class scan cannot see; a leading '/' would make it
// discard the data root entirely.
int web_slug_ok(const char *s) {
    if (!s || !*s) return 0;
    for (const char *p = s; *p; p++)
        if (!isalnum((unsigned char)*p) && *p != '.' && *p != '_' && *p != '-') return 0;
    return 1;
}

int web_site_list_load(const char *path, Site *out, int max_sites) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[web] ERROR: cannot open sites file '%s': %s\n", path, strerror(errno));
        return -1;
    }
    char line[1024];
    int lineno = 0, n = 0;
    while (fgets(line, sizeof(line), f)) {
        lineno++;
        char *p = line;
        while (*p == ' ' || *p == '\t') p++;
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        line[strcspn(line, "\r\n")] = '\0';
        char *slug = p;
        char *sep = slug + strcspn(slug, " \t");
        if (*sep == '\0') {
            fprintf(stderr, "[web] ERROR: %s:%d: expected '<slug><TAB><url>', got '%s'\n",
                    path, lineno, slug);
            fclose(f); return -1;
        }
        *sep++ = '\0';
        while (*sep == ' ' || *sep == '\t') sep++;
        if (*sep == '\0') {
            fprintf(stderr, "[web] ERROR: %s:%d: missing URL for slug '%s'\n", path, lineno, slug);
            fclose(f); return -1;
        }

        if (n >= max_sites) {
            fprintf(stderr, "[web] ERROR: %s:%d: more than %d sites\n", path, lineno, max_sites);
            fclose(f); return -1;
        }
        if (!web_slug_ok(slug)) {
            fprintf(stderr, "[web] ERROR: %s:%d: slug '%s' must match [A-Za-z0-9._-]+ "
                            "(it becomes a directory name and an .h5 label)\n", path, lineno, slug);
            fclose(f); return -1;
        }
        if (strlen(slug) >= WEB_MAX_SLUG_LEN || strlen(sep) >= WEB_MAX_URL_LEN) {
            fprintf(stderr, "[web] ERROR: %s:%d: slug or URL too long\n", path, lineno);
            fclose(f); return -1;
        }
        // Duplicate slugs would silently merge two sites into one class dir -- the samples
        // would interleave and the class would be unlabelable after the fact.
        for (int i = 0; i < n; i++) {
            if (strcmp(out[i].slug, slug) == 0) {
                fprintf(stderr, "[web] ERROR: %s:%d: duplicate slug '%s' (also line for '%s')\n",
                        path, lineno, slug, out[i].url);
                fclose(f); return -1;
            }
        }
        snprintf(out[n].slug, sizeof(out[n].slug), "%s", slug);
        snprintf(out[n].url,  sizeof(out[n].url),  "%s", sep);
        n++;
    }
    fclose(f);
    if (n < 2) {
        fprintf(stderr, "[web] ERROR: %s has %d uncommented site(s); need at least 2 to "
                        "fingerprint\n", path, n);
        return -1;
    }
    return n;
}

// ---------------------------------------------------------------------------
// Chrome
// ---------------------------------------------------------------------------

#ifdef _WIN32
// ---------------------------------------------------------------------------
// Windows: CreateProcess + a Job Object.
//
// There is no pkill and no fork. Chrome is started SUSPENDED, put into a job created with
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE, then resumed, so every process it spawns (renderers, GPU,
// utility) is born inside the job. Teardown is TerminateJobObject: it kills exactly OUR
// browser tree, with no command-line matching to get wrong. KILL_ON_JOB_CLOSE also means that
// if this process dies hard (taskkill /F), the kernel closes the job handle and takes Chrome
// down with it -- the leak that the Linux pkill backstop exists to catch cannot happen here.
//
// g_pkill_cmd still holds a PowerShell command-line-match kill, for web_pkill_profile() (called
// without a launch) and as a fallback should the job ever fail to be created.
// ---------------------------------------------------------------------------

static HANDLE g_job  = NULL;   // the browser tree's job
static HANDLE g_proc = NULL;   // the chrome.exe browser process (for diagnosis)
static int    g_prio_locked = 0;   // job pins the priority class at NORMAL (see QoS guard)

// Build the PowerShell teardown command for `profile_dir`. The path is embedded in a single-
// quoted PowerShell string used as a -like pattern, so reject the characters that would break
// out of the string (' ") or act as wildcards ([ ] * ?). Filtering on Name='chrome.exe' means
// the command can never match the powershell.exe that runs it, so no bracket trick is needed.
static int build_pkill_cmd(const char *profile_dir, char *cmd, size_t cap) {
    if (!profile_dir || !*profile_dir) return -1;
    if (strpbrk(profile_dir, "'\"[]*?")) return -1;
    int w = snprintf(cmd, cap,
        "powershell -NoProfile -NonInteractive -Command \""
        "Get-CimInstance Win32_Process -Filter \\\"Name='chrome.exe'\\\" | "
        "Where-Object { $_.CommandLine -like '*--user-data-dir=%s*' } | "
        "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }\"",
        profile_dir);
    return (w > 0 && (size_t)w < cap) ? 0 : -1;
}

void web_pkill_profile(const char *profile_dir) {
    char cmd[1024];
    if (build_pkill_cmd(profile_dir, cmd, sizeof(cmd)) != 0) {
        fprintf(stderr, "[web] WARNING: cannot build teardown command for '%s'\n",
                profile_dir ? profile_dir : "(null)");
        return;
    }
    if (system(cmd) == -1)
        fprintf(stderr, "[web] WARNING: teardown command failed to run\n");
}

// Also called from the console ctrl handler, which on Windows runs on its own thread (not in
// signal context), so blocking here is fine.
void web_pkill_armed(void) {
    if (g_job) {
        TerminateJobObject(g_job, 1);
        // Wait for the browser process itself to be gone, so :9222 is released before the
        // caller (or the next batch) moves on.
        if (g_proc) WaitForSingleObject(g_proc, 5000);
        return;
    }
    if (g_pkill_cmd[0] && system(g_pkill_cmd) == -1) { /* nothing useful to do here */ }
}

// Resolve chrome.exe: $CHROME_PATH, then the per-machine and per-user install locations.
static int find_chrome(char *out, size_t cap) {
    const char *env = getenv("CHROME_PATH");
    if (env && *env) { snprintf(out, cap, "%s", env); return 0; }
    const char *roots[] = { "ProgramFiles", "ProgramFiles(x86)", "LOCALAPPDATA" };
    for (size_t i = 0; i < sizeof(roots) / sizeof(roots[0]); i++) {
        const char *r = getenv(roots[i]);
        if (!r || !*r) continue;
        snprintf(out, cap, "%s\\Google\\Chrome\\Application\\chrome.exe", r);
        if (GetFileAttributesA(out) != INVALID_FILE_ATTRIBUTES) return 0;
    }
    return -1;
}

// Append one argument to a Windows command line, quoted so CommandLineToArgvW (which Chrome
// uses) recovers it exactly: backslashes are literal except before a quote, where they double.
static int append_arg(char *cmd, size_t cap, size_t *len, const char *arg) {
    int quote = (*arg == '\0') || strpbrk(arg, " \t\"") != NULL;
    size_t need = strlen(arg) * 2 + 4;
    if (*len + need >= cap) return -1;
    char *o = cmd + *len;
    if (*len) *o++ = ' ';
    if (quote) *o++ = '"';
    for (const char *p = arg; ; p++) {
        size_t bs = 0;
        while (*p == '\\') { bs++; p++; }
        if (*p == '\0') {                          // trailing backslashes: double them if quoted
            for (size_t i = 0; i < (quote ? bs * 2 : bs); i++) *o++ = '\\';
            break;
        }
        if (*p == '"') {                           // backslashes before a quote double, + escape
            for (size_t i = 0; i < bs * 2 + 1; i++) *o++ = '\\';
        } else {
            for (size_t i = 0; i < bs; i++) *o++ = '\\';
        }
        *o++ = *p;
    }
    if (quote) *o++ = '"';
    *o = '\0';
    *len = (size_t)(o - cmd);
    return 0;
}

pid_t web_launch_chrome(const WebVictimCfg *cfg) {
    if (!cfg || !cfg->profile_dir || !cfg->initial_url) {
        fprintf(stderr, "[web] ERROR: web_launch_chrome: incomplete config\n");
        return -1;
    }
    if (build_pkill_cmd(cfg->profile_dir, g_pkill_cmd, sizeof(g_pkill_cmd)) != 0) {
        fprintf(stderr, "[web] ERROR: profile dir '%s' cannot be torn down safely "
                        "(must not contain ' \" [ ] * ?)\n", cfg->profile_dir);
        return -1;
    }
    char chrome[MAX_PATH * 2];
    if (find_chrome(chrome, sizeof(chrome)) != 0) {
        fprintf(stderr, "[web] ERROR: chrome.exe not found (set CHROME_PATH)\n");
        return -1;
    }

    char profile_arg[WEB_MAX_URL_LEN];
    char dbg[64];
    snprintf(profile_arg, sizeof(profile_arg), "--user-data-dir=%s", cfg->profile_dir);
    snprintf(dbg, sizeof(dbg), "--remote-debugging-port=%d", FP_CDP_PORT);

    // Flag set: IDENTICAL to the POSIX branch below (see web_victim.h on why it must not drift).
    const char *argv[24];
    int a = 0;
    argv[a++] = chrome;
    argv[a++] = profile_arg;
    argv[a++] = "--no-first-run";
    argv[a++] = "--no-default-browser-check";
    argv[a++] = dbg;
    argv[a++] = "--site-per-process";
    argv[a++] = "--disable-background-timer-throttling";
    argv[a++] = "--disable-backgrounding-occluded-windows";
    argv[a++] = "--disable-renderer-backgrounding";
    argv[a++] = "--disable-features=CalculateNativeWinOcclusion,IntensiveWakeUpThrottling,"
                "SpareRendererForProcessPerSite";
    argv[a++] = "--disk-cache-size=1";
    argv[a++] = "--media-cache-size=1";
    if (cfg->no_sandbox) argv[a++] = "--no-sandbox";
    argv[a++] = "--new-window";
    argv[a++] = cfg->initial_url;

    char cmd[8192] = "";
    size_t len = 0;
    for (int i = 0; i < a; i++) {
        if (append_arg(cmd, sizeof(cmd), &len, argv[i]) != 0) {
            fprintf(stderr, "[web] ERROR: chrome command line too long\n");
            return -1;
        }
    }

    static wchar_t wcmd[8192];   // CreateProcessW may write to its command-line buffer
    if (MultiByteToWideChar(CP_UTF8, 0, cmd, -1, wcmd, (int)(sizeof(wcmd) / sizeof(wcmd[0]))) == 0) {
        fprintf(stderr, "[web] ERROR: chrome command line is not valid UTF-8\n");
        return -1;
    }

    // A job failure is not fatal -- the PowerShell fallback in g_pkill_cmd still tears Chrome
    // down -- but it is worth a loud warning, since it re-opens the leak the job closes.
    g_job = CreateJobObjectW(NULL, NULL);
    if (g_job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION li;
        memset(&li, 0, sizeof(li));
        // First choice: also pin every Chrome process at NORMAL priority class, so Chrome cannot
        // drop the background sampler to IDLE (see the QoS guard below). Windows may refuse the
        // priority limit without SeIncreaseBasePriorityPrivilege; then the watcher thread
        // restores the priority instead.
        li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
                                              JOB_OBJECT_LIMIT_PRIORITY_CLASS;
        li.BasicLimitInformation.PriorityClass = NORMAL_PRIORITY_CLASS;
        g_prio_locked = SetInformationJobObject(g_job, JobObjectExtendedLimitInformation,
                                                &li, sizeof(li)) ? 1 : 0;
        if (!g_prio_locked) {
            li.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
            li.BasicLimitInformation.PriorityClass = 0;
            if (!SetInformationJobObject(g_job, JobObjectExtendedLimitInformation, &li, sizeof(li))) {
                CloseHandle(g_job);
                g_job = NULL;
            }
        }
    }
    if (!g_job)
        fprintf(stderr, "[web] WARNING: could not create a job object for Chrome (error %lu); "
                        "falling back to command-line teardown\n", GetLastError());

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    memset(&pi, 0, sizeof(pi));
    if (!CreateProcessW(NULL, wcmd, NULL, NULL, FALSE, CREATE_SUSPENDED, NULL, NULL, &si, &pi)) {
        fprintf(stderr, "[web] ERROR: CreateProcess chrome failed (error %lu)\n", GetLastError());
        return -1;
    }
    if (g_job && !AssignProcessToJobObject(g_job, pi.hProcess)) {
        fprintf(stderr, "[web] WARNING: could not put Chrome in its job (error %lu); "
                        "falling back to command-line teardown\n", GetLastError());
        CloseHandle(g_job);
        g_job = NULL;
    }
    if (cfg->set_affinity && cfg->cpu_mask) {
        if (!SetProcessAffinityMask(pi.hProcess, (DWORD_PTR)*cfg->cpu_mask))
            fprintf(stderr, "[web] WARNING: SetProcessAffinityMask for chrome failed (error %lu)\n",
                    GetLastError());
    }
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    g_proc = pi.hProcess;
    return (pid_t)pi.dwProcessId;
}

// ---------------------------------------------------------------------------
// QoS guard (see web_victim.h). Windows only.
// ---------------------------------------------------------------------------

#define QOS_POLL_MS        10    // steady-state watcher period
#define QOS_BURST_POLL_MS   1    // period right after a navigation, when the demotion happens
#define QOS_BURST_MS      500    // how long the burst lasts

static HANDLE           g_qos_thread = NULL;
static HANDLE           g_qos_wake   = NULL;     // auto-reset: "a victim tab just opened"
static volatile LONG    g_qos_run    = 0;
static CRITICAL_SECTION g_qos_lock;
static WebQosStats      g_qos_stats  = { 0, -1.0, 0.0, 0.0, 0.0, 0.0 };
static double           g_qos_nav_ms = 0.0;      // when the last navigation was signalled

static double qos_now_ms(void) {
    static LARGE_INTEGER f;
    LARGE_INTEGER t;
    if (f.QuadPart == 0) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

// Handles to the job's processes, kept open across passes: re-opening every process every
// millisecond is the expensive part of a pass. An open handle also pins the PID, so a cached
// entry can never silently refer to a different, recycled process.
#define QOS_MAX_PROCS 256
static struct { DWORD pid; HANDLE h; int seen; } g_qos_procs[QOS_MAX_PROCS];
static int g_qos_nprocs = 0;

static HANDLE qos_handle_for(DWORD pid) {
    for (int i = 0; i < g_qos_nprocs; i++)
        if (g_qos_procs[i].pid == pid) { g_qos_procs[i].seen = 1; return g_qos_procs[i].h; }
    if (g_qos_nprocs >= QOS_MAX_PROCS) return NULL;
    HANDLE h = OpenProcess(PROCESS_SET_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!h) return NULL;   // already exited, or not ours to touch
    g_qos_procs[g_qos_nprocs].pid  = pid;
    g_qos_procs[g_qos_nprocs].h    = h;
    g_qos_procs[g_qos_nprocs].seen = 1;
    g_qos_nprocs++;
    return h;
}

// Drop cached handles for processes that have left the job (closed victim tabs).
static void qos_prune_handles(void) {
    int w = 0;
    for (int i = 0; i < g_qos_nprocs; i++) {
        if (g_qos_procs[i].seen) {
            g_qos_procs[i].seen = 0;
            g_qos_procs[w++] = g_qos_procs[i];
        } else {
            CloseHandle(g_qos_procs[i].h);
        }
    }
    g_qos_nprocs = w;
}

// Bring one process back to the Linux-equivalent baseline. Returns the number of corrections.
static int qos_fix_process(DWORD pid) {
    HANDLE h = qos_handle_for(pid);
    if (!h) return 0;
    int fixes = 0;

    // EcoQoS ("Efficiency mode"): Chrome sets EXECUTION_SPEED, and often IGNORE_TIMER_RESOLUTION,
    // on background renderers. Explicitly opting OUT (control bit set, state bit clear) is
    // stronger than clearing the control bit, which would let Windows decide on its own.
    PROCESS_POWER_THROTTLING_STATE pt;
    memset(&pt, 0, sizeof(pt));
    pt.Version = PROCESS_POWER_THROTTLING_CURRENT_VERSION;
    const ULONG qos_bits = PROCESS_POWER_THROTTLING_EXECUTION_SPEED |
                           PROCESS_POWER_THROTTLING_IGNORE_TIMER_RESOLUTION;
    if (GetProcessInformation(h, ProcessPowerThrottling, &pt, sizeof(pt)) &&
        (pt.StateMask & qos_bits)) {
        pt.ControlMask = qos_bits;
        pt.StateMask   = 0;
        if (SetProcessInformation(h, ProcessPowerThrottling, &pt, sizeof(pt))) fixes++;
    }

    // Memory priority: background renderers get their pages marked low priority.
    MEMORY_PRIORITY_INFORMATION mp;
    memset(&mp, 0, sizeof(mp));
    if (GetProcessInformation(h, ProcessMemoryPriority, &mp, sizeof(mp)) &&
        mp.MemoryPriority < MEMORY_PRIORITY_NORMAL) {
        mp.MemoryPriority = MEMORY_PRIORITY_NORMAL;
        if (SetProcessInformation(h, ProcessMemoryPriority, &mp, sizeof(mp))) fixes++;
    }

    // Priority class: normally pinned by the job; only corrected here if Windows refused that.
    if (!g_prio_locked && GetPriorityClass(h) != NORMAL_PRIORITY_CLASS) {
        if (SetPriorityClass(h, NORMAL_PRIORITY_CLASS)) fixes++;
    }
    return fixes;
}

// One pass over every process currently in Chrome's job.
static int qos_sweep(void) {
    union {
        JOBOBJECT_BASIC_PROCESS_ID_LIST list;
        BYTE raw[sizeof(JOBOBJECT_BASIC_PROCESS_ID_LIST) + 255 * sizeof(ULONG_PTR)];
    } buf;
    memset(&buf, 0, sizeof(buf));
    if (!QueryInformationJobObject(g_job, JobObjectBasicProcessIdList, &buf, sizeof(buf), NULL) &&
        GetLastError() != ERROR_MORE_DATA)
        return 0;
    int fixes = 0;
    for (DWORD i = 0; i < buf.list.NumberOfProcessIdsInList; i++)
        fixes += qos_fix_process((DWORD)buf.list.ProcessIdList[i]);
    qos_prune_handles();
    return fixes;
}

static DWORD WINAPI qos_thread_main(LPVOID arg) {
    (void)arg;
    // Measured: at normal priority this thread was preempted for up to ~10 ms per pass while the
    // sampler and a loading page saturated the CPU -- exactly when its corrections matter. It
    // sleeps >99% of the time and each pass is microseconds of work, so running it time-critical
    // costs the measurement nothing.
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    double last_clean = qos_now_ms();   // last pass that found nothing to fix
    double burst_until = 0.0, prev_start = 0.0;
    while (InterlockedCompareExchange(&g_qos_run, 1, 1)) {
        double t = qos_now_ms();
        int fixes = qos_sweep();
        double done = qos_now_ms();
        int in_burst = (t < burst_until);
        EnterCriticalSection(&g_qos_lock);
        if (done - t > g_qos_stats.max_pass_ms) g_qos_stats.max_pass_ms = done - t;
        if (in_burst && prev_start > 0 && t - prev_start > g_qos_stats.max_burst_gap_ms)
            g_qos_stats.max_burst_gap_ms = t - prev_start;
        if (fixes > 0) {
            // The throttle appeared after the last clean pass and is gone now, so the exposure
            // is at most (done - last_clean): an upper bound, not an exact figure.
            g_qos_stats.fixes += fixes;
            if (done - last_clean > g_qos_stats.max_exposure_ms)
                g_qos_stats.max_exposure_ms = done - last_clean;
            if (in_burst && done - last_clean > g_qos_stats.max_exposure_burst_ms)
                g_qos_stats.max_exposure_burst_ms = done - last_clean;
            if (g_qos_stats.first_fix_ms < 0 && g_qos_nav_ms > 0)
                g_qos_stats.first_fix_ms = t - g_qos_nav_ms;
        }
        LeaveCriticalSection(&g_qos_lock);
        prev_start = t;
        last_clean = done;
        DWORD period = (done < burst_until) ? QOS_BURST_POLL_MS : QOS_POLL_MS;
        if (WaitForSingleObject(g_qos_wake, period) == WAIT_OBJECT_0)
            burst_until = qos_now_ms() + QOS_BURST_MS;   // a victim tab just opened
    }
    return 0;
}

int web_qos_priority_locked(void) { return g_prio_locked; }

void web_qos_guard_start(void) {
    const char *off = getenv("WEB_QOS_GUARD");
    if (off && strcmp(off, "0") == 0) {
        fprintf(stderr, "[web] WARNING: QoS guard disabled (WEB_QOS_GUARD=0): Chrome will put the "
                        "background sampler in Efficiency mode\n");
        return;
    }
    if (!g_job) {
        fprintf(stderr, "[web] WARNING: no job object, so no QoS guard: the background sampler "
                        "will run in Efficiency mode\n");
        return;
    }
    if (g_qos_thread) return;
    InitializeCriticalSection(&g_qos_lock);
    g_qos_wake = CreateEventW(NULL, FALSE, FALSE, NULL);
    InterlockedExchange(&g_qos_run, 1);
    g_qos_thread = CreateThread(NULL, 0, qos_thread_main, NULL, 0, NULL);
    if (!g_qos_thread) {
        InterlockedExchange(&g_qos_run, 0);
        fprintf(stderr, "[web] WARNING: could not start the QoS guard thread (error %lu)\n",
                GetLastError());
    }
}

void web_qos_guard_navigated(void) {
    if (!g_qos_thread) return;
    EnterCriticalSection(&g_qos_lock);
    g_qos_nav_ms = qos_now_ms();
    LeaveCriticalSection(&g_qos_lock);
    SetEvent(g_qos_wake);
}

void web_qos_guard_take(WebQosStats *out) {
    if (!out) return;
    static const WebQosStats zero = { 0, -1.0, 0.0, 0.0, 0.0, 0.0 };
    if (!g_qos_thread) { *out = zero; return; }
    EnterCriticalSection(&g_qos_lock);
    *out = g_qos_stats;
    g_qos_stats = zero;
    LeaveCriticalSection(&g_qos_lock);
}

void web_qos_guard_stop(void) {
    if (!g_qos_thread) return;
    InterlockedExchange(&g_qos_run, 0);
    SetEvent(g_qos_wake);
    WaitForSingleObject(g_qos_thread, 1000);
    CloseHandle(g_qos_thread);
    g_qos_thread = NULL;
    for (int i = 0; i < g_qos_nprocs; i++) CloseHandle(g_qos_procs[i].h);
    g_qos_nprocs = 0;
}

#else // ---------------------------------------------------------------- POSIX

// Build the bracketed pkill pattern for `profile_dir` into `cmd`. Returns 0, or -1 if the
// path cannot be embedded safely. The bracket around the LAST character is what keeps the
// pattern from matching the /bin/sh -c that runs the pkill itself.
static int build_pkill_cmd(const char *profile_dir, char *cmd, size_t cap) {
    if (!profile_dir || !*profile_dir) return -1;
    size_t n = strlen(profile_dir);
    // The path is embedded in a single-quoted shell word and used as an ERE. A quote would
    // break out of the word; a non-alphanumeric final character would land inside [] where
    // ']' '^' and '\' change the bracket expression's meaning.
    if (strchr(profile_dir, '\'')) return -1;
    if (!isalnum((unsigned char)profile_dir[n - 1])) return -1;
    int w = snprintf(cmd, cap, "pkill -9 -f 'user-data-dir=%.*s[%c]'",
                     (int)(n - 1), profile_dir, profile_dir[n - 1]);
    return (w > 0 && (size_t)w < cap) ? 0 : -1;
}

void web_pkill_profile(const char *profile_dir) {
    char cmd[512];
    if (build_pkill_cmd(profile_dir, cmd, sizeof(cmd)) != 0) {
        fprintf(stderr, "[web] WARNING: cannot build teardown pattern for '%s'\n",
                profile_dir ? profile_dir : "(null)");
        return;
    }
    if (system(cmd) == -1)
        fprintf(stderr, "[web] WARNING: teardown pkill failed to run\n");
}

void web_pkill_armed(void) {
    if (g_pkill_cmd[0] && system(g_pkill_cmd) == -1) { /* nothing useful to do in a handler */ }
}

pid_t web_launch_chrome(const WebVictimCfg *cfg) {
    if (!cfg || !cfg->profile_dir || !cfg->initial_url) {
        fprintf(stderr, "[web] ERROR: web_launch_chrome: incomplete config\n");
        return -1;
    }
    // Arm the teardown BEFORE forking: if the pattern cannot be built we must fail here,
    // while nothing is running, rather than discover it when trying to kill a live browser.
    if (build_pkill_cmd(cfg->profile_dir, g_pkill_cmd, sizeof(g_pkill_cmd)) != 0) {
        fprintf(stderr, "[web] ERROR: profile dir '%s' cannot be torn down safely "
                        "(needs an alphanumeric last character and no quotes)\n", cfg->profile_dir);
        return -1;
    }

    char profile_arg[WEB_MAX_URL_LEN];
    char dbg[64];
    snprintf(profile_arg, sizeof(profile_arg), "--user-data-dir=%s", cfg->profile_dir);
    snprintf(dbg, sizeof(dbg), "--remote-debugging-port=%d", FP_CDP_PORT);

    // Flag set is identical for both arms except --no-sandbox (see web_victim.h).
    char *argv[24];
    int a = 0;
    argv[a++] = "google-chrome";
    argv[a++] = profile_arg;
    argv[a++] = "--no-first-run";
    argv[a++] = "--no-default-browser-check";
    argv[a++] = dbg;
    // Guarantee the victim page gets its own renderer process, as it would in a real deployment.
    argv[a++] = "--site-per-process";
    // The first tab is backgrounded the moment a victim tab opens. Without these, Chrome
    // throttles its timers and deprioritises its renderer.
    argv[a++] = "--disable-background-timer-throttling";
    argv[a++] = "--disable-backgrounding-occluded-windows";
    argv[a++] = "--disable-renderer-backgrounding";
    argv[a++] = "--disable-features=CalculateNativeWinOcclusion,IntensiveWakeUpThrottling,"
                "SpareRendererForProcessPerSite";
    // Suppress the HTTP disk cache so sample N of a site is not a warm-cache load that looks
    // nothing like sample 0. DNS, TLS session resumption and connection reuse still warm up --
    // a limitation to document, not one this can fix.
    argv[a++] = "--disk-cache-size=1";
    argv[a++] = "--media-cache-size=1";
    if (cfg->no_sandbox) argv[a++] = "--no-sandbox";
    argv[a++] = "--new-window";
    argv[a++] = (char *)cfg->initial_url;
    argv[a] = NULL;

    pid_t pid = fork();
    if (pid < 0) { perror("[web] fork for chrome"); return -1; }
    if (pid == 0) {
        // Affinity FIRST: it is inherited across exec, and a caller that pinned itself would
        // otherwise hand Chrome its single measurement core.
        if (cfg->set_affinity && cfg->cpu_mask) {
            if (sched_setaffinity(0, sizeof(cpu_set_t), cfg->cpu_mask) != 0)
                perror("[web] sched_setaffinity for chrome");
        }
        // Respect an operator-supplied value, but only a NON-EMPTY one (see web_victim.h).
        const char *disp = getenv("DISPLAY");
        if (!disp || !*disp) setenv("DISPLAY", ":0", 1);
        // The :0 X server's auth cookie lives in gdm's Xauthority, not ~/.Xauthority (which is
        // the :1 Xtigervnc cookie). It is owned by uid 1000, so a Chrome dropped to the login
        // user reads it without any `xhost +SI:localuser:root`.
        const char *xa = getenv("XAUTHORITY");
        if (!xa || !*xa) setenv("XAUTHORITY", "/run/user/1000/gdm/Xauthority", 1);
        if (cfg->home && *cfg->home) setenv("HOME", cfg->home, 1);

        // Drop privileges LAST (setgroups/setgid need root). gid before uid: after setuid the
        // process can no longer change its groups, so the reverse order silently leaves root's
        // supplementary groups in place.
        if (cfg->drop_privs) {
            if (setgroups(0, NULL) != 0) { perror("[web] setgroups"); _exit(127); }
            if (setgid(cfg->gid) != 0)   { perror("[web] setgid");    _exit(127); }
            if (setuid(cfg->uid) != 0)   { perror("[web] setuid");    _exit(127); }
            if (setuid(0) == 0) {        // must NOT succeed -- privileges were not really dropped
                fprintf(stderr, "[web] FATAL: privilege drop did not stick\n");
                _exit(127);
            }
        }
        execvp("google-chrome", argv);
        perror("[web] execvp google-chrome");
        _exit(127);
    }
    return pid;
}
#endif // _WIN32

int web_wait_cdp(int timeout_s) {
    char resp[1024];
    for (int s = 0; s < timeout_s; s++) {
        if (fp_http_get(FP_CDP_PORT, "/json/version", resp, sizeof(resp)) == 0 &&
            strstr(resp, "webSocketDebuggerUrl"))
            return 0;
        sleep(1);
    }
    return -1;
}

int web_cdp_alive(void) {
    char resp[1024];
    return fp_http_get(FP_CDP_PORT, "/json/version", resp, sizeof(resp)) == 0 &&
           strstr(resp, "webSocketDebuggerUrl") != NULL;
}

#ifdef _WIN32
void web_diagnose_chrome_death(pid_t pid) {
    DWORD code = 0;
    if (!g_proc) {
        fprintf(stderr, "[web] chrome pid %d: no process handle to inspect\n", (int)pid);
    } else if (WaitForSingleObject(g_proc, 0) == WAIT_TIMEOUT) {
        fprintf(stderr, "[web] chrome pid %d is still alive but its DevTools port is "
                        "unreachable (browser process wedged, not dead)\n", (int)pid);
    } else if (GetExitCodeProcess(g_proc, &code)) {
        // Windows has no signals: a forced kill shows up as an exit code. Large values are
        // NTSTATUS crash codes (e.g. 0xC0000005 access violation), so print both forms.
        fprintf(stderr, "[web] chrome pid %d exited with code %lu (0x%08lX)\n",
                (int)pid, code, code);
    } else {
        fprintf(stderr, "[web] chrome pid %d: GetExitCodeProcess failed (error %lu)\n",
                (int)pid, GetLastError());
    }
}
#else
void web_diagnose_chrome_death(pid_t pid) {
    int st;
    pid_t r = waitpid(pid, &st, WNOHANG);
    if (r == pid) {
        if (WIFSIGNALED(st))
            fprintf(stderr, "[web] chrome pid %d was KILLED by signal %d (%s)\n",
                    (int)pid, WTERMSIG(st), strsignal(WTERMSIG(st)));
        else if (WIFEXITED(st))
            fprintf(stderr, "[web] chrome pid %d exited normally with status %d\n",
                    (int)pid, WEXITSTATUS(st));
    } else if (r == 0) {
        fprintf(stderr, "[web] chrome pid %d is still alive but its DevTools port is "
                        "unreachable (browser process wedged, not dead)\n", (int)pid);
    } else {
        fprintf(stderr, "[web] chrome pid %d already reaped (waitpid: %s)\n",
                (int)pid, strerror(errno));
    }
}
#endif // _WIN32

// ---------------------------------------------------------------------------
// Victim tab
// ---------------------------------------------------------------------------

// The URL is the RAW QUERY STRING: `PUT /json/new?<url>`. Passing it as `?url=<url>` is
// accepted and silently opens about:blank instead -- verified against Chrome 150. The method
// must be PUT (it was GET before Chrome 111).
int web_victim_open_send(const char *url) {
    char enc[WEB_MAX_URL_LEN * 3 + 1], path[WEB_MAX_URL_LEN * 3 + 32];
    if (fp_url_encode(url, enc, sizeof(enc)) != 0) {
        fprintf(stderr, "[web] ERROR: URL too long to encode: %s\n", url);
        return -1;
    }
    snprintf(path, sizeof(path), "/json/new?%s", enc);
    return fp_http_send(FP_CDP_PORT, "PUT", path, NULL, NULL);
}

int web_victim_open_recv(int fd, char *id, size_t id_len) {
    char resp[4096];
    if (fp_http_recv(fd, resp, sizeof(resp)) != 0) return -1;
    if (fp_json_str(resp, "id", id, id_len) != 0) {
        fprintf(stderr, "[web] ERROR: /json/new gave no target id (response: %.200s)\n", resp);
        return -1;
    }
    return 0;
}

int web_victim_open(const char *url, char *id, size_t id_len) {
    int fd = web_victim_open_send(url);
    if (fd < 0) return -1;
    return web_victim_open_recv(fd, id, id_len);
}

void web_victim_close(const char *id) {
    char path[256], resp[512];
    snprintf(path, sizeof(path), "/json/close/%s", id);
    if (fp_http_get(FP_CDP_PORT, path, resp, sizeof(resp)) != 0)
        fprintf(stderr, "[web] WARNING: failed to close target %s\n", id);
}

#ifndef _WIN32
// QoS guard: Linux desktop Chrome does not demote background renderers, so there is nothing to
// undo. Stubs keep the callers platform-neutral.
void web_qos_guard_start(void) {}
void web_qos_guard_navigated(void) {}
void web_qos_guard_take(WebQosStats *out) {
    if (out) { out->fixes = 0; out->first_fix_ms = -1.0; out->max_exposure_ms = 0.0;
               out->max_pass_ms = 0.0; out->max_burst_gap_ms = 0.0;
               out->max_exposure_burst_ms = 0.0; }
}
void web_qos_guard_stop(void) {}
int  web_qos_priority_locked(void) { return 0; }
#endif
