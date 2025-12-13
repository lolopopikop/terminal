#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>
#include <signal.h>
#include <sys/stat.h>
#include <pwd.h>
#include <dirent.h>
#include <algorithm>
#include <readline/readline.h>
#include <readline/history.h>
#include <sys/types.h>
#include <sys/inotify.h>
#include <atomic>
#include <thread>
#include <filesystem>

extern char **environ;

extern "C" {
#include "vfs.h"
}

volatile sig_atomic_t reload_config = 0;
std::atomic<bool> running(true);

void sighup_handler(int /*sig*/) {
    const char msg[] = "Configuration reloaded\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    reload_config = 1;
}

std::vector<std::string> split(const std::string& str) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (ss >> token) tokens.push_back(token);
    return tokens;
}

std::vector<std::string> split_by_delimiter(const std::string& str, char delimiter) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    while (std::getline(ss, token, delimiter)) {
        if (!token.empty()) tokens.push_back(token);
    }
    return tokens;
}

void print_env_list(const std::string& env_var) {
    char* val = getenv(env_var.c_str());
    if (!val) return;
    std::string s = val;
    auto parts = split_by_delimiter(s, ':');
    for (auto &p : parts) std::cout << p << std::endl;
}

void list_partitions(const std::string& device) {
    std::string cmd = "lsblk " + device;
    system(cmd.c_str());
}

std::string get_history_file() {
    char* home = getenv("HOME");
    if (home) return std::string(home) + "/.kubsh_history";
    
    struct passwd* pw = getpwuid(getuid());
    if (pw) return std::string(pw->pw_dir) + "/.kubsh_history";
    
    return "/root/.kubsh_history";
}

std::string find_executable(const std::string& command) {
    char* path_env = getenv("PATH");
    if (!path_env) return "";
    
    std::string path_str = path_env;
    std::stringstream ss(path_str);
    std::string dir;
    
    while (std::getline(ss, dir, ':')) {
        std::string full_path = dir + "/" + command;
        if (access(full_path.c_str(), X_OK) == 0) {
            return full_path;
        }
    }
    
    return "";
}

void cleanup() {
    running.store(false);
    stop_users_vfs();
}

int main() {
    // Устанавливаем обработчики сигналов
    signal(SIGHUP, sighup_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    atexit(cleanup);
    
    // Запускаем VFS в каталоге users (только если не в тестовом режиме)
    // Тесты сами создают каталог users, поэтому не монтируем если он уже существует
    struct stat st;
    if (stat("users", &st) == -1) {
        if (start_users_vfs("users") != 0) {
            std::cerr << "Failed to start VFS" << std::endl;
        }
    }
    
    // Загружаем историю команд
    std::string history_file = get_history_file();
    using_history();
    read_history(history_file.c_str());
    
    bool interactive = isatty(STDIN_FILENO);
    
    while (running.load()) {
        if (reload_config) {
            std::cout << "Configuration reloaded" << std::endl;
            reload_config = 0;
        }
        
        char* line = nullptr;
        if (interactive) {
            line = readline("$ ");
        } else {
            // Неинтерактивный режим для тестов
            std::string input;
            if (!std::getline(std::cin, input)) {
                // EOF - выходим
                break;
            }
            line = strdup(input.c_str());
        }
        
        if (!line) {
            // EOF в интерактивном режиме или ошибка
            break;
        }
        
        std::string command = line;
        
        // Добавляем в историю если команда не пустая
        if (strlen(line) > 0) {
            add_history(line);
            write_history(history_file.c_str());
            
            // Также записываем в файл
            std::ofstream h(history_file, std::ios::app);
            if (h.is_open()) {
                h << command << std::endl;
            }
        }
        
        free(line);
        
        if (command.empty()) continue;
        
        // Обработка специальных команд
        if (command == "\\q") {
            break;
        }
        
        if (command.rfind("debug ", 0) == 0) {
            std::string arg = command.substr(6);
            if (arg.size() >= 2 && arg.front() == '\'' && arg.back() == '\'') {
                arg = arg.substr(1, arg.size() - 2);
            }
            std::cout << arg << std::endl;
            continue;
        }
        
        if (command.rfind("echo ", 0) == 0) {
            std::cout << command.substr(5) << std::endl;
            continue;
        }
        
        if (command.rfind("\\e $", 0) == 0) {
            std::string env_var = command.substr(4);
            if (env_var == "PATH") {
                print_env_list(env_var);
            } else {
                char* v = getenv(env_var.c_str());
                if (v) std::cout << v << std::endl;
            }
            continue;
        }
        
        if (command.rfind("\\l ", 0) == 0) {
            std::string dev = command.substr(3);
            list_partitions(dev);
            continue;
        }
        
        // Выполнение внешних команд
        auto tokens = split(command);
        if (tokens.empty()) continue;
        
        // Встроенная команда cd
        if (tokens[0] == "cd") {
            if (tokens.size() > 1) {
                if (chdir(tokens[1].c_str()) != 0) {
                    perror("cd");
                }
            } else {
                // Без аргументов - переходим в домашний каталог
                const char* home = getenv("HOME");
                if (home) {
                    chdir(home);
                } else {
                    struct passwd* pw = getpwuid(getuid());
                    if (pw) {
                        chdir(pw->pw_dir);
                    }
                }
            }
            continue;
        }
        
        // Пытаемся найти исполняемый файл
        std::string exe = find_executable(tokens[0]);
        if (exe.empty()) {
            std::cout << tokens[0] << ": command not found" << std::endl;
            continue;
        }
        
        // Подготавливаем аргументы для execve
        std::vector<char*> args;
        for (auto &s : tokens) {
            args.push_back(const_cast<char*>(s.c_str()));
        }
        args.push_back(nullptr);
        
        // Запускаем процесс
        pid_t pid = fork();
        if (pid == 0) {
            // Дочерний процесс
            execve(exe.c_str(), args.data(), environ);
            perror("execve");
            _exit(127);
        } else if (pid > 0) {
            // Родительский процесс ждет завершения
            int status = 0;
            waitpid(pid, &status, 0);
        } else {
            perror("fork");
        }
    }
    
    cleanup();
    return 0;
}