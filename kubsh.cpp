#include <iostream>
#include <sstream>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <csignal>
#include <unistd.h>
#include <sys/wait.h>

#include "vfs.h"

namespace fs = std::filesystem;

static std::atomic<bool> running(true);
static const std::string VFS_ROOT = "/home/runner/work/terminal/terminal/tests/users";

void signal_handler(int) {
    running = false;
}

void start_vfs_watcher() {
    std::thread([&]() {
        while (running) {
            for (auto &entry : fs::directory_iterator(VFS_ROOT)) {
                if (entry.is_directory()) {
                    std::string username = entry.path().filename().string();
                    if (!vfs_user_exists(username.c_str())) {
                        vfs_add_user(username.c_str());
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }).detach();
}

std::vector<std::string> split(const std::string &s) {
    std::vector<std::string> r;
    std::stringstream ss(s);
    std::string w;
    while (ss >> w) r.push_back(w);
    return r;
}

int main(int argc, char **argv) {
    signal(SIGINT, signal_handler);

    vfs_init(VFS_ROOT.c_str());
    start_vfs_watcher();

    bool interactive = isatty(STDIN_FILENO);

    //
    // --- FIX: kubsh must NOT exit immediately in batch-mode ---
    //
    bool batch_has_cmd = false;
    if (!interactive && argc > 1) {
        for (int i = 1; i < argc; i++) {
            if (argv[i][0] != '-') batch_has_cmd = true;
        }

        if (batch_has_cmd) {
            for (int i = 1; i < argc; i++) {
                if (argv[i][0] == '-') continue;

                std::string cmd = argv[i];
                auto tokens = split(cmd);

                if (tokens.empty()) continue;

                if (tokens[0] == "\\vfs_add" && tokens.size() == 2) {
                    vfs_add_user(tokens[1].c_str());
                    continue;
                }

                pid_t pid = fork();
                if (pid == 0) {
                    execlp(tokens[0].c_str(), tokens[0].c_str(), NULL);
                    _exit(1);
                } else {
                    waitpid(pid, NULL, 0);
                }
            }

            // --- FIX: give watcher time to react ---
            std::this_thread::sleep_for(std::chrono::milliseconds(400));
        }
    }

    //
    // INTERACTIVE LOOP
    //
    while (running) {
        if (interactive) std::cout << "kubsh> " << std::flush;

        std::string line;
        if (!std::getline(std::cin, line)) break;
        auto tokens = split(line);
        if (tokens.empty()) continue;

        // builtins
        if (tokens[0] == "exit") break;
        if (tokens[0] == "env") {
            for (char **env = environ; *env; env++) {
                std::cout << *env << "\n";
            }
            continue;
        }
        if (tokens[0] == "echo") {
            for (size_t i = 1; i < tokens.size(); ++i)
                std::cout << tokens[i] << (i + 1 < tokens.size() ? " " : "");
            std::cout << "\n";
            continue;
        }

        // NEW: manual VFS add
        if (tokens[0] == "\\vfs_add" && tokens.size() == 2) {
            vfs_add_user(tokens[1].c_str());
            continue;
        }

        // external command
        pid_t pid = fork();
        if (pid == 0) {
            std::vector<char*> args;
            for (auto &tok : tokens) args.push_back((char*)tok.c_str());
            args.push_back(NULL);
            execvp(args[0], args.data());
            _exit(1);
        } else {
            waitpid(pid, NULL, 0);
        }
    }

    running = false;
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    return 0;
}
