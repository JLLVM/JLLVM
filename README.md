<p align="center">
  <img height="250" src="logo.png" alt="JLLVM logo">
</p>

# JLLVM

This repository contains a best-effort implementation of a JVM as part of the course work for the course 
"[Abstrakte Maschinen](http://www.complang.tuwien.ac.at/andi/185.966.html)" at the Technical University of Vienna.

JLLVM is a on-demand JIT compiling JVM without an interpreter. It uses a Lisp style semi-space relocating garbage 
collector and LLVMs ORCv2 JIT with jitlink for the JIT. 

The currently implemented JVM spec is version 17. The plan at this point in time is to use the Java Class Library 
shipped with the OpenJDK. 

## Building JLLVM

Building JLLVM requires following dependencies:
* A C++20 capable compiler
* CMake 3.20 or newer
* Python 3.6 or newer
* OpenJDK 17 or newer
* LLVM 16

It is also highly recommended to use `ninja` as meta build system with cmake.

JLLVM is currently only supported on Linux and macOS.

### Building LLVM for JLLVM

For development it is most useful to build and install LLVM once on disk rather than building it as part of JLLVM each 
time.
LLVM itself uses standard cmake and can be built like most other cmake installations. You can find the official 
documentation here: https://llvm.org/docs/CMake.html

A minimal LLVM installation suited for development of JLLVM can be installed with the following steps:
```shell
git clone release/16.x https://github.com/llvm/llvm-project.git
cd llvm-project/llvm
mkdir build && cd build
cmake .. -G Ninja -DCMAKE_BUILD_TYPE=Release -DLLVM_TARGETS_TO_BUILD=host -DLLVM_ENABLE_ASSERTIONS=ON -DCMAKE_INSTALL_PREFIX=/your/install/dir
ninja install
```

This will install directly into `/your/install/dir` (as in, that is where it'll create `bin`, `lib`, `include` etc.).
`-GNinja` enables the use of building with Ninja which is by far the fastest of all the options. 
`LLVM_ENABLE_ASSERTIONS=ON` is a very useful option for development with LLVM as it enables all assertions in LLVM for 
catching pre-condition violations.

### Building JLLVM using an LLVM installation

Building JLLVM afterwards is very straight forward. It is also a standard cmake project and can therefore also be opened
and worked on using any IDE supporting CMake. The only cmake option which has to be set manually is pointing it where 
one installed LLVM:
```shell
cmake -G Ninja -DCMAKE_PREFIX_PATH=/your/install/llvm-dir
```

## References

### Java Virtual Machine 17 specification:
https://docs.oracle.com/javase/specs/jvms/se17/html/index.html

Notable chapters:
* 2 talks about the interpretation of JVM datastructures and execution concepts such as the operand stack, 
  local variables, method frames etc.
* 4 describes the class file format.
* 5 describes the dynamic loading, linking and initialization.
* 6 contains precise descriptions of every op

### Java Native Interface 17 Specification:
https://docs.oracle.com/en/java/javase/17/docs/specs/jni/

Contains description of how Java types map to native types, how Java methods mangle to native methods and how native 
methods can interact with a JVM.

### LLVM IR Reference
https://llvm.org/docs/LangRef.html

This contains the reference for all instructions available in LLVM and very precisely describes the structure and 
semantics of all of LLVM IR. 

It notably does not contain anything about using LLVM as a library. LLVM is luckily a rather typical C++ project with 
high discoverability of code one is looking for. There are also plenty of examples.

To get the basics of LLVMs C++ API for emitting LLVM IR one can take a look at the Kaleidoscope tutorial:
https://llvm.org/docs/tutorial/MyFirstLanguageFrontend/index.html


