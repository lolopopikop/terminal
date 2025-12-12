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

/* NEW — explicitly set VFS root */
void vfs_set_root(const char *p) {
    if (!p) return;
    strncpy(vfs_root, p, sizeof(vfs_root)-1);
    vfs_root[sizeof(vfs_root)-1] = '\0';
}

static int ensure_dir(const char *path) {
    if (!path) return -1;
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Check if user exists in /etc/passwd */
static int system_user_exists(const char *username) {
    if (!username) return 0;
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 0;
    char line[2048];
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

static int passwd_max_uid() {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 1000;
    char line[2048];
    int max_uid = 1000;
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        char *q = strchr(p, ':'); if (!q) continue; p = q + 1;
        q = strchr(p, ':'); if (!q) continue; p = q + 1;
        int uid = atoi(p);
        if (uid > max_uid) max_uid = uid;
    }
    fclose(f);
    return max_uid;
}

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

    char home[512];
    snprintf(home, sizeof(home), "/home/%s", username);
    mkdir(home, 0755);
    return 0;
}

static int write_file_no_nl(const char *path, const char *content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    ssize_t len = content ? strlen(content) : 0;
    ssize_t off = 0;
    while (off < len) {
        ssize_t r = write(fd, content + off, len - off);
        if (r <= 0) { close(fd); return -1; }
        off += r;
    }
    close(fd);
    return 0;
}

static int fill_user_from_passwd(const char *username,
                                 int *out_uid,
                                 char *out_home, size_t home_sz,
                                 char *out_shell, size_t shell_sz)
{
    if (!username) return 0;
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 0;
    char line[2048];
    size_t un_len = strlen(username);
    int found = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, username, un_len) == 0 && line[un_len] == ':') {
            char *fields[8];
            char *p = line;
            for (int i = 0; i < 7; i++) {
                fields[i] = p;
                char *q = strchr(p, ':');
                if (!q) { fields[i+1] = NULL; break; }
                *q = '\0';
                p = q + 1;
            }
            *out_uid = fields[2] ? atoi(fields[2]) : 0;

            if (fields[5]) {
                strncpy(out_home, fields[5], home_sz - 1);
                out_home[home_sz-1] = '\0';
            } else out_home[0] = '\0';

            if (fields[6]) {
                char *nl = strchr(fields[6], '\n');
                if (nl) *nl = '\0';
                strncpy(out_shell, fields[6], shell_sz - 1);
                out_shell[shell_sz-1] = '\0';
            } else out_shell[0] = '\0';

            found = 1;
            break;
        }
    }
    fclose(f);
    return found;
}

static void create_vfs_user_files(const char *username) {
    if (!username) return;
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
    ensure_dir(path);

    int uid = 0;
    char homebuf[512] = {0};
    char shellbuf[256] = {0};

    fill_user_from_passwd(username, &uid, homebuf, sizeof(homebuf), shellbuf, sizeof(shellbuf));

    char idbuf[64];
    snprintf(idbuf, sizeof(idbuf), "%d", uid);

    char idpath[1200], homepath[1200], shellpath[1200];
    snprintf(idpath, sizeof(idpath), "%s/id", path);
    snprintf(homepath, sizeof(homepath), "%s/home", path);
    snprintf(shellpath, sizeof(shellpath), "%s/shell", path);

    write_file_no_nl(idpath, idbuf);
    write_file_no_nl(homepath, homebuf);
    write_file_no_nl(shellpath, shellbuf);
}

static void *watcher_fn(void *arg) {
    (void)arg;

    int in_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    int wd = -1;

    if (in_fd >= 0)
        wd = inotify_add_watch(in_fd, vfs_root, IN_CREATE | IN_MOVED_TO | IN_ONLYDIR);

    while (watcher_running) {
        int did = 0;

        if (in_fd >= 0 && wd >= 0) {
            char buf[4096];
            ssize_t len = read(in_fd, buf, sizeof(buf));
            if (len > 0) {
                ssize_t i = 0;
                while (i < len) {
                    struct inotify_event *ev = (struct inotify_event*)(buf + i);
                    if ((ev->mask & IN_ISDIR) &&
                        (ev->mask & (IN_CREATE | IN_MOVED_TO))) {

                        if (!system_user_exists(ev->name))
                            add_user_to_passwd(ev->name);

                        create_vfs_user_files(ev->name);
                    }
                    i += sizeof(struct inotify_event) + ev->len;
                }
                did = 1;
            }
        }

        if (!did) {
            DIR *d = opendir(vfs_root);
            if (d) {
                struct dirent *e;
                while ((e = readdir(d))) {
                    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
                        continue;

                    char full[1200];
                    snprintf(full, sizeof(full), "%s/%s", vfs_root, e->d_name);

                    struct stat st;
                    if (stat(full, &st) != 0) continue;
                    if (!S_ISDIR(st.st_mode)) continue;

                    if (!system_user_exists(e->d_name))
                        add_user_to_passwd(e->d_name);

                    create_vfs_user_files(e->d_name);
                }
                closedir(d);
                did = 1;
            }
        }

        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    if (in_fd >= 0) close(in_fd);
    return NULL;
}

int start_users_vfs(const char *p) {
    if (!p) return -1;
    vfs_set_root(p);
    ensure_dir(vfs_root);

    if (!watcher_running) {
        watcher_running = 1;
        if (pthread_create(&watcher_thread, NULL, watcher_fn, NULL) != 0) {
            watcher_running = 0;
            return -1;
        }
        pthread_detach(watcher_thread);
    }

    DIR *d = opendir(vfs_root);
    if (d) {
        struct dirent *e;
        while ((e = readdir(d))) {
            if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

            char full[1200];
            snprintf(full, sizeof(full), "%s/%s", vfs_root, e->d_name);
            struct stat st;
            if (stat(full, &st) != 0) continue;
            if (!S_ISDIR(st.st_mode)) continue;

            if (!system_user_exists(e->d_name))
                add_user_to_passwd(e->d_name);

            create_vfs_user_files(e->d_name);
        }
        closedir(d);
    }

    return 0;
}

void stop_users_vfs() {
    watcher_running = 0;
    struct timespec ts = {0, 50 * 1000 * 1000};
    nanosleep(&ts, NULL);
}

int vfs_add_user(const char *username) {
    if (!username) return -1;

    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
    ensure_dir(path);

    if (!system_user_exists(username))
        add_user_to_passwd(username);

    create_vfs_user_files(username);
    return 0;
}

int vfs_user_exists(const char *username) {
    if (!username) return 0;
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void vfs_list_users(void (*cb)(const char *)) {
    if (!cb) return;

    DIR *d = opendir(vfs_root);
    if (!d) return;

    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;

        char full[1200];
        snprintf(full, sizeof(full), "%s/%s", vfs_root, e->d_name);

        struct stat st;
        if (stat(full, &st) != 0) continue;
        if (S_ISDIR(st.st_mode))
            cb(e->d_name);
    }

    closedir(d);
}
