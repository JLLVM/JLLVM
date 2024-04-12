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

#include "JNIImplementation.hpp"

#include "VirtualMachine.hpp"

jllvm::VirtualMachine& jllvm::virtualMachineFromJNIEnv(JNIEnv* env)
{
    return *reinterpret_cast<jllvm::VirtualMachine*>(env->functions->reserved0);
}

jllvm::VirtualMachine::JNINativeInterfaceUPtr jllvm::VirtualMachine::createJNIEnvironment()
{
    auto* result = new JNINativeInterface_{};
    result->reserved0 = reinterpret_cast<void*>(this);

    result->GetVersion = +[](JNIEnv*) -> jint { return JNI_VERSION_10; };
    result->FindClass = +[](JNIEnv* env, const char* name) -> jclass
    {
        VirtualMachine& virtualMachine = virtualMachineFromJNIEnv(env);
        ClassObject& classObject = virtualMachine.getClassLoader().forName(FieldType::fromMangled(name));
        return llvm::bit_cast<jclass>(virtualMachine.getGC().root(&classObject).release());
    };

    return JNINativeInterfaceUPtr(result, +[](JNINativeInterface_* ptr) { delete ptr; });
}
