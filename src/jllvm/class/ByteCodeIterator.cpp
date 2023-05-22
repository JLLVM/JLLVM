#include "ByteCodeIterator.hpp"

#include <llvm/Support/Alignment.h>
#include <llvm/Support/Endian.h>

#include <jllvm/support/Bytes.hpp>

std::optional<jllvm::SingletonOp> consumeSingleton(llvm::ArrayRef<char> bytes)
{
    auto opCode = static_cast<jllvm::SingletonOp>(bytes.front());
    switch (opCode)
    {
        default:
        {
            return std::nullopt;
        }
        case jllvm::SingletonOp::AALoad:
        case jllvm::SingletonOp::AAStore:
        case jllvm::SingletonOp::AConstNull:
        case jllvm::SingletonOp::ALoad0:
        case jllvm::SingletonOp::ALoad1:
        case jllvm::SingletonOp::ALoad2:
        case jllvm::SingletonOp::ALoad3:
        case jllvm::SingletonOp::AReturn:
        case jllvm::SingletonOp::ArrayLength:
        case jllvm::SingletonOp::AStore0:
        case jllvm::SingletonOp::AStore1:
        case jllvm::SingletonOp::AStore2:
        case jllvm::SingletonOp::AStore3:
        case jllvm::SingletonOp::AThrow:
        case jllvm::SingletonOp::BALoad:
        case jllvm::SingletonOp::BAStore:
        case jllvm::SingletonOp::CALoad:
        case jllvm::SingletonOp::CAStore:
        case jllvm::SingletonOp::D2F:
        case jllvm::SingletonOp::D2I:
        case jllvm::SingletonOp::D2L:
        case jllvm::SingletonOp::DAdd:
        case jllvm::SingletonOp::DALoad:
        case jllvm::SingletonOp::DAStore:
        case jllvm::SingletonOp::DCmpG:
        case jllvm::SingletonOp::DCmpL:
        case jllvm::SingletonOp::DConst0:
        case jllvm::SingletonOp::DConst1:
        case jllvm::SingletonOp::DDiv:
        case jllvm::SingletonOp::DLoad0:
        case jllvm::SingletonOp::DLoad1:
        case jllvm::SingletonOp::DLoad2:
        case jllvm::SingletonOp::DLoad3:
        case jllvm::SingletonOp::DMul:
        case jllvm::SingletonOp::DNeg:
        case jllvm::SingletonOp::DRem:
        case jllvm::SingletonOp::DReturn:
        case jllvm::SingletonOp::DStore0:
        case jllvm::SingletonOp::DStore1:
        case jllvm::SingletonOp::DStore2:
        case jllvm::SingletonOp::DStore3:
        case jllvm::SingletonOp::DSub:
        case jllvm::SingletonOp::Dup:
        case jllvm::SingletonOp::DupX1:
        case jllvm::SingletonOp::DupX2:
        case jllvm::SingletonOp::Dup2:
        case jllvm::SingletonOp::Dup2X1:
        case jllvm::SingletonOp::Dup2X2:
        case jllvm::SingletonOp::F2D:
        case jllvm::SingletonOp::F2I:
        case jllvm::SingletonOp::F2L:
        case jllvm::SingletonOp::FAdd:
        case jllvm::SingletonOp::FALoad:
        case jllvm::SingletonOp::FAStore:
        case jllvm::SingletonOp::FCmpG:
        case jllvm::SingletonOp::FCmpL:
        case jllvm::SingletonOp::FConst0:
        case jllvm::SingletonOp::FConst1:
        case jllvm::SingletonOp::FConst2:
        case jllvm::SingletonOp::FDiv:
        case jllvm::SingletonOp::FLoad0:
        case jllvm::SingletonOp::FLoad1:
        case jllvm::SingletonOp::FLoad2:
        case jllvm::SingletonOp::FLoad3:
        case jllvm::SingletonOp::FMul:
        case jllvm::SingletonOp::FNeg:
        case jllvm::SingletonOp::FRem:
        case jllvm::SingletonOp::FReturn:
        case jllvm::SingletonOp::FStore0:
        case jllvm::SingletonOp::FStore1:
        case jllvm::SingletonOp::FStore2:
        case jllvm::SingletonOp::FStore3:
        case jllvm::SingletonOp::FSub:
        case jllvm::SingletonOp::I2B:
        case jllvm::SingletonOp::I2C:
        case jllvm::SingletonOp::I2D:
        case jllvm::SingletonOp::I2F:
        case jllvm::SingletonOp::I2L:
        case jllvm::SingletonOp::I2S:
        case jllvm::SingletonOp::IAdd:
        case jllvm::SingletonOp::IALoad:
        case jllvm::SingletonOp::IAnd:
        case jllvm::SingletonOp::IAStore:
        case jllvm::SingletonOp::IConstM1:
        case jllvm::SingletonOp::IConst0:
        case jllvm::SingletonOp::IConst1:
        case jllvm::SingletonOp::IConst2:
        case jllvm::SingletonOp::IConst3:
        case jllvm::SingletonOp::IConst4:
        case jllvm::SingletonOp::IConst5:
        case jllvm::SingletonOp::IDiv:
        case jllvm::SingletonOp::ILoad0:
        case jllvm::SingletonOp::ILoad1:
        case jllvm::SingletonOp::ILoad2:
        case jllvm::SingletonOp::ILoad3:
        case jllvm::SingletonOp::IMul:
        case jllvm::SingletonOp::INeg:
        case jllvm::SingletonOp::IOr:
        case jllvm::SingletonOp::IRem:
        case jllvm::SingletonOp::IReturn:
        case jllvm::SingletonOp::IShl:
        case jllvm::SingletonOp::IShr:
        case jllvm::SingletonOp::IStore0:
        case jllvm::SingletonOp::IStore1:
        case jllvm::SingletonOp::IStore2:
        case jllvm::SingletonOp::IStore3:
        case jllvm::SingletonOp::ISub:
        case jllvm::SingletonOp::IUShr:
        case jllvm::SingletonOp::IXor:
        case jllvm::SingletonOp::L2D:
        case jllvm::SingletonOp::L2F:
        case jllvm::SingletonOp::L2I:
        case jllvm::SingletonOp::LAdd:
        case jllvm::SingletonOp::LALoad:
        case jllvm::SingletonOp::LAnd:
        case jllvm::SingletonOp::LAStore:
        case jllvm::SingletonOp::LCmp:
        case jllvm::SingletonOp::LConst0:
        case jllvm::SingletonOp::LConst1:
        case jllvm::SingletonOp::LDiv:
        case jllvm::SingletonOp::LLoad0:
        case jllvm::SingletonOp::LLoad1:
        case jllvm::SingletonOp::LLoad2:
        case jllvm::SingletonOp::LLoad3:
        case jllvm::SingletonOp::LMul:
        case jllvm::SingletonOp::LNeg:
        case jllvm::SingletonOp::LOr:
        case jllvm::SingletonOp::LRem:
        case jllvm::SingletonOp::LReturn:
        case jllvm::SingletonOp::LShl:
        case jllvm::SingletonOp::LShr:
        case jllvm::SingletonOp::LStore0:
        case jllvm::SingletonOp::LStore1:
        case jllvm::SingletonOp::LStore2:
        case jllvm::SingletonOp::LStore3:
        case jllvm::SingletonOp::LSub:
        case jllvm::SingletonOp::LUShr:
        case jllvm::SingletonOp::LXor:
        case jllvm::SingletonOp::MonitorEnter:
        case jllvm::SingletonOp::MonitorExit:
        case jllvm::SingletonOp::Nop:
        case jllvm::SingletonOp::Pop:
        case jllvm::SingletonOp::Pop2:
        case jllvm::SingletonOp::Return:
        case jllvm::SingletonOp::SALoad:
        case jllvm::SingletonOp::SAStore:
        case jllvm::SingletonOp::Swap:
        {
            return jllvm::consume<jllvm::SingletonOp>(bytes);
        }
    }
}

std::optional<jllvm::LocalIndexedOp> consumeLocalIndex(llvm::ArrayRef<char> bytes)
{
    auto opCode = static_cast<jllvm::LocalIndexedOpCodes>(bytes.front());
    switch (opCode)
    {
        default:
        {
            return std::nullopt;
        }
        case jllvm::LocalIndexedOpCodes::ALoad:
        case jllvm::LocalIndexedOpCodes::AStore:
        case jllvm::LocalIndexedOpCodes::DLoad:
        case jllvm::LocalIndexedOpCodes::DStore:
        case jllvm::LocalIndexedOpCodes::FLoad:
        case jllvm::LocalIndexedOpCodes::FStore:
        case jllvm::LocalIndexedOpCodes::ILoad:
        case jllvm::LocalIndexedOpCodes::IStore:
        case jllvm::LocalIndexedOpCodes::LLoad:
        case jllvm::LocalIndexedOpCodes::LStore:
        case jllvm::LocalIndexedOpCodes::Ret:
        {
            return jllvm::LocalIndexedOp{jllvm::consume<jllvm::LocalIndexedOpCodes>(bytes),
                                         jllvm::consume<std::uint8_t>(bytes)};
        };
    }
}

std::optional<jllvm::PoolIndexedOp> consumePoolIndex(llvm::ArrayRef<char> bytes)
{
    auto opCode = static_cast<jllvm::PoolIndexedOpCodes>(bytes.front());
    switch (opCode)
    {
        default:
        {
            return std::nullopt;
        }
        case jllvm::PoolIndexedOpCodes::LDC:
        {
            return jllvm::PoolIndexedOp{jllvm::consume<jllvm::PoolIndexedOpCodes>(bytes),
                                        jllvm::consume<std::uint8_t>(bytes)};
        }
        case jllvm::PoolIndexedOpCodes::ANewArray:
        case jllvm::PoolIndexedOpCodes::CheckCast:
        case jllvm::PoolIndexedOpCodes::GetField:
        case jllvm::PoolIndexedOpCodes::GetStatic:
        case jllvm::PoolIndexedOpCodes::InstanceOf:
        case jllvm::PoolIndexedOpCodes::InvokeSpecial:
        case jllvm::PoolIndexedOpCodes::InvokeStatic:
        case jllvm::PoolIndexedOpCodes::InvokeVirtual:
        case jllvm::PoolIndexedOpCodes::LDCW:
        case jllvm::PoolIndexedOpCodes::LDC2W:
        case jllvm::PoolIndexedOpCodes::New:
        case jllvm::PoolIndexedOpCodes::PutField:
        case jllvm::PoolIndexedOpCodes::PutStatic:
        {
            return jllvm::PoolIndexedOp{jllvm::consume<jllvm::PoolIndexedOpCodes>(bytes),
                                        jllvm::consume<std::uint16_t>(bytes)};
        }
        case jllvm::PoolIndexedOpCodes::InvokeDynamic:
        case jllvm::PoolIndexedOpCodes::InvokeInterface:
        {
            opCode = jllvm::consume<jllvm::PoolIndexedOpCodes>(bytes);
            auto index = jllvm::consume<std::uint16_t>(bytes);
            auto count = jllvm::consume<std::uint8_t>(bytes);
            auto padding = jllvm::consume<std::uint8_t>(bytes);
            if (opCode == jllvm::PoolIndexedOpCodes::InvokeDynamic)
            {
                assert(count == 0);
            }
            else
            {
                assert(count != 0);
            }
            assert(padding == 0);
            return jllvm::PoolIndexedOp{opCode, index};
        }
    }
}

std::optional<jllvm::BranchOffsetOp> consumeBranchOffset(llvm::ArrayRef<char> bytes, std::size_t currentOffset)
{
    auto opCode = static_cast<jllvm::BranchOffsetOpCodes>(bytes.front());
    switch (opCode)
    {
        default:
        {
            return std::nullopt;
        }
        case jllvm::BranchOffsetOpCodes::Goto:
        case jllvm::BranchOffsetOpCodes::IfACmpEq:
        case jllvm::BranchOffsetOpCodes::IfACmpNe:
        case jllvm::BranchOffsetOpCodes::IfICmpEq:
        case jllvm::BranchOffsetOpCodes::IfICmpNe:
        case jllvm::BranchOffsetOpCodes::IfICmpLt:
        case jllvm::BranchOffsetOpCodes::IfICmpGe:
        case jllvm::BranchOffsetOpCodes::IfICmpGt:
        case jllvm::BranchOffsetOpCodes::IfICmpLe:
        case jllvm::BranchOffsetOpCodes::IfEq:
        case jllvm::BranchOffsetOpCodes::IfNe:
        case jllvm::BranchOffsetOpCodes::IfLt:
        case jllvm::BranchOffsetOpCodes::IfGe:
        case jllvm::BranchOffsetOpCodes::IfGt:
        case jllvm::BranchOffsetOpCodes::IfLe:
        case jllvm::BranchOffsetOpCodes::IfNonNull:
        case jllvm::BranchOffsetOpCodes::IfNull:
        case jllvm::BranchOffsetOpCodes::JSR:
        {
            return jllvm::BranchOffsetOp{jllvm::consume<jllvm::BranchOffsetOpCodes>(bytes),
                                         jllvm::consume<std::int16_t>(bytes), currentOffset};
        }
        case jllvm::BranchOffsetOpCodes::GotoW:
        case jllvm::BranchOffsetOpCodes::JSRw:
        {
            return jllvm::BranchOffsetOp{jllvm::consume<jllvm::BranchOffsetOpCodes>(bytes),
                                         jllvm::consume<std::int32_t>(bytes), currentOffset};
        }
    }
}

jllvm::ByteCodeOp jllvm::ByteCodeIterator::currentOp() const
{
    if (auto singleton = consumeSingleton(m_current))
    {
        return singleton.value();
    }

    if (auto localIndex = consumeLocalIndex(m_current))
    {
        return localIndex.value();
    }

    if (auto poolIndex = consumePoolIndex(m_current))
    {
        return poolIndex.value();
    }

    if (auto branchOffset = consumeBranchOffset(m_current, m_offset))
    {
        return branchOffset.value();
    }

    auto copy = m_current;

    switch (consume<SpecialOpCodes>(copy))
    {
        default: llvm_unreachable("Unknown opcode");
        case SpecialOpCodes::BIPush:
        {
            return BIPushOp{consume<std::int8_t>(copy)};
        }
        case SpecialOpCodes::NewArray:
        {
            return NewArrayOp{consume<NewArrayOp::ArrayType>(copy)};
        }
        case SpecialOpCodes::IInc:
        {
            return IIncOp{consume<std::uint8_t>(copy), consume<std::int8_t>(copy)};
        }
        case SpecialOpCodes::SIPush:
        {
            return SIPushOp{consume<std::int16_t>(copy)};
        }
        case SpecialOpCodes::LookupSwitch:
        case SpecialOpCodes::MultiANewArray:
        case SpecialOpCodes::TableSwitch:
        case SpecialOpCodes::Wide:
        {
            llvm_unreachable("NOT YET IMPLEMENTED");
        }
    }
}

std::size_t jllvm::ByteCodeIterator::currentOpSize() const
{
    switch (static_cast<OpCodes>(m_current.front()))
    {
        default: llvm_unreachable("Unknown opcode");
        case OpCodes::AALoad:
        case OpCodes::AAStore:
        case OpCodes::AConstNull:
        case OpCodes::ALoad0:
        case OpCodes::ALoad1:
        case OpCodes::ALoad2:
        case OpCodes::ALoad3:
        case OpCodes::AReturn:
        case OpCodes::ArrayLength:
        case OpCodes::AStore0:
        case OpCodes::AStore1:
        case OpCodes::AStore2:
        case OpCodes::AStore3:
        case OpCodes::AThrow:
        case OpCodes::BALoad:
        case OpCodes::BAStore:
        case OpCodes::CALoad:
        case OpCodes::CAStore:
        case OpCodes::D2F:
        case OpCodes::D2I:
        case OpCodes::D2L:
        case OpCodes::DAdd:
        case OpCodes::DALoad:
        case OpCodes::DAStore:
        case OpCodes::DCmpG:
        case OpCodes::DCmpL:
        case OpCodes::DConst0:
        case OpCodes::DConst1:
        case OpCodes::DDiv:
        case OpCodes::DLoad0:
        case OpCodes::DLoad1:
        case OpCodes::DLoad2:
        case OpCodes::DLoad3:
        case OpCodes::DMul:
        case OpCodes::DNeg:
        case OpCodes::DRem:
        case OpCodes::DReturn:
        case OpCodes::DStore0:
        case OpCodes::DStore1:
        case OpCodes::DStore2:
        case OpCodes::DStore3:
        case OpCodes::DSub:
        case OpCodes::Dup:
        case OpCodes::DupX1:
        case OpCodes::DupX2:
        case OpCodes::Dup2:
        case OpCodes::Dup2X1:
        case OpCodes::Dup2X2:
        case OpCodes::F2D:
        case OpCodes::F2I:
        case OpCodes::F2L:
        case OpCodes::FAdd:
        case OpCodes::FALoad:
        case OpCodes::FAStore:
        case OpCodes::FCmpG:
        case OpCodes::FCmpL:
        case OpCodes::FConst0:
        case OpCodes::FConst1:
        case OpCodes::FConst2:
        case OpCodes::FDiv:
        case OpCodes::FLoad0:
        case OpCodes::FLoad1:
        case OpCodes::FLoad2:
        case OpCodes::FLoad3:
        case OpCodes::FMul:
        case OpCodes::FNeg:
        case OpCodes::FRem:
        case OpCodes::FReturn:
        case OpCodes::FStore0:
        case OpCodes::FStore1:
        case OpCodes::FStore2:
        case OpCodes::FStore3:
        case OpCodes::FSub:
        case OpCodes::I2B:
        case OpCodes::I2C:
        case OpCodes::I2D:
        case OpCodes::I2F:
        case OpCodes::I2L:
        case OpCodes::I2S:
        case OpCodes::IAdd:
        case OpCodes::IALoad:
        case OpCodes::IAnd:
        case OpCodes::IAStore:
        case OpCodes::IConstM1:
        case OpCodes::IConst0:
        case OpCodes::IConst1:
        case OpCodes::IConst2:
        case OpCodes::IConst3:
        case OpCodes::IConst4:
        case OpCodes::IConst5:
        case OpCodes::IDiv:
        case OpCodes::ILoad0:
        case OpCodes::ILoad1:
        case OpCodes::ILoad2:
        case OpCodes::ILoad3:
        case OpCodes::IMul:
        case OpCodes::INeg:
        case OpCodes::IOr:
        case OpCodes::IRem:
        case OpCodes::IReturn:
        case OpCodes::IShl:
        case OpCodes::IShr:
        case OpCodes::IStore0:
        case OpCodes::IStore1:
        case OpCodes::IStore2:
        case OpCodes::IStore3:
        case OpCodes::ISub:
        case OpCodes::IUShr:
        case OpCodes::IXor:
        case OpCodes::L2D:
        case OpCodes::L2F:
        case OpCodes::L2I:
        case OpCodes::LAdd:
        case OpCodes::LALoad:
        case OpCodes::LAnd:
        case OpCodes::LAStore:
        case OpCodes::LCmp:
        case OpCodes::LConst0:
        case OpCodes::LConst1:
        case OpCodes::LDiv:
        case OpCodes::LLoad0:
        case OpCodes::LLoad1:
        case OpCodes::LLoad2:
        case OpCodes::LLoad3:
        case OpCodes::LMul:
        case OpCodes::LNeg:
        case OpCodes::LOr:
        case OpCodes::LRem:
        case OpCodes::LReturn:
        case OpCodes::LShl:
        case OpCodes::LShr:
        case OpCodes::LStore0:
        case OpCodes::LStore1:
        case OpCodes::LStore2:
        case OpCodes::LStore3:
        case OpCodes::LSub:
        case OpCodes::LUShr:
        case OpCodes::LXor:
        case OpCodes::MonitorEnter:
        case OpCodes::MonitorExit:
        case OpCodes::Nop:
        case OpCodes::Pop:
        case OpCodes::Pop2:
        case OpCodes::Return:
        case OpCodes::SALoad:
        case OpCodes::SAStore:
        case OpCodes::Swap: return 1;

        case OpCodes::ALoad:
        case OpCodes::AStore:
        case OpCodes::BIPush:
        case OpCodes::DLoad:
        case OpCodes::DStore:
        case OpCodes::FLoad:
        case OpCodes::FStore:
        case OpCodes::ILoad:
        case OpCodes::IStore:
        case OpCodes::LDC:
        case OpCodes::LLoad:
        case OpCodes::LStore:
        case OpCodes::NewArray:
        case OpCodes::Ret: return 2;

        case OpCodes::ANewArray:
        case OpCodes::CheckCast:
        case OpCodes::GetField:
        case OpCodes::GetStatic:
        case OpCodes::Goto:
        case OpCodes::IfACmpEq:
        case OpCodes::IfACmpNe:
        case OpCodes::IfICmpEq:
        case OpCodes::IfICmpNe:
        case OpCodes::IfICmpLt:
        case OpCodes::IfICmpGe:
        case OpCodes::IfICmpGt:
        case OpCodes::IfICmpLe:
        case OpCodes::IfEq:
        case OpCodes::IfNe:
        case OpCodes::IfLt:
        case OpCodes::IfGe:
        case OpCodes::IfGt:
        case OpCodes::IfLe:
        case OpCodes::IfNonNull:
        case OpCodes::IfNull:
        case OpCodes::IInc:
        case OpCodes::InstanceOf:
        case OpCodes::InvokeSpecial:
        case OpCodes::InvokeStatic:
        case OpCodes::InvokeVirtual:
        case OpCodes::JSR:
        case OpCodes::LDCW:
        case OpCodes::LDC2W:
        case OpCodes::New:
        case OpCodes::PutField:
        case OpCodes::PutStatic:
        case OpCodes::SIPush: return 3;

        case OpCodes::GotoW:
        case OpCodes::InvokeDynamic:
        case OpCodes::InvokeInterface:
        case OpCodes::JSRw: return 5;

        case OpCodes::LookupSwitch:
        {
            std::uint64_t padding = llvm::offsetToAlignedAddr(m_current.drop_front().data(), llvm::Align(4));
            auto pairCountPtr = m_current.drop_front(5 + padding);
            auto pairCount = consume<std::uint32_t>(pairCountPtr);
            return 1 + padding + 4 + 8 * pairCount;
        }

        case OpCodes::MultiANewArray: return 4;

        case OpCodes::TableSwitch:
        {
            std::uint64_t padding = llvm::offsetToAlignedAddr(m_current.drop_front().data(), llvm::Align(4));
            auto padded = m_current.drop_front(5 + padding);
            auto lowByte = consume<std::uint32_t>(padded);
            auto highByte = consume<std::uint32_t>(padded);

            return 1 + padding + 4 + 4 + 4 + (highByte - lowByte + 1) * 4;
        }
        case OpCodes::Wide: return static_cast<OpCodes>(m_current[1]) == OpCodes::IInc ? 6 : 4;
    }
}
