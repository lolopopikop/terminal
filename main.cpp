#include <iostream>

int main(){
  while (true){
    std::cout << "$ ";
    std::string command;

    std::getline(std::cin, command);
    std::cout << command << std::endl;
  }

  return 0;
}
