#include "NativeImplementation.hpp"

#include <jllvm/vm/native/JDK.hpp>
#include <jllvm/vm/native/Lang.hpp>

void jllvm::registerJavaClasses(VirtualMachine& virtualMachine)
{
    using namespace lang;
    using namespace jdk;

    addModels<ObjectModel, ClassModel, ThrowableModel, FloatModel, DoubleModel, SystemModel, ReflectionModel, CDSModel,
              UnsafeModel>(virtualMachine);
}
