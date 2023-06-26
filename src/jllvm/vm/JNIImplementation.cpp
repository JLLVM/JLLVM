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

    return JNINativeInterfaceUPtr(
        result, +[](void* ptr) { delete reinterpret_cast<JNINativeInterface_*>(ptr); });
}
