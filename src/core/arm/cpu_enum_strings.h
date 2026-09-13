#pragma once

#include <nxemu-module-spec/cpu.h>
#include <string_view>

const char* CpuBackendToString(CpuBackend value);
CpuBackend CpuBackendFromString(std::string_view str);

const char* CpuAccuracyToString(CpuAccuracy value);
CpuAccuracy CpuAccuracyFromString(std::string_view str);
