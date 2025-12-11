#define FUSE_USE_VERSION 31
#include <fuse3/fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/types.h>
#include <pwd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#include <limits.h>
#include <time.h>

static int vfs_pid = -1;
static struct passwd **users = NULL;
static int user_count = 0;

int get_users_list() {
    // Освобождаем старый список если есть
    if (users != NULL) {
        for (int i = 0; i < user_count; i++) {
            if (users[i]) {
                free((void*)users[i]->pw_name);
                free((void*)users[i]->pw_passwd);
                free((void*)users[i]->pw_gecos);
                free((void*)users[i]->pw_dir);
                free((void*)users[i]->pw_shell);
                free(users[i]);
            }
        }
        free(users);
        users = NULL;
        user_count = 0;
    }
    
    // Получаем список всех пользователей
    struct passwd *pwd;
    int count = 0;
    
    // Подсчитываем количество пользователей
    setpwent();
    while ((pwd = getpwent()) != NULL) {
        count++;
    }
    endpwent();
    
    if (count <= 0) {
        return 0;
    }
    
    // Выделяем память
    users = malloc(sizeof(struct passwd *) * count);
    if (!users) {
        return -1;
    }
    
    // Заполняем массив
    setpwent();
    int i = 0;
    while ((pwd = getpwent()) != NULL && i < count) {
        users[i] = malloc(sizeof(struct passwd));
        if (!users[i]) {
            endpwent();
            return -1;
        }
        
        // Копируем структуру
        memcpy(users[i], pwd, sizeof(struct passwd));
        
        // Копируем строки отдельно
        users[i]->pw_name = strdup(pwd->pw_name);
        users[i]->pw_passwd = strdup(pwd->pw_passwd ? pwd->pw_passwd : "x");
        users[i]->pw_gecos = strdup(pwd->pw_gecos ? pwd->pw_gecos : "");
        users[i]->pw_dir = strdup(pwd->pw_dir);
        users[i]->pw_shell = strdup(pwd->pw_shell);
        
        i++;
    }
    endpwent();
    
    user_count = i;
    return user_count;
}

void free_users_list() {
    if (users != NULL) {
        for (int i = 0; i < user_count; i++) {
            if (users[i]) {
                free((void*)users[i]->pw_name);
                free((void*)users[i]->pw_passwd);
                free((void*)users[i]->pw_gecos);
                free((void*)users[i]->pw_dir);
                free((void*)users[i]->pw_shell);
                free(users[i]);
            }
        }
        free(users);
        users = NULL;
        user_count = 0;
    }
}

// Функция для проверки валидности shell
static int is_valid_shell(const char *shell) {
    if (!shell) return 0;
    
    // Shell должен заканчиваться на "sh" (bash, sh, dash, zsh и т.д.)
    int len = strlen(shell);
    if (len < 2) return 0;
    
    // Должно заканчиваться на "sh"
    if (shell[len-2] != 's' || shell[len-1] != 'h') {
        return 0;
    }
    
    // Исключаем подстроки которые не являются настоящими shell
    const char *invalid_patterns[] = {
        "git-shell",  // git-shell заканчивается на shell, не sh
        "nologin",
        "false",
        "sync",
        "halt",
        NULL
    };
    
    for (int i = 0; invalid_patterns[i] != NULL; i++) {
        if (strstr(shell, invalid_patterns[i]) != NULL) {
            return 0;
        }
    }
    
    return 1;
}

static int users_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                         off_t offset, struct fuse_file_info *fi,
                         enum fuse_readdir_flags flags) {
    (void) offset;
    (void) fi;
    (void) flags;
    
    // Корневой каталог
    if (strcmp(path, "/") == 0) {
        filler(buf, ".", NULL, 0, 0);
        filler(buf, "..", NULL, 0, 0);
        
        for (int i = 0; i < user_count; i++) {
            if (users[i] && users[i]->pw_name && users[i]->pw_shell) {
                if (is_valid_shell(users[i]->pw_shell)) {
                    filler(buf, users[i]->pw_name, NULL, 0, 0);
                }
            }
        }
        return 0;
    }
    
    // Каталог пользователя
    char username[NAME_MAX];
    if (sscanf(path, "/%255[^/]", username) == 1) {
        for (int i = 0; i < user_count; i++) {
            if (users[i] && users[i]->pw_name && strcmp(users[i]->pw_name, username) == 0) {
                filler(buf, ".", NULL, 0, 0);
                filler(buf, "..", NULL, 0, 0);
                filler(buf, "id", NULL, 0, 0);
                filler(buf, "home", NULL, 0, 0);
                filler(buf, "shell", NULL, 0, 0);
                return 0;
            }
        }
    }
    
    return -ENOENT;
}

static int users_open(const char *path, struct fuse_file_info *fi) {
    (void) path;
    (void) fi;
    return 0;
}

static int users_read(const char *path, char *buf, size_t size, off_t offset,
                      struct fuse_file_info *fi) {
    (void) fi;
    
    char username[NAME_MAX];
    char filename[NAME_MAX];
    
    if (sscanf(path, "/%255[^/]/%255s", username, filename) != 2) {
        return -ENOENT;
    }
    
    // Ищем пользователя
    struct passwd *pwd = NULL;
    for (int i = 0; i < user_count; i++) {
        if (users[i] && users[i]->pw_name && strcmp(users[i]->pw_name, username) == 0) {
            pwd = users[i];
            break;
        }
    }
    
    if (!pwd) {
        return -ENOENT;
    }
    
    const char *content = NULL;
    char id_buf[32];
    
    if (strcmp(filename, "id") == 0) {
        snprintf(id_buf, sizeof(id_buf), "%d", pwd->pw_uid);
        content = id_buf;
    } else if (strcmp(filename, "home") == 0) {
        content = pwd->pw_dir;
    } else if (strcmp(filename, "shell") == 0) {
        content = pwd->pw_shell;
    } else {
        return -ENOENT;
    }
    
    if (!content) {
        content = "";
    }
    
    size_t len = strlen(content);
    if ((size_t)offset >= len) {
        return 0;
    }
    
    size_t to_copy = len - offset;
    if (to_copy > size) {
        to_copy = size;
    }
    
    memcpy(buf, content + offset, to_copy);
    return to_copy;
}

static int users_getattr(const char *path, struct stat *stbuf,
                         struct fuse_file_info *fi) {
    (void) fi;
    
    memset(stbuf, 0, sizeof(struct stat));
    stbuf->st_uid = getuid();
    stbuf->st_gid = getgid();
    time_t now = time(NULL);
    stbuf->st_atime = stbuf->st_mtime = stbuf->st_ctime = now;
    
    // Корневой каталог
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }
    
    char username[NAME_MAX];
    char filename[NAME_MAX];
    
    // Каталог пользователя
    if (sscanf(path, "/%255[^/]", username) == 1) {
        if (strchr(path + 1, '/') == NULL) {
            for (int i = 0; i < user_count; i++) {
                if (users[i] && users[i]->pw_name && 
                    strcmp(users[i]->pw_name, username) == 0) {
                    if (is_valid_shell(users[i]->pw_shell)) {
                        stbuf->st_mode = S_IFDIR | 0755;
                        stbuf->st_nlink = 2;
                        return 0;
                    }
                }
            }
        } else if (sscanf(path, "/%255[^/]/%255s", username, filename) == 2) {
            // Файл в каталоге пользователя
            for (int i = 0; i < user_count; i++) {
                if (users[i] && users[i]->pw_name && 
                    strcmp(users[i]->pw_name, username) == 0) {
                    if (is_valid_shell(users[i]->pw_shell)) {
                        if (strcmp(filename, "id") == 0 ||
                            strcmp(filename, "home") == 0 ||
                            strcmp(filename, "shell") == 0) {
                            stbuf->st_mode = S_IFREG | 0444;
                            stbuf->st_nlink = 1;
                            
                            if (strcmp(filename, "id") == 0) {
                                char id_buf[32];
                                snprintf(id_buf, sizeof(id_buf), "%d", users[i]->pw_uid);
                                stbuf->st_size = strlen(id_buf);
                            } else if (strcmp(filename, "home") == 0) {
                                stbuf->st_size = strlen(users[i]->pw_dir);
                            } else if (strcmp(filename, "shell") == 0) {
                                stbuf->st_size = strlen(users[i]->pw_shell);
                            }
                            return 0;
                        }
                    }
                }
            }
        }
    }
    
    return -ENOENT;
}

static int users_mkdir(const char *path, mode_t mode) {
    (void) mode;
    
    char username[NAME_MAX];
    if (sscanf(path, "/%255[^/]", username) != 1) {
        return -EINVAL;
    }
    
    // Проверяем, что такого пользователя еще нет
    for (int i = 0; i < user_count; i++) {
        if (users[i] && users[i]->pw_name && strcmp(users[i]->pw_name, username) == 0) {
            return -EEXIST;
        }
    }
    
    printf("VFS: Creating user '%s' via mkdir()\n", username);
    
    // Добавляем пользователя через useradd
    char command[512];
    snprintf(command, sizeof(command), 
             "useradd -m -s /bin/bash %s >/dev/null 2>&1", username);
    
    printf("VFS: Executing: %s\n", command);
    int ret = system(command);
    
    if (ret != 0) {
        printf("VFS: useradd failed, trying manual add to /etc/passwd\n");
        // Если useradd не сработал, пробуем добавить вручную в /etc/passwd
        // (для тестов в CI/CD где нет useradd)
        FILE *f = fopen("/etc/passwd", "a");
        if (f) {
            // Находим максимальный UID
            int max_uid = 1000;
            FILE *passwd_file = fopen("/etc/passwd", "r");
            if (passwd_file) {
                char line[256];
                while (fgets(line, sizeof(line), passwd_file)) {
                    char *colon = strchr(line, ':');
                    if (colon) {
                        *colon = '\0';
                        if (strcmp(line, username) == 0) {
                            fclose(passwd_file);
                            fclose(f);
                            printf("VFS: User %s already exists\n", username);
                            return -EEXIST;
                        }
                        *colon = ':';
                    }
                    
                    // Парсим UID (третье поле)
                    char *token = strtok(line, ":");
                    for (int j = 0; j < 2 && token; j++) {
                        token = strtok(NULL, ":");
                    }
                    if (token) {
                        int uid = atoi(token);
                        if (uid > max_uid) max_uid = uid;
                    }
                }
                fclose(passwd_file);
            }
            
            int new_uid = max_uid + 1;
            printf("VFS: Adding user %s with UID %d to /etc/passwd\n", username, new_uid);
            fprintf(f, "%s:x:%d:%d::/home/%s:/bin/bash\n", 
                    username, new_uid, new_uid, username);
            fclose(f);
            ret = 0;
            
            // Также создаем домашний каталог
            char home_dir[256];
            snprintf(home_dir, sizeof(home_dir), "/home/%s", username);
            mkdir(home_dir, 0755);
            
            // Создаем файлы в VFS
            char vfs_dir[256];
            snprintf(vfs_dir, sizeof(vfs_dir), "users/%s", username);
            mkdir(vfs_dir, 0755);
            
            char id_file[256];
            snprintf(id_file, sizeof(id_file), "%s/id", vfs_dir);
            FILE *id_f = fopen(id_file, "w");
            if (id_f) {
                fprintf(id_f, "%d", new_uid);
                fclose(id_f);
            }
            
            char home_file[256];
            snprintf(home_file, sizeof(home_file), "%s/home", vfs_dir);
            FILE *home_f = fopen(home_file, "w");
            if (home_f) {
                fprintf(home_f, "/home/%s", username);
                fclose(home_f);
            }
            
            char shell_file[256];
            snprintf(shell_file, sizeof(shell_file), "%s/shell", vfs_dir);
            FILE *shell_f = fopen(shell_file, "w");
            if (shell_f) {
                fprintf(shell_f, "/bin/bash");
                fclose(shell_f);
            }
        } else {
            printf("VFS: Failed to open /etc/passwd\n");
        }
    } else {
        printf("VFS: useradd succeeded\n");
    }
    
    if (ret == 0) {
        // Обновляем список пользователей
        printf("VFS: Reloading user list\n");
        get_users_list();
    }
    
    return ret == 0 ? 0 : -EIO;
}

static int users_rmdir(const char *path) {
    char username[NAME_MAX];
    if (sscanf(path, "/%255[^/]", username) != 1) {
        return -EINVAL;
    }
    
    // Проверяем, что пользователь существует
    int found = 0;
    for (int i = 0; i < user_count; i++) {
        if (users[i] && users[i]->pw_name && strcmp(users[i]->pw_name, username) == 0) {
            found = 1;
            break;
        }
    }
    
    if (!found) {
        return -ENOENT;
    }
    
    // Удаляем пользователя через userdel
    char command[512];
    snprintf(command, sizeof(command), 
             "userdel -r %s >/dev/null 2>&1", username);
    
    int ret = system(command);
    if (ret != 0) {
        // Если userdel не сработал, удаляем из /etc/passwd вручную
        FILE *fin = fopen("/etc/passwd", "r");
        if (fin) {
            FILE *fout = fopen("/etc/passwd.tmp", "w");
            if (fout) {
                char line[256];
                while (fgets(line, sizeof(line), fin)) {
                    if (strstr(line, username) == NULL) {
                        fputs(line, fout);
                    }
                }
                fclose(fout);
                fclose(fin);
                rename("/etc/passwd.tmp", "/etc/passwd");
                ret = 0;
            } else {
                fclose(fin);
            }
        }
    }
    
    if (ret == 0) {
        // Обновляем список пользователей
        get_users_list();
    }
    
    return ret == 0 ? 0 : -EIO;
}

static struct fuse_operations users_oper = {
    .getattr = users_getattr,
    .open = users_open,
    .read = users_read,
    .readdir = users_readdir,
    .mkdir = users_mkdir,
    .rmdir = users_rmdir,
};

int start_users_vfs(const char *mount_point) {
    // Создаем точку монтирования если не существует
    mkdir(mount_point, 0755);
    
    int pid = fork();    
    if (pid == 0) {
        // Дочерний процесс
        char *fuse_argv[] = {
            "users_vfs",        // имя программы
            "-f",               // foreground mode
            "-s",               // single-threaded
            (char*)mount_point, // точка монтирования
            NULL
        };
        
        // Получаем список пользователей
        if (get_users_list() <= 0) {
            fprintf(stderr, "Не удалось получить список пользователей\n");
            exit(1);
        }
        
        // Запускаем FUSE
        printf("VFS: Starting FUSE at %s\n", mount_point);
        int ret = fuse_main(4, fuse_argv, &users_oper, NULL);
        
        // Очищаем перед выходом
        free_users_list();
        exit(ret);
    } else if (pid > 0) { 
        // Родительский процесс
        vfs_pid = pid;
        
        // Даем время на монтирование
        printf("VFS: Waiting for mount...\n");
        sleep(2);
        
        return 0;
    } else {
        perror("fork");
        return -1;
    }
}

void stop_users_vfs() {
    if (vfs_pid != -1) {
        kill(vfs_pid, SIGTERM);
        waitpid(vfs_pid, NULL, 0);
        vfs_pid = -1;
    }
}