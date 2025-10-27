mkdir ./build
clang++ -O0 -g main.cpp -o ./build/main --std=c++11  -fsanitize=thread
