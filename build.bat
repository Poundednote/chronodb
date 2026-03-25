@echo off
mkdir build
set CommonCompilerFlags=/Fobuild\ /Febuild\ /Fdbuild\ /GR- /Zi -std:c++20 /Od /W3 -wd4018 -wd4201 -w44062

IF "%1" == "tests" (
	cl %CommonCompilerFlags% /Zi /std:c++20 test_generation.cpp /MT 
) ELSE (
	cl %CommonCompilerFlags% /O2 /std:c++20 main.cpp /MT
)
