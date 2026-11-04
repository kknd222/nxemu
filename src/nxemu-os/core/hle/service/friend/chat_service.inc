// SPDX-License-Identifier: GPL-2.0-or-later
// Included inside Service::Friend. Public ABI: Switchbrew Switch_2:_Chat_services.
// Offline idle state only. No room, camera/audio transport or online authentication.
class ChatUserShim final : public ServiceFramework<ChatUserShim> {
public:
 explicit ChatUserShim(Core::System& s) : ServiceFramework{s,"nn::chat::IUserShimScopedObject"}, context{s,"ChatUserShim"} {
  static const FunctionInfo functions[]={{1,&ChatUserShim::GetEvent,"GetIsChattingEvent"},{2,&ChatUserShim::GetEvent,"AttachToRoomInfoChangedEvent"},
   {3,&ChatUserShim::GetEvent,"AttachToLocalCameraImageProcessingModeChangedEvent"},{4,&ChatUserShim::GetEvent,"AttachToLocalCameraZoomMagnificationChangedEvent"}};
  RegisterHandlers(functions);for(auto& event:events)event=context.CreateEvent("ChatIdleEvent");
 }
 ~ChatUserShim() override {for(auto* event:events)context.CloseEvent(event);}
private:
 void GetEvent(HLERequestContext& ctx) {
  const auto index=ctx.GetCommand()-1;
  // Once-per-object event acquisition is evidenced by the eden-ns2 firmware analysis.
  // Never signal an idle chat event to force the game past a wait.
  const Result result=taken[index]?Result{static_cast<ErrorModule>(226),2}:ResultSuccess;
  Core::SmResolutionTrace::Record(ctx.GetThread().GetThreadId(),GetServiceName(),taken[index]?"event-already-acquired":"idle-event-issued",result.raw);
  if(result.IsError()){IPC::ResponseBuilder rb{ctx,2};rb.Push(result);return;}
  taken[index]=true;IPC::ResponseBuilder rb{ctx,2,1};rb.Push(result);rb.PushCopyObjects(events[index]->GetReadableEvent());
 }
 KernelHelpers::ServiceContext context;std::array<Kernel::KEvent*,4> events{};std::array<bool,4> taken{};
};
class ChatUserService final : public ServiceFramework<ChatUserService> {
public:
 explicit ChatUserService(Core::System& s) : ServiceFramework{s,"nn::chat::IUserService"} {
  static const FunctionInfo functions[]={{0,&ChatUserService::OpenUserShim,"GetUserShimScopedObject"}};RegisterHandlers(functions);
 }
private:
 void OpenUserShim(HLERequestContext& ctx) {
  Core::SmResolutionTrace::Record(ctx.GetThread().GetThreadId(),GetServiceName(),"open-idle-user-shim",0);
  IPC::ResponseBuilder rb{ctx,2,0,1};rb.Push(ResultSuccess);rb.PushIpcInterface<ChatUserShim>(system);
 }
};
