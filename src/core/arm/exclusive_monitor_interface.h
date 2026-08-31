#pragma once
#include <nxemu-module-spec/cpu.h>
#include "dynarmic/interface/exclusive_monitor.h"

nxinterface IMemory;

class ExclusiveMonitor :
    public IExclusiveMonitor
{
public:
    ExclusiveMonitor(IMemory & memory, Dynarmic::ExclusiveMonitor & monitor);
    virtual ~ExclusiveMonitor();

    // IExclusiveMonitor
    uint8_t ExclusiveRead8(uint32_t coreIndex, uint64_t addr) override;
    uint16_t ExclusiveRead16(uint32_t coreIndex, uint64_t addr) override;
    uint32_t ExclusiveRead32(uint32_t coreIndex, uint64_t addr) override;
    uint64_t ExclusiveRead64(uint32_t coreIndex, uint64_t addr) override;
    void ExclusiveRead128(uint32_t coreIndex, uint64_t addr, uint64_t & outHigh, uint64_t & outLow) override;
    void ClearExclusive(uint32_t coreIndex) override;

    bool ExclusiveWrite8(uint32_t coreIndex, uint64_t addr, uint8_t value) override;
    bool ExclusiveWrite16(uint32_t coreIndex, uint64_t addr, uint16_t value) override;
    bool ExclusiveWrite32(uint32_t coreIndex, uint64_t addr, uint32_t value) override;
    bool ExclusiveWrite64(uint32_t coreIndex, uint64_t addr, uint64_t value) override;
    bool ExclusiveWrite128(uint32_t coreIndex, uint64_t addr, uint64_t valueHigh, uint64_t valueLow) override;

    void Release() override;

private:
    ExclusiveMonitor() = delete;
    ExclusiveMonitor(const ExclusiveMonitor &) = delete;
    ExclusiveMonitor & operator=(const ExclusiveMonitor &) = delete;

    IMemory & m_memory;
    Dynarmic::ExclusiveMonitor & m_monitor;
};