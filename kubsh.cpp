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

int main(int argc, char* argv[]) {
    signal(SIGHUP, sighup_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    atexit(cleanup);

    bool auto_vfs = true;
    bool test_mode = false;
    // Анализ аргументов командной строки
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--no-vfs") == 0) auto_vfs = false;
        if (strcmp(argv[i], "--test") == 0) test_mode = true;
        if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: kubsh [OPTIONS]\n"
                      << "Options:\n"
                      << "  --no-vfs    Disable VFS auto-mount\n"
                      << "  --test      Test mode (CI safe, disables VFS)\n"
                      << "  --help, -h  Show this help\n";
            return 0;
        }
    }

    // В тестовом режиме GitHub Actions нужно отключить FUSE (он запрещён в CI)
    int vfs_startup_delay = test_mode ? 100 : 300;
    
    bool interactive = isatty(STDIN_FILENO);

    if (test_mode) {
        std::cout << "TEST MODE ENABLED\n";
        interactive = false;   // <── ВАЖНО
    }


    // Запуск VFS (только если не тест)
    if (auto_vfs) {
        mkdir("users", 0755);

        std::thread vfs_thread([]() {
            if (start_users_vfs("users") != 0) {
                std::cerr << "Warning: FAILED TO START VFS" << std::endl;
            }
        });

        vfs_thread.detach();

        std::this_thread::sleep_for(std::chrono::milliseconds(vfs_startup_delay));
    }

    // Загружаем историю команд
    std::string history_file = get_history_file();
    using_history();
    read_history(history_file.c_str());

    bool interactive = isatty(STDIN_FILENO);
    // Если не интерактивный режим и есть команда, выполняем и выходим
    if (!interactive && argc > 1) {
        bool has_real_command = false;

        // Проверяем, есть ли хоть одна НЕ-опция
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] != '-') {
                has_real_command = true;
                break;
            }
        }

        if (has_real_command) {
            // Режим пакетного выполнения (для тестов)
            for (int i = 1; i < argc; i++) {
                std::string command = argv[i];

                if (command[0] == '-') continue;
                if (command == "\\q") break;

                // echo
                if (command.rfind("echo ", 0) == 0) {
                    std::cout << command.substr(5) << std::endl;
                    continue;
                }

                // \e $PATH
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

                // Токенизация команды
                auto tokens = split(command);
                if (tokens.empty()) continue;

                // Проверяем команду в PATH
                std::string exe = find_executable(tokens[0]);
                if (exe.empty()) {
                    std::cout << tokens[0] << ": command not found" << std::endl;
                    continue;
                }

                // Формируем аргументы
                std::vector<char*> args;
                for (auto &s : tokens) args.push_back(const_cast<char*>(s.c_str()));
                args.push_back(nullptr);

                // Форк + exec
                pid_t pid = fork();
                if (pid == 0) {
                    execve(exe.c_str(), args.data(), environ);
                    perror("execve");
                    _exit(127);
                } else if (pid > 0) {
                    int status = 0;
                    waitpid(pid, &status, 0);
                }
            }

            return 0; // после batch режима обязательно выходим
        }
    }
    // Интерактивный режим
    while (running.load()) {

        if (reload_config) {
            std::cout << "Configuration reloaded" << std::endl;
            reload_config = 0;
        }

        char* line = nullptr;

        if (interactive) {
            // Readline prompt
            line = readline("$ ");
        } else {
            // Неинтерактивный stdin (редко, но оставляем)
            std::string input;
            if (!std::getline(std::cin, input)) break;
            line = strdup(input.c_str());
        }

        if (!line) break;

        std::string command = line;

        // Сохраняем в историю
        if (!command.empty()) {
            add_history(line);
            write_history(history_file.c_str());

            // дублируем в файл (GitHub Actions любит текстовую историю)
            std::ofstream h(history_file, std::ios::app);
            if (h.is_open()) h << command << std::endl;
        }

        free(line);

        if (command.empty()) continue;

        // -------------------------------
        // BUILT-IN команды
        // -------------------------------

        if (command == "\\q") {
            break;
        }

        // debug 'text'
        if (command.rfind("debug ", 0) == 0) {
            std::string arg = command.substr(6);

            if (arg.size() >= 2 && arg.front() == '\'' && arg.back() == '\'') {
                arg = arg.substr(1, arg.size() - 2);
            }

            std::cout << arg << std::endl;
            continue;
        }

        // echo
        if (command.rfind("echo ", 0) == 0) {
            std::cout << command.substr(5) << std::endl;
            continue;
        }

        // \e $PATH — вывести список путей PATH
        if (command.rfind("\\e $", 0) == 0) {
            std::string env_var = command.substr(4);

            if (env_var == "PATH") {
                print_env_list("PATH");
            } else {
                char* v = getenv(env_var.c_str());
                if (v) std::cout << v << std::endl;
            }
            continue;
        }

        // \l /dev/sda
        if (command.rfind("\\l ", 0) == 0) {
            list_partitions(command.substr(3));
            continue;
        }

        // cd
        auto tokens = split(command);

        if (!tokens.empty() && tokens[0] == "cd") {
            if (tokens.size() == 1) {
                const char* home = getenv("HOME");
                if (home) chdir(home);
                else {
                    struct passwd* pw = getpwuid(getuid());
                    if (pw) chdir(pw->pw_dir);
                }
            } else {
                if (chdir(tokens[1].c_str()) != 0) perror("cd");
            }
            continue;
        }

        // -------------------------------
        // Внешние программы (exec)
        // -------------------------------

        if (tokens.empty()) continue;

        std::string exe = find_executable(tokens[0]);

        if (exe.empty()) {
            std::cout << tokens[0] << ": command not found" << std::endl;
            continue;
        }

        // готовим аргументы
        std::vector<char*> args;
        for (auto &s : tokens) args.push_back(const_cast<char*>(s.c_str()));
        args.push_back(nullptr);

        pid_t pid = fork();

        if (pid == 0) {
            execve(exe.c_str(), args.data(), environ);
            perror("execve");
            _exit(127);
        }

        if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        } else {
            perror("fork");
        }
    }

    cleanup();
    return 0;
}
