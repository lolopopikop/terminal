#include <iostream>
#include <string>
#include <fstream>
#include <cstdlib>

int main(){
  std::string command;
  while (true){
    std::cout << "$ ";
    std::getline(std::cin, command);

    if (command[0] == '\\' && command[1] == 'q') return 0;

    while (true){
      std::cout << command << std::endl
                << "Press ENTER to display again" << std::endl;
      std::string temp;
      std::getline(std::cin, temp);
    }
  }

  return 0;
}