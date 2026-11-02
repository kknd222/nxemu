// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>
namespace Common {
// Called under the existing NPad shared-memory lock. No input/fence semantics change.
class HidWriteTrace {
public:
    void BeginTick(){++tick;}
    void Record(size_t slot,uint64_t aruid,uint32_t id,uint32_t style,uint64_t mask,uint64_t sample,bool written,uint32_t skip=0){
        if(slot>=rows.size()){++overflow;return;}
        auto& r=rows[slot];if(r.aruid!=aruid)r={};r.valid=true;r.aruid=aruid;r.id=id;r.style=style;r.tick=tick;r.skip=skip;
        if(written){++r.writes;if((mask&1)&&!(r.mask&1))++r.a_pressed;if(!(mask&1)&&(r.mask&1))++r.a_released;r.mask=mask;r.sample=sample;}
        else ++r.skipped;
    }
    std::string Snapshot() const {
        std::ostringstream s;s<<"{\"tick\":"<<tick<<",\"overflow\":"<<overflow<<",\"guest_consumption_verified\":null,\"rows\":[";
        bool comma=false;
        for(const auto& r:rows){if(!r.valid)continue;if(comma)s<<',';comma=true;
            s<<"{\"aruid\":"<<r.aruid<<",\"npad_id\":"<<r.id<<",\"style\":"<<r.style<<",\"current_tick\":"<<(r.tick==tick?"true":"false")
             <<",\"writes\":"<<r.writes<<",\"skipped\":"<<r.skipped<<",\"last_skip\":"<<r.skip<<",\"buttons_mask\":"<<r.mask
             <<",\"sampling_number\":"<<r.sample<<",\"a_pressed_edges\":"<<r.a_pressed<<",\"a_released_edges\":"<<r.a_released<<"}";
        }s<<"]}";return s.str();
    }
    void Publish();
private:
    struct Row {bool valid{};uint64_t aruid{},tick{},writes{},skipped{},mask{},sample{},a_pressed{},a_released{};uint32_t id{},style{},skip{};};
    std::array<Row,160> rows{};uint64_t tick{},overflow{};std::chrono::steady_clock::time_point last{};
};
}
