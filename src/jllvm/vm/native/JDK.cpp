// Copyright (C) 2023 The JLLVM Contributors.
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

#include "JDK.hpp"

#include <llvm/Support/Path.h>

#include <jllvm/unwind/Unwinder.hpp>

#include <csignal>

const jllvm::ClassObject* jllvm::jdk::ReflectionModel::getCallerClass(VirtualMachine& virtualMachine,
                                                                      GCRootRef<ClassObject> classObject)
{
    const ClassObject* result = nullptr;
    virtualMachine.unwindJavaStack(
        [&](JavaFrame frame)
        {
            if (frame.getClassObject() == classObject)
            {
                return UnwindAction::ContinueUnwinding;
            }
            // TODO: If the method has the Java annotation '@CallerSensitive' it should be skipped by this method.
            result = frame.getClassObject();
            return UnwindAction::StopUnwinding;
        });
    return result;
}

jllvm::Array<jllvm::String*>* jllvm::jdk::SystemPropsRawModel::platformProperties(jllvm::VirtualMachine& vm,
                                                                                  jllvm::GCRootRef<jllvm::ClassObject>)
{
    auto& array =
        *vm.getGC().allocate<Array<String*>>(&vm.getClassLoader().forName("[Ljava/lang/String;"), FixedLength);

    llvm::SmallString<64> temp;
    llvm::sys::path::system_temp_directory(true, temp);
    array[JavaIoTmpdirNdx] = vm.getStringInterner().intern(temp);
#ifdef _WIN32
    array[LineSeparatorNdx] = vm.getStringInterner().intern("\n\r");
    array[PathSeparatorNdx] = vm.getStringInterner().intern(";");
    array[FileSeparatorNdx] = vm.getStringInterner().intern("\\");
#else
    array[LineSeparatorNdx] = vm.getStringInterner().intern("\n");
    array[PathSeparatorNdx] = vm.getStringInterner().intern(":");
    array[FileSeparatorNdx] = vm.getStringInterner().intern("/");
#endif
    llvm::sys::path::home_directory(temp);
    array[UserHomeNdx] = vm.getStringInterner().intern(temp);
    // TODO: This is the same as home on Linux, no clue about other OSs. Figure this out.
    array[UserDirNdx] = vm.getStringInterner().intern(temp);
    // TODO: Insert username here.
    array[UserNameNdx] = vm.getStringInterner().intern("");
    array[FileEncodingNdx] = vm.getStringInterner().intern("UTF-8");

    return &array;
}

std::int32_t jllvm::jdk::SignalModel::findSignal0(GCRootRef<ClassObject>, GCRootRef<String> sigName)
{
    std::string utf8 = sigName->toUTF8();
    static llvm::DenseMap<llvm::StringRef, std::int32_t> mapping = {
        {"ABRT", SIGABRT}, {"FPE", SIGFPE},   {"ILL", SIGILL}, {"INT", SIGINT},
        {"SEGV", SIGSEGV}, {"TERM", SIGTERM}, {"HUP", SIGHUP},
    };
    auto iter = mapping.find(utf8);
    if (iter == mapping.end())
    {
        return -1;
    }
    return iter->second;
}
