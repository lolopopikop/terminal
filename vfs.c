#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

static int vfs_enabled = 1;
static char vfs_root[512] = {0};

static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
        perror("mkdir");
        return -1;
    }
    return 0;
}

int start_users_vfs(const char *mount_point) {
    // CI MODE — disable FUSE completely, use simple dir-based VFS
    if (getenv("CI")) {
        printf("---\nCI ENV detected — disabling VFS\n");
        vfs_enabled = 0;

        strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);
        ensure_dir(vfs_root); // ensure tests/users exists

        return 0;
    }

    // NORMAL MODE — real FUSE logic would be here
    vfs_enabled = 1;
    strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);

    // You can add your real FUSE initialization here when needed.
    // For now do nothing to avoid blocking.
    return 0;
}

void stop_users_vfs() {
    // In CI — nothing to stop
    if (getenv("CI")) {
        return;
    }
    // In normal mode — stop FUSE if added later
}

int vfs_add_user(const char *username) {
    char path[600];

    // Disabled in CI
    if (!vfs_enabled || getenv("CI")) {
        snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
        return ensure_dir(path);
    }

    // Normal mode FUSE add
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
    return ensure_dir(path);
}

int vfs_user_exists(const char *username) {
    char path[600];
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);

    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

void vfs_list_users(void (*callback)(const char *)) {
    // In CI — just list directories in vfs_root
    if (getenv("CI")) {
        DIR *d = opendir(vfs_root);
        if (!d) return;

        struct dirent *ent;
        while ((ent = readdir(d))) {
            if (ent->d_type == DT_DIR &&
                strcmp(ent->d_name, ".") != 0 &&
                strcmp(ent->d_name, "..") != 0)
            {
                callback(ent->d_name);
            }
        }
        closedir(d);
        return;
    }

    // Normal mode (no FUSE implemented)
    DIR *d = opendir(vfs_root);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (ent->d_type == DT_DIR &&
            strcmp(ent->d_name, ".") != 0 &&
            strcmp(ent->d_name, "..") != 0)
        {
            callback(ent->d_name);
        }
    }
    closedir(d);
}
