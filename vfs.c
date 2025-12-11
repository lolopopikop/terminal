#include "vfs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <dirent.h>   // ← ЭТО ГЛАВНАЯ ФИКС
#include <fcntl.h>

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

    if (getenv("CI")) {
        printf("---\nCI ENV detected — disabling VFS\n");
        vfs_enabled = 0;

        strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);
        ensure_dir(vfs_root); // tests/users

        return 0;
    }

    vfs_enabled = 1;
    strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);
    ensure_dir(vfs_root);

    return 0;
}

void stop_users_vfs() {
    // nothing for now
}

int vfs_add_user(const char *username) {
    char path[600];

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
