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
    write(STDOUT_FILENO, msg, sizeof(msg) - 1);
    reload_config = 1;
}

/* Clean up on exit */
void cleanup() {
    running.store(false);
    stop_users_vfs();
}

/* Helpers */
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

int main(int argc, char* argv[]) {
    /* Setup signals and exit cleanup */
    signal(SIGHUP, sighup_handler);
    signal(SIGINT, SIG_IGN);
    signal(SIGTERM, SIG_IGN);
    atexit(cleanup);
    
    /* Parse arguments */
    bool auto_vfs = true;
    bool test_mode = false;
    std::string command_to_execute;
    
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--no-vfs") == 0) {
            auto_vfs = false;
        } else if (strcmp(argv[i], "--test") == 0) {
            test_mode = true;
        } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
            command_to_execute = argv[++i];
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            std::cout << "Usage: kubsh [OPTIONS]\n"
                      << "Options:\n"
                      << "  --no-vfs    Disable VFS auto-mount\n"
                      << "  --test      Test mode\n"
                      << "  -c CMD      Execute command and exit\n"
                      << "  --help, -h  Show this help\n";
            return 0;
        }
    }
    
    /* Start VFS */
    if (auto_vfs) {
        const char* vfs_dir = "users";
        mkdir(vfs_dir, 0755);
        
        if (start_users_vfs(vfs_dir) != 0) {
            std::cerr << "Warning: Failed to start users VFS\n";
        }
    }
    
    /* Handle -c option (execute single command) */
    if (!command_to_execute.empty()) {
        if (command_to_execute == "\\q") {
            return 0;
        }
        
        if (command_to_execute.rfind("echo ", 0) == 0) {
            std::cout << command_to_execute.substr(5) << std::endl;
            return 0;
        }
        
        if (command_to_execute.rfind("\\e $", 0) == 0) {
            std::string env_var = command_to_execute.substr(4);
            if (env_var == "PATH") {
                print_env_list("PATH");
            } else {
                char* v = getenv(env_var.c_str());
                if (v) std::cout << v << std::endl;
            }
            return 0;
        }
        
        if (command_to_execute.rfind("\\l ", 0) == 0) {
            list_partitions(command_to_execute.substr(3));
            return 0;
        }
        
        auto tokens = split(command_to_execute);
        if (!tokens.empty()) {
            if (tokens[0] == "cd") {
                if (tokens.size() > 1) {
                    if (chdir(tokens[1].c_str()) != 0) {
                        perror("cd");
                    }
                }
                return 0;
            }
            
            std::string exe = find_executable(tokens[0]);
            if (exe.empty()) {
                std::cout << tokens[0] << ": command not found" << std::endl;
                return 127;
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
                return WEXITSTATUS(status);
            } else {
                perror("fork");
                return 1;
            }
        }
        
        return 0;
    }
    
    /* Load command history */
    std::string history_file = get_history_file();
    using_history();
    read_history(history_file.c_str());
    
    /* Main interactive loop */
    bool interactive = isatty(STDIN_FILENO);
    
    while (running.load()) {
        if (reload_config) {
            std::cout << "Configuration reloaded" << std::endl;
            reload_config = 0;
        }
        
        char* line = nullptr;
        if (interactive) {
            line = readline("$ ");
        } else {
            std::string input;
            if (!std::getline(std::cin, input)) break;
            line = strdup(input.c_str());
        }
        
        if (!line) break;
        
        std::string command = line;
        
        /* Add to history */
        if (!command.empty()) {
            add_history(line);
            write_history(history_file.c_str());
            
            std::ofstream h(history_file, std::ios::app);
            if (h.is_open()) h << command << std::endl;
        }
        
        free(line);
        
        if (command.empty()) continue;
        
        /* Builtin commands */
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
            if (env_var == "PATH") {
                print_env_list("PATH");
            } else {
                char* v = getenv(env_var.c_str());
                if (v) std::cout << v << std::endl;
            }
            continue;
        }
        
        if (command.rfind("\\l ", 0) == 0) {
            list_partitions(command.substr(3));
            continue;
        }
        
        /* Split and execute */
        auto tokens = split(command);
        if (tokens.empty()) continue;
        
        if (tokens[0] == "cd") {
            if (tokens.size() > 1) {
                if (chdir(tokens[1].c_str()) != 0) {
                    perror("cd");
                }
            } else {
                const char* home = getenv("HOME");
                if (home) chdir(home);
            }
            continue;
        }
        
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
    
    cleanup();
    return 0;
}