// web_victim.h
// -----------------------------------------------------------------------------
// The VICTIM half of a website-fingerprinting run: the site list, the Chrome process,
// and the DevTools calls that open and close a victim tab.
//
// Extracted from website_orchestrator.c so that BOTH website arms drive an identical
// browser:
//   * Stage 4 (JS lazy-map sampler)      -- website_orchestrator.c
//   * Stage 4b (native Mastik sampler)   -- website_prober.c
// If the two arms are to be compared, the victim must be the same in both; keeping one
// copy of these helpers is what makes that a structural guarantee rather than a promise.
//
// Three subtleties live in here and MUST NOT be "simplified" away:
//   1. A victim tab is opened with PUT /json/new?<raw query string>. The intuitive
//      ?url=<url> form is accepted and silently opens about:blank instead (verified
//      against Chrome 150). GET was dropped in Chrome 111.
//   2. The teardown pkill pattern brackets one character ('...websit[e]') so it cannot
//      match the /bin/sh -c that is running the pkill itself.
//   3. DISPLAY/XAUTHORITY default to :0 and gdm's cookie only when UNSET OR EMPTY. A
//      set-but-empty DISPLAY (common in non-login shells and cron) would otherwise be
//      honoured and hand Chrome an unusable display.
//
// WINDOWS (_WIN32): Chrome is started with CreateProcess inside a Job Object
// (KILL_ON_JOB_CLOSE), and teardown terminates that job, which kills the whole browser tree
// with no command-line matching. (2) and (3) do not apply there; the privilege-drop fields
// are ignored (the Windows arm never runs elevated).
// -----------------------------------------------------------------------------

#ifndef WEB_VICTIM_H
#define WEB_VICTIM_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sys/types.h>
#include <stddef.h>

#ifdef _WIN32
// No uid/gid or cpu_set_t on Windows. The affinity mask is a plain processor bitmask
// (SetProcessAffinityMask); the uid/gid fields exist only so the struct stays one shape.
typedef unsigned int       web_uid_t;
typedef unsigned int       web_gid_t;
typedef unsigned long long web_cpu_mask_t;
#else
#include <sched.h>
typedef uid_t     web_uid_t;
typedef gid_t     web_gid_t;
typedef cpu_set_t web_cpu_mask_t;
#endif

#define WEB_MAX_SITES      512
#define WEB_MAX_SLUG_LEN    64
#define WEB_MAX_URL_LEN    512

typedef struct {
    char slug[WEB_MAX_SLUG_LEN];   // class label -> <data root>/<config>/<slug>/<n>.csv
    char url[WEB_MAX_URL_LEN];     // what the victim tab navigates to
} Site;

// How to launch the browser. The flag set is otherwise IDENTICAL between the two arms on
// purpose -- a difference in disk-cache or process-model flags would be a difference in the
// victim, not in the observer, and would confound the comparison the arms exist to make.
typedef struct {
    const char *profile_dir;   // --user-data-dir. MUST differ per arm: Chrome reuses a running
                               // instance when this matches, so a shared value would open the
                               // page as a tab in the other run's window, and either arm's
                               // teardown pkill would kill both.
    const char *initial_url;   // first tab: the sampler page (JS arm) or about:blank (native arm)

    // Privilege drop. The native arm's prober must be root (hugepages, /proc/self/pagemap),
    // but Chrome must not be while visiting live sites. Set drop_privs and Chrome keeps its
    // sandbox as the login user. The JS arm already runs unprivileged and leaves this 0.
    int         drop_privs;
    web_uid_t   uid;
    web_gid_t   gid;
    const char *home;          // HOME for the dropped user; NULL leaves HOME untouched

    // Only for the no-SUDO_UID fallback, where Chrome cannot avoid running as root.
    int         no_sandbox;

    // Affinity for the browser process tree. Affinity is INHERITED ACROSS fork/exec, so a
    // caller that has pinned itself (the native prober pins to core 0) MUST pass an explicit
    // mask here -- otherwise Chrome silently inherits the prober's single core and destroys
    // the measurement. set_affinity=0 means "inherit", which is only correct for an
    // unpinned caller.
    int              set_affinity;
    const web_cpu_mask_t *cpu_mask;
} WebVictimCfg;

// ---- Site list ----
// Load '<slug>\t<url>' lines into `out` (capacity `max_sites`), skipping blanks and '#'
// comments. Returns the count, or -1. Every rejection is fatal and reported with a line
// number: a malformed list must cost seconds here, not a multi-hour run that writes a
// mislabelled tree. Rejects a bad slug charset, over-length fields, duplicate slugs, and
// fewer than 2 usable sites.
int web_site_list_load(const char *path, Site *out, int max_sites);

// True iff `s` is safe as BOTH a directory name and an .h5 label_map key ([A-Za-z0-9._-]+).
int web_slug_ok(const char *s);

// ---- Chrome ----
// fork + exec google-chrome per `cfg`. Returns the pid, or -1. Also arms the teardown
// command for web_pkill_armed() (see below).
pid_t web_launch_chrome(const WebVictimCfg *cfg);

// Block until the DevTools endpoint answers, up to `timeout_s`. Returns 0, or -1.
int web_wait_cdp(int timeout_s);

// Cheap liveness check. Run it OUTSIDE a trace window, so a dead browser is caught before a
// sample is requested rather than after a CSV has already been written for it.
int web_cdp_alive(void);

// Print how Chrome died (signal / exit status / wedged-but-alive). The distinction is what
// decides what to fix, and a log that only ever says "DevTools is gone" cannot make it.
void web_diagnose_chrome_death(pid_t pid);

// ---- Victim tab ----
// Open `url` in a new tab; copy the target id into `id`. Returns 0, or -1.
int web_victim_open(const char *url, char *id, size_t id_len);

// Close the victim tab. Best-effort: a failure leaks one tab, which the next sample's fresh
// tab makes harmless, so it warns rather than aborting.
void web_victim_close(const char *id);

// Split form of web_victim_open, for a sampler that must be RUNNING before the page loads.
//
// web_victim_open_send does the connect+write (microseconds) and returns immediately, so the
// caller can enter its measurement loop while Chrome is still creating the target and spawning
// the renderer -- the navigation therefore commits INSIDE the trace, a few ms after t=0.
// web_victim_open_recv then collects the target id once the trace is over.
//
// Returns a fd that MUST be passed to web_victim_open_recv (which closes it), or -1.
int  web_victim_open_send(const char *url);
int  web_victim_open_recv(int fd, char *id, size_t id_len);

// ---- QoS guard (Windows; no-ops elsewhere) ----
// Once a victim tab is in front, Windows Chrome puts the now-background SAMPLER renderer into
// "Efficiency mode": IDLE priority class + EcoQoS power throttling (measured on Chrome 154; its
// --disable-renderer-* switches do not prevent it). Desktop Linux Chrome does no such thing, so
// left alone the Windows traces would be taken under different conditions from the Ubuntu ones.
//
// The guard undoes it: the browser's job pins every Chrome process at NORMAL priority class
// (JOB_OBJECT_LIMIT_PRIORITY_CLASS), and a watcher thread switches EcoQoS (and low memory
// priority) back off whenever Chrome turns it on -- polling every 10 ms, and every 1 ms for the
// first 500 ms after each navigation, which is when the demotion happens. WEB_QOS_GUARD=0 turns
// the watcher off, for A/B measurement.
typedef struct {
    long   fixes;             // corrections applied since the last take
    double first_fix_ms;      // first correction, ms after the last web_qos_guard_navigated(); -1 if none
    double max_exposure_ms;   // upper bound on how long a process stayed throttled before correction
    double max_exposure_burst_ms;  // same, for corrections made during a post-navigation burst
    double max_pass_ms;       // longest single watcher pass (diagnostic)
    double max_burst_gap_ms;  // longest start-to-start gap between passes during a burst (diagnostic)
} WebQosStats;

void web_qos_guard_start(void);         // after web_launch_chrome
void web_qos_guard_navigated(void);     // right after opening a victim tab: enter burst polling
void web_qos_guard_take(WebQosStats *out);   // stats since the previous take, then reset
void web_qos_guard_stop(void);
int  web_qos_priority_locked(void);     // 1 if the job pins the priority class (else the watcher does)

// ---- Teardown ----
// Kill the browser process tree belonging to `profile_dir`.
void web_pkill_profile(const char *profile_dir);

// Same, using the command prepared by the last web_launch_chrome(). Safe to call from a
// signal handler path: the pattern was built at launch time, so this only runs system() on a
// pre-existing string. No-op if no browser was launched.
void web_pkill_armed(void);

#endif // WEB_VICTIM_H
