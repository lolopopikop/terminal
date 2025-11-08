#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib>
#include <vector>
#include <sstream>
#include <unistd.h>
#include <sys/wait.h>
#include <cstring>

// Функция для разделения строки на токены
std::vector<std::string> split(const std::string& str) {
    std::vector<std::string> tokens;
    std::stringstream ss(str);
    std::string token;
    
    while (ss >> token) {
        tokens.push_back(token);
    }
    
    return tokens;
}

// Функция для поиска исполняемого файла в PATH
std::string find_executable(const std::string& command) {
    char* path_env = std::getenv("PATH");
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

int main(){
  std::string command;
  while (true){
    std::cout << "$ ";
    std::getline(std::cin, command);

    if (command == "print history"){
      std::ifstream file("kubsh_history");
      if (!file)
        std::cout << "No history yet!" << std::endl;
      else{
        std::string komanda;
        while (std::getline(file, komanda))
          std::cout << komanda << std::endl;
      }
      continue;
    }

    else if (command == "clear history"){
      if (std::remove("kubsh_history") == 0)
        std::cout << "History cleared!" << std::endl;
      else
        std::cout << "No history file found!" << std::endl;
      continue;
    }

    std::ofstream file("kubsh_history", std::ios::app);
    file << command << std::endl;

    if (command.substr(0, 5) == "echo ") {
      std::string arg = command.substr(5);

      if (arg == "$PATH"){
        char* path = std::getenv("PATH");
        if (path) std::cout << path << std::endl;
        else std::cout << "PATH environment variable is not set!" << std::endl;
      }
      else std::cout << arg << std::endl;
    }

    else if (command == "\\q") return 0;

    else {
      std::vector<std::string> tokens = split(command);
      if (tokens.empty()) continue;
    
      std::string executable_path = find_executable(tokens[0]);
    
      if (!executable_path.empty()) {
        std::vector<char*> args;
        for (auto& token : tokens)
            args.push_back(const_cast<char*>(token.c_str()));
        args.push_back(nullptr);
        
        pid_t pid = fork();
        
        if (pid == 0) {
            execve(executable_path.c_str(), args.data(), environ);
            std::cerr << "Failed to execute: " << command << std::endl;
            exit(1);
        } 
        else if (pid > 0) {
            int status;
            waitpid(pid, &status, 0);
        } 
        else {
            std::cerr << "Failed to create process!" << std::endl;
        }
      }
      else {
        std::cout << command << ": command not found" << std::endl;
      }
    }
  }

  return 0;
}