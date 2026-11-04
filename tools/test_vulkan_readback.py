"""Independent real Vulkan transfer/array-layer oracle, NOT production-path acceptance.

No game input, shader execution, emulator, cache/driver modifications. Worker owns
GPU objects until fence completion. Parent timeout terminates the worker, never
claims an in-call Vulkan cancellation or frees a live buffer in another thread.
"""
from pathlib import Path
import argparse, ctypes as C, hashlib, json, os, subprocess, sys, time
import pipeline_probe as V

U32=V.U32; I32=V.I32; U64=V.U64; H=V.Handle; P=C.POINTER
class LayerProperties(C.Structure):
    _fields_=[('layerName',C.c_char*256),('specVersion',U32),('implementationVersion',U32),('description',C.c_char*256)]
class IDProperties(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('deviceUUID',C.c_uint8*16),('driverUUID',C.c_uint8*16),('deviceLUID',C.c_uint8*8),('deviceNodeMask',U32),('deviceLUIDValid',U32)]
class MemoryType(C.Structure):
    _fields_=[('propertyFlags',U32),('heapIndex',U32)]
class MemoryHeap(C.Structure):
    _fields_=[('size',U64),('flags',U32)]
class MemoryProperties(C.Structure):
    _fields_=[('memoryTypeCount',U32),('memoryTypes',MemoryType*32),('memoryHeapCount',U32),('memoryHeaps',MemoryHeap*16)]
class MemoryRequirements(C.Structure):
    _fields_=[('size',U64),('alignment',U64),('memoryTypeBits',U32)]
class AllocateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('allocationSize',U64),('memoryTypeIndex',U32)]
class BufferCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('flags',U32),('size',U64),('usage',U32),('sharingMode',I32),('queueFamilyIndexCount',U32),('pQueueFamilyIndices',P(U32))]
class ImageCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('flags',U32),('imageType',I32),('format',I32),('extent',V.Extent3D),('mipLevels',U32),('arrayLayers',U32),('samples',U32),('tiling',I32),('usage',U32),('sharingMode',I32),('queueFamilyIndexCount',U32),('pQueueFamilyIndices',P(U32)),('initialLayout',I32)]
class CommandPoolCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('flags',U32),('queueFamilyIndex',U32)]
class CommandAllocateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('commandPool',H),('level',I32),('commandBufferCount',U32)]
class CommandBeginInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('flags',U32),('pInheritanceInfo',H)]
class FenceCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('flags',U32)]
class SubmitInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('waitSemaphoreCount',U32),('pWaitSemaphores',P(H)),('pWaitDstStageMask',P(U32)),('commandBufferCount',U32),('pCommandBuffers',P(H)),('signalSemaphoreCount',U32),('pSignalSemaphores',P(H))]
class ImageRange(C.Structure):
    _fields_=[('aspectMask',U32),('baseMipLevel',U32),('levelCount',U32),('baseArrayLayer',U32),('layerCount',U32)]
class ImageBarrier(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('srcAccessMask',U32),('dstAccessMask',U32),('oldLayout',I32),('newLayout',I32),('srcQueueFamilyIndex',U32),('dstQueueFamilyIndex',U32),('image',H),('subresourceRange',ImageRange)]
class MemoryBarrier(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('srcAccessMask',U32),('dstAccessMask',U32)]
class Layers(C.Structure):
    _fields_=[('aspectMask',U32),('mipLevel',U32),('baseArrayLayer',U32),('layerCount',U32)]
class Offset3D(C.Structure):
    _fields_=[('x',I32),('y',I32),('z',I32)]
class BufferImageCopy(C.Structure):
    _fields_=[('bufferOffset',U64),('bufferRowLength',U32),('bufferImageHeight',U32),('imageSubresource',Layers),('imageOffset',Offset3D),('imageExtent',V.Extent3D)]
class DebugData(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('flags',U32),('pMessageIdName',C.c_char_p),('messageIdNumber',I32),('pMessage',C.c_char_p)]
CALLBACK=C.WINFUNCTYPE(U32,U32,U32,P(DebugData),H) if os.name=='nt' else C.CFUNCTYPE(U32,U32,U32,P(DebugData),H)
class DebugCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',H),('flags',U32),('messageSeverity',U32),('messageType',U32),('pfnUserCallback',CALLBACK),('pUserData',H)]

def functions(api):
    signatures={
      'vkEnumerateInstanceLayerProperties':(I32,[P(U32),P(LayerProperties)]),
      'vkGetPhysicalDeviceMemoryProperties':(None,[H,P(MemoryProperties)]),
      'vkGetDeviceQueue':(None,[H,U32,U32,P(H)]),
      'vkCreateBuffer':(I32,[H,P(BufferCreateInfo),H,P(H)]),
      'vkDestroyBuffer':(None,[H,H,H]),
      'vkGetBufferMemoryRequirements':(None,[H,H,P(MemoryRequirements)]),
      'vkCreateImage':(I32,[H,P(ImageCreateInfo),H,P(H)]),
      'vkDestroyImage':(None,[H,H,H]),
      'vkGetImageMemoryRequirements':(None,[H,H,P(MemoryRequirements)]),
      'vkAllocateMemory':(I32,[H,P(AllocateInfo),H,P(H)]),
      'vkFreeMemory':(None,[H,H,H]),
      'vkBindBufferMemory':(I32,[H,H,H,U64]),
      'vkBindImageMemory':(I32,[H,H,H,U64]),
      'vkMapMemory':(I32,[H,H,U64,U64,U32,P(H)]),
      'vkUnmapMemory':(None,[H,H]),
      'vkCreateCommandPool':(I32,[H,P(CommandPoolCreateInfo),H,P(H)]),
      'vkDestroyCommandPool':(None,[H,H,H]),
      'vkAllocateCommandBuffers':(I32,[H,P(CommandAllocateInfo),P(H)]),
      'vkBeginCommandBuffer':(I32,[H,P(CommandBeginInfo)]),
      'vkEndCommandBuffer':(I32,[H]),
      'vkCmdPipelineBarrier':(None,[H,U32,U32,U32,U32,P(MemoryBarrier),U32,H,U32,P(ImageBarrier)]),
      'vkCmdCopyBufferToImage':(None,[H,H,H,I32,U32,P(BufferImageCopy)]),
      'vkCmdCopyImageToBuffer':(None,[H,H,I32,H,U32,P(BufferImageCopy)]),
      'vkCreateFence':(I32,[H,P(FenceCreateInfo),H,P(H)]),
      'vkDestroyFence':(None,[H,H,H]),
      'vkQueueSubmit':(I32,[H,U32,P(SubmitInfo),H]),
      'vkWaitForFences':(I32,[H,U32,P(H),U32,U64]),
    }
    for name,(result,args) in signatures.items():setattr(api,name,V.bind(api.dll,name,result,args))

def run(adapter_index):
    api=V.API();functions(api);instance=H();device=H();messenger=H();pool=H()
    result={'schema':1,'gpu_execution':True,'production_path_executed':False,'shader_execution':False,
      'scope':'independent Vulkan transfer, channel order, array-layer and layout-restoration oracle',
      'watchdog':'parent process; no Vulkan call cancellation','steps':[],'cases':[],'validation':{},'cleanup_complete':False}
    resources=[];pending=False;messages=[]
    def call(name,*args):
        start=time.perf_counter_ns();code=getattr(api,name)(*args)
        result['steps'].append({'operation':name,'vk_result':int(code),'elapsed_ns':time.perf_counter_ns()-start})
        if code!=0:raise V.VulkanFailure(name,code)
    @CALLBACK
    def debug(severity,kind,data,user):
        try:
            if len(messages)<128:messages.append({'severity':int(severity),'type':int(kind),
              'id':(data.contents.pMessageIdName or b'').decode('utf-8','replace'),
              'message':(data.contents.pMessage or b'').decode('utf-8','replace')[:4096]})
        except Exception:pass
        return 0
    destroy_debug=None
    try:
        exts=[bytes(x.extensionName).split(b'\0',1)[0] for x in V.enumerate_values(api.vkEnumerateInstanceExtensionProperties,[None],V.ExtensionProperties)]
        layers=[bytes(x.layerName).split(b'\0',1)[0] for x in V.enumerate_values(api.vkEnumerateInstanceLayerProperties,[],LayerProperties)]
        validation=b'VK_LAYER_KHRONOS_validation' in layers and b'VK_EXT_debug_utils' in exts
        enabled_layers=(C.c_char_p*1)(b'VK_LAYER_KHRONOS_validation') if validation else None
        enabled_exts=(C.c_char_p*1)(b'VK_EXT_debug_utils') if validation else None
        dci=DebugCreateInfo(sType=1000128004,messageSeverity=0x1100,messageType=7,pfnUserCallback=debug)
        app=V.ApplicationInfo(sType=0,pApplicationName=b'NXEmu independent readback oracle',applicationVersion=1,pEngineName=b'transfer-only',engineVersion=1,apiVersion=(1<<22)|(1<<12))
        ci=V.InstanceCreateInfo(sType=1,pNext=C.addressof(dci) if validation else None,pApplicationInfo=C.pointer(app),enabledLayerCount=int(validation),ppEnabledLayerNames=enabled_layers,enabledExtensionCount=int(validation),ppEnabledExtensionNames=enabled_exts)
        call('vkCreateInstance',C.byref(ci),None,C.byref(instance))
        result['validation']={'layer_enabled':validation,'available_layers':[x.decode() for x in layers],
          'message_capture':validation,'error_count':None if not validation else 0,'warning_count':None if not validation else 0}
        if validation:
            address=api.vkGetInstanceProcAddr(instance,b'vkCreateDebugUtilsMessengerEXT')
            create=C.WINFUNCTYPE(I32,H,P(DebugCreateInfo),H,P(H))(address)
            code=create(instance,C.byref(dci),None,C.byref(messenger))
            if code!=0:raise V.VulkanFailure('vkCreateDebugUtilsMessengerEXT',code)
            destroy_debug=C.WINFUNCTYPE(None,H,H,H)(api.vkGetInstanceProcAddr(instance,b'vkDestroyDebugUtilsMessengerEXT'))
        physicals=V.enumerate_values(api.vkEnumeratePhysicalDevices,[instance],H)
        if not 0<=adapter_index<len(physicals):raise ValueError('adapter index out of range')
        physical=physicals[adapter_index];props=V.PhysicalProperties();api.vkGetPhysicalDeviceProperties(physical,C.byref(props))
        if props.apiVersion<((1<<22)|(1<<12)):raise ValueError('Vulkan1.1 required for exact device UUID')
        ids=IDProperties(sType=1000071004);prop2=V.Properties2(sType=1000059001,pNext=C.addressof(ids));api.properties2(instance,physical,prop2)
        result['adapter']={'selection':'explicit index; not emulator identity','index':adapter_index,'name':bytes(props.deviceName).split(b'\0',1)[0].decode(),
          'vendor_id':props.vendorID,'device_id':props.deviceID,'api_version':props.apiVersion,'driver_version':props.driverVersion,
          'device_uuid':bytes(ids.deviceUUID).hex(),'driver_uuid':bytes(ids.driverUUID).hex(),'pipeline_cache_uuid':bytes(props.pipelineCacheUUID).hex()}
        count=U32();api.vkGetPhysicalDeviceQueueFamilyProperties(physical,C.byref(count),None)
        families=(V.QueueFamilyProperties*count.value)();api.vkGetPhysicalDeviceQueueFamilyProperties(physical,C.byref(count),families)
        family=next(i for i,q in enumerate(families) if q.queueCount and q.queueFlags&1)
        priorities=(C.c_float*1)(1.0);queue_ci=V.QueueCreateInfo(sType=2,queueFamilyIndex=family,queueCount=1,pQueuePriorities=priorities)
        device_ci=V.DeviceCreateInfo(sType=3,queueCreateInfoCount=1,pQueueCreateInfos=C.pointer(queue_ci))
        call('vkCreateDevice',physical,C.byref(device_ci),None,C.byref(device));queue=H();api.vkGetDeviceQueue(device,family,0,C.byref(queue))
        memprops=MemoryProperties();api.vkGetPhysicalDeviceMemoryProperties(physical,C.byref(memprops))
        pool_ci=CommandPoolCreateInfo(sType=39,queueFamilyIndex=family)
        call('vkCreateCommandPool',device,C.byref(pool_ci),None,C.byref(pool))
        def allocate(req,flags):
            index=next(i for i in range(memprops.memoryTypeCount) if req.memoryTypeBits&(1<<i) and memprops.memoryTypes[i].propertyFlags&flags==flags)
            m=H();aci=AllocateInfo(sType=5,allocationSize=req.size,memoryTypeIndex=index)
            call('vkAllocateMemory',device,C.byref(aci),None,C.byref(m));resources.append(('memory',m));return m
        def buffer(size,usage):
            b=H();info=BufferCreateInfo(sType=12,size=size,usage=usage,sharingMode=0)
            call('vkCreateBuffer',device,C.byref(info),None,C.byref(b));resources.append(('buffer',b))
            req=MemoryRequirements();api.vkGetBufferMemoryRequirements(device,b,C.byref(req));m=allocate(req,6)
            call('vkBindBufferMemory',device,b,m,0);return b,m
        def map_bytes(m,size,data=None):
            ptr=H();call('vkMapMemory',device,m,0,size,0,C.byref(ptr))
            try:
                if data is not None:C.memmove(ptr,data,len(data))
                return C.string_at(ptr,size)
            finally:api.vkUnmapMemory(device,m)
        def image_barrier(cmd,image,old,new,src,dst):
            barrier=ImageBarrier(sType=45,srcAccessMask=src,dstAccessMask=dst,oldLayout=old,newLayout=new,
              srcQueueFamilyIndex=0xffffffff,dstQueueFamilyIndex=0xffffffff,image=image,subresourceRange=ImageRange(1,0,1,0,2))
            api.vkCmdPipelineBarrier(cmd,0x10000,0x10000,0,0,None,0,None,1,C.byref(barrier))
        def mem_barrier(cmd,src,dst):
            barrier=MemoryBarrier(sType=46,srcAccessMask=src,dstAccessMask=dst)
            src_stage=0x4000 if src&0x6000 else 0x1000
            dst_stage=0x4000 if dst&0x6000 else 0x1000
            api.vkCmdPipelineBarrier(cmd,src_stage,dst_stage,0,1,C.byref(barrier),0,None,0,None)
        for fmt,name in [(37,'RGBA8_UNORM'),(43,'RGBA8_SRGB'),(44,'BGRA8_UNORM'),
                         (50,'BGRA8_SRGB'),(51,'A8B8G8R8_UNORM_PACK32'),(57,'A8B8G8R8_SRGB_PACK32')]:
            colors=[bytes([255,0,0,255,0,255,0,0,0,0,255,127,10,20,30,17]),bytes([0,0,0,0,5,60,100,255,80,9,250,128,254,253,252,1])]
            raw=[]
            for layer in colors:
                raw.append(layer if fmt not in (44,50) else b''.join(bytes((layer[i+2],layer[i+1],layer[i],layer[i+3])) for i in range(0,16,4)))
            upload,upload_mem=buffer(32,1);download,download_mem=buffer(32,2)
            map_bytes(upload_mem,32,b''.join(raw));image=H()
            ici=ImageCreateInfo(sType=14,imageType=1,format=fmt,extent=V.Extent3D(2,2,1),mipLevels=1,arrayLayers=2,samples=1,tiling=0,usage=3,sharingMode=0,initialLayout=0)
            call('vkCreateImage',device,C.byref(ici),None,C.byref(image));resources.append(('image',image))
            req=MemoryRequirements();api.vkGetImageMemoryRequirements(device,image,C.byref(req));image_mem=allocate(req,0);call('vkBindImageMemory',device,image,image_mem,0)
            passes=[]
            for attempt in range(2):
                map_bytes(download_mem,32,b'\xcc'*32)
                cmd=H();a=CommandAllocateInfo(sType=40,commandPool=pool,level=0,commandBufferCount=1);call('vkAllocateCommandBuffers',device,C.byref(a),C.byref(cmd))
                begin=CommandBeginInfo(sType=42,flags=1);call('vkBeginCommandBuffer',cmd,C.byref(begin))
                regions=(BufferImageCopy*2)(*(BufferImageCopy(bufferOffset=16*layer,imageSubresource=Layers(1,0,layer,1),imageExtent=V.Extent3D(2,2,1)) for layer in range(2)))
                if attempt==0:
                    mem_barrier(cmd,0x4000,0x800)
                    image_barrier(cmd,image,0,1,0,0x1000)
                    api.vkCmdCopyBufferToImage(cmd,upload,image,1,2,regions)
                image_barrier(cmd,image,1,6,0x18000|0x1000,0x800)
                # Separate copy calls explicitly address nonzero array layer, not a combined whole-image guess.
                for layer in (1,0):api.vkCmdCopyImageToBuffer(cmd,image,6,download,1,C.byref(regions[layer]))
                image_barrier(cmd,image,6,1,0x800,0x18000)
                mem_barrier(cmd,0x1000,0x2000)
                call('vkEndCommandBuffer',cmd);fence=H();fci=FenceCreateInfo(sType=8);call('vkCreateFence',device,C.byref(fci),None,C.byref(fence));resources.append(('fence',fence))
                cmds=(H*1)(cmd);submit=SubmitInfo(sType=4,commandBufferCount=1,pCommandBuffers=cmds)
                call('vkQueueSubmit',queue,1,C.byref(submit),fence);pending=True
                fences=(H*1)(fence);call('vkWaitForFences',device,1,fences,1,10_000_000_000);pending=False
                obtained=map_bytes(download_mem,32);assert obtained==b''.join(raw),(name,attempt,obtained.hex())
                passes.append({'attempt':attempt,'restored_general_before_read':attempt==1,'output_hex':obtained.hex(),'sha256':hashlib.sha256(obtained).hexdigest()})
            result['cases'].append({'format':name,'vk_format':fmt,'width':2,'height':2,'mip':0,'layers':[0,1],
              'samples':1,'input_hex':[x.hex() for x in raw],'rgba_expected_hex':[x.hex() for x in colors],'passes':passes})
        result['result']='VULKAN_READBACK_PASS';result['numerical_readback_matches']=True
    except Exception as error:
        result['result']='VULKAN_READBACK_FAIL';result['error']=str(error)
    finally:
        # On timeout/device error after submit, leave Vulkan-owned allocations until process exit.
        # Never free resources merely because the observer deadline passed.
        if not pending:
            # Bound object destruction order: image/buffer BEFORE bound memory, pools BEFORE device.
            for kind,h in resources:
                if kind=='image':api.vkDestroyImage(device,h,None)
                elif kind=='buffer':api.vkDestroyBuffer(device,h,None)
                elif kind=='fence':api.vkDestroyFence(device,h,None)
            for kind,h in resources:
                if kind=='memory':api.vkFreeMemory(device,h,None)
            if pool:api.vkDestroyCommandPool(device,pool,None)
            if device:api.vkDestroyDevice(device,None)
            if messenger and destroy_debug:destroy_debug(instance,messenger,None)
            if instance:api.vkDestroyInstance(instance,None)
            result['cleanup_complete']=True
        result['validation']['messages']=messages
        if result['validation'].get('message_capture'):
            result['validation']['error_count']=sum(bool(m['severity']&0x1000) for m in messages)
            result['validation']['warning_count']=sum(bool(m['severity']&0x100) for m in messages)
            result['validation']['validation_error_count']=sum(bool(m['severity']&0x1000 and m['type']&2) for m in messages)
            result['validation']['loader_error_count']=sum(bool(m['severity']&0x1000 and m['id']=='Loader Message') for m in messages)
            if result['validation']['validation_error_count']:result['result']='VULKAN_READBACK_FAIL'
    return result

def main():
    parser=argparse.ArgumentParser();parser.add_argument('--out',type=Path,required=True);parser.add_argument('--adapter-index',type=int,default=0);parser.add_argument('--worker',action='store_true');args=parser.parse_args()
    args.out.mkdir(parents=True,exist_ok=True);result_path=args.out/'RESULT.json'
    if args.worker:
        result=run(args.adapter_index);result_path.write_text(json.dumps(result,indent=2),encoding='utf-8')
        print(result['result']);return int(result['result']!='VULKAN_READBACK_PASS')
    command=[sys.executable,'-B',str(Path(__file__).resolve()),'--worker','--out',str(args.out.resolve()),'--adapter-index',str(args.adapter_index)]
    start=time.perf_counter()
    try:
        worker=subprocess.run(command,capture_output=True,text=True,timeout=25)
        record={'command':subprocess.list2cmdline(command),'input':'2x2 RGBA/BGRA two-layer color+alpha GPU upload/copy; GENERAL restore/re-read',
          'literal_stdout':worker.stdout,'literal_stderr':worker.stderr,'exit_status':worker.returncode,'elapsed_seconds':time.perf_counter()-start}
    except subprocess.TimeoutExpired as error:
        record={'command':subprocess.list2cmdline(command),'exit_status':124,'result':'parent_watchdog_terminated_worker','Vulkan_call_cancelled':False,'elapsed_seconds':time.perf_counter()-start}
    (args.out/'EXECUTION.json').write_text(json.dumps(record,indent=2),encoding='utf-8');print(json.dumps(record));return record['exit_status']
if __name__=='__main__':sys.exit(main())
