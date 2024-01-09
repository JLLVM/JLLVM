// Copyright (C) 2023 The JLLVM Contributors.
//
// This file is part of JLLVM.
//
// JLLVM is free software; you can redistribute it and/or modify it under  the terms of the GNU General Public License
// as published by the Free Software Foundation; either version 3, or (at your option) any later version.
//
// JLLVM is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty
// of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License along with JLLVM; see the file LICENSE.txt.  If not
// see <http://www.gnu.org/licenses/>.

#include "Lang.hpp"

#include <llvm/Support/Endian.h>

jllvm::ObjectInterface* jllvm::lang::ObjectModel::clone()
{
    const ClassObject* thisClass = javaThis->getClass();
    ClassLoader& classLoader = virtualMachine.getClassLoader();
    GarbageCollector& garbageCollector = virtualMachine.getGC();

    auto arrayClone = [&]<class T>(T) -> ObjectInterface*
    {
        auto original = static_cast<GCRootRef<Array<T>>>(javaThis);
        auto* clone = garbageCollector.allocate<Array<T>>(original->getClass(), original->size());
        llvm::copy(*original, clone->begin());

        return clone;
    };

    if (thisClass->isArray())
    {
        return selectForJVMType(thisClass->getComponentType()->getDescriptor(), arrayClone);
    }

    if (thisClass->wouldBeInstanceOf(&classLoader.forName("Ljava/lang/Cloneable;")))
    {
        Object* clone = garbageCollector.allocate(thisClass);
        std::memcpy(reinterpret_cast<char*>(clone) + sizeof(ObjectHeader),
                    reinterpret_cast<char*>(javaThis.address()) + sizeof(ObjectHeader), thisClass->getFieldAreaSize());
        return clone;
    }

    String* string = virtualMachine.getStringInterner().intern(thisClass->getClassName());
    virtualMachine.throwException("Ljava/lang/CloneNotSupportedException;", "(Ljava/lang/String;)V", string);
}

void jllvm::lang::SystemModel::arraycopy(VirtualMachine& vm, GCRootRef<ClassObject>, GCRootRef<Object> src,
                                         std::int32_t srcPos, GCRootRef<Object> dest, std::int32_t destPos,
                                         std::int32_t length)
{
    if (!src || !dest)
    {
        vm.throwNullPointerException();
    }
    const ClassObject* srcClass = src->getClass();
    const ClassObject* destClass = dest->getClass();

    auto throwArrayStoreException = [&](const auto& message)
    {
        String* string = vm.getStringInterner().intern(message.str());
        vm.throwException("Ljava/lang/ArrayStoreException;", "(Ljava/lang/String;)V", string);
    };

    if (!srcClass->isArray())
    {
        std::string name = srcClass->getDescriptor().pretty();
        throwArrayStoreException(llvm::formatv("arraycopy: source type {0} is not an array", name));
    }

    if (!destClass->isArray())
    {
        std::string name = destClass->getDescriptor().pretty();
        throwArrayStoreException(llvm::formatv("arraycopy: destination type {0} is not an array", name));
    }

    const ClassObject* srcComp = srcClass->getComponentType();
    const ClassObject* destComp = destClass->getComponentType();

    if (srcComp->isPrimitive() != destComp->isPrimitive())
    {
        std::string fromName = srcComp->isPrimitive() ? srcClass->getDescriptor().pretty() : "object array[]";
        std::string toName = destComp->isPrimitive() ? destClass->getDescriptor().pretty() : "object array[]";
        throwArrayStoreException(
            llvm::formatv("arraycopy: type mismatch: can not copy {0} into {1}", fromName, toName));
    }

    if (srcComp->isPrimitive() && destComp->isPrimitive() && srcComp != destComp)
    {
        std::string fromName = srcClass->getDescriptor().pretty();
        std::string toName = destClass->getDescriptor().pretty();
        throwArrayStoreException(
            llvm::formatv("arraycopy: type mismatch: can not copy {0} into {1}", fromName, toName));
    }

    auto srcArr = static_cast<GCRootRef<Array<>>>(src);
    auto destArr = static_cast<GCRootRef<Array<>>>(dest);

    auto formatComponentType = [](const ClassObject* type)
    { return type->isPrimitive() ? type->getDescriptor().pretty() : "object array"; };

    auto throwIndexOutOfBounds = [&](const auto& message)
    {
        String* string = vm.getStringInterner().intern(message.str());
        vm.throwException("Ljava/lang/ArrayIndexOutOfBoundsException;", "(Ljava/lang/String;)V", string);
    };

    if (srcPos < 0)
    {
        throwIndexOutOfBounds(llvm::formatv("arraycopy: source index {0} out of bounds for {1}[{2}]", srcPos,
                                            formatComponentType(srcComp), srcArr->size()));
    }
    if (destPos < 0)
    {
        throwIndexOutOfBounds(llvm::formatv("arraycopy: destination index {0} out of bounds for {1}[{2}]", destPos,
                                            formatComponentType(destComp), destArr->size()));
    }
    if (length < 0)
    {
        throwIndexOutOfBounds(llvm::formatv("arraycopy: length {0} is negative", length));
    }

    if (srcPos + length > srcArr->size())
    {
        throwIndexOutOfBounds(llvm::formatv("arraycopy: last source index {0} out of bounds for {1}[{2}]",
                                            srcPos + length, formatComponentType(srcComp), srcArr->size()));
    }
    if (destPos + length > destArr->size())
    {
        throwIndexOutOfBounds(llvm::formatv("arraycopy: last destination index {0} out of bounds for {1}[{2}]",
                                            destPos + length, formatComponentType(destComp), destArr->size()));
    }

    if (srcComp->isPrimitive() || srcComp->wouldBeInstanceOf(destComp))
    {
        auto* srcBytes = reinterpret_cast<char*>(src.address()) + srcClass->getInstanceSize();
        auto* destBytes = reinterpret_cast<char*>(dest.address()) + destClass->getInstanceSize();
        std::uint32_t instanceSize = srcComp->isPrimitive() ? srcComp->getInstanceSize() : sizeof(Object*);

        std::memmove(destBytes + destPos * instanceSize, srcBytes + srcPos * instanceSize, length * instanceSize);
        return;
    }

    for (ObjectInterface* object :
         llvm::ArrayRef<ObjectInterface*>{srcArr->begin(), srcArr->end()}.slice(srcPos, length))
    {
        if (object && !object->instanceOf(destComp))
        {
            std::string fromName = srcClass->getDescriptor().pretty();
            std::string toName = destComp->getDescriptor().pretty();
            throwArrayStoreException(llvm::formatv(
                "arraycopy: element type mismatch: can not cast one of the elements of {0} to the type of the destination array, {1}",
                fromName, toName));
        }
        (*destArr)[destPos++] = object;
    }
}

bool jllvm::lang::StringUTF16Model::isBigEndian(GCRootRef<ClassObject>)
{
    return llvm::sys::IsBigEndianHost;
}
