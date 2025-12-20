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

        // \l - list disk partitions
        // \l - list disk partitions
if (tokens[0] == "\\l") {
    if (tokens.size() == 2) {
        std::string device = tokens[1];
        
        // Проверяем, существует ли устройство
        struct stat st;
        if (stat(device.c_str(), &st) != 0) {
            // Попробуем найти устройство в /proc/partitions
            FILE* fp = fopen("/proc/partitions", "r");
            if (fp) {
                char line[256];
                bool device_found = false;
                bool partitions_found = false;
                
                // Извлекаем имя устройства из пути
                std::string dev_name = device;
                size_t pos = dev_name.find_last_of('/');
                if (pos != std::string::npos) {
                    dev_name = dev_name.substr(pos + 1);
                }
                
                std::cout << "Looking for device: " << dev_name << std::endl;
                
                // Пропускаем заголовок
                for (int i = 0; i < 2 && fgets(line, sizeof(line), fp); i++);
                
                while (fgets(line, sizeof(line), fp)) {
                    char name[64];
                    unsigned int major, minor, blocks;
                    
                    if (sscanf(line, "%u %u %u %63s", &major, &minor, &blocks, name) == 4) {
                        std::string part_name(name);
                        
                        // Ищем само устройство
                        if (part_name == dev_name) {
                            device_found = true;
                            std::cout << "Device found in /proc/partitions:" << std::endl;
                            std::cout << "  " << device << " (major: " << major 
                                      << ", minor: " << minor 
                                      << ", blocks: " << blocks << ")" << std::endl;
                        }
                        
                        // Ищем разделы этого устройства
                        if (part_name.find(dev_name) == 0 && part_name.length() > dev_name.length()) {
                            partitions_found = true;
                            std::cout << "  Partition: /dev/" << part_name 
                                      << " (" << blocks << " blocks)" << std::endl;
                        }
                    }
                }
                
                fclose(fp);
                
                if (!device_found) {
                    std::cout << device << ": no such device in /proc/partitions" << std::endl;
                    
                    // Покажем все доступные устройства
                    std::cout << "\nAvailable block devices:" << std::endl;
                    fp = fopen("/proc/partitions", "r");
                    if (fp) {
                        for (int i = 0; i < 2 && fgets(line, sizeof(line), fp); i++);
                        while (fgets(line, sizeof(line), fp)) {
                            char name[64];
                            unsigned int major, minor, blocks;
                            if (sscanf(line, "%u %u %u %63s", &major, &minor, &blocks, name) == 4) {
                                std::cout << "  /dev/" << name << std::endl;
                            }
                        }
                        fclose(fp);
                    }
                } else if (!partitions_found) {
                    std::cout << "No partitions found for this device" << std::endl;
                }
            } else {
                std::cout << device << ": no such device (cannot access /proc/partitions)" << std::endl;
            }
        } else {
            // Устройство существует, используем fdisk
            std::string command = "fdisk -l " + device + " 2>/dev/null";
            
            FILE* pipe = popen(command.c_str(), "r");
            if (!pipe) {
                std::cout << "Failed to execute fdisk" << std::endl;
                continue;
            }
            
            char buffer[256];
            bool found = false;
            std::cout << "Partition table for " << device << ":" << std::endl;
            while (fgets(buffer, sizeof(buffer), pipe) != NULL) {
                found = true;
                std::cout << buffer;
            }
            
            pclose(pipe);
            
            if (!found) {
                std::cout << "No partition table or unable to read" << std::endl;
            }
        }
    } else {
        std::cout << "Usage: \\l /dev/sda" << std::endl;
        std::cout << "\nAvailable block devices from /proc/partitions:" << std::endl;
        
        FILE* fp = fopen("/proc/partitions", "r");
        if (fp) {
            char line[256];
            for (int i = 0; i < 2 && fgets(line, sizeof(line), fp); i++);
            while (fgets(line, sizeof(line), fp)) {
                char name[64];
                unsigned int major, minor, blocks;
                if (sscanf(line, "%u %u %u %63s", &major, &minor, &blocks, name) == 4) {
                    std::cout << "  /dev/" << name << std::endl;
                }
            }
            fclose(fp);
        }
    }
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
