#include <iostream>
#include <vector>
#include <string>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <csignal>
#include <fcntl.h>
#include <filesystem>
#include <algorithm>
#include <fstream>

namespace fs = std::filesystem;

bool test_mode = false;
bool auto_vfs = true;

/// Detect TTY for "interactive" mode
bool is_interactive() {
    return isatty(STDIN_FILENO);
}

/// Signal handler (SIGINT)
void sighup_handler(int) {
    const char *msg = "\n";
    write(STDOUT_FILENO, msg, 1);
}

/// Split string into tokens
std::vector<std::string> split(const std::string &s) {
    std::stringstream ss(s);
    std::string tok;
    std::vector<std::string> out;
    while (ss >> tok) out.push_back(tok);
    return out;
}

/// List partitions helper
void list_partitions(const std::string &cmd) {
    std::system(cmd.c_str());
}

/// ----------------------------
/// VFS INITIALIZATION
/// ----------------------------
fs::path vfs_root;
bool vfs_ready = false;

void init_vfs() {
    // Check forced VFS in CI
    bool force_vfs = getenv("KUBSH_FORCE_VFS") != nullptr;

    // Detect CI environment
    if (getenv("CI") && !force_vfs) {
        std::cout << "---\nCI ENV detected — disabling VFS\n";
        auto_vfs = false;
        test_mode = true;
    }

    if (!auto_vfs)
        return;

    vfs_root = fs::current_path() / "users";

    try {
        fs::create_directories(vfs_root);
        vfs_ready = true;
    } catch (...) {
        vfs_ready = false;
    }

    // Auto-generate users from /etc/passwd
    if (!vfs_ready) return;

    std::ifstream f("/etc/passwd");
    if (!f.is_open()) return;

    std::string line;
    while (std::getline(f, line)) {
        if (!line.ends_with("sh")) continue;

        auto parts = split(line.substr(0, line.find(":")));
        if (parts.empty()) continue;

        std::string name = line.substr(0, line.find(":"));
        name = name.substr(0, name.find(":"));

        fs::create_directories(vfs_root / name);
    }
}

/// ----------------------------
/// EXECUTE COMMAND
/// ----------------------------
int run_command(const std::vector<std::string> &args) {
    if (args.empty()) return 0;

    // Built-in: echo
    if (args[0] == "echo") {
        for (size_t i = 1; i < args.size(); i++) {
            std::cout << args[i];
            if (i + 1 != args.size()) std::cout << " ";
        }
        std::cout << "\n";
        return 0;
    }

    // Built-in: exit
    if (args[0] == "exit") {
        return -1;
    }

    // Built-in: env
    if (args[0] == "env") {
        extern char **environ;
        for (char **env = environ; *env; env++)
            std::cout << *env << "\n";
        return 0;
    }

    // Built-in: PARTSCAN
    if (args[0] == "PARTSCAN") {
        list_partitions("lsblk");
        return 0;
    }

    // Built-in: VFS user list
    if (args[0] == "vfs-users") {
        if (!vfs_ready) return 0;
        for (auto &p : fs::directory_iterator(vfs_root)) {
            if (p.is_directory())
                std::cout << p.path().filename().string() << "\n";
        }
        return 0;
    }

    // Built-in: add user
    if (args[0] == "vfs-add-user" && args.size() >= 2) {
        if (!vfs_ready) return 0;
        fs::create_directories(vfs_root / args[1]);
        return 0;
    }

    // Try external command
    pid_t pid = fork();
    if (pid == 0) {
        std::vector<char *> execv_args;
        for (const auto &a : args)
            execv_args.push_back(const_cast<char *>(a.c_str()));
        execv_args.push_back(nullptr);

        execvp(execv_args[0], execv_args.data());

        // Command failed
        _exit(127);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    return WEXITSTATUS(status);
}

/// ----------------------------
/// MAIN LOOP
/// ----------------------------
int main(int, char **) {
    std::signal(SIGINT, sighup_handler);

    bool interactive = is_interactive();

    init_vfs();

    while (true) {
        if (interactive)
            std::cout << "$ " << std::flush;

        std::string line;
        if (!std::getline(std::cin, line))
            break;

        auto args = split(line);
        int rc = run_command(args);

        if (rc < 0)
            break;
    }

    return 0;
}
