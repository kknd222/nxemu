#include "exclusive_monitor_interface.h"
#include <yuzu_common/common_types.h>

ExclusiveMonitor::ExclusiveMonitor(IMemory & memory, Dynarmic::ExclusiveMonitor & monitor) :
    m_memory(memory),
    m_monitor(monitor)
{
}

ExclusiveMonitor::~ExclusiveMonitor()
{
}

uint8_t ExclusiveMonitor::ExclusiveRead8(uint32_t coreIndex, uint64_t addr)
{
    return m_monitor.ReadAndMark<uint8_t>(coreIndex, addr, [&]() -> uint8_t { return m_memory.Read8(addr); });
}

uint16_t ExclusiveMonitor::ExclusiveRead16(uint32_t coreIndex, uint64_t addr)
{
    return m_monitor.ReadAndMark<uint16_t>(coreIndex, addr, [&]() -> uint16_t { return m_memory.Read16(addr); });
}

uint32_t ExclusiveMonitor::ExclusiveRead32(uint32_t coreIndex, uint64_t addr)
{
    return m_monitor.ReadAndMark<uint32_t>(coreIndex, addr, [&]() -> uint32_t { return m_memory.Read32(addr); });
}

uint64_t ExclusiveMonitor::ExclusiveRead64(uint32_t coreIndex, uint64_t addr)
{
    return m_monitor.ReadAndMark<uint64_t>(coreIndex, addr, [&]() -> uint64_t { return m_memory.Read64(addr); });
}

void ExclusiveMonitor::ExclusiveRead128(uint32_t coreIndex, uint64_t addr, uint64_t & outHigh, uint64_t & outLow)
{
    u128 result = m_monitor.ReadAndMark<u128>(coreIndex, addr, [&]() -> u128 {
        u128 sresult;
        sresult[0] = m_memory.Read64(addr);
        sresult[1] = m_memory.Read64(addr + 8);
        return sresult;
    });
    outLow = result[0];
    outHigh = result[1];
}

void ExclusiveMonitor::ClearExclusive(uint32_t coreIndex)
{
    m_monitor.ClearProcessor(coreIndex);
}

bool ExclusiveMonitor::ExclusiveWrite8(uint32_t coreIndex, uint64_t addr, uint8_t value)
{
    return m_monitor.DoExclusiveOperation<uint8_t>(coreIndex, addr, [&](uint8_t expected) -> bool {
        return m_memory.WriteExclusive8(addr, value, expected);
    });
}

bool ExclusiveMonitor::ExclusiveWrite16(uint32_t coreIndex, uint64_t addr, uint16_t value)
{
    return m_monitor.DoExclusiveOperation<uint16_t>(coreIndex, addr, [&](uint16_t expected) -> bool {
        return m_memory.WriteExclusive16(addr, value, expected);
    });
}

bool ExclusiveMonitor::ExclusiveWrite32(uint32_t coreIndex, uint64_t addr, uint32_t value)
{
    return m_monitor.DoExclusiveOperation<uint32_t>(coreIndex, addr, [&](uint32_t expected) -> bool {
        return m_memory.WriteExclusive32(addr, value, expected);
    });
}

bool ExclusiveMonitor::ExclusiveWrite64(uint32_t coreIndex, uint64_t addr, uint64_t value)
{
    return m_monitor.DoExclusiveOperation<uint64_t>(coreIndex, addr, [&](uint64_t expected) -> bool {
        return m_memory.WriteExclusive64(addr, value, expected);
    });
}

bool ExclusiveMonitor::ExclusiveWrite128(uint32_t coreIndex, uint64_t addr, uint64_t valueHigh, uint64_t valueLow)
{
    return m_monitor.DoExclusiveOperation<u128>(coreIndex, addr, [&](u128 expected) -> bool {
        return m_memory.WriteExclusive128(addr, valueHigh, valueLow, expected[0], expected[1]);
    });
}

void ExclusiveMonitor::Release()
{
    delete this;
}
