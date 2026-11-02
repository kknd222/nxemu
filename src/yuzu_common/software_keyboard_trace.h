// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "debug_document.h"

namespace Common {
// Last lifecycle event, not a periodic heartbeat or proof of visible keyboard UI.
// No text content is recorded. Fixed state; history uses the existing bounded ring.
class SoftwareKeyboardTrace {
public:
    SoftwareKeyboardTrace() : instance_id(NextId()) {}
    void Begin(bool inlined, bool automatic, uint32_t min, uint32_t max) {
        std::lock_guard lock(mutex);inline_mode=inlined;auto_frontend=automatic;
        min_length=min;max_length=max;active=true;Publish("initialized");
    }
    void Event(const char* stage,bool complete,bool shown=false,bool submitted=false,
               uint32_t units=0,uint32_t result=0,bool confirmed=false,bool ignored=false) {
        std::lock_guard lock(mutex);
        if(shown)++shown_count;
        if(submitted) {++submit_count;last_units=units;last_result=result;last_confirmed=confirmed;}
        if(ignored)++ignored_count;
        active=!complete && std::string_view(stage)!="destroyed";
        Publish(stage);
    }
    std::string Snapshot() const {std::lock_guard lock(mutex);return Payload(last_stage);}
private:
    static uint64_t NextId(){static std::atomic<uint64_t> id{};return ++id;}
    std::string Payload(const char* stage) const {
        std::ostringstream out;
        out<<"{\"instance_id\":"<<instance_id<<",\"observation_kind\":\"last_lifecycle_event\",\"stage\":"<<DebugDocument::Quote(stage)
           <<",\"mode\":\""<<(inline_mode?"inline":"normal")<<"\",\"frontend_kind\":\""<<(auto_frontend?"default_auto_stub":"external_frontend")
           <<"\",\"visible_ui_verified\":null,\"focus_verified\":null,\"active_at_event\":"<<(active?"true":"false")
           <<",\"min_length\":"<<min_length<<",\"max_length\":"<<max_length<<",\"shown_count\":"<<shown_count
           <<",\"submit_count\":"<<submit_count<<",\"ignored_completed_callbacks\":"<<ignored_count
           <<",\"last_submit_units\":"<<last_units<<",\"last_result\":"<<last_result
           <<",\"last_confirmed\":"<<(last_confirmed?"true":"false")<<"}";
        return out.str();
    }
    void Publish(const char* stage){last_stage=stage;DebugDocument::Publish("software-keyboard",Payload(stage));}
    mutable std::mutex mutex;
    const uint64_t instance_id;
    const char* last_stage="not_initialized";
    bool inline_mode{},auto_frontend{},active{},last_confirmed{};
    uint32_t min_length{},max_length{},last_units{},last_result{};
    uint64_t shown_count{},submit_count{},ignored_count{};
};
}
