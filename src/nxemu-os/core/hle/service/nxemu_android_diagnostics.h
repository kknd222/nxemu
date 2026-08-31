#pragma once

#include <string>

namespace Service::NxemuAndroidDiagnostics {

void RecordEvent(const char* category, const std::string& detail);
void RecordEvent(const char* category, const char* detail);
std::string Snapshot();
void Clear();
void SetFullTrace(bool enabled);
bool IsFullTraceEnabled();
void SetSvcRingEnabled(bool enabled);
bool IsSvcRingEnabled();

} // namespace Service::NxemuAndroidDiagnostics

extern "C" const char* NxemuGetLastOsServiceDiagnostics();
