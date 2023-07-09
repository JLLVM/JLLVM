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

#pragma once

#include <jllvm/vm/NativeImplementation.hpp>

#include <chrono>

/// Model implementations for all Java classes in a 'java/lang/*' package.
namespace jllvm::lang
{

/// Model implementation for the native methods of Javas 'Object' class.
class ObjectModel : public ModelBase<ObjectModel>
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

    void notifyAll()
    {
        // Noop while we are single threaded.
    }

    constexpr static llvm::StringLiteral className = "java/lang/Object";
    constexpr static auto methods =
        std::make_tuple(&ObjectModel::hashCode, &ObjectModel::getClass, &ObjectModel::notifyAll);
};

/// Model implementation for the native methods of Javas 'Class' class.
class ClassModel : public ModelBase<ClassModel, ClassObject>
{
public:
    using Base::Base;

    static void registerNatives(GCRootRef<ClassObject>)
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

    static bool desiredAssertionStatus0(GCRootRef<ClassObject>)
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

class FloatModel : public ModelBase<FloatModel>
{
public:
    using Base::Base;

    static std::uint32_t floatToRawIntBits(GCRootRef<ClassObject>, float value)
    {
        // TODO: Use std::bit_cast once supported by our C++20.
        std::uint32_t repr;
        std::memcpy(&repr, &value, sizeof(float));
        return repr;
    }

    static float intBitsToFloat(GCRootRef<ClassObject>, std::uint32_t value)
    {
        // TODO: Use std::bit_cast once supported by our C++20.
        float repr;
        std::memcpy(&repr, &value, sizeof(float));
        return repr;
    }

    constexpr static llvm::StringLiteral className = "java/lang/Float";
    constexpr static auto methods = std::make_tuple(&floatToRawIntBits, &intBitsToFloat);
};

class DoubleModel : public ModelBase<DoubleModel>
{
public:
    using Base::Base;

    static std::uint64_t doubleToRawLongBits(GCRootRef<ClassObject>, double value)
    {
        // TODO: Use std::bit_cast once supported by our C++20.
        std::uint64_t repr;
        std::memcpy(&repr, &value, sizeof(double));
        return repr;
    }

    static double longBitsToDouble(GCRootRef<ClassObject>, std::uint64_t value)
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
class ThrowableModel : public ModelBase<ThrowableModel, Throwable>
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

class SystemModel : public ModelBase<SystemModel>
{
public:
    using Base::Base;

    struct State
    {
        StaticFieldRef<Object*> in;
        StaticFieldRef<Object*> out;
        StaticFieldRef<Object*> err;
    };

    static void arraycopy(GCRootRef<ClassObject>, GCRootRef<Object> src, std::int32_t srcPos, GCRootRef<Object> dest,
                          std::int32_t destPos, std::int32_t length);

    static void registerNatives(State& state, GCRootRef<ClassObject> classObject)
    {
        state.in = classObject->getStaticField<Object*>("in", "Ljava/io/InputStream;");
        state.out = classObject->getStaticField<Object*>("out", "Ljava/io/PrintStream;");
        state.err = classObject->getStaticField<Object*>("err", "Ljava/io/PrintStream;");
    }

    static std::int64_t nanoTime(GCRootRef<ClassObject>)
    {
        auto now = std::chrono::high_resolution_clock::now();
        return std::chrono::time_point_cast<std::chrono::nanoseconds>(now).time_since_epoch().count();
    }

    static void setIn0(State& state, GCRootRef<ClassObject>, GCRootRef<Object> stream)
    {
        state.in() = stream;
    }

    static void setOut0(State& state, GCRootRef<ClassObject>, GCRootRef<Object> stream)
    {
        state.out() = stream;
    }

    static void setErr0(State& state, GCRootRef<ClassObject>, GCRootRef<Object> stream)
    {
        state.err() = stream;
    }

    constexpr static llvm::StringLiteral className = "java/lang/System";
    constexpr static auto methods =
        std::make_tuple(&SystemModel::registerNatives, &SystemModel::nanoTime, &SystemModel::arraycopy,
                        &SystemModel::setIn0, &SystemModel::setOut0, &SystemModel::setErr0);
};

class RuntimeModel : public ModelBase<RuntimeModel>
{
public:
    using Base::Base;

    static std::int64_t maxMemory(VirtualMachine& vm, GCRootRef<ClassObject>)
    {
        return vm.getGC().getHeapSize();
    }

    static std::int32_t availableProcessors(GCRootRef<ClassObject>)
    {
        return 1;
    }

    constexpr static llvm::StringLiteral className = "java/lang/Runtime";
    constexpr static auto methods = std::make_tuple(&RuntimeModel::maxMemory, &RuntimeModel::availableProcessors);
};

class ThreadModel : public ModelBase<ThreadModel>
{
public:
    using Base::Base;

    struct State
    {
        InstanceFieldRef<std::int32_t> priorityField;
    };

    static void registerNatives(State& state, GCRootRef<ClassObject> classObject)
    {
        state.priorityField = classObject->getInstanceField<std::int32_t>("priority", "I");
    }

    static GCRootRef<Object> currentThread(VirtualMachine& vm, GCRootRef<ClassObject>)
    {
        // Once we are multi threaded, this should actually the return the corresponding Java thread
        // this function is being called from. For now we are just returning the one and only thread for the time being.
        return vm.getMainThread();
    }

    void setPriority0(std::int32_t priority)
    {
        state().priorityField(javaThis) = priority;
    }

    constexpr static llvm::StringLiteral className = "java/lang/Thread";
    constexpr static auto methods =
        std::make_tuple(&ThreadModel::registerNatives, &ThreadModel::currentThread, &ThreadModel::setPriority0);
};

class ReferenceModel : public ModelBase<ReferenceModel, Reference>
{
public:
    using Base::Base;

    bool refersTo0(GCRootRef<Object> o)
    {
        return javaThis->referent == o;
    }

    constexpr static llvm::StringLiteral className = "java/lang/ref/Reference";
    constexpr static auto methods = std::make_tuple(&ReferenceModel::refersTo0);
};

class StringUTF16Model : public ModelBase<StringUTF16Model>
{
public:

    using Base::Base;

    static bool isBigEndian(GCRootRef<ClassObject>);

    constexpr static llvm::StringLiteral className = "java/lang/StringUTF16";
    constexpr static auto methods = std::make_tuple(&StringUTF16Model::isBigEndian);
};

} // namespace jllvm::lang
