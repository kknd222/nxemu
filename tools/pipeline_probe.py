"""Isolated Windows x64 Vulkan compute creation only; no shader execution.

Invoke --metadata CAPTURE.json --out RESULT.json. A parent process supplies any
watchdog; a synchronous driver call has no cancellation API. Instance extensions
and the application cache were not captured: every run is explicitly a cold
candidate, never an exact application replay or GPU numerical certification.
"""
from pathlib import Path
import argparse
import ctypes as C
import hashlib
import json
import math
import os
import re
import struct
import sys
import time

import pipeline_cases

U32=C.c_uint32
I32=C.c_int32
U64=C.c_uint64
Handle=C.c_void_p
P=C.POINTER

# BEGIN_GENERATED_ABI: generated once from the project's pinned Vulkan headers.
# Vulkan core SHA256 a7b958105cd7a6c871411476dea18fde5291f4d75a45a3edb53e711e179a0aba
FEATURE_COUNTS = {1000059000: 55, 1000081001: 2, 1000083000: 4, 1000180000: 2, 1000063000: 1, 1000082000: 2, 1000253000: 1, 1000120000: 2, 1000161001: 20, 1000261000: 1, 1000177000: 3, 1000207000: 1, 1000276000: 1, 1000225002: 2, 1000287002: 2, 1000283000: 4, 1000355000: 1, 1000267000: 1, 1000377000: 3, 1000455000: 31, 1000340000: 2, 1000265000: 1, 1000259000: 6, 1000163000: 15, 1000356000: 2, 1000254000: 2, 1000286000: 3, 1000028000: 2, 1000352000: 1, 1000269000: 1, 1000336000: 4}
LIMIT_FIELDS = [
    ('maxImageDimension1D', U32),
    ('maxImageDimension2D', U32),
    ('maxImageDimension3D', U32),
    ('maxImageDimensionCube', U32),
    ('maxImageArrayLayers', U32),
    ('maxTexelBufferElements', U32),
    ('maxUniformBufferRange', U32),
    ('maxStorageBufferRange', U32),
    ('maxPushConstantsSize', U32),
    ('maxMemoryAllocationCount', U32),
    ('maxSamplerAllocationCount', U32),
    ('bufferImageGranularity', U64),
    ('sparseAddressSpaceSize', U64),
    ('maxBoundDescriptorSets', U32),
    ('maxPerStageDescriptorSamplers', U32),
    ('maxPerStageDescriptorUniformBuffers', U32),
    ('maxPerStageDescriptorStorageBuffers', U32),
    ('maxPerStageDescriptorSampledImages', U32),
    ('maxPerStageDescriptorStorageImages', U32),
    ('maxPerStageDescriptorInputAttachments', U32),
    ('maxPerStageResources', U32),
    ('maxDescriptorSetSamplers', U32),
    ('maxDescriptorSetUniformBuffers', U32),
    ('maxDescriptorSetUniformBuffersDynamic', U32),
    ('maxDescriptorSetStorageBuffers', U32),
    ('maxDescriptorSetStorageBuffersDynamic', U32),
    ('maxDescriptorSetSampledImages', U32),
    ('maxDescriptorSetStorageImages', U32),
    ('maxDescriptorSetInputAttachments', U32),
    ('maxVertexInputAttributes', U32),
    ('maxVertexInputBindings', U32),
    ('maxVertexInputAttributeOffset', U32),
    ('maxVertexInputBindingStride', U32),
    ('maxVertexOutputComponents', U32),
    ('maxTessellationGenerationLevel', U32),
    ('maxTessellationPatchSize', U32),
    ('maxTessellationControlPerVertexInputComponents', U32),
    ('maxTessellationControlPerVertexOutputComponents', U32),
    ('maxTessellationControlPerPatchOutputComponents', U32),
    ('maxTessellationControlTotalOutputComponents', U32),
    ('maxTessellationEvaluationInputComponents', U32),
    ('maxTessellationEvaluationOutputComponents', U32),
    ('maxGeometryShaderInvocations', U32),
    ('maxGeometryInputComponents', U32),
    ('maxGeometryOutputComponents', U32),
    ('maxGeometryOutputVertices', U32),
    ('maxGeometryTotalOutputComponents', U32),
    ('maxFragmentInputComponents', U32),
    ('maxFragmentOutputAttachments', U32),
    ('maxFragmentDualSrcAttachments', U32),
    ('maxFragmentCombinedOutputResources', U32),
    ('maxComputeSharedMemorySize', U32),
    ('maxComputeWorkGroupCount', U32*3),
    ('maxComputeWorkGroupInvocations', U32),
    ('maxComputeWorkGroupSize', U32*3),
    ('subPixelPrecisionBits', U32),
    ('subTexelPrecisionBits', U32),
    ('mipmapPrecisionBits', U32),
    ('maxDrawIndexedIndexValue', U32),
    ('maxDrawIndirectCount', U32),
    ('maxSamplerLodBias', C.c_float),
    ('maxSamplerAnisotropy', C.c_float),
    ('maxViewports', U32),
    ('maxViewportDimensions', U32*2),
    ('viewportBoundsRange', C.c_float*2),
    ('viewportSubPixelBits', U32),
    ('minMemoryMapAlignment', C.c_size_t),
    ('minTexelBufferOffsetAlignment', U64),
    ('minUniformBufferOffsetAlignment', U64),
    ('minStorageBufferOffsetAlignment', U64),
    ('minTexelOffset', I32),
    ('maxTexelOffset', U32),
    ('minTexelGatherOffset', I32),
    ('maxTexelGatherOffset', U32),
    ('minInterpolationOffset', C.c_float),
    ('maxInterpolationOffset', C.c_float),
    ('subPixelInterpolationOffsetBits', U32),
    ('maxFramebufferWidth', U32),
    ('maxFramebufferHeight', U32),
    ('maxFramebufferLayers', U32),
    ('framebufferColorSampleCounts', U32),
    ('framebufferDepthSampleCounts', U32),
    ('framebufferStencilSampleCounts', U32),
    ('framebufferNoAttachmentsSampleCounts', U32),
    ('maxColorAttachments', U32),
    ('sampledImageColorSampleCounts', U32),
    ('sampledImageIntegerSampleCounts', U32),
    ('sampledImageDepthSampleCounts', U32),
    ('sampledImageStencilSampleCounts', U32),
    ('storageImageSampleCounts', U32),
    ('maxSampleMaskWords', U32),
    ('timestampComputeAndGraphics', U32),
    ('timestampPeriod', C.c_float),
    ('maxClipDistances', U32),
    ('maxCullDistances', U32),
    ('maxCombinedClipAndCullDistances', U32),
    ('discreteQueuePriorities', U32),
    ('pointSizeRange', C.c_float*2),
    ('lineWidthRange', C.c_float*2),
    ('pointSizeGranularity', C.c_float),
    ('lineWidthGranularity', C.c_float),
    ('strictLines', U32),
    ('standardSampleLocations', U32),
    ('optimalBufferCopyOffsetAlignment', U64),
    ('optimalBufferCopyRowPitchAlignment', U64),
    ('nonCoherentAtomSize', U64),
]
# END_GENERATED_ABI

class Limits(C.Structure):
    _fields_=LIMIT_FIELDS
class SparseProperties(C.Structure):
    _fields_=[(x,U32) for x in ('residencyStandard2DBlockShape','residencyStandard2DMultisampleBlockShape','residencyStandard3DBlockShape','residencyAlignedMipSize','residencyNonResidentStrict')]
class PhysicalProperties(C.Structure):
    _fields_=[('apiVersion',U32),('driverVersion',U32),('vendorID',U32),('deviceID',U32),('deviceType',I32),('deviceName',C.c_char*256),('pipelineCacheUUID',C.c_uint8*16),('limits',Limits),('sparseProperties',SparseProperties)]
class ConformanceVersion(C.Structure):
    _fields_=[(x,C.c_uint8) for x in ('major','minor','subminor','patch')]
class DriverProperties(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('driverID',I32),('driverName',C.c_char*256),('driverInfo',C.c_char*256),('conformanceVersion',ConformanceVersion)]
class Properties2(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('properties',PhysicalProperties)]
class ExtensionProperties(C.Structure):
    _fields_=[('extensionName',C.c_char*256),('specVersion',U32)]
class ApplicationInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('pApplicationName',C.c_char_p),('applicationVersion',U32),('pEngineName',C.c_char_p),('engineVersion',U32),('apiVersion',U32)]
class InstanceCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('flags',U32),('pApplicationInfo',P(ApplicationInfo)),('enabledLayerCount',U32),('ppEnabledLayerNames',P(C.c_char_p)),('enabledExtensionCount',U32),('ppEnabledExtensionNames',P(C.c_char_p))]
class QueueCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('flags',U32),('queueFamilyIndex',U32),('queueCount',U32),('pQueuePriorities',P(C.c_float))]
class DeviceCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('flags',U32),('queueCreateInfoCount',U32),('pQueueCreateInfos',P(QueueCreateInfo)),('enabledLayerCount',U32),('ppEnabledLayerNames',P(C.c_char_p)),('enabledExtensionCount',U32),('ppEnabledExtensionNames',P(C.c_char_p)),('pEnabledFeatures',Handle)]
class Extent3D(C.Structure):
    _fields_=[('width',U32),('height',U32),('depth',U32)]
class QueueFamilyProperties(C.Structure):
    _fields_=[('queueFlags',U32),('queueCount',U32),('timestampValidBits',U32),('minImageTransferGranularity',Extent3D)]
class DescriptorBinding(C.Structure):
    _fields_=[('binding',U32),('descriptorType',I32),('descriptorCount',U32),('stageFlags',U32),('pImmutableSamplers',P(Handle))]
class DescriptorLayoutCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('flags',U32),('bindingCount',U32),('pBindings',P(DescriptorBinding))]
class PushConstantRange(C.Structure):
    _fields_=[('stageFlags',U32),('offset',U32),('size',U32)]
class PipelineLayoutCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('flags',U32),('setLayoutCount',U32),('pSetLayouts',P(Handle)),('pushConstantRangeCount',U32),('pPushConstantRanges',P(PushConstantRange))]
class ShaderModuleCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('flags',U32),('codeSize',C.c_size_t),('pCode',P(U32))]
class RequiredSubgroupSize(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('requiredSubgroupSize',U32)]
class ShaderStageCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('flags',U32),('stage',U32),('module',Handle),('pName',C.c_char_p),('pSpecializationInfo',Handle)]
class ComputePipelineCreateInfo(C.Structure):
    _fields_=[('sType',I32),('pNext',Handle),('flags',U32),('stage',ShaderStageCreateInfo),('layout',Handle),('basePipelineHandle',Handle),('basePipelineIndex',I32)]

def integer(value,name,low=0,high=0xffffffff):
    if type(value) is not int or not low<=value<=high:raise ValueError('Invalid '+name)
    return value

def check_abi():
    if C.sizeof(Handle)!=8 or sys.byteorder!='little':raise ValueError('Only 64-bit little-endian ABI is supported')
    expected={Limits:504,PhysicalProperties:824,Properties2:840,DriverProperties:536,ApplicationInfo:48,InstanceCreateInfo:64,QueueCreateInfo:40,DeviceCreateInfo:72,DescriptorBinding:24,DescriptorLayoutCreateInfo:32,PushConstantRange:12,PipelineLayoutCreateInfo:48,ShaderModuleCreateInfo:40,RequiredSubgroupSize:24,ShaderStageCreateInfo:48,ComputePipelineCreateInfo:96,QueueFamilyProperties:24}
    for cls,size in expected.items():
        if C.sizeof(cls)!=size:raise ValueError('Vulkan ABI size mismatch: '+cls.__name__)
    if PhysicalProperties.limits.offset!=296 or Properties2.properties.offset!=16:raise ValueError('Vulkan properties ABI offset mismatch')

def feature_nodes(chain):
    if not isinstance(chain,list) or not 1<=len(chain)<=64:raise ValueError('Invalid feature chain length')
    nodes=[];seen=set()
    for row in chain:
        if not isinstance(row,dict):raise ValueError('Invalid feature node')
        kind=integer(row.get('s_type'),'feature sType')
        if kind not in FEATURE_COUNTS or kind in seen:raise ValueError('Unknown or duplicate feature sType')
        seen.add(kind);count=FEATURE_COUNTS[kind];values=row.get('values')
        if type(row.get('payload_offset')) is not int or row['payload_offset']!=16 or type(row.get('payload_bytes')) is not int or row['payload_bytes']!=count*4 or not isinstance(values,list) or len(values)!=count or any(type(x) is not int or x not in (0,1) for x in values):raise ValueError('Feature payload does not match typed whitelist')
        cls=type('Feature_'+str(kind),(C.Structure,),{'_fields_':[('sType',I32),('pNext',Handle),('values',U32*count)]})
        if cls.values.offset!=16 or C.sizeof(cls)!=(16+count*4+7)//8*8:raise ValueError('Feature ABI mismatch')
        node=cls();node.sType=kind;node.values[:]=values;nodes.append(node)
    if chain[0]['s_type']!=1000059000:raise ValueError('Captured chain must start with VkPhysicalDeviceFeatures2')
    for a,b in zip(nodes,nodes[1:]):a.pNext=C.addressof(b)
    return nodes

def compute_entry(raw):
    pipeline_cases.spirv_statistics(raw)
    words=struct.unpack('<'+'I'*(len(raw)//4),raw);at=5
    found=False
    while at<len(words):
        count=words[at]>>16;op=words[at]&65535
        if not count or at+count>len(words):raise ValueError('Invalid SPIR-V word count')
        if op==15 and count>=4 and words[at+1]==5:
            encoded=struct.pack('<'+'I'*(count-3),*words[at+3:at+count])
            if b'\0' not in encoded or not 0<words[at+2]<words[3]:raise ValueError('Invalid SPIR-V entry name or ID')
            if encoded.split(b'\0',1)[0]==b'main':found=True
        at+=count
    if not found:raise ValueError('SPIR-V has no compute main entry')

def validate_recipe(recipe,raw):
    check_abi();compute_entry(raw)
    if not isinstance(recipe,dict):raise ValueError('Invalid recipe')
    device=recipe.get('device');layout=recipe.get('descriptor_layout')
    if not isinstance(device,dict) or device.get('feature_chain_complete') is not True or device.get('abi_pointer_bits')!=64:raise ValueError('Incomplete device enable recipe')
    for name in ('vendor_id','device_id','driver_id','driver_version'):integer(device.get(name),name)
    integer(device.get('instance_api_version'),'instance_api_version',1<<22)
    if not isinstance(device.get('device_name'),str) or not device['device_name'] or len(device['device_name'].encode('utf-8'))>255 or '\0' in device['device_name']:raise ValueError('Invalid device name')
    if not isinstance(device.get('pipeline_cache_uuid'),str) or not re.fullmatch('[0-9a-fA-F]{32}',device['pipeline_cache_uuid']):raise ValueError('Invalid pipeline cache UUID')
    feature_nodes(device.get('feature_chain'))
    extensions=device.get('enabled_extensions')
    if not isinstance(extensions,list) or len(extensions)>256 or any(not isinstance(x,str) or not re.fullmatch(r'VK_[A-Z0-9]+_[A-Za-z0-9_]{1,240}',x) for x in extensions):raise ValueError('Invalid enabled device extensions')
    if len(set(extensions))!=len(extensions):raise ValueError('Duplicate enabled device extensions')
    queues=device.get('queues')
    if not isinstance(queues,list) or not 1<=len(queues)<=16:raise ValueError('Invalid device queues')
    families=set()
    for q in queues:
        if not isinstance(q,dict):raise ValueError('Invalid device queue')
        family=integer(q.get('family'),'queue family',0,255)
        if family in families or integer(q.get('flags'),'queue flags')!=0:raise ValueError('Unsupported queue flags or duplicate family')
        families.add(family);priorities=q.get('priorities')
        if not isinstance(priorities,list) or not 1<=len(priorities)<=64 or any(type(x) not in (int,float) or not math.isfinite(x) or not 0<=x<=1 for x in priorities):raise ValueError('Invalid queue priorities')
    if not isinstance(layout,dict) or layout.get('recipe_complete') is not True or layout.get('pnext') is not None:raise ValueError('Incomplete or unsupported descriptor layout')
    if layout.get('set_count') not in (0,1) or type(layout.get('set_count')) is not int or integer(layout.get('set0_flags'),'set flags')!=0 or integer(layout.get('pipeline_layout_flags'),'layout flags')!=0:raise ValueError('Unsupported descriptor or pipeline layout flags')
    bindings=layout.get('bindings');seen=set()
    if not isinstance(bindings,list) or len(bindings)>1024 or (layout['set_count']==0 and bindings):raise ValueError('Invalid descriptor bindings')
    for row in bindings:
        if not isinstance(row,dict):raise ValueError('Invalid descriptor binding')
        binding=integer(row.get('binding'),'binding',0,1023)
        if binding in seen or row.get('immutable_samplers') is not None:raise ValueError('Duplicate binding or immutable samplers unsupported')
        seen.add(binding);integer(row.get('type'),'descriptor type',0,9);integer(row.get('count'),'descriptor count',1,65536)
        if row.get('stages')!=32:raise ValueError('Only compute descriptor stages are supported')
    ranges=layout.get('push_constants')
    if not isinstance(ranges,list) or len(ranges)>8:raise ValueError('Invalid push constants')
    used=0
    for row in ranges:
        if not isinstance(row,dict) or row.get('stages')!=32:raise ValueError('Only compute push constants are supported')
        offset=integer(row.get('offset'),'push offset',0,65532);size=integer(row.get('size'),'push size',4,65536)
        if offset%4 or size%4 or used&row['stages']:raise ValueError('Invalid or duplicate push constant stage ranges')
        used|=row['stages']
    for name in ('pipeline_flags','stage_flags','shader_module_flags'):
        if integer(recipe.get(name),name)!=0:raise ValueError('Unsupported '+name)
    if recipe.get('specialization') is not None:raise ValueError('Specialization constants unsupported')
    if type(recipe.get('subgroup_pnext_present')) is not bool:raise ValueError('Missing subgroup pNext presence')
    if recipe['subgroup_pnext_present']:
        integer(recipe.get('required_subgroup_size'),'required subgroup size',1,128)
        if recipe['required_subgroup_size']&(recipe['required_subgroup_size']-1):raise ValueError('Subgroup size must be a power of two')
    elif recipe.get('required_subgroup_size') not in (None,0):raise ValueError('Subgroup presence/value mismatch')
    if recipe.get('cache_blob_captured') is not False:raise ValueError('This helper supports uncaptured application cache, cold-only')
    return recipe

def load_case(path):
    path=Path(path)
    if path.is_symlink():raise ValueError('Metadata symlink unsupported')
    path=path.resolve()
    with path.open('rb') as stream:first=stream.read(262145)
    if len(first)>262144:raise ValueError('Metadata budget exceeded')
    report=pipeline_cases.inspect_cases(path.parent.parent.parent)
    selected=[x for x in report['cases'] if Path(x['metadata'])==path]
    if len(selected)!=1:raise ValueError('Captured pipeline metadata not found')
    case=selected[0];raw=Path(case['blob']).read_bytes()
    if path.read_bytes()!=first or hashlib.sha256(raw).hexdigest()!=case['sha256']:raise ValueError('Capture changed during verification')
    validate_recipe(case['recipe'],raw)
    return case,raw,hashlib.sha256(first).hexdigest()

def bind(dll,name,restype,args):
    function=getattr(dll,name);function.restype=restype;function.argtypes=args;return function

class VulkanFailure(RuntimeError):
    def __init__(self,name,result):self.operation=name;self.vk_result=int(result);super().__init__(name+' returned '+str(result))

class API:
    def __init__(self):
        if os.name!='nt':raise ValueError('Windows Vulkan loader is required')
        self.dll=C.WinDLL('vulkan-1.dll')
        signatures={
          'vkCreateInstance':(I32,[P(InstanceCreateInfo),Handle,P(Handle)]),
          'vkDestroyInstance':(None,[Handle,Handle]),
          'vkEnumerateInstanceExtensionProperties':(I32,[C.c_char_p,P(U32),P(ExtensionProperties)]),
          'vkEnumeratePhysicalDevices':(I32,[Handle,P(U32),P(Handle)]),
          'vkGetPhysicalDeviceProperties':(None,[Handle,P(PhysicalProperties)]),
          'vkGetPhysicalDeviceQueueFamilyProperties':(None,[Handle,P(U32),P(QueueFamilyProperties)]),
          'vkEnumerateDeviceExtensionProperties':(I32,[Handle,C.c_char_p,P(U32),P(ExtensionProperties)]),
          'vkCreateDevice':(I32,[Handle,P(DeviceCreateInfo),Handle,P(Handle)]),
          'vkDestroyDevice':(None,[Handle,Handle]),
          'vkCreateDescriptorSetLayout':(I32,[Handle,P(DescriptorLayoutCreateInfo),Handle,P(Handle)]),
          'vkDestroyDescriptorSetLayout':(None,[Handle,Handle,Handle]),
          'vkCreatePipelineLayout':(I32,[Handle,P(PipelineLayoutCreateInfo),Handle,P(Handle)]),
          'vkDestroyPipelineLayout':(None,[Handle,Handle,Handle]),
          'vkCreateShaderModule':(I32,[Handle,P(ShaderModuleCreateInfo),Handle,P(Handle)]),
          'vkDestroyShaderModule':(None,[Handle,Handle,Handle]),
          'vkCreateComputePipelines':(I32,[Handle,Handle,U32,P(ComputePipelineCreateInfo),Handle,P(Handle)]),
          'vkDestroyPipeline':(None,[Handle,Handle,Handle]),
          'vkGetInstanceProcAddr':(Handle,[Handle,C.c_char_p]),
        }
        for name,(result,args) in signatures.items():setattr(self,name,bind(self.dll,name,result,args))
        self.enumerate_version=bind(self.dll,'vkEnumerateInstanceVersion',I32,[P(U32)]) if hasattr(self.dll,'vkEnumerateInstanceVersion') else None
    def properties2(self,instance,physical,target):
        address=self.vkGetInstanceProcAddr(instance,b'vkGetPhysicalDeviceProperties2') or self.vkGetInstanceProcAddr(instance,b'vkGetPhysicalDeviceProperties2KHR')
        if not address:raise ValueError('Physical device properties2 entry is unavailable')
        C.WINFUNCTYPE(None,Handle,P(Properties2))(address)(physical,C.byref(target))

def enumerate_values(function,prefix,cls):
    for _ in range(4):
        count=U32();r=function(*prefix,C.byref(count),None)
        if r not in (0,5):raise VulkanFailure('enumerate count',r)
        if count.value>4096:raise ValueError('Vulkan enumeration count exceeded')
        output=(cls*count.value)();r=function(*prefix,C.byref(count),output)
        if r==0:return list(output[:count.value])
        if r!=5:raise VulkanFailure('enumerate values',r)
    raise ValueError('Vulkan enumeration kept changing')

def instance_extensions(api_version,device_extensions,available):
    required=set()
    if api_version<(1<<22|1<<12):
        required.add('VK_KHR_get_physical_device_properties2')
        if any('external_memory' in x for x in device_extensions):required.add('VK_KHR_external_memory_capabilities')
        if any('external_semaphore' in x for x in device_extensions):required.add('VK_KHR_external_semaphore_capabilities')
        if any('external_fence' in x for x in device_extensions):required.add('VK_KHR_external_fence_capabilities')
    if 'VK_KHR_swapchain' in device_extensions:required.add('VK_KHR_surface')
    if 'VK_KHR_display_swapchain' in device_extensions:required.update(('VK_KHR_surface','VK_KHR_display'))
    if 'VK_EXT_full_screen_exclusive' in device_extensions:required.update(('VK_KHR_surface','VK_KHR_get_surface_capabilities2'))
    if not required<=set(available):raise ValueError('Missing required instance dependency extensions: '+','.join(sorted(required-set(available))))
    return sorted(required)

def rss_snapshot():
    if os.name!='nt':return {'available':False}
    class Counters(C.Structure):
        _fields_=[('cb',U32),('faults',U32)]+[(x,C.c_size_t) for x in ('peak_rss','rss','peak_paged','paged','peak_nonpaged','nonpaged','pagefile','peak_pagefile','private')]
    kernel=C.WinDLL('kernel32');psapi=C.WinDLL('psapi')
    process=bind(kernel,'GetCurrentProcess',Handle,[])();query=bind(psapi,'GetProcessMemoryInfo',I32,[Handle,P(Counters),U32]);c=Counters();c.cb=C.sizeof(c)
    if not query(process,C.byref(c),C.sizeof(c)):return {'available':False,'last_error':bind(kernel,'GetLastError',U32,[])()}
    return {'available':True,'rss_bytes':c.rss,'peak_rss_bytes':c.peak_rss,'private_bytes':c.private,'source':'GetProcessMemoryInfo; entire isolated process peak'}

def select_matching_adapter(matched,result,adapter_match_index=None):
    result['replay_classification']='cold_candidate_not_exact_app_replay'
    result['matched_adapter_count']=len(matched)
    result['source_adapter_unique']=len(matched)==1
    result['explicit_matching_candidate_index']=adapter_match_index
    if adapter_match_index is not None:
        integer(adapter_match_index,'adapter match index',0,255)
        if adapter_match_index>=len(matched):raise ValueError('Explicit matching adapter index is out of range')
        result['adapter_selection']='explicit candidate experiment; not original source object identity'
        return matched[adapter_match_index]
    if len(matched)!=1:raise ValueError('Captured adapter identity did not match exactly one physical device')
    result['adapter_selection']='unique captured-property match; object UUID not captured'
    return matched[0]

def run_candidate(case,raw,metadata_hash,adapter_match_index=None):
    recipe=case['recipe'];wanted=recipe['device'];layout=recipe['descriptor_layout'];api=API();steps=[];instance=Handle();device=Handle();set_layout=Handle();pipe_layout=Handle();module=Handle();pipeline=Handle();adapter=None;cleanup_errors=[]
    result={'schema':1,'result':'cold_candidate_not_exact_app_replay','id':case['id'],'pipeline_hash':case['pipeline_hash'],'spirv_sha256':hashlib.sha256(raw).hexdigest(),'metadata_sha256':metadata_hash,'recipe_sha256':hashlib.sha256(json.dumps(recipe,sort_keys=True,separators=(',',':')).encode()).hexdigest(),'steps':steps,'exact_application_replay':False,'gpu_dispatch_performed':False,'shader_numerical_validation':False,'cache_handle':'VK_NULL_HANDLE','original_instance_extensions_captured':False,'watchdog_owner':'parent; no in-call cancellation','memory_before':rss_snapshot()}
    def call(name,*args):
        wall=time.perf_counter_ns();cpu=time.process_time_ns();code=getattr(api,name)(*args);steps.append({'operation':name,'wall_ns':time.perf_counter_ns()-wall,'process_cpu_ns':time.process_time_ns()-cpu,'vk_result':int(code)})
        if code!=0:raise VulkanFailure(name,code)
    try:
        available=[bytes(x.extensionName).split(b'\0',1)[0].decode() for x in enumerate_values(api.vkEnumerateInstanceExtensionProperties,[None],ExtensionProperties)]
        max_api=U32(1<<22)
        if api.enumerate_version:
            code=api.enumerate_version(C.byref(max_api))
            if code!=0:raise VulkanFailure('vkEnumerateInstanceVersion',code)
        requested=wanted['instance_api_version']
        if requested>max_api.value:raise ValueError('Captured instance API exceeds loader version')
        enabled=instance_extensions(requested,wanted['enabled_extensions'],available);encoded=(C.c_char_p*len(enabled))(*(x.encode() for x in enabled));app=ApplicationInfo(sType=0,pApplicationName=b'NXEmu isolated pipeline probe',applicationVersion=1,pEngineName=b'create-only',engineVersion=1,apiVersion=requested);ci=InstanceCreateInfo(sType=1,pApplicationInfo=C.pointer(app),enabledExtensionCount=len(enabled),ppEnabledExtensionNames=encoded if enabled else None)
        result['instance_extensions_enabled_for_candidate']=enabled;result['instance_api_requested']=requested;result['loader_api_max']=max_api.value
        call('vkCreateInstance',C.byref(ci),None,C.byref(instance))
        candidates=[];matched=[]
        for physical in enumerate_values(api.vkEnumeratePhysicalDevices,[instance],Handle):
            props=PhysicalProperties();api.vkGetPhysicalDeviceProperties(physical,C.byref(props))
            supported=[bytes(x.extensionName).split(b'\0',1)[0].decode() for x in enumerate_values(api.vkEnumerateDeviceExtensionProperties,[physical,None],ExtensionProperties)]
            if props.apiVersion<(1<<22|2<<12) and 'VK_KHR_driver_properties' not in supported:
                candidates.append({'vendor_id':props.vendorID,'device_id':props.deviceID,'reason':'driver identity properties unavailable'});continue
            drv=DriverProperties(sType=1000196000);prop2=Properties2(sType=1000059001,pNext=C.addressof(drv));api.properties2(instance,physical,prop2)
            row={'device_name':bytes(props.deviceName).split(b'\0',1)[0].decode('utf-8'),'vendor_id':props.vendorID,'device_id':props.deviceID,'driver_id':drv.driverID,'driver_version':props.driverVersion,'api_version':props.apiVersion,'pipeline_cache_uuid':bytes(props.pipelineCacheUUID).hex(),'driver_name':bytes(drv.driverName).split(b'\0',1)[0].decode('utf-8','replace')};candidates.append(row)
            if all(row[k]==wanted[k] for k in ('device_name','vendor_id','device_id','driver_id','driver_version')) and row['pipeline_cache_uuid']==wanted['pipeline_cache_uuid'].lower():matched.append((physical,row,props,supported))
        result['adapters_observed']=candidates
        physical,adapter,props,supported=select_matching_adapter(matched,result,adapter_match_index);result['adapter']=adapter
        if 'device_api_version' in wanted and props.apiVersion!=integer(wanted['device_api_version'],'device_api_version'):raise ValueError('Captured device API version mismatch')
        if not set(wanted['enabled_extensions'])<=set(supported):raise ValueError('Captured enabled device extension unavailable')
        families_count=U32();api.vkGetPhysicalDeviceQueueFamilyProperties(physical,C.byref(families_count),None)
        if not 1<=families_count.value<=256:raise ValueError('Invalid queue family count')
        families=(QueueFamilyProperties*families_count.value)();api.vkGetPhysicalDeviceQueueFamilyProperties(physical,C.byref(families_count),families)
        priority_arrays=[];queues=[]
        for q in wanted['queues']:
            if q['family']>=families_count.value or len(q['priorities'])>families[q['family']].queueCount:raise ValueError('Captured queue family/count unavailable')
            priorities=(C.c_float*len(q['priorities']))(*q['priorities']);priority_arrays.append(priorities);queues.append(QueueCreateInfo(sType=2,flags=q['flags'],queueFamilyIndex=q['family'],queueCount=len(priorities),pQueuePriorities=priorities))
        queue_array=(QueueCreateInfo*len(queues))(*queues);feature_storage=feature_nodes(wanted['feature_chain']);ext_array=(C.c_char_p*len(wanted['enabled_extensions']))(*(x.encode() for x in wanted['enabled_extensions']));dci=DeviceCreateInfo(sType=3,pNext=C.addressof(feature_storage[0]),queueCreateInfoCount=len(queues),pQueueCreateInfos=queue_array,enabledExtensionCount=len(ext_array),ppEnabledExtensionNames=ext_array if len(ext_array) else None)
        call('vkCreateDevice',physical,C.byref(dci),None,C.byref(device))
        bindings=(DescriptorBinding*len(layout['bindings']))(*(DescriptorBinding(binding=x['binding'],descriptorType=x['type'],descriptorCount=x['count'],stageFlags=x['stages']) for x in layout['bindings']))
        if layout['set_count']:
            lci=DescriptorLayoutCreateInfo(sType=32,flags=0,bindingCount=len(bindings),pBindings=bindings if len(bindings) else None);call('vkCreateDescriptorSetLayout',device,C.byref(lci),None,C.byref(set_layout))
        ranges=(PushConstantRange*len(layout['push_constants']))(*(PushConstantRange(stageFlags=x['stages'],offset=x['offset'],size=x['size']) for x in layout['push_constants']))
        if any(x['offset']+x['size']>props.limits.maxPushConstantsSize for x in layout['push_constants']):raise ValueError('Captured push constants exceed selected device limit')
        set_array=(Handle*layout['set_count'])(set_layout) if layout['set_count'] else None;plci=PipelineLayoutCreateInfo(sType=30,setLayoutCount=layout['set_count'],pSetLayouts=set_array,pushConstantRangeCount=len(ranges),pPushConstantRanges=ranges if len(ranges) else None);call('vkCreatePipelineLayout',device,C.byref(plci),None,C.byref(pipe_layout))
        words=(U32*(len(raw)//4)).from_buffer_copy(raw);smci=ShaderModuleCreateInfo(sType=16,flags=0,codeSize=len(raw),pCode=words);call('vkCreateShaderModule',device,C.byref(smci),None,C.byref(module))
        subgroup=RequiredSubgroupSize(sType=1000225001,requiredSubgroupSize=recipe['required_subgroup_size']) if recipe['subgroup_pnext_present'] else None
        stage=ShaderStageCreateInfo(sType=18,pNext=C.addressof(subgroup) if subgroup else None,flags=0,stage=32,module=module,pName=b'main');pci=ComputePipelineCreateInfo(sType=29,flags=0,stage=stage,layout=pipe_layout,basePipelineIndex=0)
        call('vkCreateComputePipelines',device,None,1,C.byref(pci),None,C.byref(pipeline))
        result['creation_succeeded']=True;result['result']='cold_candidate_created_not_exact_app_replay'
    except VulkanFailure as error:result.update(result='cold_candidate_create_failed',creation_succeeded=False,failed_operation=error.operation,vk_result=error.vk_result,reason=str(error))
    except (ValueError,OSError) as error:result.update(result='pipeline_probe_error',creation_succeeded=False,reason=str(error))
    finally:
        # Reached only after each synchronous driver call returns. No in-flight resource teardown.
        for name,handle in [('vkDestroyPipeline',pipeline),('vkDestroyShaderModule',module),('vkDestroyPipelineLayout',pipe_layout),('vkDestroyDescriptorSetLayout',set_layout)]:
            if handle.value:
                try:getattr(api,name)(device,handle,None)
                except Exception as error:cleanup_errors.append(name+': '+str(error))
        if device.value:
            try:api.vkDestroyDevice(device,None)
            except Exception as error:cleanup_errors.append('vkDestroyDevice: '+str(error))
        if instance.value:
            try:api.vkDestroyInstance(instance,None)
            except Exception as error:cleanup_errors.append('vkDestroyInstance: '+str(error))
        result['memory_after']=rss_snapshot();result['cleanup_errors']=cleanup_errors
    return result

def main(argv=None):
    parser=argparse.ArgumentParser(description=__doc__);parser.add_argument('--metadata','--case',dest='metadata',required=True);parser.add_argument('--out');parser.add_argument('--validate-only',action='store_true');parser.add_argument('--adapter-match-index',type=int,help='Explicit index in captured-property matching candidates; experimental, not source object identity');args=parser.parse_args(argv)
    start=time.perf_counter_ns();cpu=time.process_time_ns()
    try:
        case,raw,meta_hash=load_case(args.metadata)
        if args.adapter_match_index is not None:integer(args.adapter_match_index,'adapter match index',0,255)
        if args.validate_only:result={'schema':1,'result':'pipeline_recipe_validated_only','id':case['id'],'spirv_sha256':case['sha256'],'metadata_sha256':meta_hash,'cold_create_candidate':True,'exact_application_replay':False,'gpu_api_called':False}
        else:result=run_candidate(case,raw,meta_hash,args.adapter_match_index)
    except (ValueError,OSError,KeyError,TypeError,OverflowError) as error:result={'schema':1,'result':'pipeline_probe_error','reason':str(error),'creation_succeeded':False,'gpu_dispatch_performed':False}
    result['total_wall_ns']=time.perf_counter_ns()-start;result['total_process_cpu_ns']=time.process_time_ns()-cpu
    if args.out:Path(args.out).write_text(json.dumps(result,indent=2),encoding='utf-8')
    print(json.dumps(result));return 0 if result['result'] in ('pipeline_recipe_validated_only','cold_candidate_created_not_exact_app_replay') and not result.get('cleanup_errors') else 2

if __name__=='__main__':raise SystemExit(main())
