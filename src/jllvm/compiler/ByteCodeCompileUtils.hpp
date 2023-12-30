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

#include <llvm/IR/DerivedTypes.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InstrTypes.h>
#include <llvm/IR/LLVMContext.h>

#include <jllvm/class/Descriptors.hpp>
#include <jllvm/object/ClassObject.hpp>
#include <jllvm/unwind/Unwinder.hpp>

namespace jllvm
{
/// Returns the pointer type used by the JVM for arrays of references.
llvm::Type* arrayRefType(llvm::LLVMContext& context);

/// Returns the pointer type used for any Java array types storing elements of 'elementType'.
llvm::Type* arrayStructType(llvm::Type* elementType);

/// Returns the pointer type used by the JVM for interface tables.
llvm::Type* iTableType(llvm::LLVMContext& context);

/// Returns the pointer type used by the JVM for object headers.
llvm::Type* objectHeaderType(llvm::LLVMContext& context);

/// Returns the pointer type used for all Java reference types.
/// This is a pointer tagged with an address space for the sake of the GC.
llvm::PointerType* referenceType(llvm::LLVMContext& context);

/// Returns the global variable importing the class object of the given descriptor.
llvm::GlobalVariable* classObjectGlobal(llvm::Module& module, FieldType classObject);

/// Returns the global variable importing the given method.
llvm::GlobalVariable* methodGlobal(llvm::Module& module, const Method* method);

/// Returns the global variable importing the given interned string.
llvm::GlobalVariable* stringGlobal(llvm::Module& module, llvm::StringRef contents);

/// Returns the corresponding LLVM type for a given Java field descriptor.
llvm::Type* descriptorToType(FieldType type, llvm::LLVMContext& context);

/// Returns the corresponding LLVM function type for a given, possible static, Java method descriptor.
llvm::FunctionType* descriptorToType(MethodType type, bool isStatic, llvm::LLVMContext& context);

/// Calling convention used by a Java method.
enum class CallingConvention : std::uint8_t
{
    /// uint64_t(const Method*, const std::uint64_t).
    Interpreter,
    /// Return type and parameters matching the 'MethodType' of the method with an explicit pointer argument for 'this'
    /// if a non-static method.
    JIT,
};

/// Returns the LLVM function type for an OSR method for a given return type.
/// The calling convention used is suitable to replace a frame with the given 'callingConvention' by using the same
/// return type. The parameter list contains of a single pointer to an internal array built by 'OSRState' used to
/// initialize the abstract machine state.
llvm::FunctionType* osrMethodSignature(FieldType returnType, CallingConvention callingConvention,
                                       llvm::LLVMContext& context);

/// Generates code using 'builder' to convert 'value', which is of the corresponding LLVM type of 'type', to the
/// corresponding LLVM type as is used on the JVM operand stack.
/// This is essentially just signed-extending or zero-extending integers less than 'int' to 'int'.
llvm::Value* extendToStackType(llvm::IRBuilder<>& builder, FieldType type, llvm::Value* value);

/// Metadata attached to Java methods produced by any 'ByteCodeLayer' implementation.
class JavaMethodMetadata
{
public:
    /// Metadata contained within any Interpreter Java frame.
    struct InterpreterData
    {
        FrameValue<const Method*> method;
        FrameValue<std::uint16_t*> byteCodeOffset;
        FrameValue<std::uint16_t*> topOfStack;
        FrameValue<std::uint64_t*> operandStack;
        FrameValue<std::uint64_t*> operandGCMask;
        FrameValue<std::uint64_t*> localVariables;
        FrameValue<std::uint64_t*> localVariablesGCMask;
    };

    /// Metadata contained within any JITted Java frame.
    class JITData
    {
        const Method* m_method{};

        struct PerPCData
        {
            std::uint16_t byteCodeOffset{};
            std::vector<FrameValue<std::uint64_t>> locals;
            std::vector<std::uint64_t> localsGCMask;
        };

        /// Pointer to a dynamically allocated instance. This is not just a 'llvm::DenseMap' as that is 1) not a
        /// standard layout type and 2) requires being able to write to the object despite 'JavaMethodMetadata' being
        /// in read-only memory after linking.
        llvm::DenseMap<std::uintptr_t, PerPCData>* m_perPcData = nullptr;

    public:

        ~JITData()
        {
            delete m_perPcData;
        }

        /// Inserts new metadata for the given program counter.
        void insert(std::uintptr_t programCounter, PerPCData&& pcData)
        {
            if (!m_perPcData)
            {
                m_perPcData = new std::decay_t<decltype(*m_perPcData)>;
            }
            m_perPcData->insert({programCounter, std::move(pcData)});
        }

        /// Returns the given metadata for the given program counter.
        /// It is undefined behaviour, if no metadata is associated with the given program counter.
        /// Metadata is guaranteed to exist for every call-site capable of throwing an exception within a JITted method.
        const PerPCData& operator[](std::uintptr_t programCounter) const
        {
            auto iter = m_perPcData->find(programCounter);
            assert(iter != m_perPcData->end() && "JIT frame must have metadata associated with every call-site");
            return iter->second;
        }

        /// Returns the method object of this JITted method.
        const Method* getMethod() const
        {
            return m_method;
        }

        JITData(const JITData&) = delete;
        JITData& operator=(const JITData&) = delete;
        JITData(JITData&&) = delete;
        JITData& operator=(JITData&&) = delete;
    };

    /// Metadata contained within any JNI Java frame.
    struct NativeData
    {
        const Method* method{};
    };

    /// Possible kinds of Java frames.
    enum class Kind : std::uint8_t
    {
        /// JITted method.
        JIT = 0,
        /// Interpreter method.
        Interpreter = 1,
        /// JNI method.
        Native = 2,
    };

private:
    Kind m_kind{};
    CallingConvention m_callingConvention{};

public:
    /// 'JavaMethodMetadata' is only ever created by LLVM-IR.
    JavaMethodMetadata() = delete;

    /// Returns true if this is metadata for a JITted method.
    bool isJIT() const
    {
        return m_kind == Kind::JIT;
    }

    /// Returns true if this is metadata for an interpreted method.
    bool isInterpreter() const
    {
        return m_kind == Kind::Interpreter;
    }

    /// Returns true if this is metadata for a native method.
    bool isNative() const
    {
        return m_kind == Kind::Native;
    }

    /// Returns the kind of this metadata.
    Kind getKind() const
    {
        return m_kind;
    }

    /// Returns the calling convention used by this method.
    /// Note that the calling convention is orthogonal to the tier it is running in.
    /// A method may be JIT compiled and have JIT metadata but nevertheless use the interpreter calling convention.
    /// This commonly happens during OSR where the replacing method has to use the same calling convention as the method
    /// being replaced.
    CallingConvention getCallingConvention() const
    {
        return m_callingConvention;
    }

    /// Returns the Interpreter metadata field.
    /// It is undefined behaviour to call this method if the metadata is not for an interpreted method.
    const InterpreterData& getInterpreterData() const
    {
        assert(isInterpreter());
        return reinterpret_cast<const InterpreterData*>(this)[-1];
    }

    InterpreterData& getInterpreterData()
    {
        return const_cast<InterpreterData&>(std::as_const(*this).getInterpreterData());
    }

    /// Returns the JIT metadata field.
    /// It is undefined behaviour to call this method if the metadata is not for a JITted method.
    const JITData& getJITData() const
    {
        assert(isJIT());
        return reinterpret_cast<const JITData*>(this)[-1];
    }

    JITData& getJITData()
    {
        return const_cast<JITData&>(std::as_const(*this).getJITData());
    }

    /// Returns the Native metadata field.
    /// It is undefined behaviour to call this method if the metadata is not for a native method.
    const NativeData& getNativeData() const
    {
        assert(isNative());
        return reinterpret_cast<const NativeData*>(this)[-1];
    }
};

static_assert(std::is_standard_layout_v<JavaMethodMetadata>);

/// Adds Java JIT method metadata to the function.
void addJavaJITMethodMetadata(llvm::Function* function, const Method* method, CallingConvention callingConvention);

/// Adds Java JNI method metadata to the function.
void addJavaNativeMethodMetadata(llvm::Function* function, const Method* method);

/// Adds Java Interpreter method metadata to the function.
void addJavaInterpreterMethodMetadata(llvm::Function* function, CallingConvention callingConvention);

/// Applies all ABI relevant attributes to the function which must have a signature matching the output of
/// 'descriptorToType' when called with the given 'methodType' and 'isStatic'.
void applyABIAttributes(llvm::Function* function, MethodType methodType, bool isStatic);

/// Applies all ABI relevant attributes to the function that do not depend on its signature.
/// This is e.g. used for stubs.
void applyABIAttributes(llvm::Function* function);

/// Applies all ABI relevant attributes to the call which must call a function with the signature matching the output of
/// 'descriptorToType' when called with the given 'methodType' and 'isStatic'.
void applyABIAttributes(llvm::CallBase* call, MethodType methodType, bool isStatic);

/// Initializes 'classObject' if it is still uninitialized. If 'addDeopt' is true, an empty deopt operand bundle is added.
/// Returns a pointer to the call instruction of the initializer.
llvm::CallBase* initializeClassObject(llvm::IRBuilder<>& builder, llvm::Value* classObject, bool addDeopt = true);

/// Emits a suitable sequence of instructions for returning from a method with the given calling convention.
/// 'builder' is used to create any new instructions. 'value' is expected to be null if the method has a void return
/// type.
void emitReturn(llvm::IRBuilder<>& builder, llvm::Value* value, CallingConvention callingConvention);

} // namespace jllvm
