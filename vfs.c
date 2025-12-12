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
#include <limits.h>
#include <pwd.h>

static char vfs_root[512] = {0};
static pthread_mutex_t vfs_mutex = PTHREAD_MUTEX_INITIALIZER;

/* FUSE operations structure */
static struct fuse_operations vfs_oper = {
    .getattr = NULL,
    .readdir = NULL,
    .mkdir = NULL,
    .rmdir = NULL,
    .open = NULL,
    .read = NULL,
};

/* Helpers */
static int ensure_dir(const char *path) {
    if (mkdir(path, 0755) == -1 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

/* Check if user exists in /etc/passwd */
static int system_user_exists(const char *username) {
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

/* Return max uid found in /etc/passwd */
static int passwd_max_uid() {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 1000;
    
    char line[1024];
    int max_uid = 1000;
    
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        
        // Skip username
        p = strchr(p, ':');
        if (!p) continue;
        p++;
        
        // Skip password
        p = strchr(p, ':');
        if (!p) continue;
        p++;
        
        // Parse UID
        int uid = atoi(p);
        if (uid > max_uid) {
            max_uid = uid;
        }
    }
    
    fclose(f);
    return max_uid;
}

/* Append user to /etc/passwd */
static int add_user_to_passwd(const char *username) {
    if (!username || username[0] == '\0') return -1;
    
    pthread_mutex_lock(&vfs_mutex);
    
    // Check if user already exists
    if (system_user_exists(username)) {
        pthread_mutex_unlock(&vfs_mutex);
        return 0;
    }
    
    // Find next available UID
    int max_uid = passwd_max_uid();
    int new_uid = max_uid + 1;
    
    // Add to /etc/passwd
    FILE *f = fopen("/etc/passwd", "a");
    if (!f) {
        pthread_mutex_unlock(&vfs_mutex);
        return -1;
    }
    
    fprintf(f, "%s:x:%d:%d::/home/%s:/bin/bash\n", 
            username, new_uid, new_uid, username);
    fclose(f);
    
    pthread_mutex_unlock(&vfs_mutex);
    
    // Try to create home directory
    char home[512];
    snprintf(home, sizeof(home), "/home/%s", username);
    mkdir(home, 0755);
    
    return 0;
}

/* Get user info from /etc/passwd */
static int get_user_info(const char *username, int *uid, char *home, char *shell) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return -1;
    
    char line[1024];
    int found = 0;
    
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, username, strlen(username)) == 0 && 
            line[strlen(username)] == ':') {
            
            // Parse the line
            char *fields[7] = {0};
            char *p = line;
            
            for (int i = 0; i < 7; i++) {
                fields[i] = p;
                p = strchr(p, ':');
                if (!p) break;
                *p = '\0';
                p++;
            }
            
            if (fields[2]) *uid = atoi(fields[2]);
            if (fields[5] && home) strncpy(home, fields[5], 255);
            if (fields[6] && shell) {
                strncpy(shell, fields[6], 255);
                // Remove newline
                char *nl = strchr(shell, '\n');
                if (nl) *nl = '\0';
            }
            
            found = 1;
            break;
        }
    }
    
    fclose(f);
    return found ? 0 : -1;
}

/* Write file without trailing newline */
static int write_file_no_nl(const char *path, const char *content) {
    if (!path) return -1;
    
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    
    if (content) {
        ssize_t len = strlen(content);
        ssize_t wrote = 0;
        
        while (wrote < len) {
            ssize_t r = write(fd, content + wrote, len - wrote);
            if (r <= 0) {
                close(fd);
                return -1;
            }
            wrote += r;
        }
    }
    
    close(fd);
    return 0;
}

/* Create VFS directory and files for a user */
static void create_vfs_user_dir(const char *username) {
    char user_path[1024];
    snprintf(user_path, sizeof(user_path), "%s/%s", vfs_root, username);
    
    // Create user directory
    ensure_dir(user_path);
    
    // Get user info
    int uid = 0;
    char home[256] = "/";
    char shell[256] = "/bin/bash";
    
    if (get_user_info(username, &uid, home, shell) != 0) {
        // User doesn't exist in /etc/passwd yet, use defaults
        uid = 1000;
        snprintf(home, sizeof(home), "/home/%s", username);
    }
    
    // Create id file
    char id_str[32];
    snprintf(id_str, sizeof(id_str), "%d", uid);
    
    char id_path[1024];
    snprintf(id_path, sizeof(id_path), "%s/id", user_path);
    write_file_no_nl(id_path, id_str);
    
    // Create home file
    char home_path[1024];
    snprintf(home_path, sizeof(home_path), "%s/home", user_path);
    write_file_no_nl(home_path, home);
    
    // Create shell file
    char shell_path[1024];
    snprintf(shell_path, sizeof(shell_path), "%s/shell", user_path);
    write_file_no_nl(shell_path, shell);
}

/* Populate VFS from /etc/passwd - only users with shell ending in "sh" */
static void populate_vfs_from_passwd() {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return;
    
    char line[1024];
    
    while (fgets(line, sizeof(line), f)) {
        // Make a copy for safe tokenization
        char line_copy[1024];
        strncpy(line_copy, line, sizeof(line_copy)-1);
        line_copy[sizeof(line_copy)-1] = '\0';
        
        // Split into fields
        char *fields[7] = {0};
        char *token = strtok(line_copy, ":");
        int field_count = 0;
        
        while (token && field_count < 7) {
            fields[field_count++] = token;
            token = strtok(NULL, ":");
        }
        
        if (field_count < 7) continue;
        
        const char *username = fields[0];
        const char *shell = fields[6];
        
        // Check if shell ends with "sh" (allowing newline)
        int shell_len = strlen(shell);
        if (shell_len >= 2) {
            // Remove newline if present
            if (shell[shell_len-1] == '\n') {
                shell_len--;
            }
            
            // Check if ends with "sh"
            if (shell_len >= 2 && 
                shell[shell_len-2] == 's' && 
                shell[shell_len-1] == 'h') {
                // Also skip nologin/false shells
                if (strstr(shell, "nologin") || strstr(shell, "false")) {
                    continue;
                }
                
                // Create VFS entry
                create_vfs_user_dir(username);
            }
        }
    }
    
    fclose(f);
}

/* FUSE operation: getattr */
static int vfs_getattr(const char *path, struct stat *stbuf, struct fuse_file_info *fi) {
    (void)fi;
    
    char full_path[1024];
    snprintf(full_path, sizeof(fullpath), "%s%s", vfs_root, path);
    
    memset(stbuf, 0, sizeof(struct stat));
    
    // Root directory
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        return 0;
    }
    
    // Check if path exists
    if (lstat(full_path, stbuf) == -1) {
        return -errno;
    }
    
    return 0;
}

/* FUSE operation: readdir */
static int vfs_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                      off_t offset, struct fuse_file_info *fi,
                      enum fuse_readdir_flags flags) {
    (void)offset;
    (void)fi;
    (void)flags;
    
    char full_path[1024];
    snprintf(full_path, sizeof(fullpath), "%s%s", vfs_root, path);
    
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    
    DIR *dir = opendir(full_path);
    if (!dir) return -errno;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || 
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = entry->d_ino;
        st.st_mode = entry->d_type << 12;
        
        filler(buf, entry->d_name, &st, 0, 0);
    }
    
    closedir(dir);
    return 0;
}

/* FUSE operation: mkdir */
static int vfs_mkdir(const char *path, mode_t mode) {
    (void)mode;
    
    // Extract username from path (should be /username)
    char username[256];
    if (sscanf(path, "/%255[^/]", username) != 1) {
        return -EINVAL;
    }
    
    // Don't allow nested directories
    if (strchr(path + 1, '/') != NULL) {
        return -EPERM;
    }
    
    // Create the directory
    char full_path[1024];
    snprintf(full_path, sizeof(fullpath), "%s/%s", vfs_root, username);
    
    if (mkdir(full_path, 0755) == -1) {
        return -errno;
    }
    
    // Add user to /etc/passwd if not exists
    if (!system_user_exists(username)) {
        add_user_to_passwd(username);
    }
    
    // Create user files
    create_vfs_user_dir(username);
    
    return 0;
}

/* FUSE operation: open */
static int vfs_open(const char *path, struct fuse_file_info *fi) {
    char full_path[1024];
    snprintf(full_path, sizeof(fullpath), "%s%s", vfs_root, path);
    
    int fd = open(full_path, fi->flags);
    if (fd == -1) return -errno;
    
    close(fd);
    return 0;
}

/* FUSE operation: read */
static int vfs_read(const char *path, char *buf, size_t size, off_t offset,
                   struct fuse_file_info *fi) {
    (void)fi;
    
    char full_path[1024];
    snprintf(full_path, sizeof(fullpath), "%s%s", vfs_root, path);
    
    int fd = open(full_path, O_RDONLY);
    if (fd == -1) return -errno;
    
    int res = pread(fd, buf, size, offset);
    if (res == -1) res = -errno;
    
    close(fd);
    return res;
}

/* Initialize FUSE operations */
static void init_fuse_operations() {
    vfs_oper.getattr = vfs_getattr;
    vfs_oper.readdir = vfs_readdir;
    vfs_oper.mkdir = vfs_mkdir;
    vfs_oper.open = vfs_open;
    vfs_oper.read = vfs_read;
}

/* Start VFS */
int start_users_vfs(const char *mount_point) {
    if (!mount_point) return -1;
    
    // Store mount point
    strncpy(vfs_root, mount_point, sizeof(vfs_root)-1);
    
    // Create root directory
    ensure_dir(vfs_root);
    
    // Populate from /etc/passwd
    populate_vfs_from_passwd();
    
    // Check if we're in CI mode
    if (getenv("CI")) {
        printf("CI mode: running VFS without FUSE mount\n");
        return 0;
    }
    
    // Initialize FUSE operations
    init_fuse_operations();
    
    // Mount FUSE in background
    pid_t pid = fork();
    if (pid == 0) {
        // Child process: mount FUSE
        char *fuse_argv[] = {
            "users_vfs",
            "-f",               // foreground
            "-s",               // single threaded
            (char*)mount_point,
            NULL
        };
        
        exit(fuse_main(4, fuse_argv, &vfs_oper, NULL));
    } else if (pid > 0) {
        // Parent process: wait a bit for mount
        sleep(1);
        return 0;
    }
    
    return -1;
}

/* Stop VFS */
void stop_users_vfs() {
    // In CI mode, nothing to unmount
    if (!getenv("CI")) {
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "fusermount -u %s 2>/dev/null", vfs_root);
        system(cmd);
    }
}

/* Add user to VFS */
int vfs_add_user(const char *username) {
    if (!username) return -1;
    
    // Add to /etc/passwd if not exists
    if (!system_user_exists(username)) {
        add_user_to_passwd(username);
    }
    
    // Create VFS directory and files
    create_vfs_user_dir(username);
    
    return 0;
}

/* Check if user exists in VFS */
int vfs_user_exists(const char *username) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", vfs_root, username);
    
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}