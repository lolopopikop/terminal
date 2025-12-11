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

static int vfs_enabled = 1;
static char vfs_root[512] = {0};

static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
        perror("mkdir");
        return -1;
    }
    return 0;
}

/* check if user exists in /etc/passwd */
static int system_user_exists(const char *username) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 0;

    char line[512];
    int ok = 0;

    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, username, strlen(username)) == 0 &&
            line[strlen(username)] == ':')
        {
            ok = 1;
            break;
        }
    }

    fclose(f);
    return ok;
}

static void populate_users() {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char *shell = strrchr(line, ':');
        if (!shell) continue;
        shell++;

        if (!strstr(shell, "sh")) continue;

        char *name_end = strchr(line, ':');
        if (!name_end) continue;

        *name_end = '\0';

        char path[600];
        snprintf(path, sizeof(path), "%s/%s", vfs_root, line);
        ensure_dir(path);
    }

    fclose(f);
}

int start_users_vfs(const char *mount_point) {

    strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);
    ensure_dir(vfs_root);

    if (getenv("CI")) {
        printf("---\nCI ENV detected — disabling FUSE but populating VFS\n");

        DIR *d = opendir(vfs_root);
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                if (strcmp(ent->d_name, ".") == 0 ||
                    strcmp(ent->d_name, "..") == 0)
                    continue;

                char buf[600];
                snprintf(buf, sizeof(buf), "%s/%s", vfs_root, ent->d_name);
                unlink(buf);
                rmdir(buf);
            }
            closedir(d);
        }

        populate_users();
        vfs_enabled = 0;
        return 0;
    }

    vfs_enabled = 1;

    populate_users();

    return 0;
}

void stop_users_vfs() {
    /* nothing */
}

int vfs_add_user(const char *username) {

    if (!system_user_exists(username))
        return -1;

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
