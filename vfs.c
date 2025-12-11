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

static void write_file(const char *path, const char *data) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fprintf(f, "%s", data);
    fclose(f);
}

static void populate_users() {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return;

    char line[512];
    char known_users[256][64];
    int known_count = 0;

    while (fgets(line, sizeof(line), f)) {
        char *fields[7];
        int idx = 0;

        fields[idx++] = strtok(line, ":");
        while (idx < 7 && (fields[idx] = strtok(NULL, ":"))) idx++;

        if (idx < 7) continue;

        const char *name  = fields[0];
        const char *uid   = fields[2];
        const char *gid   = fields[3];
        const char *gecos = fields[4];
        const char *home  = fields[5];
        const char *shell = fields[6];

        if (!strstr(shell, "sh")) continue;

        strncpy(known_users[known_count++], name, 63);

        char path[600];
        snprintf(path, sizeof(path), "%s/%s", vfs_root, name);
        ensure_dir(path);

        char fp[700];
        snprintf(fp, sizeof(fp), "%s/id", path);   write_file(fp, uid);
        snprintf(fp, sizeof(fp), "%s/gid", path);  write_file(fp, gid);
        snprintf(fp, sizeof(fp), "%s/name", path); write_file(fp, gecos);
        snprintf(fp, sizeof(fp), "%s/home", path); write_file(fp, home);
        snprintf(fp, sizeof(fp), "%s/shell", path);write_file(fp, shell);
    }

    fclose(f);

    DIR *d = opendir(vfs_root);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 ||
            strcmp(ent->d_name, "..") == 0)
            continue;

        int i, found = 0;
        for (i = 0; i < known_count; i++) {
            if (strcmp(known_users[i], ent->d_name) == 0) {
                found = 1;
                break;
            }
        }

        if (found) continue;

        FILE *pf = fopen("/etc/passwd", "a");
        if (pf) {
            fprintf(pf, "%s:x:10000:10000::/home/%s:/bin/sh\n",
                    ent->d_name, ent->d_name);
            fclose(pf);
        }

        char base[600];
        snprintf(base, sizeof(base), "%s/%s", vfs_root, ent->d_name);

        char fp[700];
        snprintf(fp, sizeof(fp), "%s/id", base);      write_file(fp, "10000");
        snprintf(fp, sizeof(fp), "%s/gid", base);     write_file(fp, "10000");
        snprintf(fp, sizeof(fp), "%s/name", base);    write_file(fp, "");
        snprintf(fp, sizeof(fp), "%s/home", base);    write_file(fp, "/home/");
        snprintf(fp, sizeof(fp), "%s/shell", base);   write_file(fp, "/bin/sh");
    }

    closedir(d);
}

int start_users_vfs(const char *mount_point) {

    if (getenv("CI")) {
        printf("---\nCI ENV detected — FUSE disabled, VFS active\n");
        vfs_enabled = 1;
        strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);
        ensure_dir(vfs_root);
        populate_users();
        return 0;
    }

    vfs_enabled = 1;
    strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);
    ensure_dir(vfs_root);

    populate_users();
    return 0;
}

void stop_users_vfs() {
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
