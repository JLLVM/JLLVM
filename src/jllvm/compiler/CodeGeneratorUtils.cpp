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

#include "CodeGeneratorUtils.hpp"

using namespace jllvm;

namespace
{

bool isCategoryTwo(ByteCodeTypeChecker::JVMType type)
{
    return type.is<llvm::Type*>()
           && (type.get<llvm::Type*>()->isIntegerTy(64) || type.get<llvm::Type*>()->isDoubleTy());
}

template <class... Args>
struct OneOfBase : ByteCodeBase
{
    template <class T, class = std::enable_if_t<(std::is_same_v<std::decay_t<T>, Args> || ...)>>
    OneOfBase(T&& value) : ByteCodeBase(std::forward<T>(value))
    {
    }
};
} // namespace

void ByteCodeTypeChecker::checkBasicBlock(llvm::ArrayRef<char> block, std::uint16_t offset)
{
    // Set the types of the stack and locals to the start of the basic block.
    std::tie(m_typeStack, m_locals) = m_basicBlocks[offset];

    bool done = false;

    auto pushNext = [&](std::uint16_t next, llvm::ArrayRef<JVMType> typeStack)
    {
        // Note that 'try_emplace' does not copy its arguments unless the 'emplace' is successful.
        auto [iter, inserted] = m_basicBlocks.try_emplace(next, typeStack, m_locals);
        if (inserted)
        {
            m_offsetStack.push_back(next);
            return;
        }

        // Unlike the operand stack, local variables are allowed to have different types on entry of a basic block.
        // The Java verification algorithm simply then deems these local variables as unusable/uninitialized.
        // This uninitialized state has to be stored explicitly in our type checker since the local variables may be
        // read by deoptimization operands. We therefore implement the type-inference dataflow algorithm documented
        // in 4.10.2.2.

        // In the common case the types of local variables is identical and nothing has to be done.
        if (iter->second.second == m_locals)
        {
            return;
        }

        // Otherwise, merge the new local types with the previously seen local types. If types match, the matched types
        // are used, otherwise null is used as "unitialized" type.
        std::vector<JVMType> merged(m_locals.size());
        llvm::transform(llvm::zip_equal(iter->second.second, m_locals), merged.begin(),
                        [](auto&& pair) -> JVMType
                        {
                            auto&& [oldType, newType] = pair;
                            if (oldType == newType)
                            {
                                return oldType;
                            }
                            return nullptr;
                        });

        // If the merged types are different from the previously seen, store it as the new types and reschedule the
        // basic block for type checking to also propagate the local variable changes to successor blocks.
        // Since merging is a monotonic operation, a fixpoint will be reached and termination is guaranteed.
        if (merged != iter->second.second)
        {
            iter->second.second = std::move(merged);
            m_offsetStack.push_back(next);
        }
    };
    auto checkRet = [&](auto& ret)
    {
        std::uint16_t retAddress = m_locals[ret.index].template get<RetAddrType>();
        m_subroutineToReturnInfoMap.insert(
            {m_returnAddressToSubroutineMap[retAddress], {static_cast<std::uint16_t>(ret.offset), retAddress}});

        pushNext(retAddress, m_typeStack);
        done = true;
    };
    auto checkStore = [&](auto& store)
    {
        JVMType type = m_typeStack.back();
        m_typeStack.pop_back();
        m_locals[store.index] = type;
        // Storing double or long causes the local variable after to "used" as well.
        // Set the type to null in this case as if it was initialized as that can lead to better codegen.
        if (isCategoryTwo(type))
        {
            m_locals[store.index + 1] = nullptr;
        }
    };

    for (ByteCodeOp operation : byteCodeRange(block, offset))
    {
        if (done)
        {
            return;
        }

        if (getOffset(operation) == m_byteCodeTypeInfo.offset)
        {
            m_byteCodeTypeInfo.operandStack = m_typeStack;
            m_byteCodeTypeInfo.locals = m_locals;
        }

        if (auto result = m_exceptionHandlerStarts.find(getOffset(operation)); result != m_exceptionHandlerStarts.end())
        {
            for (std::uint16_t handlerPc : result->second)
            {
                // exception handlers have only the exception object on the type stack.
                pushNext(handlerPc, JVMType(m_addressType));
            }
        }

        match(
            operation, [](...) { llvm_unreachable("NOT YET IMPLEMENTED"); },
            [&](OneOfBase<AALoad, ANewArray, NewArray>)
            {
                if (holds_alternative<AALoad>(operation))
                {
                    m_typeStack.pop_back();
                }
                m_typeStack.back() = m_addressType;
            },
            [&](OneOfBase<AAStore, BAStore, CAStore, DAStore, FAStore, IAStore, LAStore, SAStore>)
            { m_typeStack.erase(m_typeStack.end() - 3, m_typeStack.end()); },
            [&](OneOfBase<AConstNull, ALoad, ALoad0, ALoad1, ALoad2, ALoad3, New>)
            { m_typeStack.emplace_back(m_addressType); },
            [&](OneOfBase<AReturn, AThrow, DReturn, FReturn, IReturn, LReturn, Return>) { done = true; },
            [&](OneOf<AStore, IStore, FStore, DStore, LStore> store) { checkStore(store); },
            [&](OneOf<AStore0, AStore1, AStore2, AStore3, IStore0, IStore1, IStore2, IStore3, FStore0, FStore1, FStore2,
                      FStore3, DStore0, DStore1, DStore2, DStore3, LStore0, LStore1, LStore2, LStore3>)
            {
                JVMType type = m_typeStack.back();
                m_typeStack.pop_back();

                auto index = match(
                    operation, [](...) -> std::uint8_t { llvm_unreachable("Invalid store operation"); },
                    [&](OneOf<AStore0, IStore0, FStore0, DStore0, LStore0>) { return 0; },
                    [&](OneOf<AStore1, IStore1, FStore1, DStore1, LStore1>) { return 1; },
                    [&](OneOf<AStore2, IStore2, FStore2, DStore2, LStore2>) { return 2; },
                    [&](OneOf<AStore3, IStore3, FStore3, DStore3, LStore3>) { return 3; });

                m_locals[index] = type;
                if (isCategoryTwo(type))
                {
                    m_locals[index + 1] = nullptr;
                }
            },
            [&](OneOfBase<ArrayLength, D2I, F2I, InstanceOf, L2I>) { m_typeStack.back() = m_intType; },
            [&](OneOfBase<CheckCast, DNeg, FNeg, I2B, I2C, I2S, IInc, INeg, LNeg, Nop>) { /* Types do not change */ },
            [&](OneOfBase<BALoad, CALoad, DCmpG, DCmpL, FCmpG, FCmpL, IALoad, LCmp, SALoad>)
            {
                m_typeStack.pop_back();
                m_typeStack.back() = m_intType;
            },
            [&](OneOfBase<BIPush, IConstM1, IConst0, IConst1, IConst2, IConst3, IConst4, IConst5, ILoad, ILoad0, ILoad1,
                          ILoad2, ILoad3, SIPush>) { m_typeStack.emplace_back(m_intType); },
            [&](OneOfBase<D2F, I2F, L2F, FALoad>)
            {
                if (holds_alternative<FALoad>(operation))
                {
                    m_typeStack.pop_back();
                }
                m_typeStack.back() = m_floatType;
            },
            [&](OneOfBase<D2L, F2L, I2L, LALoad>)
            {
                if (holds_alternative<LALoad>(operation))
                {
                    m_typeStack.pop_back();
                }
                m_typeStack.back() = m_longType;
            },
            [&](OneOfBase<DAdd, DDiv, DMul, DRem, DSub, FAdd, FDiv, FMul, FRem, FSub, IAdd, IAnd, IDiv, IMul, IOr, IRem,
                          IShl, IShr, ISub, IUShr, IXor, LAdd, LAnd, LDiv, LMul, LOr, LRem, LShl, LShr, LSub, LUShr,
                          LXor, MonitorEnter, MonitorExit, Pop, PutStatic>) { m_typeStack.pop_back(); },
            [&](OneOfBase<DALoad, F2D, I2D, L2D>)
            {
                if (holds_alternative<DALoad>(operation))
                {
                    m_typeStack.pop_back();
                }
                m_typeStack.back() = m_doubleType;
            },
            [&](OneOfBase<DConst0, DConst1, DLoad, DLoad0, DLoad1, DLoad2, DLoad3>)
            { m_typeStack.emplace_back(m_doubleType); },
            [&](Dup) { m_typeStack.push_back(m_typeStack.back()); },
            [&](DupX1)
            {
                auto iter = m_typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                assert(!isCategoryTwo(type1) && !isCategoryTwo(type2));

                m_typeStack.insert(iter.base(), type1);
            },
            [&](DupX2)
            {
                auto iter = m_typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                if (!isCategoryTwo(type2))
                {
                    // Form 1: where value1, value2, and value3 are all values of a category 1 computational type
                    ++iter;
                }

                m_typeStack.insert(iter.base(), type1);
            },
            [&](Dup2)
            {
                auto iter = m_typeStack.rbegin();
                JVMType type = *iter++;

                if (!isCategoryTwo(type))
                {
                    // Form 1: where both value1 and value2 are values of a category 1 computational type
                    JVMType type2 = *iter++;

                    m_typeStack.push_back(type2);
                }

                m_typeStack.push_back(type);
            },
            [&](Dup2X1)
            {
                auto iter = m_typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                if (!isCategoryTwo(type1))
                {
                    // Form 1: where value1, value2, and value3 are all values of a category 1 computational type

                    m_typeStack.insert((++iter).base(), {type2, type1});
                }
                else
                {
                    // Form 2: where value1 is a value of a category 2 computational type and value2 is a value of a
                    // category 1 computational type
                    m_typeStack.insert(iter.base(), type1);
                }
            },
            [&](Dup2X2)
            {
                auto iter = m_typeStack.rbegin();
                JVMType type1 = *iter++;
                JVMType type2 = *iter++;

                if (!isCategoryTwo(type1))
                {
                    JVMType type3 = *iter++;

                    if (!isCategoryTwo(type3))
                    {
                        // Form 1: where value1, value2, value3, and value4 are all values of a category 1 computational
                        // type
                        ++iter;
                    }

                    m_typeStack.insert(iter.base(), {type2, type1});
                }
                else
                {
                    if (!isCategoryTwo(type2))
                    {
                        // Form 2: where value1 is a value of a category 2 computational type and value2 and value3 are
                        // both values of a category 1 computational type
                        ++iter;
                    }

                    m_typeStack.insert(iter.base(), type1);
                }
            },
            [&](OneOfBase<FConst0, FConst1, FConst2, FLoad, FLoad0, FLoad1, FLoad2, FLoad3>)
            { m_typeStack.emplace_back(m_floatType); },
            [&](OneOf<GetField, GetStatic> get)
            {
                if (holds_alternative<GetField>(operation))
                {
                    m_typeStack.pop_back();
                }

                auto descriptor = FieldType(PoolIndex<FieldRefInfo>{get.index}
                                                .resolve(m_classFile)
                                                ->nameAndTypeIndex.resolve(m_classFile)
                                                ->descriptorIndex.resolve(m_classFile)
                                                ->text);

                llvm::Type* type = descriptorToType(descriptor, m_context);
                if (type->isIntegerTy() && !type->isIntegerTy(64))
                {
                    type = m_intType;
                }

                m_typeStack.emplace_back(type);
            },
            [&](OneOf<Goto, GotoW> gotoOp)
            {
                pushNext(gotoOp.offset + gotoOp.target, m_typeStack);
                done = true;
            },
            [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe, IfEq, IfNe, IfLt,
                      IfGe, IfGt, IfLe, IfNonNull, IfNull>
                    cmpOp)
            {
                m_typeStack.pop_back();

                match(
                    operation,
                    [&](OneOf<IfACmpEq, IfACmpNe, IfICmpEq, IfICmpNe, IfICmpLt, IfICmpGe, IfICmpGt, IfICmpLe>)
                    { m_typeStack.pop_back(); },
                    [](...) {});

                pushNext(cmpOp.offset + cmpOp.target, m_typeStack);
                pushNext(cmpOp.offset + sizeof(OpCodes) + sizeof(std::int16_t), m_typeStack);
                done = true;
            },
            // TODO InvokeDynamic
            [&](OneOf<InvokeInterface, InvokeSpecial, InvokeStatic, InvokeVirtual> invoke)
            {
                auto descriptor = MethodType(PoolIndex<RefInfo>{invoke.index}
                                                 .resolve(m_classFile)
                                                 ->nameAndTypeIndex.resolve(m_classFile)
                                                 ->descriptorIndex.resolve(m_classFile)
                                                 ->text);

                for (std::size_t i = 0; i < descriptor.size(); i++)
                {
                    m_typeStack.pop_back();
                }

                // static does not pop this
                if (!holds_alternative<InvokeStatic>(operation))
                {
                    m_typeStack.pop_back();
                }

                llvm::Type* type = descriptorToType(descriptor.returnType(), m_context);
                if (type->isIntegerTy() && !type->isIntegerTy(64))
                {
                    type = m_intType;
                }

                if (!type->isVoidTy())
                {
                    m_typeStack.emplace_back(type);
                }
            },
            [&](OneOf<JSR, JSRw> jsr)
            {
                std::uint16_t retAddress =
                    jsr.offset + sizeof(OpCodes)
                    + (holds_alternative<JSRw>(operation) ? sizeof(std::int32_t) : sizeof(std::int16_t));
                std::uint16_t target = jsr.offset + jsr.target;

                m_returnAddressToSubroutineMap.insert({retAddress, target});

                // check if the subroutine has already been type-checked. If so use the previously calculated typeStack
                if (auto iter = m_subroutineToReturnInfoMap.find(target); iter != m_subroutineToReturnInfoMap.end())
                {
                    std::tie(m_typeStack, m_locals) = m_basicBlocks[iter->second.returnAddress];
                    pushNext(retAddress, m_typeStack);
                }
                else
                {
                    m_typeStack.emplace_back(retAddress);
                    pushNext(target, m_typeStack);
                }

                done = true;
            },
            [&](OneOfBase<LConst0, LConst1, LLoad, LLoad0, LLoad1, LLoad2, LLoad3>)
            { m_typeStack.emplace_back(m_longType); },
            [&](OneOf<LDC, LDCW, LDC2W> ldc)
            {
                PoolIndex<IntegerInfo, FloatInfo, LongInfo, DoubleInfo, StringInfo, ClassInfo, MethodRefInfo,
                          InterfaceMethodRefInfo, MethodTypeInfo, DynamicInfo>
                    pool{ldc.index};

                match(
                    pool.resolve(m_classFile), [&](const ClassInfo*) { m_typeStack.emplace_back(m_addressType); },
                    [&](const DoubleInfo*) { m_typeStack.emplace_back(m_doubleType); },
                    [&](const FloatInfo*) { m_typeStack.emplace_back(m_floatType); },
                    [&](const IntegerInfo*) { m_typeStack.emplace_back(m_intType); },
                    [&](const LongInfo*) { m_typeStack.emplace_back(m_longType); },
                    [&](const StringInfo*) { m_typeStack.emplace_back(m_addressType); },
                    [](const auto*) { llvm::report_fatal_error("Not yet implemented"); });
            },
            [&](const OneOf<LookupSwitch, TableSwitch>& switchOp)
            {
                m_typeStack.pop_back();

                pushNext(switchOp.offset + switchOp.defaultOffset, m_typeStack);

                for (std::int32_t target : llvm::make_second_range(switchOp.matchOffsetsPairs))
                {
                    pushNext(switchOp.offset + target, m_typeStack);
                }
                done = true;
            },
            [&](MultiANewArray multiANewArray)
            {
                for (int i = 0; i < multiANewArray.dimensions; ++i)
                {
                    m_typeStack.pop_back();
                }

                m_typeStack.emplace_back(m_addressType);
            },
            [&](Pop2)
            {
                JVMType type = m_typeStack.back();
                m_typeStack.pop_back();
                if (!isCategoryTwo(type))
                {
                    m_typeStack.pop_back();
                }
            },
            [&](PutField)
            {
                m_typeStack.pop_back();
                m_typeStack.pop_back();
            },
            [&](Ret ret) { checkRet(ret); },
            [&](Swap) { std::swap(m_typeStack.back(), *std::next(m_typeStack.rbegin())); },
            [&](Wide wide)
            {
                llvm::Type* type;
                switch (wide.opCode)
                {
                    default: llvm_unreachable("Invalid wide operation");
                    case OpCodes::AStore:
                    case OpCodes::DStore:
                    case OpCodes::FStore:
                    case OpCodes::IStore:
                    case OpCodes::LStore:
                    {
                        checkStore(wide);
                        return;
                    }
                    case OpCodes::Ret:
                    {
                        checkRet(wide);
                        return;
                    }
                    case OpCodes::IInc:
                    {
                        return;
                    }
                    case OpCodes::ALoad:
                    {
                        type = m_addressType;
                        break;
                    }
                    case OpCodes::DLoad:
                    {
                        type = m_doubleType;
                        break;
                    }
                    case OpCodes::FLoad:
                    {
                        type = m_floatType;
                        break;
                    }
                    case OpCodes::ILoad:
                    {
                        type = m_intType;
                        break;
                    }
                    case OpCodes::LLoad:
                    {
                        type = m_longType;
                        break;
                    }
                }

                m_typeStack.emplace_back(type);
            });
    }
}

const ByteCodeTypeChecker::TypeInfo& ByteCodeTypeChecker::checkAndGetTypeInfo(std::uint16_t offset)
{
    for (const auto& exception : m_code.getExceptionTable())
    {
        m_exceptionHandlerStarts[exception.startPc].push_back(exception.handlerPc);
    }

    m_basicBlocks.insert({0, {{}, m_locals}});
    m_offsetStack.push_back(0);
    m_byteCodeTypeInfo.offset = offset;

    while (!m_offsetStack.empty())
    {
        std::uint16_t startOffset = m_offsetStack.back();
        m_offsetStack.pop_back();

        checkBasicBlock(m_code.getCode(), startOffset);
    }

    return m_byteCodeTypeInfo;
}

ByteCodeTypeChecker::PossibleRetsMap ByteCodeTypeChecker::makeRetToMap() const
{
    PossibleRetsMap map;

    for (const auto& [returnAddr, subroutine] : m_returnAddressToSubroutineMap)
    {
        map[m_subroutineToReturnInfoMap.lookup(subroutine).retOffset].insert(returnAddr);
    }

    return map;
}

LocalVariables::Proxy::operator llvm::Value*() const
{
    // Uninitialized locals return null.
    if (!m_localVariable->m_types[m_index])
    {
        return nullptr;
    }

    return m_localVariable->m_builder.CreateLoad(m_localVariable->m_types[m_index], m_localVariable->m_locals[m_index]);
}

const LocalVariables::Proxy& LocalVariables::Proxy::operator=(llvm::Value* value) const
{
    m_localVariable->m_types[m_index] = value->getType();
    m_localVariable->m_builder.CreateStore(value, m_localVariable->m_locals[m_index]);
    if (isCategoryTwo(value->getType()))
    {
        // The next local variable is also "occupied" if storing a double or long.
        // We simply mark it as uninitialized.
        m_localVariable->m_types[m_index + 1] = nullptr;
    }
    return *this;
}
