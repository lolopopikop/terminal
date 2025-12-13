#include <iostream>
#include <string>
#include <vector>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>
#include <signal.h>
#include <sys/stat.h>
#include <pwd.h>
#include <readline/readline.h>
#include <readline/history.h>
#include <atomic>

extern char **environ;

extern "C" {
#include "vfs.h"
}

std::atomic<bool> running(true);
volatile sig_atomic_t reload_config = 0;

void sighup_handler(int) {
    std::cout << "Configuration reloaded" << std::endl;
    reload_config = 1;
}

std::vector<std::string> split(const std::string &s) {
    std::stringstream ss(s);
    std::vector<std::string> out;
    std::string t;
    while (ss >> t) out.push_back(t);
    return out;
}

std::string find_executable(const std::string &cmd) {
    char *path = getenv("PATH");
    if (!path) return "";
    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        std::string full = dir + "/" + cmd;
        if (access(full.c_str(), X_OK) == 0) return full;
    }
    return "";
}

void cleanup() {
    running = false;
    stop_users_vfs();
}

int main() {
    signal(SIGHUP, sighup_handler);
    atexit(cleanup);

    // ВСЕГДА монтируем VFS
    start_users_vfs("users");

    using_history();
    read_history("~/.kubsh_history");

    bool interactive = isatty(STDIN_FILENO);

    while (running) {
        std::string command;
        if (interactive) {
            char *line = readline("$ ");
            if (!line) break;
            command = line;
            free(line);
        } else {
            if (!std::getline(std::cin, command)) break;
        }

        if (command.empty()) continue;

        add_history(command.c_str());
        write_history("~/.kubsh_history");

        auto tokens = split(command);
        if (tokens.empty()) continue;

        // \q
        if (tokens[0] == "\\q") break;

        // debug
        if (tokens[0] == "debug") {
            if (tokens.size() > 1) {
                std::string out = command.substr(6);
                if (out.front() == '\'' && out.back() == '\'')
                    out = out.substr(1, out.size() - 2);
                std::cout << out << std::endl;
            }
            continue;
        }

        // echo
        if (tokens[0] == "echo") {
            std::cout << command.substr(5) << std::endl;
            continue;
        }

        // \e
        if (tokens[0] == "\\e" && tokens.size() == 2) {
            char *v = getenv(tokens[1].substr(1).c_str());
            if (v) {
                std::stringstream ss(v);
                std::string p;
                while (std::getline(ss, p, ':'))
                    std::cout << p << std::endl;
            }
            continue;
        }

        // cd
        if (tokens[0] == "cd") {
            chdir(tokens.size() > 1 ? tokens[1].c_str() : getenv("HOME"));
            continue;
        }

        std::string exe = find_executable(tokens[0]);
        if (exe.empty()) {
            std::cout << tokens[0] << ": command not found" << std::endl;
            continue;
        }

        std::vector<char *> args;
        for (auto &s : tokens) args.push_back((char *)s.c_str());
        args.push_back(nullptr);

        if (fork() == 0) {
            execve(exe.c_str(), args.data(), environ);
            _exit(127);
        }
        wait(nullptr);
    }

    return 0;
}
