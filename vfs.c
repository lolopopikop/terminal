#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pwd.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <time.h>

static struct passwd **users = NULL;
static int user_count = 0;
static int vfs_pid = -1;

static int shell_is_sh(const char *shell) {
    size_t l = strlen(shell);
    return l >= 2 && strcmp(shell + l - 2, "sh") == 0;
}

static void free_users() {
    for (int i = 0; i < user_count; i++) {
        free(users[i]->pw_name);
        free(users[i]->pw_passwd);
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
    setpwent();

    struct passwd *pw;
    while ((pw = getpwent())) {
        if (!shell_is_sh(pw->pw_shell)) continue;

        users = realloc(users, sizeof(struct passwd*) * (user_count + 1));
        users[user_count] = malloc(sizeof(struct passwd));

        users[user_count]->pw_name   = strdup(pw->pw_name);
        users[user_count]->pw_passwd = strdup("x");
        users[user_count]->pw_dir    = strdup(pw->pw_dir);
        users[user_count]->pw_shell  = strdup(pw->pw_shell);
        users[user_count]->pw_uid    = pw->pw_uid;
        users[user_count]->pw_gid    = pw->pw_gid;

        user_count++;
    }
    endpwent();
}

static int users_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t off, struct fuse_file_info *fi,
                         enum fuse_readdir_flags flags) {
    (void)off; (void)fi; (void)flags;

    if (strcmp(path, "/") == 0) {
        filler(buf, ".", NULL, 0, 0);
        filler(buf, "..", NULL, 0, 0);
        for (int i = 0; i < user_count; i++)
            filler(buf, users[i]->pw_name, NULL, 0, 0);
        return 0;
    }

    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    filler(buf, "id", NULL, 0, 0);
    filler(buf, "home", NULL, 0, 0);
    filler(buf, "shell", NULL, 0, 0);
    return 0;
}

static int users_getattr(const char *path, struct stat *st,
                         struct fuse_file_info *fi) {
    (void)fi;
    memset(st, 0, sizeof(*st));
    st->st_uid = getuid();
    st->st_gid = getgid();
    st->st_atime = st->st_mtime = st->st_ctime = time(NULL);

    if (strcmp(path, "/") == 0) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }

    if (strchr(path + 1, '/') == NULL) {
        st->st_mode = S_IFDIR | 0755;
        st->st_nlink = 2;
        return 0;
    }

    st->st_mode = S_IFREG | 0444;
    st->st_nlink = 1;
    st->st_size = 64;
    return 0;
}

static int users_read(const char *path, char *buf, size_t size,
                      off_t off, struct fuse_file_info *fi) {
    (void)fi;

    char user[128], file[128];
    sscanf(path, "/%127[^/]/%127s", user, file);

    for (int i = 0; i < user_count; i++) {
        if (strcmp(users[i]->pw_name, user) == 0) {
            const char *out =
                strcmp(file, "id") == 0 ? "1000" :
                strcmp(file, "home") == 0 ? users[i]->pw_dir :
                strcmp(file, "shell") == 0 ? users[i]->pw_shell : "";

            size_t len = strlen(out);
            if (off >= len) return 0;
            if (size > len - off) size = len - off;
            memcpy(buf, out + off, size);
            return size;
        }
    }
    return -ENOENT;
}

static int users_mkdir(const char *path, mode_t mode) {
    (void)mode;

    char user[128];
    sscanf(path, "/%127s", user);

    FILE *f = fopen("/etc/passwd", "a");
    fprintf(f, "%s:x:1001:1001::/home/%s:/bin/sh\n", user, user);
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

    int pid = fork();
    if (pid == 0) {
        char *argv[] = { "users", "-f", (char*)mnt, NULL };
        fuse_main(3, argv, &ops, NULL);
        exit(0);
    }

    vfs_pid = pid;
    sleep(1);
    return 0;
}

void stop_users_vfs() {
    if (vfs_pid > 0) kill(vfs_pid, SIGTERM);
}
