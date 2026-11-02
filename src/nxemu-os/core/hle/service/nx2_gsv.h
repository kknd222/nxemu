// SPDX-FileCopyrightText: Copyright 2026 Eden Emulator Project
// SPDX-License-Identifier: GPL-3.0-or-later
#pragma once
#include <array>
#include <map>
#include <mutex>
#include <span>
#include <vector>
#include <thread>
#include "core/perf_stats.h"
#include "core/hle/service/nvdrv/devices/native_gpu_kind.h"
#include "yuzu_common/alignment.h"
#include "core/core.h"
#include "core/core_timing.h"
#include "core/hle/kernel/k_client_session.h"
#include "core/hle/kernel/k_process.h"
#include "core/hle/kernel/k_resource_limit.h"
#include "core/hle/kernel/k_server_session.h"
#include "core/hle/kernel/k_session.h"
#include "core/hle/kernel/k_event.h"
#include "core/hle/kernel/k_transfer_memory.h"
#include "core/hle/service/ipc_helpers.h"
#include "core/hle/service/nx2_ipc_layout.h"
#include "core/hle/service/kernel_helpers.h"
#include "core/hle/service/server_manager.h"
#include "core/hle/service/nx2_stub_interface.h"
#include "core/hle/service/nvdrv/nvdrv_interface.h"
#include "core/hle/service/sm/sm.h"
#include <nxemu-module-spec/video.h>
#include "core/hle/service/nvdrv/devices/nx2_gpu_memory_registry.h"
namespace Service {
// Creates a session served by `iface` and returns its client end. Unlike PushIpcInterface this
// works from a domain too, for callers that want a raw session handle.
template <class T>
inline Kernel::KClientSession* CreateRawSession(Core::System& system, HLERequestContext& ctx,
                                                std::shared_ptr<T> iface) {
    auto& kernel = system.Kernel();
    ASSERT(Kernel::GetCurrentProcess(kernel).GetResourceLimit()->Reserve(
        Kernel::LimitableResource::SessionCountMax, 1));

    auto* session = Kernel::KSession::Create(kernel);
    session->Initialize(nullptr, 0);
    Kernel::KSession::Register(kernel, session);

    auto manager =
        std::make_shared<SessionRequestManager>(kernel, ctx.GetManager()->GetServerManager());
    manager->SetSessionHandler(std::move(iface));
    ctx.GetManager()->GetServerManager().RegisterSession(&session->GetServerSession(), manager);
    return &session->GetClientSession();
}

// Replies to a TIPC request with success and raw u32 data words. ResponseBuilder::Push(Result)
// writes the CMIF two-word result, which shifts TIPC data one word past where the client reads
// it (and past the copied size); TIPC has a one-word result followed directly by the data.
inline void TipcReplyWords(HLERequestContext& ctx, std::span<const u32> words) {
    IPC::ResponseBuilder rb{ctx, static_cast<u32>(2 + words.size())};
    rb.PushRaw<u32>(ResultSuccess.raw);
    for (const u32 w : words) {
        rb.Push<u32>(w);
    }
}

inline void TipcReplyWords(HLERequestContext& ctx, std::initializer_list<u32> words) {
    TipcReplyWords(ctx, std::span<const u32>{words.begin(), words.size()});
}

// Replies with success and a copied handle to a new session served by `iface`.
template <class T>
inline void ReturnRawSession(Core::System& system, HLERequestContext& ctx, std::shared_ptr<T> iface) {
    auto* client = CreateRawSession(system, ctx, std::move(iface));
    IPC::ResponseBuilder rb{ctx, 2, 1, 0};
    if (ctx.IsTipc()) rb.PushRaw<u32>(ResultSuccess.raw);
    else rb.Push(ResultSuccess);
    rb.PushCopyObjects( *client);
}

// Syncpoint events use the modular video API. Timing callbacks are drained before destruction.
class INx2SyncptSession final : public ServiceFramework<INx2SyncptSession> {
public:
 explicit INx2SyncptSession(Core::System& sys):ServiceFramework{sys,"nx2:syncpt"}, context{sys,"nx2:syncpt"} {
  static const FunctionInfo functions[]={{0,&INx2SyncptSession::Handshake,"Handshake"},{1,&INx2SyncptSession::Count,"GetCount"},{22,&INx2SyncptSession::GetEvent,"GetSlotEvent"},{103,&INx2SyncptSession::Query,"QueryCounter"},{201,&INx2SyncptSession::Arm,"RequestSlotEvent"},{202,&INx2SyncptSession::Cancel,"CancelSlotEvent"}};
  RegisterHandlersTipc(functions);
  poll=Core::Timing::CreateEvent("nx2:syncpt:Poll",[this](s64,std::chrono::nanoseconds)->std::optional<std::chrono::nanoseconds>{Poll();return std::nullopt;});
  system.CoreTiming().ScheduleLoopingEvent(std::chrono::microseconds{500},std::chrono::microseconds{500},poll);
 }
 ~INx2SyncptSession() override {system.CoreTiming().UnscheduleEvent(poll);for(auto& [slot,event]:events)context.CloseEvent(event);}
private:
 struct Request {u32 id{};u64 value{};u32 mode{};bool pending{};};
 Kernel::KEvent* Event(u32 slot){auto*& e=events[slot];if(!e)e=context.CreateEvent(fmt::format("nx2:syncpt:Slot{}",slot));return e;}
 void Handshake(HLERequestContext& ctx){const auto*w=ctx.CommandBuffer();TipcReplyWords(ctx,{w[2],w[3]});}
 void Count(HLERequestContext& ctx){TipcReplyWords(ctx,{32});}
 void Query(HLERequestContext& ctx){const u32 id=~ctx.CommandBuffer()[2];const u64 value=id<192?system.GetVideo().HostSyncpointValue(id):0;TipcReplyWords(ctx,{u32(value),u32(value>>32),0});}
 void GetEvent(HLERequestContext& ctx){std::scoped_lock lk{mutex};IPC::ResponseBuilder rb{ctx,2,1,0};rb.PushRaw<u32>(ResultSuccess.raw);rb.PushCopyObjects(Event(ctx.CommandBuffer()[2])->GetReadableEvent());}
 void Arm(HLERequestContext& ctx){std::scoped_lock lk{mutex};const auto*w=ctx.CommandBuffer();Event(w[2])->Clear();requests[w[2]]={~w[3],u64(w[4])|(u64(w[5])<<32),w[6],true};TipcReplyWords(ctx,{});}
 void Cancel(HLERequestContext& ctx){std::scoped_lock lk{mutex};requests[ctx.CommandBuffer()[2]].pending=false;TipcReplyWords(ctx,{});}
 void Poll(){std::scoped_lock lk{mutex};for(auto& [slot,r]:requests){if(!r.pending)continue;const u64 value=r.id<192?system.GetVideo().HostSyncpointValue(r.id):r.value;const bool reached=r.mode==1?value>=r.value:s32(u32(value)-u32(r.value))>=0;if(reached){r.pending=false;Event(slot)->Signal();}}}
 KernelHelpers::ServiceContext context;std::mutex mutex;std::map<u32,Kernel::KEvent*> events;std::map<u32,Request> requests;std::shared_ptr<Core::Timing::EventType> poll;
};
class INx2GpuSession final : public ServiceFramework<INx2GpuSession> {
public:
    explicit INx2GpuSession(Core::System& system_)
        : ServiceFramework{system_, "nx2:gpu"}, m_context{system_, "nx2:gpu"} {
        static const FunctionInfo functions[]={
            {101, &INx2GpuSession::SubRegisterMemory,"CreateGpuAccessibleMemory"},
            {211, &INx2GpuSession::SubSubmitJob,"NormalMapGpuMemory4K"},
            {212, &INx2GpuSession::SubModifyKind,"NormalModifyGpuMemory4K"},
            {213, &INx2GpuSession::SubUnmapJob,"NormalUnmapGpuMemory4K"},
            {102, &INx2GpuSession::SubDestroyMemory,"DestroyGpuAccessibleMemory"},
            {11, &INx2GpuSession::GetIdRange, "GetIdRange (guessed)"},
            {12, &INx2GpuSession::GetIdEvent, "GetIdEvent (guessed)"},
            {21, &INx2GpuSession::Start, "Start (guessed)"},
            {22, &INx2GpuSession::OpenSubSession, "OpenSubSession (guessed)"},
            {33, &INx2GpuSession::GetCount33, "Unknown33 (u32 count)"},
            {201, &INx2GpuSession::GetUserPartition, "GetUserPartition (newer SDKs)"},
            {202, &INx2GpuSession::GetGsvPartition, "GetGsvPartition (newer SDKs)"},
            {203, &INx2GpuSession::GetVaRange, "GetVaRange (guessed)"},
            // Sub-session commands (gpu 22 hands out a second session to this same object).
        };
        RegisterHandlersTipc(functions);
    }
    ~INx2GpuSession() override {for(auto* event:m_events)if(event)m_context.CloseEvent(event);}
private:
    static constexpr u32 NumIds = 32;

    Kernel::KEvent* EventFor(u32 id) {
        id %= NumIds;
        if (m_events[id] == nullptr) {
            m_events[id] = m_context.CreateEvent(fmt::format("nx2:gpu:Event{}", id));
        }
        return m_events[id];
    }

    void GetIdRange(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 11, returning [0, {}]", NumIds - 1);
        TipcReplyWords(ctx, {0, NumIds - 1});
    }

    void GetIdEvent(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 12 id={}", id);
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.PushRaw<u32>(ResultSuccess.raw);
        rb.PushCopyObjects( EventFor(id)->GetReadableEvent());
    }

    void Start(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 21 id={}, signalling completion", id);
        EventFor(id)->Signal();
        IPC::ResponseBuilder rb{ctx, 2};
        rb.PushRaw<u32>(ResultSuccess.raw);
    }

    void GetCount33(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 33 [2]={:#x}, returning 1", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {1});
    }

    // Used by nn::gpu::sdk::CreateGpuAddressSpace: three u64s after the result, a kind (< 5, 0 =
    // none) and the GPU VA range of the in-process VirtualMachine that serves AllocSpace
    // (nn::gpu::sdk::util::ExecuteVirtualMachineAllocSpace). NVN reserves fixed VA from 0x8000000
    // up to 512 GiB, so report the same range NvRm gives AllocAsEx: [0x8000000, 0xC000000000).
    void GetVaRange(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 203 [2]={:#x}", ctx.CommandBuffer()[2]);
        constexpr u64 kind = 1;
        constexpr u64 start = 0x8000000ULL;
        constexpr u64 size = 0xC000000000ULL - start;
        TipcReplyWords(ctx, {static_cast<u32>(kind), static_cast<u32>(kind >> 32),
                             static_cast<u32>(start), static_cast<u32>(start >> 32),
                             static_cast<u32>(size), static_cast<u32>(size >> 32)});
    }

    // Newer SDKs (title 1) query the address space's user and gsv partitions with 201/202(id):
    // two u64s each, stored as the partition's min/max. The user value works whether it is read
    // as (start, size) or (min, max). Title 4 never needed the gsv partition, so it stays empty.
    void GetUserPartition(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 201 [2]={:#x}", ctx.CommandBuffer()[2]);
        constexpr u64 first = 0x8000000ULL;
        constexpr u64 second = 0xC000000000ULL - first;
        TipcReplyWords(ctx, {static_cast<u32>(first), static_cast<u32>(first >> 32),
                             static_cast<u32>(second), static_cast<u32>(second >> 32)});
    }

    void GetGsvPartition(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 202 [2]={:#x}, empty", ctx.CommandBuffer()[2]);
        TipcReplyWords(ctx, {0, 0, 0, 0});
    }

    void OpenSubSession(HLERequestContext& ctx) {
        // Served by this object so sub-session jobs can signal the per-id events.
        LOG_WARNING(Service, "(STUBBED) nx2:gpu TIPC 22, returning a sub-session on nx2:gpu");
        ReturnRawSession(system, ctx, std::static_pointer_cast<INx2GpuSession>(shared_from_this()));
    }

    struct MemoryRange{u64 address,size;};
    std::map<u32,MemoryRange> m_memory;
    u32 m_next_memory_handle{1};
    void SubRegisterMemory(HLERequestContext& ctx){
        const auto*w=ctx.CommandBuffer();const u64 start=u64(w[2])|(u64(w[3])<<32),last=u64(w[4])|(u64(w[5])<<32);
        if(last<start || !Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Register(start,last-start+1)){IPC::ResponseBuilder rb{ctx,2};rb.PushRaw<u32>(Kernel::ResultOutOfMemory.raw);return;}
        const u32 handle=m_next_memory_handle++;m_memory[handle]={start,last-start+1};
        LOG_INFO(Service,"nx2:gpu 101 CPU={:#x} size={:#x} handle={}",start,last-start+1,handle);TipcReplyWords(ctx,{handle});
    }
    void SubSubmitJob(HLERequestContext& ctx){
        const auto*w=ctx.CommandBuffer();const u64 first=u64(w[3])|(u64(w[4])<<32),last=u64(w[5])|(u64(w[6])<<32),offset=u64(w[8])|(u64(w[9])<<32);
        auto it=m_memory.find(w[7]);
        const bool valid=it!=m_memory.end() && last>=first && offset<=it->second.size && last-first<it->second.size-offset;
        if(!valid || !Nvidia::Devices::Nx2GpuMemoryRegistry::Get().MapAt(first,it->second.address+offset,last-first+1)){IPC::ResponseBuilder rb{ctx,2};rb.PushRaw<u32>(Kernel::ResultInvalidAddress.raw);return;}
        const auto kind=Nvidia::Devices::SelectNativeMemoryKind(u8(w[12]),u8(w[13]));
        const bool selected=kind && Nvidia::Devices::Nx2GpuMemoryRegistry::Get().ModifyKind(first,last-first+1,kind->backend);
        EventFor(w[17])->Signal();LOG_INFO(Service,"nx2:gpu 211 map GPU={:#x} size={:#x} slot={} kind={:#x} completed",first,last-first+1,w[17],selected?kind->selected:0);
        if(selected)TipcReplyWords(ctx,{kind->selected});else TipcReplyWords(ctx,{0});
    }
    void SubModifyKind(HLERequestContext& ctx){
        const auto*w=ctx.CommandBuffer();const u64 first=u64(w[3])|(u64(w[4])<<32),last=u64(w[5])|(u64(w[6])<<32);
        const u32 wanted=w[7]&0xff,fallback=w[8]&0xff,slot=w[10];
        const auto kind=Nvidia::Devices::SelectNativeMemoryKind(u8(wanted),u8(fallback));
        const bool valid=last>=first && kind && w[10]<NumIds && (w[9]&0xff)<=1;
        bool mapped=false;
        if(valid){system.GetVideo().SynchronizeGpuCommands();mapped=Nvidia::Devices::Nx2GpuMemoryRegistry::Get().ModifyKind(first,last-first+1,kind->backend);EventFor(w[10])->Signal();}
        IPC::ResponseBuilder rb{ctx,3};rb.PushRaw<u32>(mapped?ResultSuccess.raw:Kernel::ResultInvalidAddress.raw);rb.PushRaw<u32>(mapped?kind->selected:0);
        LOG_INFO(Service,"NX2_KIND first={:#x} size={:#x} requested={:#x} fallback={:#x} selected={:#x} slot={} success={}",first,last-first+1,wanted,fallback,mapped?kind->selected:0,slot,mapped);
    }
    void SubUnmapJob(HLERequestContext& ctx){
        const auto*w=ctx.CommandBuffer();const u64 first=u64(w[3])|(u64(w[4])<<32),last=u64(w[5])|(u64(w[6])<<32);
        const bool valid=last>=first && w[8]<NumIds && (w[7]&0xff)<=1;
        bool unmapped=false;if(valid){system.GetVideo().SynchronizeGpuCommands();unmapped=Nvidia::Devices::Nx2GpuMemoryRegistry::Get().Unmap(first,last-first+1);EventFor(w[8])->Signal();}
        IPC::ResponseBuilder rb{ctx,4};rb.PushRaw<u32>(unmapped?ResultSuccess.raw:Kernel::ResultInvalidAddress.raw);rb.PushRaw<u64>(unmapped?last-first+1:0);
    }
    void SubDestroyMemory(HLERequestContext& ctx){
        const auto*w=ctx.CommandBuffer();auto it=m_memory.find(w[2]);
        bool removed=false;if(it!=m_memory.end() && w[3]<NumIds){system.GetVideo().SynchronizeGpuCommands();auto&registry=Nvidia::Devices::Nx2GpuMemoryRegistry::Get();removed=!registry.IsMapped(it->second.address,it->second.size) || registry.Unmap(it->second.address,it->second.size);if(removed)m_memory.erase(it);EventFor(w[3])->Signal();}
        IPC::ResponseBuilder rb{ctx,2};rb.PushRaw<u32>(removed?ResultSuccess.raw:Kernel::ResultInvalidAddress.raw);
    }
    std::array<Kernel::KEvent*,NumIds> m_events{};
    KernelHelpers::ServiceContext m_context;
};
struct Nx2NativeObjects {
    struct Memory {
        u64 size{};
        u64 address{}; // CPU (== GPU) address once bound by 114/423
    };
    struct Image {
        u32 memory{};
        std::array<u32, 16> preference{};
        u64 size{};
        u32 count{1}; // images in the pool; handle id + i is image i
        u64 offset{}; // into its memory (images of a decoder pool share one)
        bool window_pool{};
        bool decoder{}; // a video decoder output image (gsv 911)
    };
    std::mutex mutex;
    std::map<u32, Memory> memories;
    std::map<u32, Image> images;
    u32 next_memory{1};
    u32 next_image{1};
};

// Block-linear layout of an InterModuleUserImagePreference (16 words): word 2 holds
// (width - 1) | (height - 1) << 16, word 4 the block height (log2 of GOB rows), word 5 the row
// pitch in 64-byte GOBs. Formula from changes.patch (native_image_layout.h); preferences outside
// it fall back to 4 bytes per pixel and 128-row blocks.
struct Nx2ImageLayout {
    u32 width;
    u32 height;
    u64 pitch;
    u32 block_height_log2;
    u64 size;
    u64 alignment;
};

inline Nx2ImageLayout Nx2CalculateImageLayout(const u32* preference) {
    Nx2ImageLayout layout{};
    layout.width = (preference[2] & 0xFFFF) + 1;
    layout.height = (preference[2] >> 16) + 1;
    const u64 pitch_gobs = preference[5];
    const u32 block_height_log2 = preference[4];
    if (block_height_log2 >= 3 && block_height_log2 <= 8 && pitch_gobs >= (u64{layout.width} * 4 + 63) / 64 &&
        pitch_gobs <= 4096) {
        layout.pitch = pitch_gobs * 64;
        layout.block_height_log2 = block_height_log2;
    } else {
        layout.pitch = Common::AlignUp(u64{layout.width} * 4, 64);
        layout.block_height_log2 = 7;
    }
    const u64 block_rows = u64{1} << layout.block_height_log2;
    layout.size = layout.pitch * Common::AlignUp(u64{layout.height}, block_rows);
    layout.alignment = std::max<u64>(64 * block_rows, 0x1000);
    return layout;
}

class INx2Vi2Session final : public ServiceFramework<INx2Vi2Session> {
public:
    explicit INx2Vi2Session(Core::System& system_, std::shared_ptr<Nx2NativeObjects> objects_)
        : ServiceFramework{system_, "nx2:vi2"}, m_context{system_, "nx2:vi2"},
          m_objects{std::move(objects_)} {
        static const FunctionInfo functions[]={
            {1, &INx2Vi2Session::ReturnOne, "Unknown1"},
            {3, &INx2Vi2Session::ReturnOne, "GetDisplayCount (guessed)"},
            {5, &INx2Vi2Session::GetTimestampRecorderCount, "GetTimestampRecorderCount (guessed)"},
            {11, &INx2Vi2Session::ReturnEvent, "Unknown11"},
            {31, &INx2Vi2Session::GetVsyncEvent, "GetVsyncEvent (guessed)"},
            {32, &INx2Vi2Session::GetVsyncEvent, "GetDisplayVsyncEvent (guessed)"},
            // Title 1 calls these ~75 times a second with the display name "default"; the
            // auto stub's empty success reply works, but logged two lines per call (a 50 MB log
            // in an 18 minute session).
            {104, &INx2Vi2Session::SucceedQuietly, "Unknown104 (display \"default\")"},
            {105, &INx2Vi2Session::SucceedQuietly, "Unknown105 (display \"default\")"},
            {228, &INx2Vi2Session::CancelClientBuffer, "CancelClientBuffer"},
            {229, &INx2Vi2Session::PresentClientBuffer, "PresentClientBuffer"},
            {212, &INx2Vi2Session::CancelClientBuffer, "GetClientAvailability"},
        };
        RegisterHandlersTipc(functions);
    }

    ~INx2Vi2Session() override {
        // The vsync callback may be running on the HostTiming thread: empty its event list under
        // its lock first (it holds the state, not this session), then close the events.
        {
            std::scoped_lock lk{m_vsync->mutex};
            m_vsync->events.clear();
        }
        if (m_vsync_timer) {
            system.CoreTiming().UnscheduleEvent(m_vsync_timer);
        }
        for (auto* event : m_events) {
            m_context.CloseEvent(event);
        }
    }

private:
    void PresentClientBuffer(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        u64 address{};
        Nx2ImageLayout layout{};
        {
            std::scoped_lock lock{m_objects->mutex};
            const auto image = m_objects->images.find(w[5]);
            if (image == m_objects->images.end()) {
                IPC::ResponseBuilder rb{ctx, 2}; rb.PushRaw<u32>(Kernel::ResultNotFound.raw); return;
            }
            const auto memory = m_objects->memories.find(image->second.memory);
            if (memory == m_objects->memories.end() || memory->second.address == 0) {
                IPC::ResponseBuilder rb{ctx, 2}; rb.PushRaw<u32>(Kernel::ResultInvalidAddress.raw); return;
            }
            address = memory->second.address + image->second.offset;
            layout = Nx2CalculateImageLayout(image->second.preference.data());
        }
        const auto device = Nvidia::Devices::Nx2GpuMemoryRegistry::Get().DeviceAddressOf(address);
        if (!device || layout.width > 16384 || layout.height > 16384 || layout.block_height_log2 > 8) {
            IPC::ResponseBuilder rb{ctx, 2}; rb.PushRaw<u32>(Kernel::ResultInvalidAddress.raw); return;
        }
        VideoFramebufferConfig frame{};
        frame.address = *device; frame.width = layout.width; frame.height = layout.height;
        frame.stride = static_cast<u32>(layout.pitch / 4); frame.pixelFormat = 1;
        frame.cropRight = static_cast<s32>(layout.width); frame.cropBottom = static_cast<s32>(layout.height);
        frame.blending = 0; frame.nativeTiling = true; frame.blockHeightLog2 = layout.block_height_log2 - 3;
        VideoNvFence fence{static_cast<s32>(~w[22]), w[24]};
        auto& video = system.GetVideo();
        LOG_INFO(Service, "NX2_PRESENT_QUEUE image={} device={:#x} layout={}x{} block={} fence={}/{} current={}", w[5], *device, frame.width, frame.height, frame.blockHeightLog2, fence.id, fence.value, fence.id >= 0 && fence.id < 192 ? video.HostSyncpointValue(fence.id) : 0);
        video.RequestComposite(&frame, 1, &fence, static_cast<u32>(fence.id >= 0 && fence.id < 192));
        // Keep the current acquire fence on the GPU request, but return to the guest.
        // Waiting here can prevent later GPU jobs from releasing this first frame.
        // RequestComposite serializes against the previously queued composite.
        ++m_presents;
        if (m_presents <= 4 || m_presents % 600 == 0) {
            LOG_INFO(Service, "NX2_PRESENT frame={} image={} device={:#x} layout={}x{} stride={} block={} fence={}/{}",
                     m_presents, w[5], *device, frame.width, frame.height, frame.stride,
                     frame.blockHeightLog2, fence.id, fence.value);
        }
        system.GetPerfStats().EndSystemFrame(); system.GetPerfStats().BeginSystemFrame();
        CancelClientBuffer(ctx);
    }
    void CancelClientBuffer(HLERequestContext& ctx) {
        constexpr u64 AvailableSlots = 0x7fff;
        TipcReplyWords(ctx, {static_cast<u32>(AvailableSlots),
                             static_cast<u32>(AvailableSlots >> 32), 0, 0, 0, 0});
    }

    void ReturnOne(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC {}, returning 1", ctx.GetCommand());
        TipcReplyWords(ctx, {1});
    }

    // The same reply as the auto stub (success, no data), without its log lines.
    void SucceedQuietly(HLERequestContext& ctx) {
        IPC::ResponseBuilder rb{ctx, 2};
        rb.PushRaw<u32>(ResultSuccess.raw);
    }

    // Must be < 8; nn::vi::detail::LayerHistory creates 7 timestamp recorders per layer.
    void GetTimestampRecorderCount(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC 5, returning 7");
        TipcReplyWords(ctx, {7});
    }

    void ReturnEvent(HLERequestContext& ctx) {
        LOG_WARNING(Service, "(STUBBED) nx2:vi2 TIPC {} [2]={:#x} [3]={:#x}, returning an event",
                    ctx.GetCommand(), ctx.CommandBuffer()[2], ctx.CommandBuffer()[3]);
        auto* event = m_context.CreateEvent(fmt::format("nx2:vi2:Event{}", m_events.size()));
        m_events.push_back(event);
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.PushRaw<u32>(ResultSuccess.raw);
        rb.PushCopyObjects( event->GetReadableEvent());
    }

    // 31(u32 kind) and 32(u32 display): events signalled on every vblank. Title 1's main thread
    // waits on the 32 event until it has fired twice before it submits any GPU work, and hung
    // there while it never fired.
    void GetVsyncEvent(HLERequestContext& ctx) {
        LOG_INFO(Service, "nx2:vi2 {} vsync event, [2]={:#x}", ctx.GetCommand(), ctx.CommandBuffer()[2]);
        auto* event = m_context.CreateEvent(fmt::format("nx2:vi2:Vsync{}", m_events.size()));
        m_events.push_back(event);
        {
            std::scoped_lock lk{m_vsync->mutex};
            m_vsync->events.push_back(event);
        }
        if (!m_vsync_timer) {
            m_vsync_timer = Core::Timing::CreateEvent(
                "nx2:vi2 vsync",
                [vsync_state = m_vsync, &kernel = system.Kernel()](
                    s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                    std::scoped_lock lk{vsync_state->mutex};
                    for (auto* vsync : vsync_state->events) {
                        vsync->Signal();
                    }
                    return std::nullopt;
                });
            // EDEN_NX2_VSYNC_HZ=<rate> (experiment): the game starts frames on this event, so a
            // frame that misses a 60 Hz tick idles until the next one.
            static const std::chrono::nanoseconds VblankInterval{[] {
                const char* text = std::getenv("EDEN_NX2_VSYNC_HZ");
                const long long hz = text ? (std::max)(std::strtoll(text, nullptr, 10), 1LL) : 60LL;
                return 1'000'000'000LL / hz;
            }()};
            system.CoreTiming().ScheduleLoopingEvent(VblankInterval, VblankInterval, m_vsync_timer);
        }
        IPC::ResponseBuilder rb{ctx, 2, 1, 0};
        rb.PushRaw<u32>(ResultSuccess.raw);
        rb.PushCopyObjects( event->GetReadableEvent());
    }

    KernelHelpers::ServiceContext m_context;
    std::shared_ptr<Nx2NativeObjects> m_objects;
    std::vector<Kernel::KEvent*> m_events;
    // Shared with the vsync timer callback, which can outlive the session by one call.
    struct VsyncState {
        std::mutex mutex;
        std::vector<Kernel::KEvent*> events;
    };
    std::shared_ptr<VsyncState> m_vsync = std::make_shared<VsyncState>();
    std::shared_ptr<Core::Timing::EventType> m_vsync_timer;
    u64 m_presents{};
    u64 m_shown_layout{}; // image and crop size last logged
    std::shared_ptr<std::atomic<bool>> m_capture_pending = std::make_shared<std::atomic<bool>>(false);
};

class INx2GraphicsSystemSession final:public ServiceFramework<INx2GraphicsSystemSession> {
public:
 explicit INx2GraphicsSystemSession(Core::System& sys):ServiceFramework{sys,"nx2:gsv"},m_context{sys,"nx2:gsv"}{
 static const FunctionInfo functions[]={{305,&INx2GraphicsSystemSession::OpenViSession,"OpenViSession"},{111,&INx2GraphicsSystemSession::CreateNativeMemory,"CreateNativeMemory"},{112,&INx2GraphicsSystemSession::DestroyNativeMemory,"DestroyNativeMemory"},{114,&INx2GraphicsSystemSession::BindNativeMemory,"BindNativeMemory"},{423,&INx2GraphicsSystemSession::BindNativeGpuMemory,"BindNativeGpuMemory"},{429,&INx2GraphicsSystemSession::CreateNativeImage,"CreateNativeImage"},{430,&INx2GraphicsSystemSession::DestroyNativeImage,"DestroyNativeImage"},{426,&INx2GraphicsSystemSession::QueryNativeImage,"MapNativeImage"},{428,&INx2GraphicsSystemSession::QueryNativeImage,"QueryNativeImage"},{435,&INx2GraphicsSystemSession::CalculateNativeImageSize,"CalculateNativeImageSize"},{314,&INx2GraphicsSystemSession::ConfigureWindowImagePool,"DestroyNativeWindowAliasImagePool"},{315,&INx2GraphicsSystemSession::ConfigureWindowImagePool,"CreateNativeWindowAliasImagePool"},{1510,&INx2GraphicsSystemSession::CalculateRequirements,"CalculateRequirements"},{202,&INx2GraphicsSystemSession::OpenSyncptSession,"OpenSyncptSession"},{204,&INx2GraphicsSystemSession::AttachSyncptMemory,"AttachSyncptMemory"},{497,&INx2GraphicsSystemSession::OpenGpuSession,"OpenGpuSession"},{1402,&INx2GraphicsSystemSession::OpenNvsrvSession,"OpenNvsrvSession"}};
 RegisterHandlersTipc(functions);
 }
 ~INx2GraphicsSystemSession() override {if(m_syncpt_poll)system.CoreTiming().UnscheduleEvent(m_syncpt_poll);for(auto*e:m_syncpt_events)if(e)m_context.CloseEvent(e);}
private:
    // Index of the first raw data word of a TIPC request: header (2 words), then an optional
    // special header with pid and copy/move handle words.
    static size_t TipcDataOffset(const u32* cmd_buf) {
        return Nx2TipcDataOffset(cmd_buf);
    }

    void OpenSyncptSession(HLERequestContext& ctx) {
        // Used by nn::syncpt::sdk::InitializeEx; handle at word 3, result at word 4.
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 202, returning a nx2:syncpt session");
        ReturnRawSession(system, ctx, std::make_shared<INx2SyncptSession>(system));
    }

    // nn::syncpt::sdk::InitializeEx sends two transfer memories (0x1000 bytes RW, then 0x2000
    // bytes the client maps read-only) and expects the server to have written a header into the
    // second one: u32 version (must be 1) at +0, u32 base id at +4, u32 count at +8 (the client
    // requires ((count + 0x40) >> 6) - 1 < 0x40), then offsets of: the u64 syncpoint values (RO,
    // +0xc), the RW "armed" summary word (+0x10), the RO summary word (+0x14), the RW armed bits
    // (+0x18) and the RO acknowledge bits (+0x1c).
    // To wait, the client makes its RW bit differ from the RO bit, signals the first returned
    // event (it attaches that handle as writable), and waits on the second until the server makes
    // the bits equal again; it then rereads the value and rearms if the threshold isn't reached.
    // Reply: the two handles (words 3 and 4), then the result.
    static constexpr u32 SyncptValuesOffset = 0x40;   // RO, u64 per syncpoint
    static constexpr u32 SyncptRoSummaryOffset = 0x1040;
    static constexpr u32 SyncptRoBitsOffset = 0x1100; // RO, up to 0x40 u64 words
    static constexpr u32 SyncptRwSummaryOffset = 0x0;
    static constexpr u32 SyncptRwBitsOffset = 0x100;  // RW, up to 0x40 u64 words
    static constexpr u32 HostSyncpoints = 192;        // Tegra::Host1x::SyncpointManager

    void AttachSyncptMemory(HLERequestContext& ctx) {
        // RequestParser doesn't skip TIPC handle words, so read the raw data words.
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        const u32 version_requested = cmd_buf[data];
        const u32 max_syncpoints = std::min<u32>(cmd_buf[data + 1], (0x1000 - 0x40) / 8);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 204, version={} max_syncpoints={:#x}",
                    version_requested, max_syncpoints);

        auto rw = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(0));
        auto ro = ctx.GetObjectFromHandle<Kernel::KTransferMemory>(ctx.GetCopyHandle(1));
        if (rw.IsNotNull() && ro.IsNotNull() && ro->GetSize() >= 0x2000 && rw->GetSize() >= 0x1000) {
            m_syncpt_memory = &ro->GetOwner()->GetMemory();
            m_syncpt_rw = rw->GetSourceAddress().GetValue();
            m_syncpt_ro = ro->GetSourceAddress().GetValue();
            m_syncpt_count = max_syncpoints;
            auto& memory = *m_syncpt_memory;
            memory.Write32(m_syncpt_ro + 0x00, 1); // version
            memory.Write32(m_syncpt_ro + 0x04, 0); // base id (guess)
            memory.Write32(m_syncpt_ro + 0x08, max_syncpoints);
            memory.Write32(m_syncpt_ro + 0x0c, SyncptValuesOffset);
            memory.Write32(m_syncpt_ro + 0x10, SyncptRwSummaryOffset);
            memory.Write32(m_syncpt_ro + 0x14, SyncptRoSummaryOffset);
            memory.Write32(m_syncpt_ro + 0x18, SyncptRwBitsOffset);
            memory.Write32(m_syncpt_ro + 0x1c, SyncptRoBitsOffset);
            UpdateSyncptValues();
            if (!m_syncpt_poll) {
                m_syncpt_poll = Core::Timing::CreateEvent(
                    "nx2:gsv:SyncptPoll",
                    [this](s64, std::chrono::nanoseconds) -> std::optional<std::chrono::nanoseconds> {
                        PollSyncpts();
                        return std::nullopt;
                    });
                system.CoreTiming().ScheduleLoopingEvent(SyncptPollInterval, SyncptPollInterval,
                                                         m_syncpt_poll);
            }
        } else {
            LOG_ERROR(Service, "nx2:gsv TIPC 204: missing syncpt transfer memory");
        }

        if (m_syncpt_events[0] == nullptr) {
            m_syncpt_events[0] = m_context.CreateEvent("nx2:gsv:SyncptRequest");
            m_syncpt_events[1] = m_context.CreateEvent("nx2:gsv:SyncptNotify");
        }
        IPC::ResponseBuilder rb{ctx, 2, 2, 0};
        rb.PushRaw<u32>(ResultSuccess.raw);
        rb.PushCopyObjects( *m_syncpt_events[0]); // signalled by the client
        rb.PushCopyObjects( m_syncpt_events[1]->GetReadableEvent());
    }

    // Mirrors Eden's guest syncpoint values into the shared RO page. Returns true if any changed.
    bool UpdateSyncptValues() {
        auto& manager = system.GetVideo();
        bool changed = false;
        for (u32 id = 0; id < std::min(m_syncpt_count, HostSyncpoints); ++id) {
            const u64 value = manager.HostSyncpointValue(id);
            const u64 address = m_syncpt_ro + SyncptValuesOffset + id * 8;
            if (m_syncpt_memory->Read64(address) != value) {
                m_syncpt_memory->Write64(address, value);
                changed = true;
            }
        }
        return changed;
    }

    // Answers every armed wait: the value page is current, so the client rechecks its threshold
    // and rearms if it still has to wait. Acknowledging without a value change costs the client a
    // wakeup per poll, which keeps waits whose syncpoint we never see from hanging.
    void PollSyncpts() {
        if (m_syncpt_memory == nullptr) {
            return;
        }
        const bool values_changed = UpdateSyncptValues();
        static std::atomic<u32> poll_reported{};
        const bool report_poll = poll_reported.fetch_add(1, std::memory_order_relaxed) < 8;
        if (report_poll) {
            LOG_INFO(Service, "NX2_SYNCPT_POLL count={} changed={} armed={:#x} ack={:#x}",
                     m_syncpt_count, values_changed,
                     m_syncpt_memory->Read64(m_syncpt_rw + SyncptRwBitsOffset),
                     m_syncpt_memory->Read64(m_syncpt_ro + SyncptRoBitsOffset));
        }
        bool acked = false;
        const u32 words = (m_syncpt_count + 63) / 64;
        for (u32 w = 0; w < words; ++w) {
            const u64 rw_bits = m_syncpt_memory->Read64(m_syncpt_rw + SyncptRwBitsOffset + w * 8);
            const u64 ro_address = m_syncpt_ro + SyncptRoBitsOffset + w * 8;
            if (m_syncpt_memory->Read64(ro_address) != rw_bits) {
                m_syncpt_memory->Write64(ro_address, rw_bits);
                acked = true;
            }
        }
        m_syncpt_memory->Write64(m_syncpt_ro + SyncptRoSummaryOffset,
                                 m_syncpt_memory->Read64(m_syncpt_rw + SyncptRwSummaryOffset));
        if (acked) {
            m_syncpt_events[1]->Signal();
        }
    }

    void OpenGpuSession(HLERequestContext& ctx) {
        // Used by nn::gpu::sdk::BootWithRemoteMemory; takes one u64, returns a session (handle at
        // word 3, result at word 4) that the GPU SDK keeps.
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 497 arg={:#x}, returning a nx2:gpu session",
                    u64{cmd_buf[data]} | (u64{cmd_buf[data + 1]} << 32));
        ReturnRawSession(system, ctx, std::make_shared<INx2GpuSession>(system));
    }

    void OpenNvsrvSession(HLERequestContext& ctx) {
        // The SDK reads the handle at word 3 and the result at word 4.
        // nvsrv speaks the nvdrv INvDrvServices protocol (Open/Ioctl/Close/...,
        // 13 = SetGraphicsFirmwareMemoryMarginEnabled), so serve it with Eden's nvdrv on the shared
        // module. The nx2: name keeps unknown commands auto-stubbed and logged.
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 1402, returning an nvdrv-backed nx2:nvsrv session");
        auto module = system.ServiceManager().GetService<Nvidia::NVDRV>("nvdrv", true)->GetModule();
        auto nvsrv = std::make_shared<Nvidia::NVDRV>(system, module, "nx2:nvsrv");
        nvsrv->InitializeForProcess(ctx.GetThread().GetOwnerKProcess());
        ReturnRawSession(system, ctx, std::move(nvsrv));
    }

    void CalculateRequirements(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout = Nx2CalculateImageLayout(w + 2);
        LOG_DEBUG(Service, "nx2:gsv 1510 kind={} {}x{} block={} pitch={:#x} format={} -> size={:#x} align={:#x}",
                  w[3], layout.width, layout.height, layout.block_height_log2, layout.pitch, w[8],
                  layout.size, layout.alignment);
        TipcReplyWords(ctx, {static_cast<u32>(layout.size), static_cast<u32>(layout.size >> 32),
                             static_cast<u32>(layout.alignment),
                             static_cast<u32>(layout.alignment >> 32)});
    }

    // nn::codec::CalculateDecoderImagePoolBuffer(size*, alignment*, VideoDecoderImagePreference,
    // u32 count) sends 991(preference[16], u32 count) and reads result, u64 size, u64 alignment;
    // the client raises the alignment to at least 4 KiB and rounds the size up to it. The
    // preference (MakeDecodeImagePreference) holds an output layout code at word 1 (1/4 for
    // format 3, which the movie player uses, read here as NV12) and (width - 1) | (height - 1) << 16
    // at word 2. Left to the auto stub, the reply words keep the request's, so the size became
    // 0x167027f'00000001 and title 1's movie player aborted (main+0x19e5b58) when its first movie
    // started. Sizes are block-linear NV12 (luma, then half-height chroma, 128-row blocks) or 4
    // bytes per pixel for the other codes; nothing is decoded into them yet.
    void CalculateNativeImageSize(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 size = Nx2CalculateImageLayout(w + 2).size * std::max<u32>(w[18], 1);
        LOG_DEBUG(Service, "nx2:gsv 435 count={} -> size={:#x}", w[18], size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32)});
    }

    static u64 Pair(const u32* w, size_t i) {
        return u64{w[i]} | (u64{w[i + 1]} << 32);
    }

    // 111(u8 mode at word 2, u64 size): result, u32 native memory id.
    void CreateNativeMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const u64 size = Pair(w, 3);
        std::scoped_lock lk{m_objects->mutex};
        const u32 id = m_objects->next_memory++;
        m_objects->memories[id] = {size, 0};
        LOG_INFO(Service, "nx2:gsv 111 CreateNativeMemory mode={} size={:#x} -> id {}", w[2] & 0xFF,
                 size, id);
        TipcReplyWords(ctx, {id});
    }

    void DestroyNativeMemory(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        std::scoped_lock lk{m_objects->mutex};
        m_objects->memories.erase(id);
        LOG_DEBUG(Service, "nx2:gsv 112 DestroyNativeMemory id {}", id);
        TipcReplyWords(ctx, {});
    }

    // 114(u32 id, u64 address, u64 size, u64 flags, u64 offset): back native memory with process
    // memory.
    void BindNativeMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        BindMemory(w[2], Pair(w, 3), Pair(w, 5), "114");
        TipcReplyWords(ctx, {});
    }

    // 423(u32 id, u64 address, u64 size, u32 parent GpuMemoryHandle, u64 offset, ...): back
    // native memory with part of GPU-accessible memory.
    void BindNativeGpuMemory(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        LOG_DEBUG(Service, "nx2:gsv 423 parent={} offset={:#x}", w[7], Pair(w, 8));
        BindMemory(w[2], Pair(w, 3), Pair(w, 5), "423");
        TipcReplyWords(ctx, {});
    }

    void BindMemory(u32 id, u64 address, u64 size, const char* command) {
        std::scoped_lock lk{m_objects->mutex};
        auto& memory = m_objects->memories[id];
        memory.address = address;
        if (memory.size == 0) {
            memory.size = size;
        }
        LOG_INFO(Service, "nx2:gsv {} bind native memory {} at {:#x}+{:#x}", command, id, address,
                 size);
    }

    // 429(preference[16], u32 count, u32 memory id): result, u32 native image id.
    void CreateNativeImage(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        std::scoped_lock lk{m_objects->mutex};
        const u32 id = m_objects->next_image;
        auto& image = m_objects->images[id];
        image.count = std::max<u32>(w[18], 1);
        // nn::gs::sdk::CalculateNativeImageHandle(pool, i) is pool + i, so keep those free.
        m_objects->next_image += image.count;
        image.memory = w[19];
        std::copy_n(w + 2, image.preference.size(), image.preference.begin());
        image.size = Nx2CalculateImageLayout(w + 2).size;
        LOG_INFO(Service, "nx2:gsv 429 CreateNativeImage memory={} count={} size={:#x} -> id {}",
                 w[19], w[18], image.size, id);
        TipcReplyWords(ctx, {id});
    }

    // nn::chat::gsv::CalculateRequiredBufferMemory: 1291(preference[16], u32 count), the request
    // of 1510, replies result, u64 size, u64 alignment. Auto-stubbed, the size became 0x1000 and
    // the camera pool (1211) got memory for none of its images.
    void CalculateChatImagePoolSize(HLERequestContext& ctx) {
        const auto* w = ctx.CommandBuffer();
        const auto layout = Nx2CalculateImageLayout(w + 2);
        const u64 size = layout.size * std::max<u32>(w[18], 1);
        LOG_DEBUG(Service, "nx2:gsv 1291 {}x{} count={} -> size={:#x}", layout.width,
                  layout.height, w[18], size);
        TipcReplyWords(ctx, {static_cast<u32>(size), static_cast<u32>(size >> 32),
                             static_cast<u32>(layout.alignment),
                             static_cast<u32>(layout.alignment >> 32)});
    }

    // 428(u32 native image handle) nn::gpu::sdk::QueryNativeImage and 426(u32 handle, u32, u32
    // address space, u8 permission) nn::gpu::sdk::MapNativeImage, used by NVN for images another
    // server created (GameChat's camera pool from 1211). Reply words 3-4 the GPU address of the
    // image (426 only; GPU == CPU address, see Nx2NativeObjects::Memory), 5-6 the u64 offset of
    // the image in its memory, 7-38 a GpuOunceUserImageDescriptor and 39 the flags. The SDK
    // requires descriptor byte 0x00 == 1, byte 0x20 == 1 and a non-zero size at 0x2c (offset at
    // 0x28); NVN reads (width - 1) | (height - 1) << 16 at 0x08 and the layout kind at 0x24 (1
    // takes RGBA formats 0x25/0x37/0x38, 2 the 0x99-0x9c formats). Block height and pitch use
    // the 429 preference words (0x10, 0x14), a guess.
    void QueryNativeImage(HLERequestContext& ctx) {
        const u32 handle = ctx.CommandBuffer()[2];
        std::array<u32, 16> preference{};
        u64 image_offset = 0;
        u64 memory_address = 0;
        {
            std::scoped_lock lk{m_objects->mutex};
            auto it = m_objects->images.upper_bound(handle);
            if (it != m_objects->images.begin()) {
                --it;
                if (handle - it->first < it->second.count) {
                    preference = it->second.preference;
                    image_offset = (handle - it->first) * it->second.size;
                    if (const auto memory = m_objects->memories.find(it->second.memory);
                        memory != m_objects->memories.end()) {
                        memory_address = memory->second.address;
                    }
                }
            }
        }
        const auto layout = Nx2CalculateImageLayout(preference.data());
        const u64 gpu_address = ctx.GetCommand() == 426 ? memory_address + image_offset : 0;
        std::array<u32, 37> words{};
        words[0] = static_cast<u32>(gpu_address);
        words[1] = static_cast<u32>(gpu_address >> 32);
        words[2] = static_cast<u32>(image_offset);
        words[3] = static_cast<u32>(image_offset >> 32);
        u32* const desc = words.data() + 4;
        desc[0] = 1;
        desc[1] = preference[1];
        desc[2] = preference[2];
        desc[4] = layout.block_height_log2;
        desc[5] = static_cast<u32>(layout.pitch / 64);
        desc[8] = 1;
        desc[9] = 1;
        desc[11] = static_cast<u32>(layout.size);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv {} handle={} {}x{} size={:#x} gpu={:#x}",
                    ctx.GetCommand(), handle, layout.width, layout.height, layout.size,
                    gpu_address);
        IPC::ResponseBuilder rb{ctx, static_cast<u32>(2 + words.size())};
        rb.PushRaw<u32>(ResultSuccess.raw);
        for (const u32 w : words) {
            rb.Push<u32>(w);
        }
    }

    void DestroyNativeImage(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        std::scoped_lock lk{m_objects->mutex};
        m_objects->images.erase(id);
        TipcReplyWords(ctx, {});
    }

    // 315(u32 image, u32 count) / 314(u32 image): add/remove a window alias image pool.
    void ConfigureWindowImagePool(HLERequestContext& ctx) {
        const u32 id = ctx.CommandBuffer()[2];
        const bool create = ctx.GetCommand() == 315;
        std::scoped_lock lk{m_objects->mutex};
        if (auto it = m_objects->images.find(id); it != m_objects->images.end()) {
            it->second.window_pool = create;
        }
        LOG_INFO(Service, "nx2:gsv {} window image pool image {}", ctx.GetCommand(), id);
        TipcReplyWords(ctx, {});
    }

    void OpenViSession(HLERequestContext& ctx) {
        // Used by nn::vi2::sdk::Initialize; takes one u64, returns a session (handle at word 3,
        // result at word 4).
        const auto* cmd_buf = ctx.CommandBuffer();
        const size_t data = TipcDataOffset(cmd_buf);
        LOG_WARNING(Service, "(STUBBED) nx2:gsv TIPC 305 arg={:#x}, returning a nx2:vi2 session",
                    u64{cmd_buf[data]} | (u64{cmd_buf[data + 1]} << 32));
        ReturnRawSession(system, ctx, std::make_shared<INx2Vi2Session>(system, m_objects));
    }

    std::shared_ptr<Nx2NativeObjects> m_objects{std::make_shared<Nx2NativeObjects>()};
    static constexpr std::chrono::nanoseconds SyncptPollInterval{std::chrono::microseconds{500}};
    KernelHelpers::ServiceContext m_context;
    std::array<Kernel::KEvent*,2> m_syncpt_events{};
    IMemory* m_syncpt_memory{};
    u64 m_syncpt_rw{},m_syncpt_ro{};
    u32 m_syncpt_count{};
    std::shared_ptr<Core::Timing::EventType> m_syncpt_poll;
};
} // namespace Service
