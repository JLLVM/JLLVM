#include "Lang.hpp"

void jllvm::lang::SystemModel::arraycopy(jllvm::VirtualMachine&, jllvm::GCRootRef<jllvm::ClassObject>,
                                         jllvm::GCRootRef<Object> src, std::int32_t srcPos,
                                         jllvm::GCRootRef<Object> dest, std::int32_t destPos, std::int32_t length)
{
    if (!src || !dest)
    {
        // TODO: throw NullPointerException
        llvm_unreachable("Not yet implemented");
    }
    const ClassObject* srcClass = src->getClass();
    const ClassObject* destClass = dest->getClass();

    if (!srcClass->isArray() || !destClass->isArray())
    {
        // TODO: throw ArrayStoreException
        llvm_unreachable("Not yet implemented");
    }
    const ClassObject* srcComp = srcClass->getComponentType();
    const ClassObject* destComp = destClass->getComponentType();

    if (srcComp->isPrimitive() != destComp->isPrimitive())
    {
        // TODO: throw ArrayStoreException
        llvm_unreachable("Not yet implemented");
    }

    if (srcComp->isPrimitive() && destComp->isPrimitive() && srcComp != destComp)
    {
        // TODO: throw ArrayStoreException
        llvm_unreachable("Not yet implemented");
    }

    auto srcArr = static_cast<GCRootRef<Array<>>>(src);
    auto destArr = static_cast<GCRootRef<Array<>>>(dest);

    if (srcPos < 0)
    {
        // TODO: throw IndexOutOfBoundsException
        llvm_unreachable("Not yet implemented");
    }
    if (destPos < 0)
    {
        // TODO: throw IndexOutOfBoundsException
        llvm_unreachable("Not yet implemented");
    }
    if (length < 0)
    {
        // TODO: throw IndexOutOfBoundsException
        llvm_unreachable("Not yet implemented");
    }

    if (srcPos + length > srcArr->size())
    {
        // TODO: throw IndexOutOfBoundsException
        llvm_unreachable("Not yet implemented");
    }
    if (destPos + length > destArr->size())
    {
        // TODO: throw IndexOutOfBoundsException
        llvm_unreachable("Not yet implemented");
    }

    if (srcComp->isPrimitive() || srcComp->wouldBeInstanceOf(destComp))
    {
        auto* srcBytes = reinterpret_cast<char*>(src.address()) + srcClass->getInstanceSize();
        auto* destBytes = reinterpret_cast<char*>(dest.address()) + destClass->getInstanceSize();
        std::uint32_t instanceSize = srcComp->isPrimitive() ? srcComp->getInstanceSize() : sizeof(Object*);

        std::memmove(destBytes + destPos * instanceSize, srcBytes + srcPos * instanceSize, length * instanceSize);
        return;
    }

    for (Object* object : llvm::ArrayRef<Object*>{srcArr->begin(), srcArr->end()}.slice(srcPos, length))
    {
        if (object && !object->instanceOf(destComp))
        {
            // TODO: Throw ArrayStoreException
            llvm_unreachable("Not yet implemented");
        }
        (*destArr)[destPos++] = object;
    }
}
