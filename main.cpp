#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib>

int main(){
  std::string command;
  while (true){
    std::cout << "$ ";
    std::getline(std::cin, command);

    if (command.substr(0, 13) == "print history"){
      std::ifstream file("kubsh_history");
      if (!file)
        std::cout << "No history yet!" << std::endl;
      else{
        std::string komanda;
        while (std::getline(file, komanda))
          std::cout << komanda << std::endl;
      }
    }

    else if (command.substr(0, 13) == "clear history"){
      if (std::remove("kubsh_history") == 0)
        std::cout << "History cleared!" << std::endl;
      else
        std::cout << "No history file found!" << std::endl;
    }

    if (command.substr(0, 13) != "print history" && command.substr(0, 13) != "clear history"){
      std::ofstream file("kubsh_history", std::ios::app);
      file << command << std::endl;
    }

    if (command.substr(0, 2) == "\\q") return 0;
  }

  return 0;
}