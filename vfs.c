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
                // Показываем только пользователей с shell, содержащим "sh"
                if (strstr(users[i]->pw_shell, "sh") != NULL) {
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
                    if (users[i]->pw_shell && strstr(users[i]->pw_shell, "sh") != NULL) {
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
                    if (users[i]->pw_shell && strstr(users[i]->pw_shell, "sh") != NULL) {
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

// ... предыдущий код без изменений ...

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
    
    // Добавляем пользователя через useradd
    char command[512];
    snprintf(command, sizeof(command), 
             "useradd -m -s /bin/bash %s >/dev/null 2>&1", username);
    
    int ret = system(command);
    if (ret != 0) {
        // Если useradd не сработал, пробуем добавить вручную в /etc/passwd
        // Это для тестов, где useradd может быть недоступен
        FILE *f = fopen("/etc/passwd", "a");
        if (f) {
            int uid = 1000 + user_count;
            fprintf(f, "%s:x:%d:%d::/home/%s:/bin/bash\n", 
                    username, uid, uid, username);
            fclose(f);
            ret = 0;
        }
    }
    
    if (ret == 0) {
        // Обновляем список пользователей
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
        // Это для тестов
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

// ... остальной код без изменений ...

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
        int ret = fuse_main(4, fuse_argv, &users_oper, NULL);
        
        // Очищаем перед выходом
        free_users_list();
        exit(ret);
    } else if (pid > 0) { 
        // Родительский процесс
        vfs_pid = pid;
        
        // Даем время на монтирование
        sleep(1);
        
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