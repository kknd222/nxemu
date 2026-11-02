// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <vector>
#include "yuzu_common/common_types.h"
namespace Service::Nvidia::Devices {
class Nx2GpuMemoryRegistry {
public:
 using KindListener=std::function<bool(u64,u64,u64,u8)>;
 using UnmapListener=std::function<bool(u64,u64)>;
 using Listener=std::function<bool(u64,u64,u64)>;
 static Nx2GpuMemoryRegistry& Get(){static Nx2GpuMemoryRegistry r;return r;}
 bool Register(u64 addr,u64 size){return MapAt(addr,addr,size);}
 bool MapAt(u64 gpu,u64 cpu,u64 size){std::scoped_lock lk{mutex};bool ok=true;for(auto&[owner,cb]:listeners)ok=cb(gpu,cpu,size)&&ok;if(ok)mappings.push_back({gpu,cpu,size});return ok;}
 void AddListener(const void*owner,Listener cb){std::scoped_lock lk{mutex};for(auto&m:mappings)cb(m.gpu,m.cpu,m.size);listeners[owner]=std::move(cb);}
 void AddOperations(const void* owner,KindListener kind,UnmapListener unmap){std::scoped_lock lk{mutex};kinds[owner]=std::move(kind);unmaps[owner]=std::move(unmap);}
 bool ModifyKind(u64 gpu,u64 size,u8 kind){
  std::scoped_lock lk{mutex};if(size==0 || gpu+size<gpu || kinds.empty())return false;
  for(auto it=mappings.rbegin();it!=mappings.rend();++it){if(gpu>=it->gpu && gpu-it->gpu<=it->size && size<=it->size-(gpu-it->gpu)){
   bool ok=true;for(auto&[owner,cb]:kinds)ok=cb(gpu,it->cpu+(gpu-it->gpu),size,kind)&&ok;return ok;
  }}return false;
 }
 bool IsMapped(u64 gpu,u64 size){std::scoped_lock lk{mutex};for(const auto&m:mappings)if(gpu>=m.gpu && gpu-m.gpu<m.size && size<=m.size-(gpu-m.gpu))return true;return false;}
 bool Unmap(u64 gpu,u64 size){
  std::scoped_lock lk{mutex};if(size==0 || gpu+size<gpu || unmaps.empty())return false;
  bool covered=false;for(const auto&m:mappings)if(gpu>=m.gpu && gpu-m.gpu<=m.size && size<=m.size-(gpu-m.gpu)){covered=true;break;}
  if(!covered)return false;
  bool ok=true;for(auto&[owner,cb]:unmaps)ok=cb(gpu,size)&&ok;
  if(ok){std::vector<Mapping> next;for(const auto&m:mappings){const u64 end=m.gpu+m.size,unmap_end=gpu+size;if(m.gpu>=unmap_end || end<=gpu){next.push_back(m);continue;}if(m.gpu<gpu)next.push_back({m.gpu,m.cpu,gpu-m.gpu});if(end>unmap_end)next.push_back({unmap_end,m.cpu+unmap_end-m.gpu,end-unmap_end});}mappings=std::move(next);}return ok;
 }
 void RemoveListener(const void*owner){std::scoped_lock lk{mutex};listeners.erase(owner);kinds.erase(owner);unmaps.erase(owner);if(listeners.empty()){mappings.clear();std::scoped_lock device_lk{device_mutex};devices.clear();}}
 void RecordDeviceRange(u64 cpu,u64 size,u64 device){std::scoped_lock lk{device_mutex};devices[cpu]={size,device};}
 std::optional<u64> DeviceAddressOf(u64 cpu){std::scoped_lock lk{device_mutex};auto it=devices.upper_bound(cpu);if(it==devices.begin())return {};--it;if(cpu-it->first>=it->second.first)return {};return it->second.second+cpu-it->first;}
private:
 struct Mapping{u64 gpu,cpu,size;};std::mutex mutex,device_mutex;std::vector<Mapping>mappings;std::map<const void*,Listener>listeners;std::map<const void*,KindListener>kinds;std::map<const void*,UnmapListener>unmaps;std::map<u64,std::pair<u64,u64>>devices;
};
}
