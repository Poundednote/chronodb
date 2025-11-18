@echo off
mkdir build
set CommonCompilerFlags=/Fobuild\ /Febuild\ /Fdbuild\ /GR- /Zi -std:c++20 /Od /W3 -wd4018 -wd4201 -w44062 
cl %CommonCompilerFlags% /Od /Zi /std:c++20 main.cpp /MT 
