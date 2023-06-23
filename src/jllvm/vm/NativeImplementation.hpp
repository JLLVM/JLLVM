#pragma once

#include <llvm/ADT/STLExtras.h>
#include <llvm/ADT/StringRef.h>

#include <memory>
#include <utility>

#include <jni.h>

#include "VirtualMachine.hpp"

namespace jllvm
{
/// Method used to add a (possibly static) member function to the 'method' tuple of a 'ModelBase' instance.
/// This uses an implementation defined trick to get the name of the member function and returns it together with the
/// function pointer.
template <auto fnPtr>
constexpr auto addMember()
{
    // GCC and Clang encode the typename here by appending at the back something similar to [fnPtr = Class::name]
    std::string_view demangledName = __PRETTY_FUNCTION__;
    std::size_t rstart = demangledName.rfind(']');
    assert(rstart != std::string_view::npos);
    // Skip over possible whitespace.
    while (rstart > 0 && demangledName[rstart - 1] == ' ')
    {
        rstart--;
    }

    // Last namespace qualifier from the back.
    constexpr std::string_view namespaceQual = "::";
    std::size_t lastNamespaceQual = demangledName.rfind(namespaceQual, rstart);
    assert(lastNamespaceQual != std::string_view::npos);

    demangledName = demangledName.substr(lastNamespaceQual + namespaceQual.size(),
                                         rstart - (lastNamespaceQual + namespaceQual.size()));
    return std::pair{fnPtr, demangledName};
}

/// Base class for any Models used as our high level API for implementing native methods of Java.
/// This high level API builds on top of the JNI and translates the JNIs general and JVM agnostic C interface to
/// a more high level C++ API specific to our JVM implementation.
///
/// To implement a new implementation of native methods for a Java class, simply create a new model class inheriting
/// from 'ModelBase'. The one optional template parameter describes the type that should be used as object
/// representation for any 'this' object coming from Java. By default it is simply 'Object'.
///
/// The model subclass may then simply implement member functions with the EXACT same name as the 'native' method in
/// Java. Function parameters from Java can easily be translated to C++ types: For integer types simply use signed
/// integer types with the same width as the corresponding Java type. For Java Objects, use 'GCRootRef<Object>' or any
/// other subclass of 'ObjectInterface' (such as e.g. 'GCRootRef<Array<int>>') that corresponds to, or is at least a
/// super class, of the object used in the Java API.
/// For convenience, the return type may be either 'GCRootRef<T>' or 'T*' when returning Java objects.
///
/// Non-static method members do NOT need an explicit 'this' pointer parameter. You can use 'javaThis' from within
/// member functions to get the 'this' reference from Java.
///
/// Static methods must always have 'VirtualMachine&' and 'GCRootRef<ClassObject>' as their first two parameters. The
/// parameters from the static method from Java follow right afterwards. The 'GCRootRef<ClassObject>' is the class
/// object of the class the static method is defined in.
///
/// As a final step, 'ModelBase' subclasses must add the following 'constexpr static' fields:
/// * 'llvm::StringLiteral className' which should contain the fully qualified name (i.e. with slashes) of the class
///    being modelled
/// *  'auto methods = std::make_tuple(addMember<&ModelClass::aNativeMethod>(), ...)' which is a tuple using
///    'addMember' that should list ALL implementations of 'native' methods that should be registered in the VM.
template <class JavaObject = Object>
class ModelBase
{
    static_assert(
        std::is_base_of_v<ObjectInterface, JavaObject>,
        "JavaObject must be a valid Java object representation with an object header and inherits from ObjectInterface");

protected:
    /// 'this' object from Java to be used in member functions.
    GCRootRef<JavaObject> javaThis;
    /// Instance of the virtual machine this model is registered in.
    VirtualMachine& virtualMachine;

    using Base = ModelBase<JavaObject>;

public:
    /// Constructor required to be implemented by any subclasses. Simply adding 'using Base::Base' to subclasses will
    /// make them inherit this constructor.
    ModelBase(GCRootRef<JavaObject> javaThis, VirtualMachine& virtualMachine)
        : javaThis(javaThis), virtualMachine(virtualMachine)
    {
    }

    /// Object representation type of Javas 'this'.
    using ThisType = JavaObject;
};

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
    constexpr static auto methods =
        std::make_tuple(addMember<&ObjectModel::hashCode>(), addMember<&ObjectModel::getClass>());
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
            {"double", 'D'},  {"float", 'F'}, {"void", 'V'}, {"long", 'L'},
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
        std::make_tuple(addMember<&ClassModel::registerNatives>(), addMember<&ClassModel::isArray>(),
                        addMember<&ClassModel::desiredAssertionStatus0>(), addMember<&ClassModel::getPrimitiveClass>());
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
    constexpr static auto methods = std::make_tuple(addMember<&floatToRawIntBits>(), addMember<&intBitsToFloat>());
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
    constexpr static auto methods = std::make_tuple(addMember<&doubleToRawLongBits>(), addMember<&longBitsToDouble>());
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
    constexpr static auto methods = std::make_tuple(addMember<&ThrowableModel::fillInStackTrace>());
};

class SystemModel : public ModelBase<Object>
{
public:
    using Base::Base;

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
    constexpr static auto methods = std::make_tuple(addMember<&SystemModel::registerNatives>(),
                                                    addMember<&SystemModel::nanoTime>());
};

class ReflectionModel : public ModelBase<>
{
public:
    using Base::Base;

    static const ClassObject* getCallerClass(VirtualMachine& virtualMachine, GCRootRef<ClassObject> classObject);

    constexpr static llvm::StringLiteral className = "jdk/internal/reflect/Reflection";
    constexpr static auto methods = std::make_tuple(addMember<&ReflectionModel::getCallerClass>());
};

class CDSModel : public ModelBase<Object>
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
        std::make_tuple(addMember<&CDSModel::isDumpingClassList0>(), addMember<&CDSModel::isDumpingArchive0>(),
                        addMember<&CDSModel::isSharingEnabled0>(), addMember<&CDSModel::getRandomSeedForDumping>(),
                        addMember<&CDSModel::initializeFromArchive>());
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
    constexpr static auto methods =
        std::make_tuple(addMember<&UnsafeModel::registerNatives>(), addMember<&UnsafeModel::arrayBaseOffset0>(),
                        addMember<&UnsafeModel::arrayIndexScale0>(), addMember<&UnsafeModel::objectFieldOffset1>(),
                        addMember<&UnsafeModel::storeFence>(), addMember<&UnsafeModel::loadFence>(),
                        addMember<&UnsafeModel::fullFence>(), addMember<&UnsafeModel::compareAndSetByte>(),
                        addMember<&UnsafeModel::compareAndSetShort>(), addMember<&UnsafeModel::compareAndSetChar>(),
                        addMember<&UnsafeModel::compareAndSetBoolean>(), addMember<&UnsafeModel::compareAndSetInt>(),
                        addMember<&UnsafeModel::compareAndSetLong>(), addMember<&UnsafeModel::compareAndSetReference>(),
                        addMember<&UnsafeModel::getIntVolatile>(), addMember<&UnsafeModel::getReferenceVolatile>(),
                        addMember<&UnsafeModel::putIntVolatile>(), addMember<&UnsafeModel::putReferenceVolatile>());
};

/// Register any models for builtin Java classes in the VM.
void registerJavaClasses(VirtualMachine& virtualMachine);

namespace detail
{
jllvm::VirtualMachine& virtualMachineFromJNIEnv(JNIEnv* env);

auto coerceReturnType(std::derived_from<ObjectInterface> auto* object, VirtualMachine& virtualMachine)
{
    // JNI only accepts roots as parameters and return type. Coerce any pointer to objects returned to roots.
    // The JNI bridge will delete the root and pass the object back to the Java stack.
    return virtualMachine.getGC().root(object).release();
}

template <class T>
auto coerceReturnType(T object, VirtualMachine&) requires(std::is_arithmetic_v<T>)
{
    return object;
}

template <class T>
auto coerceReturnType(GCRootRef<T> ref, VirtualMachine&)
{
    return ref;
}

template <class, class Ret, class... Args>
auto createMethodBridge(Ret (*ptr)(VirtualMachine&, GCRootRef<ClassObject>, Args...))
{
    return [ptr](JNIEnv* env, GCRootRef<ClassObject> classObject, Args... args)
    {
        VirtualMachine& virtualMachine = virtualMachineFromJNIEnv(env);
        if constexpr (!std::is_void_v<Ret>)
        {
            return coerceReturnType(ptr(virtualMachine, classObject, args...), virtualMachine);
        }
        else
        {
            return ptr(virtualMachine, classObject, args...);
        }
    };
}

template <class Model, class Ret, class... Args>
auto createMethodBridge(Ret (Model::*ptr)(Args...))
{
    return [ptr](JNIEnv* env, GCRootRef<typename Model::ThisType> javaThis, Args... args)
    {
        VirtualMachine& virtualMachine = virtualMachineFromJNIEnv(env);
        if constexpr (!std::is_void_v<Ret>)
        {
            return coerceReturnType((Model(javaThis, virtualMachine).*ptr)(args...), virtualMachine);
        }
        else
        {
            return (Model(javaThis, virtualMachine).*ptr)(args...);
        }
    };
}

template <class Model>
using hasClassName = decltype(Model::className);

template <class Model>
using hasMethods = decltype(Model::methods);

} // namespace detail

/// Registers all methods of a model 'Model' within 'virtualMachine'.
template <class Model>
void addModel(jllvm::VirtualMachine& virtualMachine)
{
    static_assert(
        llvm::is_detected<detail::hasClassName, Model>{},
        "'Model' must have a 'constexpr static llvm::StringLiteral className' field with fully qualified name of the class it is modelling");
    static_assert(
        llvm::is_detected<detail::hasMethods, Model>{},
        "'Model' must have a 'constexpr static' tuple called 'methods' listing all 'native' methods implemented");

    std::apply(
        [&](const auto&... subTuples)
        {
            (
                [&](const auto& subTuple)
                {
                    auto [ptr, methodName] = subTuple;
                    virtualMachine.getJIT().addJNISymbol(jllvm::formJNIMethodName(Model::className, methodName),
                                                         detail::createMethodBridge<Model>(ptr));
                }(subTuples),
                ...);
        },
        Model::methods);
}

template <class... Models>
void addModels(jllvm::VirtualMachine& virtualMachine)
{
    (addModel<Models>(virtualMachine), ...);
}

} // namespace jllvm
