#include <iostream>
#include <string>
#include <fstream>
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

extern char **environ;

extern "C" {
#include "vfs.h"
}

volatile sig_atomic_t reload_config = 0;

void sighup_handler(int) {
    std::cout << "Configuration reloaded" << std::endl;
}

std::vector<std::string> split(const std::string& s) {
    std::stringstream ss(s);
    std::string tok;
    std::vector<std::string> out;
    while (ss >> tok) out.push_back(tok);
    return out;
}

void print_env_list(const std::string& var) {
    char* v = getenv(var.c_str());
    if (!v) return;

    std::stringstream ss(v);
    std::string item;
    while (std::getline(ss, item, ':')) {
        std::cout << item << std::endl;
    }
}

std::string find_exec(const std::string& cmd) {
    char* p = getenv("PATH");
    if (!p) return "";

    std::stringstream ss(p);
    std::string dir;
    while (std::getline(ss, dir, ':')) {
        std::string full = dir + "/" + cmd;
        if (access(full.c_str(), X_OK) == 0)
            return full;
    }
    return "";
}

int main() {
    signal(SIGHUP, sighup_handler);

    struct stat st;
    if (stat("users", &st) == -1) {
        start_users_vfs("users");
    }

    bool interactive = isatty(STDIN_FILENO);

    while (true) {
        std::string cmd;
        if (interactive) {
            char* line = readline("$ ");
            if (!line) break;
            cmd = line;
            if (!cmd.empty()) add_history(line);
            free(line);
        } else {
            if (!std::getline(std::cin, cmd)) break;
        }

        if (cmd.empty()) continue;
        if (cmd == "\\q") break;

        // echo
        if (cmd.rfind("echo ", 0) == 0) {
            std::string out = cmd.substr(5);
            if (out.size() >= 2 && out.front() == '"' && out.back() == '"') {
                out = out.substr(1, out.size() - 2);
            }
            std::cout << out << std::endl;
            continue;
        }

        // env
        if (cmd.rfind("\\e $", 0) == 0) {
            std::string var = cmd.substr(4);
            if (var == "PATH") {
                print_env_list(var);
            } else {
                char* v = getenv(var.c_str());
                if (v) std::cout << v << std::endl;
            }
            continue;
        }

        auto args = split(cmd);
        if (args.empty()) continue;

        std::string exe = find_exec(args[0]);
        if (exe.empty()) {
            std::cout << args[0] << ": command not found" << std::endl;
            continue;
        }

        std::vector<char*> argv;
        for (auto& s : args)
            argv.push_back(const_cast<char*>(s.c_str()));
        argv.push_back(nullptr);

        pid_t pid = fork();
        if (pid == 0) {
            execve(exe.c_str(), argv.data(), environ);
            _exit(127);
        } else {
            waitpid(pid, nullptr, 0);
        }
    }

    stop_users_vfs();
    return 0;
}
