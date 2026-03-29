@echo off
mkdir build
set CommonCompilerFlags=/Fobuild\ /Febuild\ /Fdbuild\ /GR- /Zi /std:c++20 /MT /Od /W3 -wd4018 -wd4201 -w44062 

IF "%1" == "tests" (
	cl %CommonCompilerFlags% test_generation.cpp 
) ELSE IF "%1" == "reader" (
  cl %CommonCompilerFlags% /Od reader.cpp 
) ELSE (
	cl %CommonCompilerFlags% /Od main.cpp 
)

