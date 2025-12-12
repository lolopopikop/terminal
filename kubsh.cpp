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
#include <chrono>

extern char **environ;

extern "C" {
#include "vfs.h"
}

// Forward declaration of the new function in vfs.c
extern "C" void vfs_set_root(const char*);

using namespace std;

volatile sig_atomic_t reload_config = 0;
std::atomic<bool> running(true);

void sighup_handler(int /*sig*/) {
    const char msg[] = "Configuration reloaded\n";
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    reload_config = 1;
}

static string trim(const string &s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static vector<string> split_ws(const string &s) {
    vector<string> out;
    istringstream iss(s);
    string tok;
    while (iss >> tok) out.push_back(tok);
    return out;
}

static string strip_quotes(const string &s) {
    if (s.size() >= 2 &&
       ((s.front() == '"' && s.back() == '"') ||
        (s.front()=='\'' && s.back()=='\'')))
        return s.substr(1, s.size()-2);
    return s;
}

static string find_executable(const string& command) {
    if (command.empty()) return "";
    char* path_env = getenv("PATH");
    if (!path_env) return "";
    string path_str = path_env;
    string dir;
    stringstream ss(path_str);
    while (getline(ss, dir, ':')) {
        string full = dir + "/" + command;
        if (access(full.c_str(), X_OK) == 0) return full;
    }
    return "";
}

static void vfs_sync_loop(const string &vfs_dir) {
    using namespace std::chrono_literals;
    while (running.load()) {
        DIR *d = opendir(vfs_dir.c_str());
        if (d) {
            struct dirent *ent;
            while ((ent = readdir(d))) {
                if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
                string cand = vfs_dir + "/" + ent->d_name;
                struct stat st;
                if (stat(cand.c_str(), &st) != 0) continue;
                if (!S_ISDIR(st.st_mode)) continue;

                vfs_add_user(ent->d_name); // Call to add user in VFS
            }
            closedir(d);
        }
        this_thread::sleep_for(25ms);
    }
}

void cleanup() {
    running.store(false);
    stop_users_vfs();
    this_thread::sleep_for(std::chrono::milliseconds(50));
}

int main(int argc, char* argv[]) {
    signal(SIGHUP, sighup_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    atexit(cleanup);

    bool auto_vfs = true;
    bool test_mode = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-vfs") == 0) auto_vfs = false;
        else if (strcmp(argv[i], "--test") == 0) test_mode = true;
    }

    if (test_mode) auto_vfs = false;

    bool ci_mode = getenv("CI") != nullptr;
    if (ci_mode) {
        fprintf(stderr, "---\nCI ENV detected — VFS sync loop active\n");
    }

    string vfs_dir = "users";
    char* env_v = getenv("KUBSH_VFS_DIR");
    if (env_v) vfs_dir = string(env_v);

    mkdir(vfs_dir.c_str(), 0755);

    // Set the root for VFS
    vfs_set_root(vfs_dir.c_str());

    if (auto_vfs) {
        std::thread t1([vfs_dir]() { start_users_vfs(vfs_dir.c_str()); });
        t1.detach();

        std::thread t2([vfs_dir]() { vfs_sync_loop(vfs_dir); });
        t2.detach();
    }
    else if (ci_mode) {
        std::thread t2([vfs_dir]() { vfs_sync_loop(vfs_dir); });
        t2.detach();
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(150));

    string history_file = get_history_file();
    using_history();
    read_history(history_file.c_str());

    bool interactive = isatty(STDIN_FILENO);
    if (test_mode) interactive = false;

    string input;
    while (running.load()) {
        if (reload_config) {
            cout << "Configuration reloaded" << endl;
            reload_config = 0;
        }

        if (interactive) {
            char *ln = readline("$ ");
            if (!ln) break;
            input = string(ln);
            free(ln);
        } else {
            if (!getline(cin, input)) break;
        }

        input = trim(input);
        if (input.empty()) continue;

        add_history(input.c_str());
        write_history(history_file.c_str());
        ofstream hf(history_file, ios::app);
        if (hf.is_open()) { hf << input << "\n"; hf.close(); }

        if (input == "\\q") break;

        if (input.rfind("debug ", 0) == 0) { do_debug(input); continue; }
        if (input.rfind("echo ", 0) == 0) { do_echo(input); continue; }
        if (input.rfind("\\e ", 0) == 0) { do_env(input); continue; }
        if (input.rfind("\\l ", 0) == 0) { do_list(input); continue; }

        auto toks = split_ws(input);
        if (toks.empty()) continue;
        string exe = find_executable(toks[0]);
        if (exe.empty()) {
            cout << toks[0] << ": command not found" << endl;
            continue;
        }

        vector<char*> argv_exec;
        for (auto &s : toks) argv_exec.push_back(const_cast<char*>(s.c_str()));
        argv_exec.push_back(nullptr);

        pid_t pid = fork();
        if (pid == 0) {
            execv(exe.c_str(), argv_exec.data());
            _exit(127);
        }
        else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        }
        else {
            perror("fork");
        }
    }

    cleanup();
    return 0;
}
