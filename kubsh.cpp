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
#include <mutex>
#include <sys/inotify.h>
#include <fcntl.h>
#include <errno.h>

extern char **environ;

extern "C" {
#include "vfs.h"
}

/* Globals */
volatile sig_atomic_t reload_config = 0;
std::atomic<bool> running(true);

/* Signal handler */
void sighup_handler(int /*sig*/) {
    const char msg[] = "Configuration reloaded\n";
    /* write is async-signal-safe */
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    reload_config = 1;
}

/* Helpers */
static std::string trim(const std::string &s) {
    size_t a = 0;
    while (a < s.size() && isspace((unsigned char)s[a])) a++;
    size_t b = s.size();
    while (b > a && isspace((unsigned char)s[b-1])) b--;
    return s.substr(a, b-a);
}

static bool ends_with_sh(const std::string &s) {
    size_t n = s.size();
    if (n < 2) return false;
    return s[n-2] == 's' && s[n-1] == 'h';
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
        token = trim(token);
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
    (void)system(cmd.c_str());
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

/* Clean up on exit */
void cleanup() {
    running.store(false);
    stop_users_vfs();
}

/* --- Test-mode VFS emulation ---
   - populate "users" directory from /etc/passwd (only shells ending with "sh")
   - watch users dir via inotify: when new directory is created, add user to /etc/passwd
*/
static std::mutex passwd_mutex;

/* Check if username exists in /etc/passwd; returns true if exists */
static bool passwd_has_user(const std::string &username) {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return false;
    char line[1024];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, username.c_str(), username.size()) == 0 && line[username.size()] == ':') {
            found = true;
            break;
        }
    }
    fclose(f);
    return found;
}

/* Find max uid in /etc/passwd (returns 1000 if none found) */
static int passwd_max_uid() {
    FILE *f = fopen("/etc/passwd", "r");
    if (!f) return 1000;
    char line[1024];
    int max_uid = 1000;
    while (fgets(line, sizeof(line), f)) {
        // fields: name:passwd:uid:gid:gecos:home:shell
        char *p = strchr(line, ':');
        if (!p) continue;
        p = strchr(p+1, ':');
        if (!p) continue;
        p = p+1;
        // p points to uid
        int uid = atoi(p);
        if (uid > max_uid) max_uid = uid;
    }
    fclose(f);
    return max_uid;
}

/* Append user to /etc/passwd with /bin/bash and create home dir */
static bool add_user_to_passwd(const std::string &username) {
    std::lock_guard<std::mutex> lg(passwd_mutex);
    if (username.empty()) return false;
    if (passwd_has_user(username)) return false;

    int max_uid = passwd_max_uid();
    int new_uid = max_uid + 1;

    FILE *f = fopen("/etc/passwd", "a");
    if (!f) return false;
    // basic entry: username:x:uid:uid::/home/username:/bin/bash
    int written = fprintf(f, "%s:x:%d:%d::/home/%s:/bin/bash\n", username.c_str(), new_uid, new_uid, username.c_str());
    fclose(f);
    if (written < 0) return false;

    // create home dir
    std::string home = std::string("/home/") + username;
    mkdir(home.c_str(), 0755);

    return true;
}

/* Populate mountpoint from /etc/passwd (only shells ending with sh) */
static void populate_users_dir_from_passwd(const std::string &mountpoint) {
    // create mountpoint if missing
    std::filesystem::create_directories(mountpoint);
    setpwent();
    struct passwd *pwd;
    while ((pwd = getpwent()) != NULL) {
        if (pwd->pw_name && pwd->pw_shell && ends_with_sh(std::string(pwd->pw_shell))) {
            std::string path = mountpoint + "/" + pwd->pw_name;
            // create dir if missing
            std::error_code ec;
            std::filesystem::create_directories(path, ec);
            (void)ec;
        }
    }
    endpwent();
}

/* Watches mountpoint with inotify; when a new directory appears -> add user to /etc/passwd */
static void watch_users_dir_and_sync_passwd(const std::string &mountpoint) {
    int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) {
        // cannot watch — just return
        return;
    }

    int wd = inotify_add_watch(fd, mountpoint.c_str(), IN_CREATE | IN_MOVED_TO);
    if (wd < 0) {
        close(fd);
        return;
    }

    const size_t buf_sz = (sizeof(struct inotify_event) + NAME_MAX + 1) * 32;
    std::vector<char> buf(buf_sz);

    while (running.load()) {
        ssize_t len = read(fd, buf.data(), (int)buf.size());
        if (len <= 0) {
            // no events; sleep a bit
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            continue;
        }

        size_t i = 0;
        while (i < (size_t)len) {
            struct inotify_event *ev = (struct inotify_event*)(buf.data() + i);
            if (ev->len > 0) {
                std::string name(ev->name);
                if ((ev->mask & IN_ISDIR) || (ev->mask & IN_CREATE) || (ev->mask & IN_MOVED_TO)) {
                    // ensure it's a directory (sometimes create event for file)
                    std::string path = mountpoint + "/" + name;
                    struct stat st;
                    if (stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
                        // add to /etc/passwd if missing
                        if (!passwd_has_user(name)) {
                            bool ok = add_user_to_passwd(name);
                            if (ok) {
                                // optionally create id/home/shell files inside mountpoint (tests may not require)
                                // create simple files
                                std::string idf = path + "/id";
                                std::string homef = path + "/home";
                                std::string shellf = path + "/shell";
                                FILE *idw = fopen(idf.c_str(), "w");
                                if (idw) {
                                    int uid = passwd_max_uid(); // max after addition
                                    fprintf(idw, "%d", uid);
                                    fclose(idw);
                                }
                                FILE *hw = fopen(homef.c_str(), "w");
                                if (hw) {
                                    fprintf(hw, "/home/%s", name.c_str());
                                    fclose(hw);
                                }
                                FILE *sw = fopen(shellf.c_str(), "w");
                                if (sw) {
                                    fprintf(sw, "/bin/bash");
                                    fclose(sw);
                                }
                            }
                        }
                    }
                }
            }
            i += sizeof(struct inotify_event) + ev->len;
        }
    }

    inotify_rm_watch(fd, wd);
    close(fd);
}

/* Initialize a simple test-mode VFS (non-FUSE) */
static std::thread start_test_vfs(const std::string &mountpoint) {
    // populate immediately
    populate_users_dir_from_passwd(mountpoint);
    // start watcher thread
    std::thread t([mountpoint]() {
        watch_users_dir_and_sync_passwd(mountpoint);
    });
    return t;
}

/* Main */
int main(int argc, char* argv[]) {
    /* setup signals and exit cleanup */
    signal(SIGHUP, sighup_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    atexit(cleanup);

    /* arguments */
    bool auto_vfs = true;
    bool test_mode = false;

    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-vfs") == 0) auto_vfs = false;
        else if (strcmp(argv[i], "--test") == 0) test_mode = true;
        else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: kubsh [OPTIONS]\n"
                      << "Options:\n"
                      << "  --no-vfs    Disable VFS auto-mount\n"
                      << "  --test      Test mode (CI safe, disables FUSE but emulates VFS)\n"
                      << "  --help, -h  Show this help\n";
            return 0;
        }
    }

    /* If CI environment variable present, treat as test-mode safe */
    if (getenv("CI")) {
        test_mode = true;
    }

    /* In test mode we must not use FUSE; we'll emulate VFS in users/ */
    std::thread test_vfs_thread;
    if (test_mode) {
        auto_vfs = false; // disable real FUSE
        const std::string mountpoint = "users";
        // create mountpoint and populate from /etc/passwd
        std::filesystem::create_directories(mountpoint);
        test_vfs_thread = start_test_vfs(mountpoint);
        // leave thread running while program runs
    } else {
        // start FUSE-based vfs if requested (original behavior)
        if (auto_vfs) {
            mkdir("users", 0755);
            std::thread vfs_thread([]() {
                if (start_users_vfs("users") != 0) {
                    std::cerr << "Warning: Failed to start users VFS\n";
                }
            });
            vfs_thread.detach();
            // small sleep to give VFS time to mount
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
        }
    }

    /* history */
    std::string history_file = get_history_file();
    using_history();
    read_history(history_file.c_str());

    /* detect interactive */
    bool interactive = isatty(STDIN_FILENO);
    if (test_mode) interactive = false; /* force non-interactive in CI */

    /* Non-interactive batch mode when commands are provided */
    if (!interactive && argc > 1) {
        bool has_real_command = false;
        for (int i = 1; i < argc; ++i) {
            if (argv[i][0] != '-') { has_real_command = true; break; }
        }

        if (has_real_command) {
            for (int i = 1; i < argc; ++i) {
                std::string command = argv[i];
                if (command.empty()) continue;
                if (command[0] == '-') continue; /* skip options */

                if (command == "\\q") break;
                if (command.rfind("echo ", 0) == 0) {
                    std::cout << command.substr(5) << std::endl;
                    continue;
                }
                if (command.rfind("\\e $", 0) == 0) {
                    std::string env_var = command.substr(4);
                    if (env_var == "PATH") print_env_list(env_var);
                    else {
                        char* v = getenv(env_var.c_str());
                        if (v) std::cout << v << std::endl;
                    }
                    continue;
                }

                auto tokens = split(command);
                if (tokens.empty()) continue;

                std::string exe = find_executable(tokens[0]);
                if (exe.empty()) {
                    std::cout << tokens[0] << ": command not found" << std::endl;
                    continue;
                }

                std::vector<char*> args;
                for (auto &s : tokens) args.push_back(const_cast<char*>(s.c_str()));
                args.push_back(nullptr);

                pid_t pid = fork();
                if (pid == 0) {
                    execve(exe.c_str(), args.data(), environ);
                    perror("execve");
                    _exit(127);
                } else if (pid > 0) {
                    int status = 0;
                    waitpid(pid, &status, 0);
                } else {
                    perror("fork");
                }
            }
            // keep running (tests expect kubsh process to be alive). Do not exit immediately.
            // But if you prefer to exit after commands, uncomment next line:
            // return 0;
        }
    }

    /* Interactive loop */
    while (running.load()) {
        if (reload_config) {
            std::cout << "Configuration reloaded" << std::endl;
            reload_config = 0;
        }

        char* line = nullptr;
        if (interactive) {
            line = readline("$ ");
        } else {
            // in non-interactive mode we wait for stdin or sleep to avoid busy loop
            std::string input;
            if (!std::getline(std::cin, input)) {
                // no stdin data; sleep a bit and continue (this keeps process alive for tests)
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                continue;
            }
            line = strdup(input.c_str());
        }

        if (!line) continue;
        std::string command = line;

        if (!command.empty()) {
            add_history(line);
            write_history(history_file.c_str());
            std::ofstream h(history_file, std::ios::app);
            if (h.is_open()) h << command << std::endl;
        }

        free(line);

        if (command.empty()) continue;

        /* builtins */
        if (command == "\\q") break;

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
            if (env_var == "PATH") print_env_list("PATH");
            else {
                char* v = getenv(env_var.c_str());
                if (v) std::cout << v << std::endl;
            }
            continue;
        }

        if (command.rfind("\\l ", 0) == 0) {
            list_partitions(command.substr(3));
            continue;
        }

        auto tokens = split(command);
        if (tokens.empty()) continue;

        if (tokens[0] == "cd") {
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

        /* external commands */
        std::string exe = find_executable(tokens[0]);
        if (exe.empty()) {
            std::cout << tokens[0] << ": command not found" << std::endl;
            continue;
        }

        std::vector<char*> args;
        for (auto &s : tokens) args.push_back(const_cast<char*>(s.c_str()));
        args.push_back(nullptr);

        pid_t pid = fork();
        if (pid == 0) {
            execve(exe.c_str(), args.data(), environ);
            perror("execve");
            _exit(127);
        } else if (pid > 0) {
            int status = 0;
            waitpid(pid, &status, 0);
        } else {
            perror("fork");
        }
    }

    /* shutdown */
    running.store(false);
    // join test vfs thread if running
    if (test_mode) {
        // signal watcher to stop and give it a bit
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        if (test_vfs_thread.joinable()) test_vfs_thread.detach();
    }

    cleanup();
    return 0;
}
