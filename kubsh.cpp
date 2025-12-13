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
#include <atomic>
#include <fstream>
#include <readline/readline.h>
#include <readline/history.h>

extern char **environ;

extern "C" {
#include "vfs.h"
}

static std::atomic<bool> running(true);

void cleanup() {
    running.store(false);
    stop_users_vfs();
}

std::vector<std::string> split(const std::string& s) {
    std::stringstream ss(s);
    std::string tok;
    std::vector<std::string> out;
    while (ss >> tok) out.push_back(tok);
    return out;
}

std::string find_executable(const std::string& cmd) {
    char *path = getenv("PATH");
    if (!path) return "";

    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        std::string p = dir + "/" + cmd;
        if (access(p.c_str(), X_OK) == 0)
            return p;
    }
    return "";
}

int main() {
    atexit(cleanup);

    struct stat st;
    if (stat("/opt/users", &st) == -1) {
        start_users_vfs("/opt/users");
    }

    bool interactive = isatty(STDIN_FILENO);

    while (running.load()) {
        char *line = nullptr;

        if (interactive) {
            line = readline("$ ");
        } else {
            std::string input;
            if (!std::getline(std::cin, input)) break;
            line = strdup(input.c_str());
        }

        if (!line) break;

        std::string cmd(line);
        free(line);

        if (cmd.empty()) continue;
        if (cmd == "\\q") break;

        if (cmd.rfind("echo ", 0) == 0) {
            std::cout << cmd.substr(5) << std::endl;
            continue;
        }

        auto tokens = split(cmd);
        if (tokens.empty()) continue;

        if (tokens[0] == "cd") {
            if (tokens.size() > 1) chdir(tokens[1].c_str());
            continue;
        }

        std::string exe = find_executable(tokens[0]);
        if (exe.empty()) {
            std::cout << tokens[0] << ": command not found" << std::endl;
            continue;
        }

        std::vector<char*> args;
        for (auto &s : tokens)
            args.push_back(const_cast<char*>(s.c_str()));
        args.push_back(nullptr);

        pid_t pid = fork();
        if (pid == 0) {
            execve(exe.c_str(), args.data(), environ);
            _exit(127);
        } else {
            waitpid(pid, nullptr, 0);
        }
    }

    return 0;
}
