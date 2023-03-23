
#include <jni.h>

#include "VirtualMachine.hpp"

jllvm::VirtualMachine::JNINativeInterfaceUPtr jllvm::VirtualMachine::createJNIEnvironment()
{
    auto* result = new JNINativeInterface_{};
    result->reserved0 = reinterpret_cast<void*>(this);

    return JNINativeInterfaceUPtr(
        result, +[](void* ptr) { delete reinterpret_cast<JNINativeInterface_*>(ptr); });
}
