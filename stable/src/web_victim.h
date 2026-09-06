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
// -----------------------------------------------------------------------------

#ifndef WEB_VICTIM_H
#define WEB_VICTIM_H

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <sched.h>
#include <sys/types.h>
#include <stddef.h>

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
    uid_t       uid;
    gid_t       gid;
    const char *home;          // HOME for the dropped user; NULL leaves HOME untouched

    // Only for the no-SUDO_UID fallback, where Chrome cannot avoid running as root.
    int         no_sandbox;

    // Affinity for the browser process tree. Affinity is INHERITED ACROSS fork/exec, so a
    // caller that has pinned itself (the native prober pins to core 0) MUST pass an explicit
    // mask here -- otherwise Chrome silently inherits the prober's single core and destroys
    // the measurement. set_affinity=0 means "inherit", which is only correct for an
    // unpinned caller.
    int              set_affinity;
    const cpu_set_t *cpu_mask;
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

// ---- Teardown ----
// Kill the browser process tree belonging to `profile_dir`.
void web_pkill_profile(const char *profile_dir);

// Same, using the command prepared by the last web_launch_chrome(). Safe to call from a
// signal handler path: the pattern was built at launch time, so this only runs system() on a
// pre-existing string. No-op if no browser was launched.
void web_pkill_armed(void);

#endif // WEB_VICTIM_H
