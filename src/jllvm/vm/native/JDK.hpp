
#pragma once

#include <jllvm/vm/NativeImplementation.hpp>

/// Model implementations for all JDK classes in a 'jdk/internal/*' package.
namespace jllvm::jdk
{

class ReflectionModel : public ModelBase<>
{
public:
    using Base::Base;

    static const ClassObject* getCallerClass(VirtualMachine& virtualMachine, GCRootRef<ClassObject> classObject);

    constexpr static llvm::StringLiteral className = "jdk/internal/reflect/Reflection";
    constexpr static auto methods = std::make_tuple(&ReflectionModel::getCallerClass);
};

class CDSModel : public ModelBase<>
{
public:
    using Base::Base;

    static bool isDumpingClassList0(VirtualMachine&, GCRootRef<ClassObject>)
    {
        return false;
    }

    static bool isDumpingArchive0(VirtualMachine&, GCRootRef<ClassObject>)
    {
        return false;
    }

    static bool isSharingEnabled0(VirtualMachine&, GCRootRef<ClassObject>)
    {
        return false;
    }

    static std::int64_t getRandomSeedForDumping(VirtualMachine&, GCRootRef<ClassObject>)
    {
        return 0;
    }

    static void initializeFromArchive(VirtualMachine&, GCRootRef<ClassObject>, GCRootRef<ClassObject>) {}

    constexpr static llvm::StringLiteral className = "jdk/internal/misc/CDS";
    constexpr static auto methods =
        std::make_tuple(&CDSModel::isDumpingClassList0, &CDSModel::isDumpingArchive0, &CDSModel::isSharingEnabled0,
                        &CDSModel::getRandomSeedForDumping, &CDSModel::initializeFromArchive);
};

class UnsafeModel : public ModelBase<>
{
    template <class T>
    bool compareAndSet(GCRootRef<Object> object, std::uint64_t offset, T expected, T desired)
    {
        // TODO: use C++20 std::atomic_ref instead of atomic builtins in the future.
        return __atomic_compare_exchange_n(reinterpret_cast<T*>(reinterpret_cast<char*>(object.address()) + offset),
                                           &expected, desired, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
    }

    template <class T>
    T getVolatile(GCRootRef<Object> object, std::uint64_t offset)
    {
        // TODO: use C++20 std::atomic_ref instead of atomic builtins in the future.
        return __atomic_load_n(reinterpret_cast<T*>(reinterpret_cast<char*>(object.address()) + offset),
                               __ATOMIC_SEQ_CST);
    }

    template <class T>
    void putVolatile(GCRootRef<Object> object, std::uint64_t offset, T value)
    {
        // TODO: use C++20 std::atomic_ref instead of atomic builtins in the future.
        __atomic_store_n(reinterpret_cast<T*>(reinterpret_cast<char*>(object.address()) + offset), value,
                         __ATOMIC_SEQ_CST);
    }

public:
    using Base::Base;

    static void registerNatives(VirtualMachine&, GCRootRef<ClassObject>) {}

    std::uint32_t arrayBaseOffset0(GCRootRef<ClassObject> arrayClass)
    {
        assert(arrayClass->isArray());
        const ClassObject* componentType = arrayClass->getComponentType();
        if (!componentType->isPrimitive())
        {
            return Array<>::arrayElementsOffset();
        }

        static llvm::DenseMap<llvm::StringRef, std::uint32_t> mapping = {
            {"Z", Array<bool>::arrayElementsOffset()},         {"C", Array<std::uint16_t>::arrayElementsOffset()},
            {"B", Array<std::int8_t>::arrayElementsOffset()},  {"S", Array<std::int16_t>::arrayElementsOffset()},
            {"I", Array<std::int32_t>::arrayElementsOffset()}, {"D", Array<double>::arrayElementsOffset()},
            {"F", Array<float>::arrayElementsOffset()},        {"L", Array<std::int64_t>::arrayElementsOffset()},
        };
        return mapping.lookup(componentType->getClassName());
    }

    std::uint32_t arrayIndexScale0(GCRootRef<ClassObject> arrayClass)
    {
        assert(arrayClass->isArray());
        const ClassObject* componentType = arrayClass->getComponentType();
        if (!componentType->isPrimitive())
        {
            return sizeof(Object*);
        }

        static llvm::DenseMap<llvm::StringRef, std::uint32_t> mapping = {
            {"Z", sizeof(bool)},         {"C", sizeof(std::uint16_t)}, {"B", sizeof(std::int8_t)},
            {"S", sizeof(std::int16_t)}, {"I", sizeof(std::int32_t)},  {"D", sizeof(double)},
            {"F", sizeof(float)},        {"L", sizeof(std::int64_t)},
        };
        return mapping.lookup(componentType->getClassName());
    }

    std::uint32_t objectFieldOffset1(GCRootRef<ClassObject> clazz, GCRootRef<String> fieldName)
    {
        std::string fieldNameStr = fieldName->toUTF8();
        for (const ClassObject* curr : clazz->getSuperClasses())
        {
            const Field* iter = llvm::find_if(curr->getFields(), [&](const Field& field)
                                              { return !field.isStatic() && field.getName() == fieldNameStr; });
            if (iter != curr->getFields().end())
            {
                return iter->getOffset();
            }
        }

        // TODO: throw InternalError
        return 0;
    }

    void storeFence()
    {
        std::atomic_thread_fence(std::memory_order_release);
    }

    void loadFence()
    {
        std::atomic_thread_fence(std::memory_order_acquire);
    }

    void fullFence()
    {
        std::atomic_thread_fence(std::memory_order_seq_cst);
    }

    bool compareAndSetByte(GCRootRef<Object> object, std::uint64_t offset, std::int8_t expected, std::int8_t desired)
    {
        return compareAndSet(object, offset, expected, desired);
    }

    bool compareAndSetShort(GCRootRef<Object> object, std::uint64_t offset, std::int16_t expected, std::int16_t desired)
    {
        return compareAndSet(object, offset, expected, desired);
    }

    bool compareAndSetChar(GCRootRef<Object> object, std::uint64_t offset, std::uint16_t expected,
                           std::uint16_t desired)
    {
        return compareAndSet(object, offset, expected, desired);
    }

    bool compareAndSetBoolean(GCRootRef<Object> object, std::uint64_t offset, bool expected, bool desired)
    {
        return compareAndSet(object, offset, expected, desired);
    }

    bool compareAndSetInt(GCRootRef<Object> object, std::uint64_t offset, std::int32_t expected, std::int32_t desired)
    {
        return compareAndSet(object, offset, expected, desired);
    }

    bool compareAndSetLong(GCRootRef<Object> object, std::uint64_t offset, std::int64_t expected, std::int64_t desired)
    {
        return compareAndSet(object, offset, expected, desired);
    }

    bool compareAndSetReference(GCRootRef<Object> object, std::uint64_t offset, GCRootRef<Object> expected,
                                GCRootRef<Object> desired)
    {
        return compareAndSet(object, offset, expected.address(), desired.address());
    }

    Object* getReferenceVolatile(GCRootRef<Object> object, std::uint64_t offset)
    {
        return getVolatile<Object*>(object, offset);
    }

    std::int32_t getIntVolatile(GCRootRef<Object> object, std::uint64_t offset)
    {
        return getVolatile<std::int32_t>(object, offset);
    }

    void putReferenceVolatile(GCRootRef<Object> object, std::uint64_t offset, GCRootRef<Object> value)
    {
        putVolatile(object, offset, value.address());
    }

    void putIntVolatile(GCRootRef<Object> object, std::uint64_t offset, std::int32_t value)
    {
        putVolatile(object, offset, value);
    }

    constexpr static llvm::StringLiteral className = "jdk/internal/misc/Unsafe";
    constexpr static auto methods = std::make_tuple(
        &UnsafeModel::registerNatives, &UnsafeModel::arrayBaseOffset0, &UnsafeModel::arrayIndexScale0,
        &UnsafeModel::objectFieldOffset1, &UnsafeModel::storeFence, &UnsafeModel::loadFence, &UnsafeModel::fullFence,
        &UnsafeModel::compareAndSetByte, &UnsafeModel::compareAndSetShort, &UnsafeModel::compareAndSetChar,
        &UnsafeModel::compareAndSetBoolean, &UnsafeModel::compareAndSetInt, &UnsafeModel::compareAndSetLong,
        &UnsafeModel::compareAndSetReference, &UnsafeModel::getIntVolatile, &UnsafeModel::getReferenceVolatile,
        &UnsafeModel::putIntVolatile, &UnsafeModel::putReferenceVolatile);
};

} // namespace jllvm::jdk
