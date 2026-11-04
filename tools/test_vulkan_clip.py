"""Independent Vulkan clip experiment. Real shaders/vertex/index resources and readback.
Not emulator production execution, not native CSMTEST, and not hardware binding proof.
Worker owns all handles; parent30s watchdog kills worker without in-call cancellation.
"""
from pathlib import Path
import argparse,ctypes as C,hashlib,json,os,subprocess,sys,time,struct,math
import pipeline_probe as V
import test_vulkan_readback as R
import native_draw_inputs as N
U32=V.U32;I32=V.I32;U64=V.U64;H=V.Handle;P=C.POINTER;F=C.c_float

def S(name,fields):return type(name,(C.Structure,),{'_fields_':fields})
BASE=[('sType',I32),('pNext',H),('flags',U32)]
Attachment=S('Attachment',[(x,U32) for x in ['flags','format','samples','loadOp','storeOp','stencilLoadOp','stencilStoreOp','initialLayout','finalLayout']])
Ref=S('Ref',[('attachment',U32),('layout',I32)])
Subpass=S('Subpass',[('flags',U32),('pipelineBindPoint',I32),('inputAttachmentCount',U32),('pInputAttachments',P(Ref)),('colorAttachmentCount',U32),('pColorAttachments',P(Ref)),('pResolveAttachments',P(Ref)),('pDepthStencilAttachment',P(Ref)),('preserveAttachmentCount',U32),('pPreserveAttachments',P(U32))])
Dependency=S('Dependency',[(x,U32) for x in ['srcSubpass','dstSubpass','srcStageMask','dstStageMask','srcAccessMask','dstAccessMask','dependencyFlags']])
RenderPassCI=S('RenderPassCI',BASE+[('attachmentCount',U32),('pAttachments',P(Attachment)),('subpassCount',U32),('pSubpasses',P(Subpass)),('dependencyCount',U32),('pDependencies',P(Dependency))])
ViewCI=S('ViewCI',BASE+[('image',H),('viewType',I32),('format',I32),('components',U32*4),('subresourceRange',R.ImageRange)])
FramebufferCI=S('FramebufferCI',BASE+[('renderPass',H),('attachmentCount',U32),('pAttachments',P(H)),('width',U32),('height',U32),('layers',U32)])
Offset2=S('Offset2',[('x',I32),('y',I32)]);Extent2=S('Extent2',[('width',U32),('height',U32)]);Rect=S('Rect',[('offset',Offset2),('extent',Extent2)])
RenderBegin=S('RenderBegin',[('sType',I32),('pNext',H),('renderPass',H),('framebuffer',H),('renderArea',Rect),('clearValueCount',U32),('pClearValues',H)])
Binding=S('Binding',[(x,U32) for x in ['binding','stride','inputRate']]);Attribute=S('Attribute',[(x,U32) for x in ['location','binding','format','offset']])
VertexCI=S('VertexCI',BASE+[('vertexBindingDescriptionCount',U32),('pVertexBindingDescriptions',P(Binding)),('vertexAttributeDescriptionCount',U32),('pVertexAttributeDescriptions',P(Attribute))])
AssemblyCI=S('AssemblyCI',BASE+[('topology',I32),('primitiveRestartEnable',U32)])
Viewport=S('Viewport',[(x,F) for x in ['x','y','width','height','minDepth','maxDepth']])
ViewportCI=S('ViewportCI',BASE+[('viewportCount',U32),('pViewports',P(Viewport)),('scissorCount',U32),('pScissors',P(Rect))])
RasterCI=S('RasterCI',BASE+[('depthClampEnable',U32),('rasterizerDiscardEnable',U32),('polygonMode',I32),('cullMode',U32),('frontFace',I32),('depthBiasEnable',U32)]+[(x,F) for x in ['depthBiasConstantFactor','depthBiasClamp','depthBiasSlopeFactor','lineWidth']])
MultiCI=S('MultiCI',BASE+[('rasterizationSamples',U32),('sampleShadingEnable',U32),('minSampleShading',F),('pSampleMask',P(U32)),('alphaToCoverageEnable',U32),('alphaToOneEnable',U32)])
Blend=S('Blend',[(x,U32) for x in ['blendEnable','srcColorBlendFactor','dstColorBlendFactor','colorBlendOp','srcAlphaBlendFactor','dstAlphaBlendFactor','alphaBlendOp','colorWriteMask']])
BlendCI=S('BlendCI',BASE+[('logicOpEnable',U32),('logicOp',I32),('attachmentCount',U32),('pAttachments',P(Blend)),('blendConstants',F*4)])
GraphicsCI=S('GraphicsCI',BASE+[('stageCount',U32),('pStages',P(V.ShaderStageCreateInfo))]+[(x,H) for x in ['pVertexInputState','pInputAssemblyState','pTessellationState','pViewportState','pRasterizationState','pMultisampleState','pDepthStencilState','pColorBlendState','pDynamicState','layout','renderPass']]+[('subpass',U32),('basePipelineHandle',H),('basePipelineIndex',I32)])
DrawFeatures=S('DrawFeatures',[('sType',I32),('pNext',H),('shaderDrawParameters',U32)])
Features2=S('Features2',[('sType',I32),('pNext',H),('features',U32*55)])
PoolSize=S('PoolSize',[('type',I32),('descriptorCount',U32)])
PoolCI=S('PoolCI',BASE+[('maxSets',U32),('poolSizeCount',U32),('pPoolSizes',P(PoolSize))])
SetAllocate=S('SetAllocate',[('sType',I32),('pNext',H),('descriptorPool',H),('descriptorSetCount',U32),('pSetLayouts',P(H))])
BufferInfo=S('BufferInfo',[('buffer',H),('offset',U64),('range',U64)])
WriteSet=S('WriteSet',[('sType',I32),('pNext',H),('dstSet',H),('dstBinding',U32),('dstArrayElement',U32),('descriptorCount',U32),('descriptorType',I32),('pImageInfo',H),('pBufferInfo',P(BufferInfo)),('pTexelBufferView',P(H))])
QueryCI=S('QueryCI',BASE+[('queryType',I32),('queryCount',U32),('pipelineStatistics',U32)])
def functions(api):
 R.functions(api)
 sig={
 'vkCreateDescriptorPool':(I32,[H,P(PoolCI),H,P(H)]),'vkDestroyDescriptorPool':(None,[H,H,H]),
 'vkAllocateDescriptorSets':(I32,[H,P(SetAllocate),P(H)]),'vkUpdateDescriptorSets':(None,[H,U32,P(WriteSet),U32,H]),
 'vkCmdBindDescriptorSets':(None,[H,I32,H,U32,U32,P(H),U32,P(U32)]),
 'vkGetPhysicalDeviceFeatures':(None,[H,P(U32)]),
 'vkCreateRenderPass':(I32,[H,P(RenderPassCI),H,P(H)]),'vkDestroyRenderPass':(None,[H,H,H]),
 'vkCreateImageView':(I32,[H,P(ViewCI),H,P(H)]),'vkDestroyImageView':(None,[H,H,H]),
 'vkCreateFramebuffer':(I32,[H,P(FramebufferCI),H,P(H)]),'vkDestroyFramebuffer':(None,[H,H,H]),
 'vkCreateGraphicsPipelines':(I32,[H,H,U32,P(GraphicsCI),H,P(H)]),
 'vkCreateQueryPool':(I32,[H,P(QueryCI),H,P(H)]),'vkDestroyQueryPool':(None,[H,H,H]),
 'vkGetQueryPoolResults':(I32,[H,H,U32,U32,C.c_size_t,H,U64,U32]),
 'vkCmdResetQueryPool':(None,[H,H,U32,U32]),'vkCmdBeginQuery':(None,[H,H,U32,U32]),'vkCmdEndQuery':(None,[H,H,U32]),
 'vkCmdBeginRenderPass':(None,[H,P(RenderBegin),I32]),'vkCmdEndRenderPass':(None,[H]),
 'vkCmdBindPipeline':(None,[H,I32,H]),'vkCmdBindVertexBuffers':(None,[H,U32,U32,P(H),P(U64)]),
 'vkCmdBindIndexBuffer':(None,[H,H,U64,I32]),'vkCmdDrawIndexed':(None,[H,U32,U32,U32,I32,U32])}
 for name,(ret,args) in sig.items():setattr(api,name,V.bind(api.dll,name,ret,args))
VERT='#version 450\nlayout(location=0) in vec4 position;void main(){gl_Position=position;}\n'
FRAG='#version 450\nlayout(location=0) out vec4 color;void main(){color=vec4(1,0.25,0.5,1);}\n'
def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def capabilities(path):
    if not 20<=path.stat().st_size<=16*1024*1024 or path.stat().st_size%4:raise ValueError('SPV size')
    words=struct.unpack('<'+str(path.stat().st_size//4)+'I',path.read_bytes());assert words[0]==0x07230203;offset=5;caps=set()
    while offset<len(words):
        count=words[offset]>>16;opcode=words[offset]&0xffff
        if not count or offset+count>len(words):raise ValueError('SPV instruction bounds')
        if opcode==17:
            if count!=2:raise ValueError('SPV capability instruction')
            caps.add(words[offset+1])
        offset+=count
    unknown=caps-{1,25,4427,50,46}
    if unknown:raise ValueError('Unsupported capabilities in this minimal tool: '+str(sorted(unknown)))
    return caps
def source_case(capture):
 if not capture:return {'name':'fullscreen','positions':[[-1,-1,0,1],[3,-1,0,1],[-1,3,0,1]],'indices':[0,1,2],'source':'synthetic fixed fullscreen triangle'}
 d=json.loads(capture.read_text());c=d['cases'][0];manifest=Path(c['files']['cbuf']['path']).with_name(c['data']['id']+'.json');verified=N.read_case(manifest,d['live_state']);assert verified['matches_current_session']
 for role,f in verified['files'].items():assert f['sha256']==c['files'][role]['sha256']
 b=Path(verified['files']['cbuf']['path']).read_bytes();assert verified['data']['cbuf']['bank']==1 and verified['data']['cbuf']['offset']==0
 positions=[list(struct.unpack_from('<4f',b,n*32)) for n in range(3)];indices=verified['index_input']['index_prefix_values'];assert indices==[0,1,2] and all(math.isfinite(x) for p in positions for x in p)
 return {'name':'captured_fullscreen','positions':positions,'indices':indices,'source':'verified historical CPU bank1 stride32 first three vec4 + decoded index prefix; re-uploaded vertexbuffer, not actual shader replay','manifest':str(manifest),'capture_sha256':sha(capture),'manifest_sha256':sha(manifest),'verified_files':verified['files'],'session':d['live_state']}
def run(adapter_index,out,capture,vertex_spv,position_binding):
    api=V.API();functions(api);instance=H();device=H();messenger=H();pool=H()
    result={'schema':1,'gpu_execution':False,'production_path_executed':False,'shader_execution':False,'native_CSMTEST_executed':False,
      'scope':'independent graphics vertex/index/shader clip experiment, not production GPU path or native CSMTEST',
      'watchdog':'parent process; no Vulkan call cancellation','steps':[],'cases':[],'validation':{},'cleanup_complete':False}
    resources=[];pending=False;messages=[];messages_seen=0
    def call(name,*args):
        start=time.perf_counter_ns();code=getattr(api,name)(*args)
        result['steps'].append({'operation':name,'vk_result':int(code),'elapsed_ns':time.perf_counter_ns()-start})
        if code!=0:raise V.VulkanFailure(name,code)
    @R.CALLBACK
    def debug(severity,kind,data,user):
        nonlocal messages_seen
        messages_seen+=1
        try:
            if len(messages)<128:messages.append({'severity':int(severity),'type':int(kind),
              'id':(data.contents.pMessageIdName or b'').decode('utf-8','replace'),
              'message':(data.contents.pMessage or b'').decode('utf-8','replace')[:4096]})
        except Exception:pass
        return 0
    destroy_debug=None
    try:
        exts=[bytes(x.extensionName).split(b'\0',1)[0] for x in V.enumerate_values(api.vkEnumerateInstanceExtensionProperties,[None],V.ExtensionProperties)]
        layers=[bytes(x.layerName).split(b'\0',1)[0] for x in V.enumerate_values(api.vkEnumerateInstanceLayerProperties,[],R.LayerProperties)]
        validation=b'VK_LAYER_KHRONOS_validation' in layers and b'VK_EXT_debug_utils' in exts
        enabled_layers=(C.c_char_p*1)(b'VK_LAYER_KHRONOS_validation') if validation else None
        enabled_exts=(C.c_char_p*1)(b'VK_EXT_debug_utils') if validation else None
        dci=R.DebugCreateInfo(sType=1000128004,messageSeverity=0x1100,messageType=7,pfnUserCallback=debug)
        app=V.ApplicationInfo(sType=0,pApplicationName=b'NXEmu independent readback oracle',applicationVersion=1,pEngineName=b'transfer-only',engineVersion=1,apiVersion=(1<<22)|(1<<12))
        ci=V.InstanceCreateInfo(sType=1,pNext=C.addressof(dci) if validation else None,pApplicationInfo=C.pointer(app),enabledLayerCount=int(validation),ppEnabledLayerNames=enabled_layers,enabledExtensionCount=int(validation),ppEnabledExtensionNames=enabled_exts)
        call('vkCreateInstance',C.byref(ci),None,C.byref(instance))
        result['validation']={'layer_enabled':validation,'available_layers':[x.decode() for x in layers],
          'message_capture':validation,'error_count':None if not validation else 0,'warning_count':None if not validation else 0}
        if validation:
            address=api.vkGetInstanceProcAddr(instance,b'vkCreateDebugUtilsMessengerEXT')
            create=C.WINFUNCTYPE(I32,H,P(R.DebugCreateInfo),H,P(H))(address)
            code=create(instance,C.byref(dci),None,C.byref(messenger))
            if code!=0:raise V.VulkanFailure('vkCreateDebugUtilsMessengerEXT',code)
            destroy_debug=C.WINFUNCTYPE(None,H,H,H)(api.vkGetInstanceProcAddr(instance,b'vkDestroyDebugUtilsMessengerEXT'))
        physicals=V.enumerate_values(api.vkEnumeratePhysicalDevices,[instance],H)
        inventory=[]
        for i,physical in enumerate(physicals):
            prop=V.PhysicalProperties();api.vkGetPhysicalDeviceProperties(physical,C.byref(prop));inventory.append({'index':i,'name':bytes(prop.deviceName).split(b'\0',1)[0].decode(),'vendor_id':prop.vendorID,'device_id':prop.deviceID})
        result['adapter_inventory']=inventory
        if not 0<=adapter_index<len(physicals):raise ValueError('adapter index out of range')
        physical=physicals[adapter_index];props=V.PhysicalProperties();api.vkGetPhysicalDeviceProperties(physical,C.byref(props))
        if props.apiVersion<((1<<22)|(1<<12)):raise ValueError('Vulkan1.1 required for exact device UUID')
        ids=R.IDProperties(sType=1000071004);prop2=V.Properties2(sType=1000059001,pNext=C.addressof(ids));api.properties2(instance,physical,prop2)
        result['adapter']={'selection':'explicit index; not emulator identity','index':adapter_index,'name':bytes(props.deviceName).split(b'\0',1)[0].decode(),
          'vendor_id':props.vendorID,'device_id':props.deviceID,'api_version':props.apiVersion,'driver_version':props.driverVersion,
          'device_uuid':bytes(ids.deviceUUID).hex(),'driver_uuid':bytes(ids.driverUUID).hex(),'pipeline_cache_uuid':bytes(props.pipelineCacheUUID).hex(),'device_luid':bytes(ids.deviceLUID).hex(),'device_luid_valid':bool(ids.deviceLUIDValid),'node_mask':ids.deviceNodeMask}
        count=U32();api.vkGetPhysicalDeviceQueueFamilyProperties(physical,C.byref(count),None)
        families=(V.QueueFamilyProperties*count.value)();api.vkGetPhysicalDeviceQueueFamilyProperties(physical,C.byref(count),families)
        family=next(i for i,q in enumerate(families) if q.queueCount and q.queueFlags&1)
        priorities=(C.c_float*1)(1.0);queue_ci=V.QueueCreateInfo(sType=2,queueFamilyIndex=family,queueCount=1,pQueuePriorities=priorities)
        features=(U32*55)();api.vkGetPhysicalDeviceFeatures(physical,features);enabled=(U32*55)();enabled[23]=features[23];enabled[24]=features[24]
        caps=capabilities(out/'clip.vert.spv')|capabilities(out/'clip.frag.spv');draw_feature=None;draw_supported=None
        if 25 in caps:
            if not features[28]:raise ValueError('ImageGatherExtended requires unsupported shaderImageGatherExtended')
            enabled[28]=1
        if 4427 in caps:
            support=DrawFeatures(sType=1000063000);f2=Features2(sType=1000059000,pNext=C.addressof(support));addr=api.vkGetInstanceProcAddr(instance,b'vkGetPhysicalDeviceFeatures2')
            if not addr:raise ValueError('Features2 entry unavailable for DrawParameters')
            C.WINFUNCTYPE(None,H,P(Features2))(addr)(physical,C.byref(f2));draw_supported=bool(support.shaderDrawParameters)
            if not draw_supported:raise ValueError('DrawParameters requires unsupported shaderDrawParameters')
            draw_feature=DrawFeatures(sType=1000063000,shaderDrawParameters=1)
        result['query_features']={'occlusion_precise':bool(features[23]),'pipeline_statistics':bool(features[24])}
        result['shader_capabilities']=sorted(caps)
        result['device_features']={'physical_support':{'occlusionQueryPrecise':bool(features[23]),'pipelineStatisticsQuery':bool(features[24]),'shaderImageGatherExtended':bool(features[28]),'shaderDrawParameters':draw_supported},
          'enabled_core':{'occlusionQueryPrecise':bool(enabled[23]),'pipelineStatisticsQuery':bool(enabled[24]),'shaderImageGatherExtended':bool(enabled[28])},
          'enabled_chain':[{'s_type':1000063000,'shaderDrawParameters':True}] if draw_feature else [],'unknown_capabilities_rejected':True}
        device_ci=V.DeviceCreateInfo(sType=3,pNext=C.addressof(draw_feature) if draw_feature else None,queueCreateInfoCount=1,pQueueCreateInfos=C.pointer(queue_ci),pEnabledFeatures=C.addressof(enabled))
        call('vkCreateDevice',physical,C.byref(device_ci),None,C.byref(device));queue=H();api.vkGetDeviceQueue(device,family,0,C.byref(queue))
        memprops=R.MemoryProperties();api.vkGetPhysicalDeviceMemoryProperties(physical,C.byref(memprops))
        pool_ci=R.CommandPoolCreateInfo(sType=39,queueFamilyIndex=family)
        call('vkCreateCommandPool',device,C.byref(pool_ci),None,C.byref(pool))
        def allocate(req,flags):
            index=next(i for i in range(memprops.memoryTypeCount) if req.memoryTypeBits&(1<<i) and memprops.memoryTypes[i].propertyFlags&flags==flags)
            m=H();aci=R.AllocateInfo(sType=5,allocationSize=req.size,memoryTypeIndex=index)
            call('vkAllocateMemory',device,C.byref(aci),None,C.byref(m));resources.append(('memory',m));return m
        def buffer(size,usage):
            b=H();info=R.BufferCreateInfo(sType=12,size=size,usage=usage,sharingMode=0)
            call('vkCreateBuffer',device,C.byref(info),None,C.byref(b));resources.append(('buffer',b))
            req=R.MemoryRequirements();api.vkGetBufferMemoryRequirements(device,b,C.byref(req));m=allocate(req,6)
            call('vkBindBufferMemory',device,b,m,0);return b,m
        def map_bytes(m,size,data=None):
            ptr=H();call('vkMapMemory',device,m,0,size,0,C.byref(ptr))
            try:
                if data is not None:C.memmove(ptr,data,len(data))
                return C.string_at(ptr,size)
            finally:api.vkUnmapMemory(device,m)
        def image_barrier(cmd,image,old,new,src,dst):
            barrier=R.ImageBarrier(sType=45,srcAccessMask=src,dstAccessMask=dst,oldLayout=old,newLayout=new,
              srcQueueFamilyIndex=0xffffffff,dstQueueFamilyIndex=0xffffffff,image=image,subresourceRange=R.ImageRange(1,0,1,0,2))
            api.vkCmdPipelineBarrier(cmd,0x10000,0x10000,0,0,None,0,None,1,C.byref(barrier))
        def mem_barrier(cmd,src,dst):
            barrier=R.MemoryBarrier(sType=46,srcAccessMask=src,dstAccessMask=dst)
            src_stage=0x4000 if src&0x6000 else 0x1000
            dst_stage=0x4000 if dst&0x6000 else 0x1000
            api.vkCmdPipelineBarrier(cmd,src_stage,dst_stage,0,1,C.byref(barrier),0,None,0,None)
        fixed=source_case(capture);result['input_case']=fixed
        cases=[fixed,{'name':'same_plane_outside','positions':[[-3,-1,0,1],[-2,3,0,1],[-2,-3,0,1]],'indices':[0,1,2]},
          {'name':'different_plane_mixed','positions':[[-2,-2,0,1],[2,-2,0,1],[0,2,0,1]],'indices':[0,1,2]}]
        modules=[]
        for suffix,source in [('vert',VERT),('frag',FRAG)]:
            path=out/('clip.'+suffix);spv=path.with_suffix('.'+suffix+'.spv');assert 20<=spv.stat().st_size<=16*1024*1024;raw=spv.read_bytes();assert len(raw)%4==0
            words=(U32*(len(raw)//4)).from_buffer_copy(raw);module=H();ci=V.ShaderModuleCreateInfo(sType=16,codeSize=len(raw),pCode=words)
            call('vkCreateShaderModule',device,C.byref(ci),None,C.byref(module));resources.append(('shader',module));modules.append(module)
        result['shaders']={suffix:{'source_used':not(vertex_spv and suffix=='vert'),'source_sha256':sha(out/('clip.'+suffix)),'spv_sha256':sha(out/('clip.'+suffix+'.spv'))} for suffix in ['vert','frag']}
        attachment=Attachment(format=37,samples=1,loadOp=1,storeOp=0,stencilLoadOp=2,stencilStoreOp=1,initialLayout=0,finalLayout=6)
        ref=Ref(0,2);subpass=Subpass(pipelineBindPoint=0,colorAttachmentCount=1,pColorAttachments=C.pointer(ref))
        deps=(Dependency*2)(Dependency(0xffffffff,0,1,0x400,0,0x100,0),Dependency(0,0xffffffff,0x400,0x1000,0x100,0x800,0))
        rp=H();ci=RenderPassCI(sType=38,attachmentCount=1,pAttachments=C.pointer(attachment),subpassCount=1,pSubpasses=C.pointer(subpass),dependencyCount=2,pDependencies=deps)
        call('vkCreateRenderPass',device,C.byref(ci),None,C.byref(rp));resources.append(('renderpass',rp))
        descriptor_layout=H();descriptor_pool=H();zero_buffer=None;descriptor_bindings=[]
        if vertex_spv:
            words=struct.unpack('<'+str(vertex_spv.stat().st_size//4)+'I',vertex_spv.read_bytes());decorations={};offset=5
            while offset<len(words):
                count=words[offset]>>16;opcode=words[offset]&0xffff;assert count and offset+count<=len(words)
                if opcode==71 and count==4 and words[offset+2] in (33,34):decorations.setdefault(words[offset+1],{})[words[offset+2]]=words[offset+3]
                offset+=count
            descriptor_bindings=sorted(v[33] for v in decorations.values() if v.get(34)==0 and 33 in v)
            assert descriptor_bindings==list(range(14)),descriptor_bindings
            assert position_binding in descriptor_bindings
            bindings=(V.DescriptorBinding*14)(*(V.DescriptorBinding(binding=n,descriptorType=6,descriptorCount=1,stageFlags=1) for n in descriptor_bindings))
            ci=V.DescriptorLayoutCreateInfo(sType=32,bindingCount=14,pBindings=bindings);call('vkCreateDescriptorSetLayout',device,C.byref(ci),None,C.byref(descriptor_layout));resources.append(('descriptor_layout',descriptor_layout))
            ps=PoolSize(6,42);ci=PoolCI(sType=33,maxSets=3,poolSizeCount=1,pPoolSizes=C.pointer(ps));call('vkCreateDescriptorPool',device,C.byref(ci),None,C.byref(descriptor_pool));resources.append(('descriptor_pool',descriptor_pool))
            zero_buffer,zero_mem=buffer(65536,0x10);map_bytes(zero_mem,65536,b'\0'*65536)
            result['native_vertex_spv']={'path':str(vertex_spv),'sha256':sha(vertex_spv),'descriptor_set':0,'ubo_bindings':descriptor_bindings,'position_binding':position_binding,'bank_bytes':65536,'input_scope':'externally supplied SPV; provenance requires companion evidence; not original native program or CSMTEST'}
        layouts=(H*1)(descriptor_layout) if vertex_spv else None
        layout=H();ci=V.PipelineLayoutCreateInfo(sType=30,setLayoutCount=1 if vertex_spv else 0,pSetLayouts=layouts);call('vkCreatePipelineLayout',device,C.byref(ci),None,C.byref(layout));resources.append(('layout',layout))
        stages=(V.ShaderStageCreateInfo*2)(V.ShaderStageCreateInfo(sType=18,stage=1,module=modules[0],pName=b'main'),V.ShaderStageCreateInfo(sType=18,stage=16,module=modules[1],pName=b'main'))
        binding=Binding(0,16,0);attribute=Attribute(0,0,109,0)
        vertex=VertexCI(sType=19,vertexBindingDescriptionCount=1,pVertexBindingDescriptions=C.pointer(binding),vertexAttributeDescriptionCount=1,pVertexAttributeDescriptions=C.pointer(attribute))
        if vertex_spv:vertex=VertexCI(sType=19)
        assembly=AssemblyCI(sType=20,topology=3);viewport=Viewport(0,0,16,16,0,1);rect=Rect(Offset2(0,0),Extent2(16,16))
        vp=ViewportCI(sType=22,viewportCount=1,pViewports=C.pointer(viewport),scissorCount=1,pScissors=C.pointer(rect))
        raster=RasterCI(sType=23,lineWidth=1);multi=MultiCI(sType=24,rasterizationSamples=1);blend=Blend(colorWriteMask=15);bc=BlendCI(sType=26,attachmentCount=1,pAttachments=C.pointer(blend))
        # VkPipelineColorBlendStateCreateInfo is26, VkGraphicsPipelineCreateInfo is28.
        ci=GraphicsCI(sType=28,stageCount=2,pStages=stages,pVertexInputState=C.addressof(vertex),pInputAssemblyState=C.addressof(assembly),pViewportState=C.addressof(vp),pRasterizationState=C.addressof(raster),pMultisampleState=C.addressof(multi),pColorBlendState=C.addressof(bc),layout=layout,renderPass=rp,basePipelineIndex=-1)
        pipeline=H();call('vkCreateGraphicsPipelines',device,None,1,C.byref(ci),None,C.byref(pipeline));resources.append(('pipeline',pipeline))
        occlusion=H();ci=QueryCI(sType=11,queryType=0,queryCount=3);call('vkCreateQueryPool',device,C.byref(ci),None,C.byref(occlusion));resources.append(('query',occlusion))
        stats=H();stats_names=['input_assembly_vertices','input_assembly_primitives','vertex_shader_invocations','clipping_invocations','clipping_primitives','fragment_shader_invocations']
        if features[24]:
            ci=QueryCI(sType=11,queryType=1,queryCount=3,pipelineStatistics=1|2|4|32|64|128);call('vkCreateQueryPool',device,C.byref(ci),None,C.byref(stats));resources.append(('query',stats))
        for number,case in enumerate(cases):
            positions=case['positions'];indices=case['indices'];vertex_raw=struct.pack('<12f',*(v for p in positions for v in p));index_raw=struct.pack('<3H',*indices)
            vertex_buffer,vertex_mem=buffer(48,0x80);index_buffer,index_mem=buffer(6,0x40);download,download_mem=buffer(1024,2)
            descriptor_set=H();uniform_raw=None
            if vertex_spv:
                uniform_raw=bytearray(65536)
                if 'verified_files' in fixed:
                    captured=Path(fixed['verified_files']['cbuf']['path']).read_bytes();uniform_raw[:len(captured)]=captured
                for n,position in enumerate(positions):struct.pack_into('<4f',uniform_raw,n*32,*position)
                position_buffer,position_mem=buffer(65536,0x10);map_bytes(position_mem,65536,bytes(uniform_raw));ci=SetAllocate(sType=34,descriptorPool=descriptor_pool,descriptorSetCount=1,pSetLayouts=layouts);call('vkAllocateDescriptorSets',device,C.byref(ci),C.byref(descriptor_set))
                infos=(BufferInfo*14)(*(BufferInfo(position_buffer if n==position_binding else zero_buffer,0,65536) for n in range(14)))
                writes=(WriteSet*14)(*(WriteSet(sType=35,dstSet=descriptor_set,dstBinding=n,descriptorCount=1,descriptorType=6,pBufferInfo=C.pointer(infos[n])) for n in range(14)))
                api.vkUpdateDescriptorSets(device,14,writes,0,None)
            map_bytes(vertex_mem,48,vertex_raw);map_bytes(index_mem,6,index_raw);map_bytes(download_mem,1024,b'\xcc'*1024)
            image=H();ici=R.ImageCreateInfo(sType=14,imageType=1,format=37,extent=V.Extent3D(16,16,1),mipLevels=1,arrayLayers=1,samples=1,tiling=0,usage=0x11,sharingMode=0,initialLayout=0)
            call('vkCreateImage',device,C.byref(ici),None,C.byref(image));resources.append(('image',image));req=R.MemoryRequirements();api.vkGetImageMemoryRequirements(device,image,C.byref(req));memory=allocate(req,0);call('vkBindImageMemory',device,image,memory,0)
            view=H();ci=ViewCI(sType=15,image=image,viewType=1,format=37,subresourceRange=R.ImageRange(1,0,1,0,1));call('vkCreateImageView',device,C.byref(ci),None,C.byref(view));resources.append(('view',view))
            views=(H*1)(view);fb=H();ci=FramebufferCI(sType=37,renderPass=rp,attachmentCount=1,pAttachments=views,width=16,height=16,layers=1);call('vkCreateFramebuffer',device,C.byref(ci),None,C.byref(fb));resources.append(('framebuffer',fb))
            cmd=H();a=R.CommandAllocateInfo(sType=40,commandPool=pool,level=0,commandBufferCount=1);call('vkAllocateCommandBuffers',device,C.byref(a),C.byref(cmd));begin=R.CommandBeginInfo(sType=42,flags=1);call('vkBeginCommandBuffer',cmd,C.byref(begin))
            api.vkCmdResetQueryPool(cmd,occlusion,number,1)
            if stats:api.vkCmdResetQueryPool(cmd,stats,number,1)
            # Host coherent writes are visible at queue submit; explicit stage/access
            # barrier also documents vertex/index reads without any GPU address read.
            mb=R.MemoryBarrier(sType=46,srcAccessMask=0x4000,dstAccessMask=0xe if vertex_spv else 0x6)
            api.vkCmdPipelineBarrier(cmd,0x4000,12 if vertex_spv else 4,0,1,C.byref(mb),0,None,0,None)
            clear=(F*4)(0,0,0,1);rb=RenderBegin(sType=43,renderPass=rp,framebuffer=fb,renderArea=rect,clearValueCount=1,pClearValues=C.addressof(clear));api.vkCmdBeginRenderPass(cmd,C.byref(rb),0)
            api.vkCmdBindPipeline(cmd,0,pipeline);vb=(H*1)(vertex_buffer);offsets=(U64*1)(0);api.vkCmdBindVertexBuffers(cmd,0,1,vb,offsets);api.vkCmdBindIndexBuffer(cmd,index_buffer,0,0)
            if vertex_spv:sets=(H*1)(descriptor_set);api.vkCmdBindDescriptorSets(cmd,0,layout,0,1,sets,0,None)
            api.vkCmdBeginQuery(cmd,occlusion,number,1 if features[23] else 0)
            if stats:api.vkCmdBeginQuery(cmd,stats,number,0)
            api.vkCmdDrawIndexed(cmd,3,1,0,0,0)
            if stats:api.vkCmdEndQuery(cmd,stats,number)
            api.vkCmdEndQuery(cmd,occlusion,number);api.vkCmdEndRenderPass(cmd)
            region=R.BufferImageCopy(imageSubresource=R.Layers(1,0,0,1),imageExtent=V.Extent3D(16,16,1));api.vkCmdCopyImageToBuffer(cmd,image,6,download,1,C.byref(region));mem_barrier(cmd,0x1000,0x2000)
            call('vkEndCommandBuffer',cmd);fence=H();ci=R.FenceCreateInfo(sType=8);call('vkCreateFence',device,C.byref(ci),None,C.byref(fence));resources.append(('fence',fence));cmds=(H*1)(cmd);submit=R.SubmitInfo(sType=4,commandBufferCount=1,pCommandBuffers=cmds)
            call('vkQueueSubmit',queue,1,C.byref(submit),fence);pending=True;fences=(H*1)(fence);call('vkWaitForFences',device,1,fences,1,5_000_000_000);pending=False;result['gpu_execution']=True;result['shader_execution']=True
            raw=map_bytes(download_mem,1024);path=out/(case['name']+'.rgba.bin');path.write_bytes(raw)
            colored=sum(any(raw[n:n+3]) for n in range(0,1024,4));q=(U64*2)();call('vkGetQueryPoolResults',device,occlusion,number,1,C.sizeof(q),C.addressof(q),C.sizeof(q),1|2|4)
            assert q[1]==1;counts=None
            if stats:
                values=(U64*7)();call('vkGetQueryPoolResults',device,stats,number,1,C.sizeof(values),C.addressof(values),C.sizeof(values),1|2|4);assert values[6]==1;counts=dict(zip(stats_names,list(values)[:6]))
                assert counts['input_assembly_vertices']==3 and counts['input_assembly_primitives']==1
            expected=colored==256 if number==0 else colored==0 if number==1 else 0<colored<256
            assert expected,(case['name'],colored)
            assert (q[0]>0)==(colored>0)
            if features[23]:assert q[0]==colored
            row={'name':case['name'],'positions':positions,'indices':indices,'vertex_bytes_sha256':hashlib.sha256(vertex_raw).hexdigest(),'index_bytes_hex':index_raw.hex(),'index_bytes_sha256':hashlib.sha256(index_raw).hexdigest(),'indexed_draw':{'count':3,'first_index':0,'base_vertex':0,'instance_count':1},'fence_completed':True,'rgba_file':str(path),'rgba_sha256':sha(path),'nonblack_pixels':colored,'black_pixels':256-colored,'occlusion_samples':q[0],'occlusion_available':bool(q[1]),'pipeline_statistics':counts,'passed':True,'uniform_position_bank_sha256':hashlib.sha256(uniform_raw).hexdigest() if uniform_raw is not None else None}
            result['cases'].append(row)
        result['result']='VULKAN_CLIP_PASS';result['numerical_readback_matches']=True
    except Exception as error:
        result['result']='VULKAN_CLIP_FAIL';result['error']=str(error)
    finally:
        if not pending:
            # All submitted work fenced. Dependent objects destroyed before memory.
            destroy={'descriptor_pool':'vkDestroyDescriptorPool','framebuffer':'vkDestroyFramebuffer','pipeline':'vkDestroyPipeline','query':'vkDestroyQueryPool','view':'vkDestroyImageView','image':'vkDestroyImage','buffer':'vkDestroyBuffer','fence':'vkDestroyFence','shader':'vkDestroyShaderModule','layout':'vkDestroyPipelineLayout','renderpass':'vkDestroyRenderPass','descriptor_layout':'vkDestroyDescriptorSetLayout','memory':'vkFreeMemory'}
            for kind in destroy:
                for k,h in resources:
                    if k==kind:getattr(api,destroy[kind])(device,h,None)
            if pool:api.vkDestroyCommandPool(device,pool,None)
            if device:api.vkDestroyDevice(device,None)
            if messenger and destroy_debug:destroy_debug(instance,messenger,None)
            if instance:api.vkDestroyInstance(instance,None)
            result['cleanup_complete']=True
        result['validation']['messages']=messages;result['validation']['messages_seen']=messages_seen;result['validation']['messages_truncated']=messages_seen>len(messages)
        if result['validation'].get('message_capture'):
            result['validation']['error_count']=sum(bool(m['severity']&0x1000) for m in messages)
            result['validation']['warning_count']=sum(bool(m['severity']&0x100) for m in messages)
            result['validation']['validation_error_count']=sum(bool(m['severity']&0x1000 and m['type']&2) for m in messages)
            result['validation']['loader_error_count']=sum(bool(m['severity']&0x1000 and m['id']=='Loader Message') for m in messages)
            if result['validation']['validation_error_count']:result['result']='VULKAN_CLIP_FAIL'
    return result

def main():
    p=argparse.ArgumentParser();p.add_argument('--out',type=Path,required=True);p.add_argument('--adapter-index',type=int,default=0);p.add_argument('--capture',type=Path);p.add_argument('--compiler',type=Path,default=Path('C:/VulkanSDK/1.4.357.0/Bin/glslangValidator.exe'));p.add_argument('--vertex-spv',type=Path);p.add_argument('--position-binding',type=int,default=1);p.add_argument('--worker',action='store_true');a=p.parse_args()
    if a.worker:
        r=run(a.adapter_index,a.out,a.capture,a.vertex_spv,a.position_binding);(a.out/'RESULT.json').write_text(json.dumps(r,indent=2));print(r['result']);return int(r['result']!='VULKAN_CLIP_PASS')
    a.out.mkdir(parents=True,exist_ok=False);compiles=[]
    for suffix,source in [('vert',VERT),('frag',FRAG)]:
        path=a.out/('clip.'+suffix);path.write_text(source);spv=path.with_suffix('.'+suffix+'.spv')
        if suffix=='vert' and a.vertex_spv:
            assert 20<=a.vertex_spv.stat().st_size<=16*1024*1024;spv.write_bytes(a.vertex_spv.read_bytes());compiles.append({'source':'external vertex SPV','path':str(a.vertex_spv.resolve()),'sha256':sha(a.vertex_spv),'exit_status':0});continue
        cmd=[str(a.compiler),'-V','--target-env','vulkan1.1',str(path),'-o',str(spv)];r=subprocess.run(cmd,capture_output=True,text=True,timeout=20);compiles.append({'command':subprocess.list2cmdline(cmd),'literal_stdout':r.stdout,'literal_stderr':r.stderr,'exit_status':r.returncode});assert r.returncode==0,r.stderr
    (a.out/'COMPILER.json').write_text(json.dumps({'compiler':str(a.compiler),'compiler_sha256':sha(a.compiler),'commands':compiles},indent=2))
    bound=[Path(__file__),Path(V.__file__),Path(R.__file__),Path(N.__file__),*[a.out/('clip.'+s+ext) for s in ['vert','frag'] for ext in ['', '.spv']]]
    if a.capture:bound.append(a.capture)
    if a.vertex_spv:bound.append(a.vertex_spv)
    hashes={str(p.resolve()):sha(p) for p in bound}
    cmd=[sys.executable,'-B',str(Path(__file__).resolve()),'--worker','--out',str(a.out.resolve()),'--adapter-index',str(a.adapter_index)]
    if a.capture:cmd+=['--capture',str(a.capture.resolve())]
    if a.vertex_spv:cmd+=['--vertex-spv',str(a.vertex_spv.resolve()),'--position-binding',str(a.position_binding)]
    start=time.perf_counter()
    try:
        w=subprocess.run(cmd,capture_output=True,text=True,timeout=30);record={'command':subprocess.list2cmdline(cmd),'input':'three indexed vec4 triangles with shader clipping; captured bank1 positions re-uploaded only','literal_stdout':w.stdout,'literal_stderr':w.stderr,'exit_status':w.returncode,'elapsed_seconds':time.perf_counter()-start}
    except subprocess.TimeoutExpired:
        record={'command':subprocess.list2cmdline(cmd),'exit_status':124,'result':'parent_watchdog_terminated_worker','Vulkan_call_cancelled':False,'elapsed_seconds':time.perf_counter()-start}
    record.update(hashes=hashes,inputs_unchanged=all(sha(Path(p))==h for p,h in hashes.items()),watchdog_seconds=30)
    if not record['inputs_unchanged']:record['exit_status']=1;record['input_mutation_detected']=True
    (a.out/'EXECUTION.json').write_text(json.dumps(record,indent=2));print(json.dumps(record));return record['exit_status']
if __name__=='__main__':sys.exit(main())

