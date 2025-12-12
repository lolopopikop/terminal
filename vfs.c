#define _GNU_SOURCE
#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <time.h>
#include <sys/inotify.h>
#include <limits.h>

static char vfs_root[512] = {0};
static pthread_t watcher_thread;
static int watcher_running = 0;

/* Helpers */
static int ensure_dir(const char *path) {
    if (!path) return -1;
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Check if user exists in /etc/passwd (exact match on username) */
static int system_user_exists(const char *username) {
    if (!username) return 0;
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 0;
    char line[1024];
    size_t un_len = strlen(username);
    int ok = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, username, un_len) == 0 && line[un_len] == ':') {
            ok = 1;
            break;
        }
    }
    fclose(f);
    return ok;
}

/* Return max uid found in /etc/passwd (or 1000) */
static int passwd_max_uid() {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 1000;
    char line[1024];
    int max_uid = 1000;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        p = strchr(p, ':'); if (!p) continue; p++;
        p = strchr(p, ':'); if (!p) continue; p++;
        int uid = atoi(p);
        if (uid > max_uid) max_uid = uid;
    }
    fclose(f);
    return max_uid;
}

/* Append user to /etc/passwd with /bin/bash shell (ends with newline).
   Returns 0 on success, -1 on failure. */
static int add_user_to_passwd(const char *username) {
    if (!username || username[0] == '\0') return -1;
    if (system_user_exists(username)) return 0;
    int max_uid = passwd_max_uid();
    int new_uid = max_uid + 1;
    FILE *f = fopen("/etc/passwd", "a");
    if (!f) return -1;
    int w = fprintf(f, "%s:x:%d:%d::/home/%s:/bin/bash\n",
                    username, new_uid, new_uid, username);
    fclose(f);
    if (w < 0) return -1;
    /* best-effort create home directory */
    char home[512];
    snprintf(home, sizeof(home), "/home/%s", username);
    mkdir(home, 0755);
    return 0;
}

/* Write small file WITHOUT trailing newline (important for tests) */
static int write_file_no_nl(const char *path, const char *content) {
    if (!path) return -1;
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t len = content ? (ssize_t)strlen(content) : 0;
    ssize_t wrote = 0;
    while (wrote < len) {
        ssize_t r = write(fd, content + wrote, len - wrote);
        if (r <= 0) { close(fd); return -1; }
        wrote += r;
    }
    close(fd);
    return 0;
}

/* Create vfs entry for a username: create dir and id/home/shell files (no trailing newline) */
static void create_vfs_user_files(const char *username) {
    if (!username || username[0] == '\0') return;
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
    ensure_dir(path);

    /* read UID, home, shell from /etc/passwd if present */
    int uid = 0;
    char homebuf[512] = {0};
    char shellbuf[256] = {0};
    FILE *f = fopen("/etc/passwd", "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, username, strlen(username)) == 0 && line[strlen(username)] == ':') {
                /* parse fields: name:pw:uid:gid:gecos:home:shell */
                char *fields[8] = {0};
                char *p = line;
                for (int i = 0; i < 7 && p; ++i) {
                    fields[i] = p;
                    char *q = strchr(p, ':');
                    if (!q) break;
                    *q = '\0';
                    p = q + 1;
                }
                if (fields[2]) uid = atoi(fields[2]);
                if (fields[5]) strncpy(homebuf, fields[5], sizeof(homebuf)-1);
                if (fields[6]) {
                    char *nl = strchr(fields[6], '\n');
                    if (nl) *nl = '\0';
                    strncpy(shellbuf, fields[6], sizeof(shellbuf)-1);
                }
                break;
            }
        }
        fclose(f);
    }

    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%d", uid);
    char idpath[800], homepath[800], shellpath[800];
    snprintf(idpath, sizeof(idpath), "%s/id", path);
    snprintf(homepath, sizeof(homepath), "%s/home", path);
    snprintf(shellpath, sizeof(shellpath), "%s/shell", path);

    write_file_no_nl(idpath, idbuf);
    write_file_no_nl(homepath, homebuf);
    write_file_no_nl(shellpath, shellbuf);
}

/* Ensure that a username ends up in /etc/passwd within timeout.
   Calls add_user_to_passwd() and then polls until system_user_exists() or timeout (ms). */
static int ensure_user_in_passwd_wait(const char *username, int timeout_ms) {
    if (!username) return -1;
    /* attempt immediate add (best-effort) */
    add_user_to_passwd(username);

    const int step_ms = 10;
    int waited = 0;
    while (waited < timeout_ms) {
        if (system_user_exists(username)) return 0;
        struct timespec ts = {0, step_ms * 1000 * 1000};
        nanosleep(&ts, NULL);
        waited += step_ms;
    }
    /* final check */
    return system_user_exists(username) ? 0 : -1;
}

/* Scan existing directories under vfs_root and ensure they are present in /etc/passwd,
   and create VFS files. This is used at start and periodically to reduce race windows. */
static void scan_existing_dirs_and_sync() {
    DIR *d = opendir(vfs_root);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char candpath[800];
        snprintf(candpath, sizeof(candpath), "%s/%s", vfs_root, ent->d_name);
        struct stat st;
        if (stat(candpath, &st) != 0) continue;
        if (!S_ISDIR(st.st_mode)) continue;
        /* ensure passwd entry (wait briefly for it to appear) */
        if (!system_user_exists(ent->d_name)) {
            ensure_user_in_passwd_wait(ent->d_name, 300);
        }
        /* create the vfs files (uid/home/shell) */
        create_vfs_user_files(ent->d_name);
    }
    closedir(d);
}

/* inotify + fallback watcher that aggressively scans to minimize race.
   On new dir, try to add to /etc/passwd and create vfs files, waiting shortly for /etc/passwd update. */
static void *watcher_fn(void *arg) {
    (void)arg;
    int inotify_fd = -1;
    int wd = -1;

    /* try inotify */
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd >= 0) {
        wd = inotify_add_watch(inotify_fd, vfs_root, IN_CREATE | IN_MOVED_TO | IN_ONLYDIR);
        /* ignore errors; fallback scan will still work */
    }

    /* initial scan to pick up dirs created before watcher started */
    scan_existing_dirs_and_sync();

    while (watcher_running) {
        int did_work = 0;

        if (inotify_fd >= 0 && wd >= 0) {
            char buf[4096]
                __attribute__ ((aligned(__alignof__(struct inotify_event))));
            ssize_t len = read(inotify_fd, buf, sizeof(buf));
            if (len > 0) {
                ssize_t i = 0;
                while (i < len) {
                    struct inotify_event *ev = (struct inotify_event *)(buf + i);
                    if (ev->len > 0) {
                        if ((ev->mask & IN_ISDIR) && (ev->mask & (IN_CREATE | IN_MOVED_TO))) {
                            /* new directory created/moved into vfs_root */
                            ensure_user_in_passwd_wait(ev->name, 300);
                            create_vfs_user_files(ev->name);
                        }
                    }
                    i += sizeof(struct inotify_event) + ev->len;
                }
                did_work = 1;
            } else if (len == -1 && errno != EAGAIN) {
                /* inotify failed; close and fallback to scanning */
                close(inotify_fd);
                inotify_fd = -1;
                wd = -1;
            }
        }

        /* quick safety scan: detect any dirs in vfs_root and ensure they are reflected in passwd */
        if (!did_work) {
            DIR *d = opendir(vfs_root);
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d))) {
                    if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                    char candpath[800];
                    snprintf(candpath, sizeof(candpath), "%s/%s", vfs_root, ent->d_name);
                    struct stat st;
                    if (stat(candpath, &st) != 0) continue;
                    if (!S_ISDIR(st.st_mode)) continue;
                    if (!system_user_exists(ent->d_name)) {
                        ensure_user_in_passwd_wait(ent->d_name, 300);
                        create_vfs_user_files(ent->d_name);
                    } else {
                        /* refresh files in case they were removed */
                        create_vfs_user_files(ent->d_name);
                    }
                }
                closedir(d);
                did_work = 1;
            }
        }

        /* short sleep to keep responsive */
        struct timespec ts = {0, 10 * 1000 * 1000}; /* 10 ms */
        nanosleep(&ts, NULL);
    }

    if (inotify_fd >= 0) close(inotify_fd);
    return NULL;
}

/* Public API */

int start_users_vfs(const char *mount_point) {
    if (!mount_point) return -1;
    strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);
    ensure_dir(vfs_root);

    /* initial population from /etc/passwd -> create vfs dirs/files for users whose shell contains "sh" */
    FILE *f = fopen("/etc/passwd", "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            char *last_colon = strrchr(line, ':');
            if (!last_colon) continue;
            char *shell = last_colon + 1;
            if (!strstr(shell, "sh")) continue;
            char *first_colon = strchr(line, ':');
            if (!first_colon) continue;
            *first_colon = '\0';
            const char *username = line;
            if (username && username[0]) {
                char path[800];
                snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
                ensure_dir(path);
                create_vfs_user_files(username);
            }
        }
        fclose(f);
    }

    /* scan existing directories (in case tests created dirs before start) */
    scan_existing_dirs_and_sync();

    /* start watcher thread if not running */
    if (!watcher_running) {
        watcher_running = 1;
        if (pthread_create(&watcher_thread, NULL, watcher_fn, NULL) != 0) {
            watcher_running = 0;
            return -1;
        }
        pthread_detach(watcher_thread);
    }

    return 0;
}

void stop_users_vfs() {
    if (watcher_running) {
        watcher_running = 0;
        /* let watcher exit */
        struct timespec ts = {0, 30 * 1000 * 1000}; /* 30 ms */
        nanosleep(&ts, NULL);
    }
    /* final sync before exit */
    scan_existing_dirs_and_sync();
}

int vfs_add_user(const char *username) {
    if (!username) return -1;
    char path[800];
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
    if (ensure_dir(path) != 0) return -1;
    if (!system_user_exists(username)) {
        ensure_user_in_passwd_wait(username, 300);
    }
    create_vfs_user_files(username);
    return 0;
}

int vfs_user_exists(const char *username) {
    if (!username) return 0;
    char path[800];
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void vfs_list_users(void (*callback)(const char *)) {
    if (!callback) return;
    DIR *d = opendir(vfs_root);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        char candpath[800];
        snprintf(candpath, sizeof(candpath), "%s/%s", vfs_root, ent->d_name);
        struct stat st;
        if (stat(candpath, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) callback(ent->d_name);
    }
    closedir(d);
}
