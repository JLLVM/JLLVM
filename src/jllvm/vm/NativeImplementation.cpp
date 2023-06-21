#include "NativeImplementation.hpp"

#include <jllvm/unwind/Unwinder.hpp>

jllvm::VirtualMachine& jllvm::detail::virtualMachineFromJNIEnv(JNIEnv* env)
{
    return *reinterpret_cast<jllvm::VirtualMachine*>(env->functions->reserved0);
}

void jllvm::registerJavaClasses(VirtualMachine& virtualMachine)
{
    addModels<ObjectModel, ClassModel, ThrowableModel, FloatModel, DoubleModel, SystemModel, ReflectionModel, CDSModel>(
        virtualMachine);
}

const jllvm::ClassObject* jllvm::ReflectionModel::getCallerClass(VirtualMachine& virtualMachine,
                                                                 GCRootRef<ClassObject> classObject)
{
    const ClassObject* result = nullptr;
    unwindStack(
        [&](UnwindFrame frame)
        {
            std::uintptr_t fp = frame.getFunctionPointer();
            std::optional<JavaMethodMetadata> data = virtualMachine.getJIT().getJavaMethodMetadata(fp);
            if (!data)
            {
                return UnwindAction::ContinueUnwinding;
            }

            if (data->classObject == classObject)
            {
                return UnwindAction::ContinueUnwinding;
            }
            // TODO: If the method has the Java annotation '@CallerSensitive' it should be skipped by this method.
            result = data->classObject;
            return UnwindAction::StopUnwinding;
        });
    return result;
}
