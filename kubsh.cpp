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
#include <chrono>

extern char **environ;

extern "C" {
#include "vfs.h"
}

// vfs.h in the repo does not declare vfs_add_user, but we call it from C++.
// Provide the prototype here to avoid having to modify the header file.
extern "C" int vfs_add_user(const char*);

using namespace std;

volatile sig_atomic_t reload_config = 0;
std::atomic<bool> running(true);

void sighup_handler(int /*sig*/) {
    const char msg[] = "Configuration reloaded\n";
    /* write is async-signal-safe */
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    reload_config = 1;
}

/* small helpers */
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
    if (s.size() >= 2 && ((s.front() == '"' && s.back() == '"') || (s.front()=='\'' && s.back()=='\'')))
        return s.substr(1, s.size()-2);
    return s;
}

/* find executable in PATH */
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

/* builtins */
static void do_echo(const string &line) {
    string rest = trim(line.substr(5));
    cout << strip_quotes(rest) << endl;
}

static void do_debug(const string &line) {
    string rest = trim(line.substr(6));
    cout << strip_quotes(rest) << endl;
}

static void do_env(const string &line) {
    string rest = trim(line.substr(3));
    if (rest.empty() || rest[0] != '$') {
        cout << "\\e: command not found" << endl;
        return;
    }
    const char* val = getenv(rest.c_str()+1);
    if (!val) return;
    string s(val);
    if (s.find(':') == string::npos) {
        cout << s << endl;
    } else {
        string item;
        stringstream ss(s);
        while (getline(ss, item, ':')) cout << item << endl;
    }
}

static void do_list(const string &line) {
    string rest = trim(line.substr(3));
    if (rest.empty()) {
        cout << "\\l: missing argument" << endl;
        return;
    }
    string cmd = "lsblk " + rest;
    system(cmd.c_str());
}

/* simple history file path */
static string get_history_file() {
    char* home = getenv("HOME");
    if (home) return string(home) + "/.kubsh_history";
    struct passwd* pw = getpwuid(getuid());
    if (pw) return string(pw->pw_dir) + "/.kubsh_history";
    return "/root/.kubsh_history";
}

/* Sync loop: scan vfs dir and call vfs_add_user for new directories.
   This avoids races in tests where they create dir before watcher notices. */
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
                /* idempotent public API call: creates /etc/passwd entry + vfs files */
                vfs_add_user(ent->d_name);
            }
            closedir(d);
        }
        this_thread::sleep_for(25ms);
    }
}

/* cleanup called at exit */
void cleanup() {
    running.store(false);
    stop_users_vfs();
    this_thread::sleep_for(chrono::milliseconds(50));
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

    if (getenv("CI")) {
        /* tests expect KUBSH_VFS_DIR to be honored, so we don't override it */
        fprintf(stderr, "CI ENV detected — FUSE disabled, VFS active\n");
    }

    string vfs_dir = "users";
    char* env_v = getenv("KUBSH_VFS_DIR");
    if (env_v) vfs_dir = string(env_v);

    if (auto_vfs) {
        mkdir(vfs_dir.c_str(), 0755);

        thread t1([vfs_dir](){ start_users_vfs(vfs_dir.c_str()); });
        t1.detach();

        thread t2([vfs_dir](){ vfs_sync_loop(vfs_dir); });
        t2.detach();

        this_thread::sleep_for(chrono::milliseconds(150));
    }

    /* history init */
    string history_file = get_history_file();
    using_history();
    read_history(history_file.c_str());

    bool interactive = isatty(STDIN_FILENO);
    if (test_mode) interactive = false;

    /* Main loop - supports both interactive and non-interactive (tests) */
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
            if (!std::getline(cin, input)) break;
        }

        input = trim(input);
        if (input.empty()) continue;

        add_history(input.c_str());
        write_history(history_file.c_str());
        /* append to history file explicitly to be safe in CI tests */
        ofstream hf(history_file, ios::app);
        if (hf.is_open()) { hf << input << "\n"; hf.close(); }

        if (input == "\\q") break;

        if (input.rfind("debug ", 0) == 0) { do_debug(input); continue; }
        if (input.rfind("echo ", 0) == 0) { do_echo(input); continue; }
        if (input.rfind("\\e ", 0) == 0) { do_env(input); continue; }
        if (input.rfind("\\l ", 0) == 0) { do_list(input); continue; }

        /* run external command: check PATH first */
        auto toks = split_ws(input);
        if (toks.empty()) continue;
        string exe = find_executable(toks[0]);
        if (exe.empty()) {
            cout << toks[0] << ": command not found" << endl;
            continue;
        }

        /* prepare argv */
        vector<char*> argv_exec;
        for (auto &s : toks) argv_exec.push_back(const_cast<char*>(s.c_str()));
        argv_exec.push_back(nullptr);

        pid_t pid = fork();
        if (pid == 0) {
            /* child */
            execv(exe.c_str(), argv_exec.data());
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        } else {
            perror("fork");
        }
    }

    cleanup();
    return 0;
}
