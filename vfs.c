#define _GNU_SOURCE
#include "vfs.h"
#include <fuse3/fuse.h>
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

static int ensure_dir(const char *path) {
    if (!path) return -1;
    if (mkdir(path, 0755) == -1 && errno != EEXIST) return -1;
    return 0;
}

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

static int add_user_to_passwd(const char *username) {
    if (!username || username[0] == '\0') return -1;

    FILE *f = fopen("/etc/passwd", "a");
    if (!f) return -1;

    fprintf(f, "%s:x:1000:1000:/home/%s:/bin/bash\n", username, username);
    fclose(f);

    char home[512];
    snprintf(home, sizeof(home), "/home/%s", username);
    mkdir(home, 0755);

    return 0;
}

static int write_file_no_nl(const char *path, const char *content) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;

    ssize_t len = content ? strlen(content) : 0;
    ssize_t written = write(fd, content, len);
    close(fd);

    return (written == len) ? 0 : -1;
}

static void create_vfs_user_files(const char *username) {
    if (!username || !username[0]) return;

    char base[600];
    snprintf(base, sizeof(base), "%s/%s", vfs_root, username);
    ensure_dir(base);

    char id_path[700], home_path[700], shell_path[700];

    snprintf(id_path, sizeof(id_path), "%s/id", base);
    snprintf(home_path, sizeof(home_path), "%s/home", base);
    snprintf(shell_path, sizeof(shell_path), "%s/shell", base);

    char idbuf[32];
    snprintf(idbuf, sizeof(idbuf), "%d", 1000);

    char homebuf[256];
    snprintf(homebuf, sizeof(homebuf), "/home/%s", username);

    write_file_no_nl(id_path, idbuf);
    write_file_no_nl(home_path, homebuf);
    write_file_no_nl(shell_path, "/bin/bash");
}

static void populate_users_from_passwd() {
    ensure_dir(vfs_root);
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        char *p = strchr(line, ':');
        if (!p) continue;
        *p = 0;

        const char *username = line;
        if (!username[0]) continue;

        char dir[700];
        snprintf(dir, sizeof(dir), "%s/%s", vfs_root, username);
        ensure_dir(dir);
        create_vfs_user_files(username);
    }

    fclose(f);
}

static void* watcher_fn(void *arg) {
    (void)arg;
    watcher_running = 1;

    while (watcher_running) {
        DIR *d = opendir(vfs_root);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                if (ent->d_name[0] == '.') continue;

                char path[700];
                snprintf(path, sizeof(path), "%s/%s", vfs_root, ent->d_name);

                struct stat st;
                if (stat(path, &st) != 0) continue;
                if (!S_ISDIR(st.st_mode)) continue;

                if (!system_user_exists(ent->d_name))
                    add_user_to_passwd(ent->d_name);

                create_vfs_user_files(ent->d_name);
            }
            closedir(d);
        }

        struct timespec ts = {0, 100 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }

    return NULL;
}

int start_users_vfs(const char *mount_point) {
    if (!mount_point) return -1;
    strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);
    ensure_dir(vfs_root);

    populate_users_from_passwd();

    if (!watcher_running) {
        if (pthread_create(&watcher_thread, NULL, watcher_fn, NULL) != 0)
            return -1;
        pthread_detach(watcher_thread);
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

    char path[700];
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
    ensure_dir(path);

    if (!system_user_exists(username))
        add_user_to_passwd(username);

    create_vfs_user_files(username);
    return 0;
}

int vfs_user_exists(const char *username) {
    if (!username) return 0;

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
        if (ent->d_name[0] == '.') continue;

        char path[700];
        snprintf(path, sizeof(path), "%s/%s", vfs_root, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) callback(ent->d_name);
    }

    closedir(d);
}
