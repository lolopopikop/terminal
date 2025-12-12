// kubsh.cpp
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
#include <atomic>
#include <thread>
#include <filesystem>

extern char **environ;

extern "C" {
#include "vfs.h"
}

volatile sig_atomic_t reload_config = 0;
std::atomic<bool> running(true);

void sighup_handler(int) {
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
    auto parts = split_by_delimiter(val, ':');
    for (auto &p : parts) std::cout << p << std::endl;
}

std::string get_history_file() {
    char* home = getenv("HOME");
    if (home) return std::string(home) + "/.kubsh_history";

    struct passwd* pw = getpwuid(getuid());
    if (pw) return std::string(pw->pw_dir) + "/.kubsh_history";

    return "/root/.kubsh_history";
}

std::string find_executable(const std::string& c) {
    char* path_env = getenv("PATH");
    if (!path_env) return "";

    std::stringstream ss(path_env);
    std::string dir;

    while (std::getline(ss, dir, ':')) {
        std::string full = dir + "/" + c;
        if (access(full.c_str(), X_OK) == 0) return full;
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

    if (getenv("CI")) {
        fprintf(stderr, "CI ENV detected — FUSE disabled, VFS active\n");
        auto_vfs = true;
    }

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-vfs") == 0) auto_vfs = false;
        else if (strcmp(argv[i], "--test") == 0) test_mode = true;
        else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
            std::cout << "Usage: kubsh [OPTIONS]\n"
                      << "  --no-vfs    Disable VFS\n"
                      << "  --test      Test mode\n";
            return 0;
        }
    }

    if (test_mode) auto_vfs = false;

    if (auto_vfs) {
        const char* vfs_dir = getenv("KUBSH_VFS_DIR");
        if (!vfs_dir) vfs_dir = "users";

        mkdir(vfs_dir, 0755);

        std::string mount_point = vfs_dir;

        std::thread vfs_thread([mount_point]() {
            start_users_vfs(mount_point.c_str());
        });
        vfs_thread.detach();

        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }

    std::string history_file = get_history_file();
    using_history();
    read_history(history_file.c_str());

    bool interactive = isatty(STDIN_FILENO);
    if (test_mode) interactive = false;

    while (running.load()) {
        if (reload_config) {
            std::cout << "Configuration reloaded" << std::endl;
            reload_config = 0;
        }

        char* line = nullptr;

        if (interactive)
            line = readline("$ ");
        else {
            std::string input;
            if (!std::getline(std::cin, input)) break;
            line = strdup(input.c_str());
        }

        if (!line) break;
        std::string command = line;
        free(line);

        if (command.empty()) continue;

        if (command == "\\q") break;

        if (command.rfind("echo ", 0) == 0) {
            std::cout << command.substr(5) << std::endl;
            continue;
        }

        if (command.rfind("\\e $", 0) == 0) {
            std::string e = command.substr(4);
            char* v = getenv(e.c_str());
            if (v) std::cout << v << std::endl;
            continue;
        }

        // external commands
        auto tokens = split(command);
        if (tokens.empty()) continue;

        std::string exe = find_executable(tokens[0]);
        if (exe.empty()) {
            std::cout << tokens[0] << ": command not found" << std::endl;
            continue;
        }

        std::vector<char*> args;
        for (auto &s : tokens) args.push_back((char*)s.c_str());
        args.push_back(nullptr);

        pid_t pid = fork();
        if (pid == 0) {
            execve(exe.c_str(), args.data(), environ);
            perror("execve");
            _exit(127);
        } else if (pid > 0) {
            int s = 0;
            waitpid(pid, &s, 0);
        } else perror("fork");
    }

    return 0;
}
