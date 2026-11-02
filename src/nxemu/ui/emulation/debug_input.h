// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <sstream>
#include <string>

// One bounded, nonblocking sequence. UI loop time, not guest input latency.
class DebugInput {
public:
    bool Parse(const std::string& text) {
        std::istringstream in(text); std::string trailing;
        unsigned p{},b{},hold{},gap{},n{};
        if (!(in >> p >> b >> hold >> gap >> n) || (in >> trailing) ||
            p > 9 || b > 19 || hold < 20 || hold > 2000 || gap < 20 || gap > 2000 || !n || n > 20)
            return false;
        if (active) return false;
        player=p; button=b; hold_ms=hold; gap_ms=gap; count=n; return true;
    }
    template<class Send> void Start(uint64_t request,uint64_t gen,uint64_t now,Send&& send) {
        request_id=request; generation=gen; active=true; down=true; completed=0;
        pressed_seen=0; released_seen=0; send(player,button,true); ++presses;
        due=now+hold_ms; status="running";
    }
    template<class Send> void Advance(uint64_t now,bool usable,uint64_t gen,Send&& send) {
        if (!active) return;
        if (!usable || gen!=generation) {
            // Never access a new or tearing-down module to release the old one.
            if (usable && gen==generation && down) send(player,button,false);
            active=false;down=false;status="cancelled";return;
        }
        if(now<due)return;
        if(down) {
            send(player,button,false);++releases;down=false;++completed;
            if(completed==count){active=false;status="completed";return;}
            due=now+gap_ms;
        }else{
            send(player,button,true);++presses;down=true;due=now+hold_ms;
        }
    }
    template<class Send> void Cancel(bool usable,uint64_t gen,Send&& send) {
        if (!active) return;
        if (usable && gen==generation && down) { send(player,button,false); ++releases; }
        active=false;down=false;status="cancelled";
    }
    void Observe(bool value) { if(value)++pressed_seen;else ++released_seen; }
    uint32_t player{},button{},hold_ms{},gap_ms{},count{},completed{};
    uint64_t request_id{},generation{},due{},presses{},releases{},pressed_seen{},released_seen{};
    bool active{},down{};
    const char* status="idle";
};
