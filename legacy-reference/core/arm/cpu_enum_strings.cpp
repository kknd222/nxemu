#include "cpu_enum_strings.h"

const char* CpuBackendToString(CpuBackend value)
{
    switch (value) {
    case CpuBackend::Dynarmic: return "Dynarmic";
    case CpuBackend::Nce: return "Nce";
    }
    return "Dynarmic";
}

CpuBackend CpuBackendFromString(std::string_view str)
{
    if (str == "Dynarmic") return CpuBackend::Dynarmic;
    if (str == "Nce") return CpuBackend::Nce;
    return CpuBackend::Dynarmic;
}

const char* CpuAccuracyToString(CpuAccuracy value)
{
    switch (value) {
    case CpuAccuracy::Auto: return "Auto";
    case CpuAccuracy::Accurate: return "Accurate";
    case CpuAccuracy::Unsafe: return "Unsafe";
    case CpuAccuracy::Paranoid: return "Paranoid";
    }
    return "Auto";
}

CpuAccuracy CpuAccuracyFromString(std::string_view str)
{
    if (str == "Auto") return CpuAccuracy::Auto;
    if (str == "Accurate") return CpuAccuracy::Accurate;
    if (str == "Unsafe") return CpuAccuracy::Unsafe;
    if (str == "Paranoid") return CpuAccuracy::Paranoid;
    return CpuAccuracy::Auto;
}
