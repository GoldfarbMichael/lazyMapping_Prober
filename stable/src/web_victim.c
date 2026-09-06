// web_victim.c -- see web_victim.h for the contract and the three load-bearing subtleties.

#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

#include "web_victim.h"
#include "fp_ctl.h"

// Teardown command, built once at launch so the signal-handler path never formats a string.
static char g_pkill_cmd[512] = "";

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
