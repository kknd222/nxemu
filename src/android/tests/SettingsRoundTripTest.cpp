#include <nxemu-module-spec/base.h>
#include <nxemu-video/video_settings.h>
#include <nxemu-video/video_settings_identifiers.h>
#include <nxemu-os/os_settings.h>
#include <nxemu-os/os_settings_identifiers.h>
#include <yuzu_common/logging/log.h>
#include <common/json.h>
#include <cassert>
#include <map>
#include <vector>
#include <algorithm>
#include <stdexcept>
#include <iostream>

void assert_fail_impl() { throw std::runtime_error("native settings assertion"); }
[[noreturn]] void unreachable_impl() { throw std::runtime_error("unreachable"); }
namespace Common::Log {
LogLevel g_classLevel[(uint8_t)LogClass::Count]{};
void FmtLogMessageImpl(LogClass, LogLevel, const char *, unsigned int, const char *, const char *, const fmt::format_args &) {}
}
class Store : public IModuleSettings {
    struct Callback { SettingChangeCallback fn; void * data; };
    std::map<std::string,std::string> strings, sections;
    std::map<std::string,int32_t> ints;
    std::map<std::string,bool> bools;
    std::map<std::string,float> floats;
    std::map<std::string,std::vector<Callback>> callbacks;
    void notify(const char * key) { for (auto cb : callbacks[key]) cb.fn(key,cb.data); }
public:
    const char * GetString(const char * k) const override { auto i=strings.find(k); return i==strings.end()?"":i->second.c_str(); }
    bool GetBool(const char * k) const override { auto i=bools.find(k); return i!=bools.end()&&i->second; }
    int32_t GetInt(const char * k) const override { auto i=ints.find(k); return i==ints.end()?0:i->second; }
    float GetFloat(const char * k) const override { auto i=floats.find(k); return i==floats.end()?0:i->second; }
    void SetString(const char * k,const char * v) override { strings[k]=v; notify(k); }
    void SetBool(const char * k,bool v) override { bools[k]=v; notify(k); }
    void SetInt(const char * k,int32_t v) override { ints[k]=v; notify(k); }
    void SetFloat(const char * k,float v) override { floats[k]=v; notify(k); }
    void SetDefaultBool(const char *,bool) override {}
    void SetDefaultInt(const char *,int) override {}
    void SetDefaultFloat(const char *,float) override {}
    void SetDefaultString(const char *,const char *) override {}
    const char * GetSectionSettings(const char * k) const override { auto i=sections.find(k); return i==sections.end()?"":i->second.c_str(); }
    void SetSectionSettings(const char * k,const char * v) override { sections[k]=v; }
    void RegisterCallback(const char * k,SettingChangeCallback fn,void * data) override { callbacks[k].push_back({fn,data}); }
    void UnregisterCallback(const char * k,SettingChangeCallback fn,void * data) override {
        auto & c=callbacks[k]; c.erase(std::remove_if(c.begin(),c.end(),[&](auto cb){return cb.fn==fn&&cb.data==data;}),c.end());
    }
};
IModuleSettings * g_settings;
int main() {
    Store store; g_settings=&store;
    SetupVideoSetting(); SetupOsSetting();
    for (int resolution : {0,1,2,3,4}) {
        store.SetInt(NXVideoSetting::ResolutionSetup,resolution);
        store.SetInt(NXOsSetting::LanguageIndex,15);
        store.SetInt(NXOsSetting::RegionIndex,4);
        store.SetBool(NXOsSetting::VibrationEnabled,false);
        SaveVideoSettings(); SaveOsSettings();
        CleanupVideoSetting(); CleanupOsSetting();
        SetupVideoSetting(); SetupOsSetting();
        const float expected[]={0.5f,0.75f,1.0f,1.5f,2.0f};
        assert(videoSettings.resolution_setup==static_cast<ResolutionSetup>(resolution));
        assert(videoSettings.resolution_info.up_factor==expected[resolution]);
        assert(store.GetFloat(NXVideoSetting::ResolutionUpFactor)==expected[resolution]);
        assert(osSettings.language_index==Language::ChineseSimplified);
        assert(osSettings.region_index==Region::China);
        assert(!osSettings.vibration_enabled);
    }
    // Returning to defaults must remove old non-default values in persisted sections.
    store.SetInt(NXOsSetting::LanguageIndex,1); store.SetInt(NXOsSetting::RegionIndex,1);
    store.SetBool(NXOsSetting::VibrationEnabled,true);
    SaveOsSettings(); CleanupOsSetting(); SetupOsSetting();
    assert(osSettings.language_index==Language::EnglishAmerican);
    assert(osSettings.region_index==Region::Usa && osSettings.vibration_enabled);
    CleanupVideoSetting(); CleanupOsSetting();
    std::cout << "PASS: resolution 0.5/0.75/1/1.5/2x, Chinese, China region and vibration persist across module recreation; defaults restore\n";
}
