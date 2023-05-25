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
/// integer types with the same width as the corresponding Java type. For Java Objects, use 'Object*' or any other
/// subclass of 'ObjectInterface' (such as e.g. 'Array<int>*') that corresponds to, or is at least a super class, of
/// the object used in the Java API.
///
/// Non-static method members do NOT need an explicit 'this' pointer parameter. You can use 'javaThis' from within
/// member functions to get the 'this' pointer from Java.
///
/// Static methods must always have 'VirtualMachine&' and 'ClassObject*' as their first two parameters. The parameters
/// from the static method from Java follow right afterwards. The 'ClassObject*' is the class object of the class the
/// static method is defined in.
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
    JavaObject& javaThis;
    /// Instance of the virtual machine this model is registered in.
    VirtualMachine& virtualMachine;

    using Base = ModelBase<JavaObject>;

public:
    /// Constructor required to be implemented by any subclasses. Simply adding 'using Base::Base' to subclasses will
    /// make them inherit this constructor.
    ModelBase(JavaObject& javaThis, VirtualMachine& virtualMachine) : javaThis(javaThis), virtualMachine(virtualMachine)
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
        return javaThis.getClass();
    }

    std::int32_t hashCode()
    {
        std::int32_t& hashCode = javaThis.getObjectHeader().hashCode;
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

    static void registerNatives(VirtualMachine&, ClassObject*)
    {
        // Noop until (if?) we need some C++ initialization code.
    }

    bool isArray()
    {
        return javaThis.isArray();
    }

    static bool desiredAssertionStatus0(VirtualMachine&, ClassObject*)
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
                        addMember<&ClassModel::desiredAssertionStatus0>());
};

/// Model implementation for the native methods of Javas 'Thowable' class.
class ThrowableModel : public ModelBase<Throwable>
{
public:
    using Base::Base;

    Throwable* fillInStackTrace(int)
    {
        // TODO: Set backtrace and depth of 'javaThis'. See
        // https://github.com/openjdk/jdk/blob/4f096eb7c9066e5127d9ab8c1c893e991a23d316/src/hotspot/share/classfile/javaClasses.cpp#L2491
        return &javaThis;
    }

    constexpr static llvm::StringLiteral className = "java/lang/Throwable";
    constexpr static auto methods = std::make_tuple(addMember<&ThrowableModel::fillInStackTrace>());
};

/// Register any models for builtin Java classes in the VM.
void registerJavaClasses(VirtualMachine& virtualMachine);

namespace detail
{
jllvm::VirtualMachine& virtualMachineFromJNIEnv(JNIEnv* env);

template <class, class Ret, class... Args>
auto createMethodBridge(Ret (*ptr)(jllvm::VirtualMachine&, jllvm::ClassObject*, Args...))
{
    return [ptr](JNIEnv* env, jllvm::ClassObject* classObject, Args... args)
    { return ptr(virtualMachineFromJNIEnv(env), classObject, args...); };
}

template <class Model, class Ret, class... Args>
auto createMethodBridge(Ret (Model::*ptr)(Args...))
{
    return [ptr](JNIEnv* env, typename Model::ThisType* javaThis, Args... args)
    { return (Model(*javaThis, virtualMachineFromJNIEnv(env)).*ptr)(args...); };
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
