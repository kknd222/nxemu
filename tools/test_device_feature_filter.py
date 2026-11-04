"""Compile the actual production robust/UAB filter with all eight input combinations."""
from pathlib import Path
import argparse,hashlib,json,subprocess,sys

def sha(p):return hashlib.sha256(p.read_bytes()).hexdigest()
def main():
 ap=argparse.ArgumentParser();ap.add_argument('--out',type=Path,required=True);a=ap.parse_args()
 R=Path(__file__).resolve().parents[1];O=a.out.resolve();O.mkdir(parents=True,exist_ok=False)
 src=R/'src/yuzu_video_core/vulkan_common/vulkan_device.cpp';s=src.read_text()
 start=s.index('    if (features2.features.robustBufferAccess &&');end=s.index('    // Unload extensions',start);block=s[start:end]
 assert block.count('descriptorBinding')==4
 code='''#include <iostream>
#include <stdexcept>
constexpr unsigned VK_FALSE=0;
struct Core {unsigned robustBufferAccess;};
struct Index {unsigned descriptorBindingUniformBufferUpdateAfterBind,descriptorBindingStorageBufferUpdateAfterBind,descriptorBindingUniformTexelBufferUpdateAfterBind,descriptorBindingStorageTexelBufferUpdateAfterBind;unsigned sampledImage=7,storageImage=9;};
struct Prop {unsigned robustBufferAccessUpdateAfterBind;};
int main(){unsigned cases=0;for(unsigned r=0;r<2;++r)for(unsigned p=0;p<2;++p)for(unsigned u=0;u<2;++u){
struct {Core features;}features2{{r}};struct {Prop descriptor_indexing;}properties{{p}};struct {Index descriptor_indexing;}features{{u,u,u,u}};
''' +block+'''auto& i=features.descriptor_indexing;unsigned expected=r&&!p?0:u;
if(i.descriptorBindingUniformBufferUpdateAfterBind!=expected||i.descriptorBindingStorageBufferUpdateAfterBind!=expected||i.descriptorBindingUniformTexelBufferUpdateAfterBind!=expected||i.descriptorBindingStorageTexelBufferUpdateAfterBind!=expected||i.sampledImage!=7||i.storageImage!=9||features2.features.robustBufferAccess!=r)return 1;++cases;}
std::cout<<"DEVICE_FEATURE_FILTER_PASS cases="<<cases<<" core_robust_preserved=True image_UAB_preserved=True\\n";return 0;}
'''
 cpp=O/'filter.cpp';cpp.write_text(code);exe=O/'filter.exe';cmd=['cl','/nologo','/EHsc','/std:c++20',str(cpp),f'/Fe:{exe}',f'/Fo:{O}/']
 bat=O/'build.cmd';bat.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'+subprocess.list2cmdline(cmd)+'\n')
 h=sha(src);b=subprocess.run(['cmd','/c',str(bat)],capture_output=True,text=True,timeout=30)
 j={'command':subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]]),'source':str(src),'source_sha256':h,'actual_extracted_branch':block,'build':dict(command=subprocess.list2cmdline(cmd),exit_status=b.returncode,literal_stdout=b.stdout,literal_stderr=b.stderr)}
 if b.returncode==0:
  t=subprocess.run([str(exe)],capture_output=True,text=True,timeout=5);j['run']=dict(command=str(exe),input='robust 0/1 x property 0/1 x four buffer UAB 0/1; image fields sentinels',exit_status=t.returncode,literal_stdout=t.stdout,literal_stderr=t.stderr)
 j['inputs_unchanged']=sha(src)==h;j['passed']=b.returncode==0 and j['run']['exit_status']==0 and j['inputs_unchanged'];j['exit_status']=0 if j['passed'] else 1
 (O/'RESULT.json').write_text(json.dumps(j,indent=2));print('DEVICE_FEATURE_FILTER_TEST_PASS' if j['passed'] else 'DEVICE_FEATURE_FILTER_TEST_FAILED');return j['exit_status']
if __name__=='__main__':raise SystemExit(main())
