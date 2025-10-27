#include <iostream>
#include <string>

int main(){
  std::string command;
  while (true){
    std::cout << "$ ";
    std::getline(std::cin, command);

    while (true){
      std::cout << command << std::endl
                << "Press ENTER to display again" << std::endl;
      std::string temp;
      std::getline(std::cin, temp);
    }
  }

  return 0;
}
