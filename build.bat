@echo off
mkdir build
set CommonCompilerFlags=/Fobuild\ /Febuild\ /Fdbuild\ /GR- /Zi /std:c++20 /MT /Od /W3 -wd4018 -wd4201 -w44062 

IF "%1" == "tests" (
	cl %CommonCompilerFlags% test_generation.cpp 
) ELSE IF "%1" == "reader" (
  cl %CommonCompilerFlags% /O2 reader.cpp 
) ELSE IF "%1" == "run-all" (
	cl %CommonCompilerFlags% test_generation.cpp 
	cl %CommonCompilerFlags% /O2 main.cpp 
  mkdir TEST_DB
  del .\out.txt
  .\build\test_generation.exe
  python filegen.py exist
  .\build\main.exe 1
  del .\TEST_DB\tables\table0\active_partition.data
  .\build\main.exe 2
  del .\TEST_DB\tables\table0\active_partition.data
  .\build\main.exe 3
  del .\TEST_DB\tables\table0\active_partition.data
  .\build\main.exe 4
  del .\TEST_DB\tables\table0\active_partition.data
  .\build\main.exe 5
  del .\TEST_DB\tables\table0\active_partition.data
  .\build\main.exe 6
  del .\TEST_DB\tables\table0\active_partition.data
) ELSE (
	cl %CommonCompilerFlags% /Od main.cpp 
)
