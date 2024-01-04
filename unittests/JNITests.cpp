// Copyright (C) 2024 The JLLVM Contributors.
//
// This file is part of JLLVM.
//
// JLLVM is free software; you can redistribute it and/or modify it under  the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 3, or (at your option) any later version.
//
// JLLVM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with JLLVM; see the file LICENSE.txt.  If not
// see <http://www.gnu.org/licenses/>.

#include <catch2/catch_test_macros.hpp>

#include <llvm/Support/Path.h>

#include <jllvm/vm/VirtualMachine.hpp>

#include <jni.h>

using namespace jllvm;

namespace
{
class VirtualMachineFixture
{
protected:
    VirtualMachine virtualMachine;
    JNIEnv jniEnv;

public:
    VirtualMachineFixture()
        : virtualMachine(VirtualMachine::create(
              []
              {
                  BootOptions bootOptions;
                  bootOptions.classPath = {JAVA_BASE_PATH, INPUTS_BASE_PATH};
                  bootOptions.systemInitialization = false;
                  bootOptions.javaHome = llvm::sys::path::parent_path(llvm::sys::path::parent_path(JAVA_BASE_PATH));
                  return bootOptions;
              }())),
          jniEnv(virtualMachine.getJNINativeInterface())
    {
    }
};
} // namespace

TEST_CASE_METHOD(VirtualMachineFixture, "JNI Get Version", "[JNI]")
{
    CHECK(jniEnv.GetVersion() == JNI_VERSION_10);
}

TEST_CASE_METHOD(VirtualMachineFixture, "JNI FindClass", "[JNI]")
{
    CHECK(jniEnv.FindClass("TestSimpleJNI") != nullptr);
}
