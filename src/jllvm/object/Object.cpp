#include "Object.hpp"

#include "ClassObject.hpp"

bool jllvm::ObjectInterface::instanceOf(const ClassObject* classObject) const
{
    return getClass()->wouldBeInstanceOf(classObject);
}
