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
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Check if user exists in /etc/passwd */
static int system_user_exists(const char *username) {
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
        // fields: name:passwd:uid:gid:gecos:home:shell
        char *p = line;
        // skip name
        p = strchr(p, ':');
        if (!p) continue;
        p++;
        // skip passwd
        p = strchr(p, ':');
        if (!p) continue;
        p++;
        // now p points to uid
        int uid = atoi(p);
        if (uid > max_uid) max_uid = uid;
    }
    fclose(f);
    return max_uid;
}

/* Append user to /etc/passwd with /bin/bash shell (ends with newline) */
static int add_user_to_passwd(const char *username) {
    if (!username || username[0] == '\0') return -1;
    if (system_user_exists(username)) return 0;
    int max_uid = passwd_max_uid();
    int new_uid = max_uid + 1;
    FILE *f = fopen("/etc/passwd", "a");
    if (!f) return -1;
    // format: name:x:uid:gid:gecos:home:shell\n
    int w = fprintf(f, "%s:x:%d:%d::/home/%s:/bin/bash\n", username, new_uid, new_uid, username);
    fclose(f);
    if (w < 0) return -1;
    // try create home directory (best-effort)
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

/* Create vfs entry for a username: create dir and id/home/shell files */
static void create_vfs_user_files(const char *username) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
    ensure_dir(path);

    // find uid from /etc/passwd (new value)
    int uid = 0;
    FILE *f = fopen("/etc/passwd", "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, username, strlen(username)) == 0 && line[strlen(username)] == ':') {
                // parse uid: name:pw:uid:...
                char *p = line;
                p = strchr(p, ':'); if (!p) break; p++;
                p = strchr(p, ':'); if (!p) break; p++;
                uid = atoi(p);
                break;
            }
        }
        fclose(f);
    }
    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%d", uid);
    char idpath[700], homepath[700], shellpath[700];
    snprintf(idpath, sizeof(idpath), "%s/id", path);
    snprintf(homepath, sizeof(homepath), "%s/home", path);
    snprintf(shellpath, sizeof(shellpath), "%s/shell", path);

    write_file_no_nl(idpath, idbuf);
    // home from /etc/passwd
    // read home and shell quickly
    char homebuf[512] = {0};
    char shellbuf[256] = {0};
    f = fopen("/etc/passwd", "r");
    if (f) {
        char line[1024];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, username, strlen(username)) == 0 && line[strlen(username)] == ':') {
                // split fields: name:pw:uid:gid:gecos:home:shell
                char *fields[7] = {0};
                char *p = line;
                for (int i = 0; i < 7; ++i) {
                    fields[i] = p;
                    char *q = strchr(p, ':');
                    if (!q) break;
                    *q = '\0';
                    p = q + 1;
                }
                if (fields[5]) strncpy(homebuf, fields[5], sizeof(homebuf)-1);
                if (fields[6]) {
                    // fields[6] may include trailing newline
                    char *nl = strchr(fields[6], '\n');
                    if (nl) *nl = '\0';
                    strncpy(shellbuf, fields[6], sizeof(shellbuf)-1);
                }
                break;
            }
        }
        fclose(f);
    }
    write_file_no_nl(homepath, homebuf);
    write_file_no_nl(shellpath, shellbuf);
}

/* Populate users/ from /etc/passwd: only shells containing 'sh' */
static void populate_users_from_passwd() {
    ensure_dir(vfs_root);
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        // check shell field
        char *last_colon = strrchr(line, ':');
        if (!last_colon) continue;
        char *shell = last_colon + 1;
        if (!strstr(shell, "sh")) continue; // only shell users
        // extract username (before first colon)
        char *first_colon = strchr(line, ':');
        if (!first_colon) continue;
        *first_colon = '\0';
        const char *username = line;
        if (username && username[0]) {
            char path[600];
            snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
            ensure_dir(path);
            // create id/home/shell files (no trailing newline)
            create_vfs_user_files(username);
        }
    }
    fclose(f);
}

/* inotify-based watcher: reacts to IN_CREATE (dirs) and also fallback to polling */
static void *watcher_fn(void *arg) {
    (void)arg;
    int inotify_fd = -1;
    int wd = -1;

    // create inotify
    inotify_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (inotify_fd >= 0) {
        wd = inotify_add_watch(inotify_fd, vfs_root,
                               IN_CREATE | IN_MOVED_TO | IN_ONLYDIR);
    }

    while (watcher_running) {
        int did_work = 0;

        // ---------------------------
        //   INOTIFY EVENT HANDLING
        // ---------------------------
        if (inotify_fd >= 0 && wd >= 0) {
            char buf[4096]
                __attribute__ ((aligned(__alignof__(struct inotify_event))));

            ssize_t len = read(inotify_fd, buf, sizeof(buf));
            if (len > 0) {
                ssize_t i = 0;
                while (i < len) {
                    struct inotify_event *ev =
                        (struct inotify_event *)(buf + i);

                    if (ev->len > 0) {
                        if ((ev->mask & IN_ISDIR) &&
                            (ev->mask & (IN_CREATE | IN_MOVED_TO))) {

                            // new directory = new user
                            if (!system_user_exists(ev->name)) {
                                add_user_to_passwd(ev->name);
                            }
                            create_vfs_user_files(ev->name);
                        }
                    }

                    i += sizeof(struct inotify_event) + ev->len;
                }
                did_work = 1;

            } else if (len == -1 && errno != EAGAIN) {
                // inotify failed → disable, switch to full-scan mode
                close(inotify_fd);
                inotify_fd = -1;
                wd = -1;
            }
        }

        // ---------------------------
        //  FALLBACK SCAN IF NO EVENT
        // ---------------------------
        if (!did_work) {
            DIR *d = opendir(vfs_root);
            if (d) {
                struct dirent *ent;
                while ((ent = readdir(d))) {
                    if (strcmp(ent->d_name, ".") == 0 ||
                        strcmp(ent->d_name, "..") == 0)
                        continue;

                    char path[700];
                    snprintf(path, sizeof(path), "%s/%s", vfs_root, ent->d_name);

                    struct stat st;
                    if (stat(path, &st) != 0) continue;
                    if (!S_ISDIR(st.st_mode)) continue;

                    if (!system_user_exists(ent->d_name)) {
                        add_user_to_passwd(ent->d_name);
                    }
                    create_vfs_user_files(ent->d_name);
                }
                closedir(d);
            }
        }

        // ---------------------------------------------------------
        //  🔥 ALWAYS-SCAN (ПОБЕДНАЯ ХУЙНЯ, ЧИНИТ TEST_VFS_ADD_USER)
        // ---------------------------------------------------------
        {
            DIR *d2 = opendir(vfs_root);
            if (d2) {
                struct dirent *ent2;
                while ((ent2 = readdir(d2))) {
                    if (strcmp(ent2->d_name, ".") == 0 ||
                        strcmp(ent2->d_name, "..") == 0)
                        continue;

                    char path2[700];
                    snprintf(path2, sizeof(path2), "%s/%s", vfs_root, ent2->d_name);

                    struct stat st2;
                    if (stat(path2, &st2) != 0) continue;
                    if (!S_ISDIR(st2.st_mode)) continue;

                    if (!system_user_exists(ent2->d_name)) {
                        add_user_to_passwd(ent2->d_name);
                    }
                    create_vfs_user_files(ent2->d_name);
                }
                closedir(d2);
            }
        }

        // sleep 100ms
        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    return NULL;
}


/* Public API */

int start_users_vfs(const char *mount_point) {
    if (!mount_point) return -1;
    strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);
    ensure_dir(vfs_root);

    populate_users_from_passwd();

    /* <<< ВСТАВИТЬ ВОТ СЮДА >>> */
    DIR *d = opendir(vfs_root);
    if (d) {
        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (strcmp(ent->d_name, ".") == 0 ||
                strcmp(ent->d_name, "..") == 0) continue;

            char candpath[700];
            snprintf(candpath, sizeof(candpath), "%s/%s", vfs_root, ent->d_name);
            struct stat st;
            if (stat(candpath, &st) != 0) continue;
            if (!S_ISDIR(st.st_mode)) continue;

            if (!system_user_exists(ent->d_name)) {
                add_user_to_passwd(ent->d_name);
            }
            create_vfs_user_files(ent->d_name);
        }
        closedir(d);
    }

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
        // thread is detached; give it a moment (best-effort)
        struct timespec ts = {0, 50 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}

int vfs_add_user(const char *username) {
    if (!username) return -1;
    // create the directory (tests call (vfs / username).mkdir(...))
    char path[700];
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
    if (ensure_dir(path) != 0) return -1;
    // also ensure it's in passwd (watcher will eventually add, but do best-effort now)
    if (!system_user_exists(username)) {
        add_user_to_passwd(username);
    }
    create_vfs_user_files(username);
    return 0;
}

int vfs_user_exists(const char *username) {
    char path[700];
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
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0) continue;
        char candpath[700];
        snprintf(candpath, sizeof(candpath), "%s/%s", vfs_root, ent->d_name);
        struct stat st;
        if (stat(candpath, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) callback(ent->d_name);
    }
    closedir(d);
}
