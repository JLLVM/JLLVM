#include "Lang.hpp"
void jllvm::lang::SystemModel::arraycopy(jllvm::VirtualMachine&, jllvm::GCRootRef<jllvm::ClassObject>,
                                         jllvm::GCRootRef<Object> src, std::int32_t srcPos,
                                         jllvm::GCRootRef<Object> dest, std::int32_t destPos, std::int32_t length)
{
    if (!src || !dest)
    {
        llvm_unreachable("Not yet implemented"); // TODO throw NullPointerException
    }
    const ClassObject* srcClass = src->getClass();
    const ClassObject* destClass = dest->getClass();

    if (!srcClass->isArray() || !destClass->isArray())
    {
        llvm_unreachable("Not yet implemented"); // TODO throw ArrayStoreException
    }
    const ClassObject* srcComp = srcClass->getComponentType();
    const ClassObject* destComp = destClass->getComponentType();

    if (srcComp->isPrimitive() != destComp->isPrimitive())
    {
        llvm_unreachable("Not yet implemented"); // TODO throw ArrayStoreException
    }

    if (srcComp->isPrimitive() && destComp->isPrimitive() && srcComp != destComp)
    {
        llvm_unreachable("Not yet implemented"); // TODO throw ArrayStoreException
    }

    auto srcArr = static_cast<GCRootRef<Array<>>>(src);
    auto destArr = static_cast<GCRootRef<Array<>>>(dest);

    if (srcPos < 0)
    {
        llvm_unreachable("Not yet implemented"); // TODO throw IndexOutOfBoundsException
    }
    if (destPos < 0)
    {
        llvm_unreachable("Not yet implemented"); // TODO throw IndexOutOfBoundsException
    }
    if (length < 0)
    {
        llvm_unreachable("Not yet implemented"); // TODO throw IndexOutOfBoundsException
    }

    if (srcPos + length > srcArr->size())
    {
        llvm_unreachable("Not yet implemented"); // TODO throw IndexOutOfBoundsException
    }
    if (destPos + length > destArr->size())
    {
        llvm_unreachable("Not yet implemented"); // TODO throw IndexOutOfBoundsException
    }

    if (srcComp->isPrimitive() || srcComp->wouldBeInstanceOf(destComp))
    {
        auto* srcBytes = reinterpret_cast<char*>(src.address()) + srcClass->getInstanceSize();
        auto* destBytes = reinterpret_cast<char*>(dest.address()) + destClass->getInstanceSize();
        std::uint32_t instanceSize = srcComp->isPrimitive() ? srcComp->getInstanceSize() : sizeof(Object*);

        std::memmove(destBytes + destPos * instanceSize, srcBytes + srcPos * instanceSize, length * instanceSize);
    }
    else
    {
        for (Object* object : llvm::ArrayRef<Object*>{srcArr->begin(), srcArr->end()}.slice(srcPos, length))
        {
            if (object && !object->instanceOf(destComp))
            {
                llvm_unreachable("Not yet implemented"); // TODO: Throw ArrayStoreException
            }
            (*destArr)[destPos++] = object;
        }
    }
}
