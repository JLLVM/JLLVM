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

#include "JNIBridge.hpp"

#include <unwind.h>

#include "VirtualMachine.hpp"

// NOLINTNEXTLINE(*-reserved-identifier, *-identifier-naming): Name standardized by the Itanium ABI.
extern "C" int __gxx_personality_v0(...);

jllvm::JNIBridge::JNIBridge(VirtualMachine& virtualMachine, void* jniEnv)
    : m_virtualMachine(virtualMachine),
      m_jniSymbols(virtualMachine.getRuntime().getSession().createBareJITDylib("<jniSymbols>")),
      m_interpreter2JNISymbols(virtualMachine.getRuntime().getSession().createBareJITDylib("<interpreter2jni>")),
      m_jniImplementationLayer(virtualMachine.getRuntime().getSession(), virtualMachine.getRuntime().getInterner(),
                               virtualMachine.getRuntime().getLLVMIRLayer(),
                               virtualMachine.getRuntime().getDataLayout(), jniEnv)
{
    GarbageCollector& gc = virtualMachine.getGC();
    virtualMachine.getRuntime().addImplementationSymbols(
        m_jniSymbols, std::pair{"jllvm_new_local_root", [&](Object* object) { return gc.root(object).release(); }},
        std::pair{"jllvm_throw", [&](Throwable* exception) { virtualMachine.throwJavaException(exception); }},
        std::pair{"jllvm_throw_unsatisfied_link_error",
                  [&](Method* method)
                  {
                      String* string = virtualMachine.getStringInterner().intern(method->prettySignature());
                      virtualMachine.throwException("Ljava/lang/UnsatisfiedLinkError;", "(Ljava/lang/String;)V",
                                                    string);
                  }},
        std::pair{"jllvm_push_local_frame", [&] { gc.pushLocalFrame(); }},
        std::pair{"jllvm_pop_local_frame", [&] { gc.popLocalFrame(); }},
        std::pair{"__gxx_personality_v0", &__gxx_personality_v0}, std::pair{"_Unwind_Resume", &_Unwind_Resume});

    m_jniSymbols.addToLinkOrder(virtualMachine.getRuntime().getClassAndMethodObjectsDylib());
    m_jniSymbols.addToLinkOrder(virtualMachine.getRuntime().getCLibDylib());
}

void jllvm::JNIBridge::add(const Method& method)
{
    llvm::cantFail(m_jniImplementationLayer.add(m_jniSymbols, &method));
    llvm::cantFail(
        m_virtualMachine.getRuntime().getInterpreter2JITLayer().add(m_interpreter2JNISymbols, method, getJITCCDylib()));
}
