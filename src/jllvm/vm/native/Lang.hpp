#pragma once

#include <jllvm/vm/NativeImplementation.hpp>

#include <chrono>

/// Model implementations for all Java classes in a 'java/lang/*' package.
namespace jllvm::lang
{

/// Model implementation for the native methods of Javas 'Object' class.
class ObjectModel : public ModelBase<>
{
public:
    using Base::Base;

    const ClassObject* getClass()
    {
        return javaThis->getClass();
    }

    std::int32_t hashCode()
    {
        std::int32_t& hashCode = javaThis->getObjectHeader().hashCode;
        if (!hashCode)
        {
            hashCode = virtualMachine.createNewHashCode();
        }
        return hashCode;
    }

    constexpr static llvm::StringLiteral className = "java/lang/Object";
    constexpr static auto methods = std::make_tuple(&ObjectModel::hashCode, &ObjectModel::getClass);
};

/// Model implementation for the native methods of Javas 'Class' class.
class ClassModel : public ModelBase<ClassObject>
{
public:
    using Base::Base;

    static void registerNatives(VirtualMachine&, GCRootRef<ClassObject>)
    {
        // Noop until (if?) we need some C++ initialization code.
    }

    static const ClassObject* getPrimitiveClass(VirtualMachine& vm, GCRootRef<ClassObject>, GCRootRef<String> string)
    {
        static llvm::DenseMap<llvm::StringRef, char> mapping = {
            {"boolean", 'Z'}, {"char", 'C'},  {"byte", 'B'}, {"short", 'S'}, {"int", 'I'},
            {"double", 'D'},  {"float", 'F'}, {"void", 'V'}, {"long", 'J'},
        };
        std::string utf8 = string->toUTF8();
        char c = mapping.lookup(utf8);
        if (c == 0)
        {
            return nullptr;
        }
        return &vm.getClassLoader().forName(llvm::Twine(c));
    }

    bool isArray()
    {
        return javaThis->isArray();
    }

    bool isPrimitive()
    {
        return javaThis->isPrimitive();
    }

    static bool desiredAssertionStatus0(VirtualMachine&, GCRootRef<ClassObject>)
    {
#ifndef NDEBUG
        return true;
#else
        return false;
#endif
    }

    constexpr static llvm::StringLiteral className = "java/lang/Class";
    constexpr static auto methods =
        std::make_tuple(&ClassModel::registerNatives, &ClassModel::isArray, &ClassModel::desiredAssertionStatus0,
                        &ClassModel::getPrimitiveClass, &ClassModel::isPrimitive);
};

class FloatModel : public ModelBase<>
{
public:
    using Base::Base;

    static std::uint32_t floatToRawIntBits(VirtualMachine&, GCRootRef<ClassObject>, float value)
    {
        // TODO: Use std::bit_cast once supported by our C++20.
        std::uint32_t repr;
        std::memcpy(&repr, &value, sizeof(float));
        return repr;
    }

    static float intBitsToFloat(VirtualMachine&, GCRootRef<ClassObject>, std::uint32_t value)
    {
        // TODO: Use std::bit_cast once supported by our C++20.
        float repr;
        std::memcpy(&repr, &value, sizeof(float));
        return repr;
    }

    constexpr static llvm::StringLiteral className = "java/lang/Float";
    constexpr static auto methods = std::make_tuple(&floatToRawIntBits, &intBitsToFloat);
};

class DoubleModel : public ModelBase<>
{
public:
    using Base::Base;

    static std::uint64_t doubleToRawLongBits(VirtualMachine&, GCRootRef<ClassObject>, double value)
    {
        // TODO: Use std::bit_cast once supported by our C++20.
        std::uint64_t repr;
        std::memcpy(&repr, &value, sizeof(double));
        return repr;
    }

    static double longBitsToDouble(VirtualMachine&, GCRootRef<ClassObject>, std::uint64_t value)
    {
        // TODO: Use std::bit_cast once supported by our C++20.
        double repr;
        std::memcpy(&repr, &value, sizeof(double));
        return repr;
    }

    constexpr static llvm::StringLiteral className = "java/lang/Double";
    constexpr static auto methods = std::make_tuple(&doubleToRawLongBits, &longBitsToDouble);
};

/// Model implementation for the native methods of Javas 'Thowable' class.
class ThrowableModel : public ModelBase<Throwable>
{
public:
    using Base::Base;

    GCRootRef<Throwable> fillInStackTrace(int)
    {
        // TODO: Set backtrace and depth of 'javaThis'. See
        // https://github.com/openjdk/jdk/blob/4f096eb7c9066e5127d9ab8c1c893e991a23d316/src/hotspot/share/classfile/javaClasses.cpp#L2491
        return javaThis;
    }

    constexpr static llvm::StringLiteral className = "java/lang/Throwable";
    constexpr static auto methods = std::make_tuple(&ThrowableModel::fillInStackTrace);
};

class SystemModel : public ModelBase<>
{
public:
    using Base::Base;

    static void arraycopy(VirtualMachine&, GCRootRef<ClassObject>, GCRootRef<Object> src, std::int32_t srcPos,
                          GCRootRef<Object> dest, std::int32_t destPos, std::int32_t length);

    static void registerNatives(VirtualMachine&, GCRootRef<ClassObject>)
    {
        // Noop in our implementation.
    }

    static std::int64_t nanoTime(VirtualMachine&, GCRootRef<ClassObject>)
    {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::time_point_cast<std::chrono::nanoseconds>(now).time_since_epoch().count();
    }

    constexpr static llvm::StringLiteral className = "java/lang/System";
    constexpr static auto methods =
        std::make_tuple(&SystemModel::registerNatives, &SystemModel::nanoTime, &SystemModel::arraycopy);
};

class ThreadModel : public ModelBase<Thread>
{
public:
    using Base::Base;

    static void registerNatives(VirtualMachine&, GCRootRef<ClassObject>)
    {
        // Noop in our implementation.
    }

    static GCRootRef<Thread> currentThread(VirtualMachine& vm, GCRootRef<ClassObject>)
    {
        // Once we are multi threaded, this should actually the return the corresponding Java thread
        // this function is being called from. For now we are just returning the one and only thread for the time being.
        return vm.getMainThread();
    }

    void setPriority0(std::int32_t priority)
    {
        javaThis->priority = priority;
    }

    constexpr static llvm::StringLiteral className = "java/lang/Thread";
    constexpr static auto methods =
        std::make_tuple(&ThreadModel::registerNatives, &ThreadModel::currentThread, &ThreadModel::setPriority0);
};

} // namespace jllvm::lang
