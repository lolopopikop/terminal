#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pwd.h>
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>
#include <time.h>

static struct passwd **users = NULL;
static int user_count = 0;
static int vfs_pid = -1;

/* ================= helpers ================= */

static int ends_with_sh(const char *s) {
    size_t l = strlen(s);
    return l >= 2 && strcmp(s + l - 2, "sh") == 0;
}

static void free_users() {
    if (!users) return;
    for (int i = 0; i < user_count; i++) {
        free(users[i]->pw_name);
        free(users[i]->pw_dir);
        free(users[i]->pw_shell);
        free(users[i]);
    }
    free(users);
    users = NULL;
    user_count = 0;
}

static void load_users() {
    free_users();

    struct passwd *p;
    setpwent();
    while ((p = getpwent()))
        user_count++;
    endpwent();

    users = calloc(user_count, sizeof(struct passwd*));

    setpwent();
    int i = 0;
    while ((p = getpwent())) {
        users[i] = calloc(1, sizeof(struct passwd));
        users[i]->pw_name  = strdup(p->pw_name);
        users[i]->pw_dir   = strdup(p->pw_dir);
        users[i]->pw_shell = strdup(p->pw_shell);
        users[i]->pw_uid   = p->pw_uid;
        i++;
    }
    endpwent();
}

/* ================= FUSE ================= */

static int users_getattr(const char *path, struct stat *st, struct fuse_file_info *fi) {
    (void) fi;
    memset(st, 0, sizeof(*st));

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }

    char user[256], file[256];
    if (sscanf(path, "/%255[^/]/%255s", user, file) == 2) {
        st->st_mode = S_IFREG | 0444;
        st->st_nlink = 1;
        return 0;
    }

    if (sscanf(path, "/%255[^/]", user) == 1) {
        for (int i = 0; i < user_count; i++) {
            if (!strcmp(users[i]->pw_name, user) &&
                ends_with_sh(users[i]->pw_shell)) {
                st->st_mode = S_IFDIR | 0755;
                st->st_nlink = 2;
                return 0;
            }
        }
    }

    return -ENOENT;
}

static int users_readdir(const char *path, void *buf, fuse_fill_dir_t fill,
                         off_t off, struct fuse_file_info *fi,
                         enum fuse_readdir_flags flags) {
    (void) off; (void) fi; (void) flags;

    fill(buf, ".", NULL, 0, 0);
    fill(buf, "..", NULL, 0, 0);

    if (strcmp(path, "/") == 0) {
        for (int i = 0; i < user_count; i++) {
            if (ends_with_sh(users[i]->pw_shell))
                fill(buf, users[i]->pw_name, NULL, 0, 0);
        }
        return 0;
    }

    fill(buf, "id", NULL, 0, 0);
    fill(buf, "home", NULL, 0, 0);
    fill(buf, "shell", NULL, 0, 0);
    return 0;
}

static int users_read(const char *path, char *buf, size_t sz,
                      off_t off, struct fuse_file_info *fi) {
    (void) fi;

    char user[256], file[256];
    if (sscanf(path, "/%255[^/]/%255s", user, file) != 2)
        return -ENOENT;

    for (int i = 0; i < user_count; i++) {
        if (!strcmp(users[i]->pw_name, user)) {
            const char *out = NULL;
            char tmp[32];

            if (!strcmp(file, "id")) {
                snprintf(tmp, sizeof(tmp), "%d", users[i]->pw_uid);
                out = tmp;
            } else if (!strcmp(file, "home")) {
                out = users[i]->pw_dir;
            } else if (!strcmp(file, "shell")) {
                out = users[i]->pw_shell;
            }

            size_t len = strlen(out);
            if (off >= (off_t)len) return 0;
            if (off + sz > len) sz = len - off;
            memcpy(buf, out + off, sz);
            return sz;
        }
    }

    return -ENOENT;
}

static int users_mkdir(const char *path, mode_t mode) {
    (void) mode;

    char user[256];
    sscanf(path, "/%255s", user);

    FILE *f = fopen("/etc/passwd", "a");
    int uid = 1000 + rand() % 10000;
    fprintf(f, "%s:x:%d:%d::/home/%s:/bin/sh\n",
            user, uid, uid, user);
    fclose(f);

    load_users();
    return 0;
}

static struct fuse_operations ops = {
    .getattr = users_getattr,
    .readdir = users_readdir,
    .read    = users_read,
    .mkdir   = users_mkdir,
};

int start_users_vfs(const char *mnt) {
    mkdir(mnt, 0755);
    load_users();

    if ((vfs_pid = fork()) == 0) {
        char *argv[] = { "vfs", "-f", (char*)mnt, NULL };
        fuse_main(3, argv, &ops, NULL);
        exit(0);
    }

    sleep(1);
    return 0;
}

void stop_users_vfs() {
    if (vfs_pid > 0)
        kill(vfs_pid, SIGTERM);
}
