#include "JDK.hpp"

#include <jllvm/unwind/Unwinder.hpp>

const jllvm::ClassObject* jllvm::jdk::ReflectionModel::getCallerClass(VirtualMachine& virtualMachine,
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
