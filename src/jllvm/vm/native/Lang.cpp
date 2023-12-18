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

    auto arrayClone = [&]<class T = Object*>(T = {})->ObjectInterface*
    {
        auto original = static_cast<GCRootRef<Array<T>>>(javaThis);
        auto* clone = garbageCollector.allocate<Array<T>>(original->getClass(), original->size());
        llvm::copy(*original, clone->begin());

        return clone;
    };

    if (thisClass->isArray())
    {
        return match(
            thisClass->getComponentType()->getDescriptor(),
            [&](BaseType baseType)
            {
                switch (baseType.getValue())
                {
                    case BaseType::Boolean:
                    case BaseType::Char:
                    case BaseType::Byte:
                    case BaseType::Short:
                    case BaseType::Int: return arrayClone(std::int32_t{});
                    case BaseType::Float: return arrayClone(float{});
                    case BaseType::Double: return arrayClone(double{});
                    case BaseType::Long: return arrayClone(std::int64_t{});
                    case BaseType::Void: break;
                }
                llvm_unreachable("void parameter is not possible");
            },
            [&](auto) { return arrayClone(); });
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
        vm.throwException("Ljava/lang/NullPointerException;", "()V");
    }
    const ClassObject* srcClass = src->getClass();
    const ClassObject* destClass = dest->getClass();

    if (!srcClass->isArray())
    {
        std::string name = srcClass->getDescriptor().pretty();
        String* string =
            vm.getStringInterner().intern(llvm::formatv("arraycopy: source type {0} is not an array", name).str());
        vm.throwException("Ljava/lang/ArrayStoreException;", "(Ljava/lang/String;)V", string);
    }

    if (!destClass->isArray())
    {
        std::string name = destClass->getDescriptor().pretty();
        String* string =
            vm.getStringInterner().intern(llvm::formatv("arraycopy: destination type {0} is not an array", name).str());
        vm.throwException("Ljava/lang/ArrayStoreException;", "(Ljava/lang/String;)V", string);
    }

    const ClassObject* srcComp = srcClass->getComponentType();
    const ClassObject* destComp = destClass->getComponentType();

    if (srcComp->isPrimitive() != destComp->isPrimitive())
    {
        std::string fromName = srcComp->isPrimitive() ? srcClass->getDescriptor().pretty() : "object array[]";
        std::string toName = destComp->isPrimitive() ? destClass->getDescriptor().pretty() : "object array[]";
        String* string = vm.getStringInterner().intern(
            llvm::formatv("arraycopy: type mismatch: can not copy {0} into {1}", fromName, toName).str());
        vm.throwException("Ljava/lang/ArrayStoreException;", "(Ljava/lang/String;)V", string);
    }

    if (srcComp->isPrimitive() && destComp->isPrimitive() && srcComp != destComp)
    {
        std::string fromName = srcClass->getDescriptor().pretty();
        std::string toName = destClass->getDescriptor().pretty();
        String* string = vm.getStringInterner().intern(
            llvm::formatv("arraycopy: type mismatch: can not copy {0} into {1}", fromName, toName).str());
        vm.throwException("Ljava/lang/ArrayStoreException;", "(Ljava/lang/String;)V", string);
    }

    auto srcArr = static_cast<GCRootRef<Array<>>>(src);
    auto destArr = static_cast<GCRootRef<Array<>>>(dest);

    if (srcPos < 0)
    {
        std::string elemName = srcComp->isPrimitive() ? srcComp->getDescriptor().pretty() : "object array";
        String* string = vm.getStringInterner().intern(
            llvm::formatv("arraycopy: source index {0} out of bounds for {1}[{2}]", srcPos, elemName, srcArr->size())
                .str());
        vm.throwException("Ljava/lang/ArrayIndexOutOfBoundsException;", "(Ljava/lang/String;)V", string);
    }
    if (destPos < 0)
    {
        std::string elemName = destComp->isPrimitive() ? destComp->getDescriptor().pretty() : "object array";
        String* string = vm.getStringInterner().intern(
            llvm::formatv("arraycopy: destination index {0} out of bounds for {1}[{2}]", destPos, elemName, destArr->size())
                .str());
        vm.throwException("Ljava/lang/ArrayIndexOutOfBoundsException;", "(Ljava/lang/String;)V", string);
    }
    if (length < 0)
    {
        String* string =
            vm.getStringInterner().intern(llvm::formatv("arraycopy: length {0} is negative", length).str());
        vm.throwException("Ljava/lang/ArrayIndexOutOfBoundsException;", "(Ljava/lang/String;)V", string);
    }

    if (srcPos + length > srcArr->size())
    {
        std::string elemName = srcComp->isPrimitive() ? srcComp->getDescriptor().pretty() : "object array";
        String* string =
            vm.getStringInterner().intern(llvm::formatv("arraycopy: last source index {0} out of bounds for {1}[{2}]",
                                                        srcPos + length, elemName, srcArr->size())
                                              .str());
        vm.throwException("Ljava/lang/ArrayIndexOutOfBoundsException;", "(Ljava/lang/String;)V", string);
    }
    if (destPos + length > destArr->size())
    {
        std::string elemName = destComp->isPrimitive() ? destComp->getDescriptor().pretty() : "object array";
        String* string =
            vm.getStringInterner().intern(llvm::formatv("arraycopy: last destination index {0} out of bounds for {1}[{2}]",
                                                        destPos + length, elemName, destArr->size())
                                              .str());
        vm.throwException("Ljava/lang/ArrayIndexOutOfBoundsException;", "(Ljava/lang/String;)V", string);
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
            String* string = vm.getStringInterner().intern(
                llvm::formatv(
                    "arraycopy: element type mismatch: can not cast one of the elements of {0} to the type of the destination array, {1}",
                    fromName, toName)
                    .str());
            vm.throwException("Ljava/lang/ArrayStoreException;", "(Ljava/lang/String;)V", string);
        }
        (*destArr)[destPos++] = object;
    }
}

bool jllvm::lang::StringUTF16Model::isBigEndian(GCRootRef<ClassObject>)
{
    return llvm::sys::IsBigEndianHost;
}
