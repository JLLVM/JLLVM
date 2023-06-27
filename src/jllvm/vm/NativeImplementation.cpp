#include "NativeImplementation.hpp"

#include <jllvm/vm/native/JDK.hpp>
#include <jllvm/vm/native/Lang.hpp>
#include <jllvm/vm/native/Security.hpp>

void jllvm::registerJavaClasses(VirtualMachine& virtualMachine)
{
    using namespace lang;
    using namespace jdk;
    using namespace security;

    addModels<ObjectModel, ClassModel, ThrowableModel, FloatModel, DoubleModel, SystemModel, ReflectionModel, CDSModel,
              UnsafeModel, ThreadModel, AccessControllerModel, VMModel>(virtualMachine);
}
