#include "NativeImplementation.hpp"

jllvm::VirtualMachine& jllvm::detail::virtualMachineFromJNIEnv(JNIEnv* env)
{
    return *reinterpret_cast<jllvm::VirtualMachine*>(env->functions->reserved0);
}

void jllvm::registerJavaClasses(VirtualMachine& virtualMachine)
{
    addModel<ObjectModel>(virtualMachine);
}
