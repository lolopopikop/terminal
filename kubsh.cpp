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
#include <algorithm>
#include <readline/readline.h>
#include <readline/history.h>
#include <atomic>

extern char **environ;

extern "C" {
#include "vfs.h"
}

std::atomic<bool> running(true);

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
    char* path = getenv("PATH");
    if (!path) return "";

    std::stringstream ss(path);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        std::string full = dir + "/" + cmd;
        if (access(full.c_str(), X_OK) == 0)
            return full;
    }
    return "";
}

int main() {
    atexit(cleanup);

    // 🔴 ВАЖНО: VFS ТОЛЬКО ТУТ
    start_users_vfs("/opt/users");

    using_history();

    bool interactive = isatty(STDIN_FILENO);

    while (running.load()) {
        char* line = nullptr;

        if (interactive) {
            line = readline("$ ");
        } else {
            std::string s;
            if (!std::getline(std::cin, s))
                break;
            line = strdup(s.c_str());
        }

        if (!line) break;

        std::string cmd(line);
        free(line);

        if (cmd.empty()) continue;

        auto args = split(cmd);
        if (args.empty()) continue;

        if (args[0] == "exit")
            break;

        std::string exe = find_executable(args[0]);
        if (exe.empty()) {
            std::cout << args[0] << ": command not found\n";
            continue;
        }

        std::vector<char*> argv;
        for (auto& a : args)
            argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid == 0) {
            execve(exe.c_str(), argv.data(), environ);
            _exit(127);
        } else {
            int st;
            waitpid(pid, &st, 0);
        }
    }

    cleanup();
    return 0;
}
