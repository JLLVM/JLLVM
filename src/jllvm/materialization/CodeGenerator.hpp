#pragma once

#include "CodeGeneratorUtils.hpp"
#include "jllvm/class/ByteCodeIterator.hpp"
#include "jllvm/vm/StringInterner.hpp"

namespace jllvm
{
/// Class for generating LLVM IR from Java Bytecode
/// This class should ideally be used to generate code for a single method
class CodeGenerator
{
    llvm::Function* m_function;
    const ClassFile& m_classFile;
    LazyClassLoaderHelper m_helper;
    StringInterner& stringInterner;
    const MethodType& functionMethodType;
    llvm::IRBuilder<> builder;
    OperandStack operandStack;
    std::vector<llvm::AllocaInst*> locals;
    llvm::DenseMap<std::uint16_t, llvm::BasicBlock*> basicBlocks;
    llvm::DenseMap<llvm::BasicBlock*, OperandStack::State> basicBlockStackStates;

    using HandlerInfo = std::pair<std::uint16_t, PoolIndex<ClassInfo>>;

    // std::list because we want the iterator stability when deleting handlers (requires random access).
    std::list<HandlerInfo> activeHandlers;
    // std::map because it is the easiest to use with std::list key.
    std::map<std::list<HandlerInfo>, llvm::BasicBlock*> m_alreadyGeneratedHandlers;

    void calculateBasicBlocks(const Code& code);

    void generateCodeBody(const Code& code);

    void generateInstruction(ByteCodeOp operation);

    void generateEHDispatch();

    llvm::BasicBlock* generateHandlerChain(llvm::Value* exception, llvm::BasicBlock* newPred);

    llvm::Value* loadClassObjectFromPool(PoolIndex<ClassInfo> index);

    llvm::Value* generateAllocArray(llvm::StringRef descriptor, llvm::Value* classObject, llvm::Value* size);

public:
    CodeGenerator(llvm::Function* function, const ClassFile& classFile, LazyClassLoaderHelper helper,
                  StringInterner& stringInterner, const MethodType& methodType, std::uint16_t maxStack,
                  std::uint16_t maxLocals)
        : m_function{function},
          m_classFile{classFile},
          m_helper{std::move(helper)},
          stringInterner{stringInterner},
          functionMethodType{methodType},
          builder{llvm::BasicBlock::Create(function->getContext(), "entry", function)},
          operandStack{builder, maxStack},
          locals{maxLocals}
    {
    }

    /// This function should ideally be only called once. 'code' must have at most a maximum stack depth of 'maxStack'
    /// and have at most 'maxLocals' local variables.
    void generateCode(const Code& code);
};
} // namespace jllvm
