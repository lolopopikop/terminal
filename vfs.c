#include "vfs.h"
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <dirent.h>
#include <stdlib.h>

static char g_root[1024];

void vfs_init(const char *root) {
    snprintf(g_root, sizeof(g_root), "%s", root);
}

int vfs_user_exists(const char *username) {
    char path[2048];
    snprintf(path, sizeof(path), "%s/%s", g_root, username);

    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static void write_file(const char *path, const char *content) {
    FILE *f = fopen(path, "w");
    if (!f) return;
    fputs(content, f);
    fclose(f);
}

static void add_user_to_passwd(const char *username) {
    FILE *f = fopen("/etc/passwd", "a");
    if (!f) return;

    fprintf(f, "%s:x:1001:1001::/home/%s:/bin/sh\n",
            username, username);

    fclose(f);
}

static void create_files(const char *username) {
    char dir[2048];
    snprintf(dir, sizeof(dir), "%s/%s", g_root, username);

    char id_path[2048];
    char home_path[2048];
    char shell_path[2048];

    snprintf(id_path, sizeof(id_path), "%s/id", dir);
    snprintf(home_path, sizeof(home_path), "%s/home", dir);
    snprintf(shell_path, sizeof(shell_path), "%s/shell", dir);

    write_file(id_path, "1001");
    char homebuf[100];
    snprintf(homebuf, sizeof(homebuf), "/home/%s", username);
    write_file(home_path, homebuf);
    write_file(shell_path, "/bin/sh");
}

int vfs_add_user(const char *username) {
    char dir[2048];
    snprintf(dir, sizeof(dir), "%s/%s", g_root, username);

    mkdir(dir, 0777);
    create_files(username);
    add_user_to_passwd(username);

    // --- FIX: force disk flush ---
    sync();
    return 0;
}
