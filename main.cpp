#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib>

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

    else std::cout << command << ": command not found" << std::endl;
  }

  return 0;
}