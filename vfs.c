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

    // Track system users that have a valid shell
    char line[512];
    std::set<std::string> system_shell_users;

    while (fgets(line, sizeof(line), f)) {
        char *fields[7];
        int idx = 0;

        fields[idx++] = strtok(line, ":");
        while (idx < 7 && (fields[idx] = strtok(NULL, ":"))) idx++;

        if (idx < 7) continue;

        const char *name = fields[0];
        const char *uid  = fields[2];
        const char *gid  = fields[3];
        const char *gecos = fields[4];
        const char *home = fields[5];
        const char *shell = fields[6];

        // only include valid shell users
        if (!strstr(shell, "sh")) continue;

        system_shell_users.insert(name);

        // Create directory
        char path[600];
        snprintf(path, sizeof(path), "%s/%s", vfs_root, name);
        ensure_dir(path);

        // Write fields
        write_file(std::string(path) + "/id", uid);
        write_file(std::string(path) + "/gid", gid);
        write_file(std::string(path) + "/name", gecos);
        write_file(std::string(path) + "/home", home);
        write_file(std::string(path) + "/shell", shell);
    }

    fclose(f);

    // Scan users/ for directories not in /etc/passwd → create user in passwd
    DIR *d = opendir(vfs_root);
    if (!d) return;

    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;

        if (system_shell_users.count(ent->d_name))
            continue;

        // user exists in VFS but not in passwd → add

        char passwd_entry[512];
        snprintf(passwd_entry, sizeof(passwd_entry),
                 "%s:x:10000:10000::/home/%s:/bin/sh\n",
                 ent->d_name, ent->d_name);

        FILE *pf = fopen("/etc/passwd", "a");
        if (pf) {
            fputs(passwd_entry, pf);
            fclose(pf);
        }

        // Ensure files exist
        std::string base = std::string(vfs_root) + "/" + ent->d_name;
        write_file(base + "/id", "10000");
        write_file(base + "/gid", "10000");
        write_file(base + "/name", "");
        write_file(base + "/home", ("/home/" + std::string(ent->d_name)).c_str());
        write_file(base + "/shell", "/bin/sh");
    }

    closedir(d);
}



int start_users_vfs(const char *mount_point) {

    strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);
    ensure_dir(vfs_root);

    if (getenv("CI")) {
        // В CI: FUSE не монтируем, но VFS ДОЛЖЕН работать полностью.
        printf("---\nCI ENV detected — FUSE disabled, VFS active\n");

        populate_users();
        vfs_enabled = 1;     // ВАЖНО: VFS НЕ отключаем
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
