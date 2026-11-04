"""Compile actual NXEmu SSA and bounded fixture codec; test round trips or replay paired IR.

This is SSA-only replay, not full shader translation, hardware or GPU validation.
"""
from pathlib import Path
import argparse
import hashlib
import json
import subprocess
import sys

CPP = r'''
#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>
#include "yuzu_shader_recompiler/ssa_fixture.h"
#include "yuzu_shader_recompiler/ssa_fixture_capture.h"
#include "yuzu_common/debug_document.h"
#include "yuzu_shader_recompiler/frontend/ir/post_order.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
using namespace Shader;
using namespace Shader::IR;
using namespace Shader::SsaFixture;
void check(bool v,const char* why){if(!v)throw std::runtime_error(why);}
std::string quote(const std::string& s){std::string r="\"";for(unsigned char c:s){if(c=='"'||c=='\\'){r+='\\';r+=c;}else if(c=='\n')r+="\\n";else if(c=='\r')r+="\\r";else if(c=='\t')r+="\\t";else if(c<32)r+='?';else r+=c;}return r+'"';}
Bytes read(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary|std::ios::ate);check(bool(f),"read open failed");auto n=f.tellg();check(n>=0&&n<=static_cast<std::streamoff>(MaxBytes),"read budget exceeded");Bytes b(static_cast<size_t>(n));f.seekg(0);f.read(reinterpret_cast<char*>(b.data()),n);check(bool(f),"read failed");return b;}
void write(const std::filesystem::path& p,const Bytes& b){std::ofstream f(p,std::ios::binary);f.write(reinterpret_cast<const char*>(b.data()),b.size());check(bool(f),"write failed");}
std::uint64_t hash(const Bytes& b){std::uint64_t h=14695981039346656037ULL;for(auto c:b)h=(h^c)*1099511628211ULL;return h;}
struct Fixture{
    ObjectPool<Inst> ip;ObjectPool<Block> bp;Program p;
    Block* block(){auto* b=bp.Create(ip);b->SetOrder(static_cast<u32>(p.blocks.size()));p.blocks.push_back(b);return b;}
    Inst* emit(Block* b,Opcode op,std::initializer_list<Value> args={}){b->AppendNewInst(op,args);return &b->back();}
    void order(std::initializer_list<Block*> bs){p.post_order_blocks.assign(bs);std::reverse(p.post_order_blocks.begin(),p.post_order_blocks.end());}
};
size_t mismatch(const Bytes& a,const Bytes& b){size_t n=std::min(a.size(),b.size());for(size_t j=0;j<n;++j)if(a[j]!=b[j])return j;return a.size()==b.size()?static_cast<size_t>(-1):n;}
void same(const Bytes& a,const Bytes& b,const char* message){if(a!=b){std::cerr<<message<<" first_byte="<<mismatch(a,b)<<" sizes="<<a.size()<<'/'<<b.size()<<'\n';throw std::runtime_error(message);}}
void roundtrip(const Bytes& b){auto r=Decode(b);same(b,Encode(r->program,r->ids,r->phase),"codec roundtrip mismatch");}
struct Pair{Bytes pre,post;std::unique_ptr<Replay> replay;};
Pair paired(Fixture& f){
    IdentityMap ids;Pair out;out.pre=Encode(f.p,ids,0);roundtrip(out.pre);
    out.replay=Decode(out.pre);Optimization::SsaRewritePass(f.p);out.post=Encode(f.p,ids,1);roundtrip(out.post);
    Optimization::SsaRewritePass(out.replay->program);same(out.post,Encode(out.replay->program,out.replay->ids,1),"actualSSA replay mismatch");return out;
}
void scalar(const std::filesystem::path& dir){
    Fixture f;auto* a=f.block();auto* b=f.block();a->AddBranch(b);f.order({a,b});f.p.stage=Stage::Compute;f.p.workgroup_size={8,4,2};f.p.local_memory_size=128;f.p.shared_memory_size=256;
    f.emit(a,Opcode::SetRegister,{Value(Reg::R0),Value(u32{42})});auto* q=f.emit(b,Opcode::GetRegister,{Value(Reg::R0)});
    f.emit(a,Opcode::SetPred,{Value(Pred::P0),Value(true)});f.emit(b,Opcode::GetPred,{Value(Pred::P0)});
    f.emit(a,Opcode::SetZFlag,{Value(true)});f.emit(b,Opcode::GetZFlag);f.emit(a,Opcode::SetIndirectBranchVariable,{Value(u32{71})});f.emit(b,Opcode::GetIndirectBranchVariable);
    f.emit(a,Opcode::SetGotoVariable,{Value(u32{123456}),Value(false)});f.emit(b,Opcode::GetGotoVariable,{Value(u32{123456})});
    for(auto v:{Value(u8{0xef}),Value(u16{0xfade}),Value(u32{0xffffffff}),Value(u64{0xfedcba9876543210ULL}),Value(std::bit_cast<float>(u32{0x7fc12345})),Value(std::bit_cast<double>(u64{0x8000000000000000ULL})),Value(Reg::RZ),Value(Pred::PT),Value(Attribute::DrawID),Value(Patch::Component119)}){
        auto* i=f.emit(a,Opcode::Identity,{v});i->SetFlags(u32{0xa5a5a5a5});i->SetDefinition(u32{12345});
    }
    auto pair=paired(f);check(Value(q).Resolve().U32()==42,"scalar semantic oracle failed");write(dir/"scalar.pre.bin",pair.pre);write(dir/"scalar.post.bin",pair.post);
}
void diamond(const std::filesystem::path& dir){
    Fixture f;auto* e=f.block();auto* l=f.block();auto* r=f.block();auto* x=f.block();e->AddBranch(r);e->AddBranch(l);r->AddBranch(x);l->AddBranch(x);f.order({e,r,l,x});
    f.emit(l,Opcode::SetRegister,{Value(Reg::R1),Value(u32{11})});f.emit(r,Opcode::SetRegister,{Value(Reg::R1),Value(u32{22})});auto* q=f.emit(x,Opcode::GetRegister,{Value(Reg::R1)});
    auto pair=paired(f);auto* phi=Value(q).Resolve().Inst();check(phi->GetOpcode()==Opcode::Phi&&phi->NumArgs()==2,"diamond phi oracle failed");check(phi->PhiBlock(0)==l&&phi->PhiBlock(1)==r,"ordered predecessor oracle failed");check(phi->Arg(0).Resolve().U32()==11&&phi->Arg(1).Resolve().U32()==22,"diamond values failed");write(dir/"diamond.pre.bin",pair.pre);write(dir/"diamond.post.bin",pair.post);
}
void loop(const std::filesystem::path& dir){
    Fixture f;auto* e=f.block();auto* h=f.block();auto* b=f.block();auto* x=f.block();e->AddBranch(h);h->AddBranch(b);b->AddBranch(h);h->AddBranch(x);AbstractSyntaxNode root{};root.type=AbstractSyntaxNode::Type::Block;root.data.block=e;f.p.post_order_blocks=IR::PostOrder(root);
    std::vector<u32> visit;for(auto i=f.p.post_order_blocks.rbegin();i!=f.p.post_order_blocks.rend();++i)visit.push_back((*i)->GetOrder());check(visit==std::vector<u32>{0,1,3,2},"actual PostOrder changed");
    f.emit(e,Opcode::SetRegister,{Value(Reg::R0),Value(u32{7})});auto* q=f.emit(h,Opcode::GetRegister,{Value(Reg::R0)});f.emit(b,Opcode::SetRegister,{Value(Reg::R0),Value(u32{11})});f.emit(x,Opcode::GetRegister,{Value(Reg::R0)});
    auto pair=paired(f);auto* phi=Value(q).Resolve().Inst();check(phi->GetOpcode()==Opcode::Phi&&phi->NumArgs()==2,"loop phi absent");check(phi->Arg(0).Resolve().U32()==7,"loop entry changed");check(phi->Arg(1).Resolve().Inst()==phi,"known loop defect no longer reproduces; re-evaluate baseline");write(dir/"loop.pre.bin",pair.pre);write(dir/"loop.post.bin",pair.post);
}
void detached_cache(const std::filesystem::path& dir){
    Fixture f;auto* a=f.block();f.order({a});auto* detached=f.ip.Create(Opcode::Identity,0);detached->SetArg(0,Value(u32{87}));a->SetSsaRegValue(Reg::R9,Value(detached));a->SsaSeal();IdentityMap ids;auto bytes=Encode(f.p,ids,0);roundtrip(bytes);auto replay=Decode(bytes);check(replay->ids.inst_ids.size()==1,"detached closure missing");check(replay->program.blocks[0]->IsSsaSealed()&&replay->program.blocks[0]->SsaRegValue(Reg::R9).Resolve().U32()==87,"cache/sealed roundtrip failed");write(dir/"detached-cache.pre.bin",bytes);
}
void pseudo(const std::filesystem::path& dir){
    Fixture f;auto* b=f.block();f.order({b});auto* producer=f.emit(b,Opcode::IAdd32,{Value(u32{4}),Value(u32{9})});auto* z=f.emit(b,Opcode::GetZeroFromOp,{Value(producer)});auto* c=f.emit(b,Opcode::GetCarryFromOp,{Value(producer)});
    check(producer->GetAssociatedPseudoOperation(Opcode::GetZeroFromOp)==z&&producer->GetAssociatedPseudoOperation(Opcode::GetCarryFromOp)==c,"source pseudo links failed");auto pair=paired(f);auto* restored=pair.replay->ids.inst_ids[0];check(restored->GetAssociatedPseudoOperation(Opcode::GetZeroFromOp)==pair.replay->ids.inst_ids[1]&&restored->GetAssociatedPseudoOperation(Opcode::GetCarryFromOp)==pair.replay->ids.inst_ids[2],"restored pseudo links failed");write(dir/"pseudo.pre.bin",pair.pre);write(dir/"pseudo.post.bin",pair.post);
}
void encoder_negatives(const std::filesystem::path& dir){
    Fixture f;auto* b=f.block();f.order({b});auto* i=f.emit(b,Opcode::Identity,{Value(u32{1})});i->SetArg(0,Value(i));IdentityMap ids;bool rejected=false;try{auto encoded=Encode(f.p,ids,0);}catch(const std::exception&){rejected=true;}check(rejected,"Identity cycle encoder accepted");
    Fixture null_fixture;auto* nb=null_fixture.block();null_fixture.order({nb});nb->SetSsaRegValue(Reg::R0,Value(static_cast<Inst*>(nullptr)));ids={};rejected=false;try{auto encoded=Encode(null_fixture.p,ids,0);}catch(const std::exception&){rejected=true;}check(rejected,"null opaque encoder accepted");std::ofstream file(dir/"encoder-negative.reason.txt");file<<"Identity-only cycle and null opaque cache rejected by actual Encode; legal cyclic Phi covered by loop pair\n";
}
uint32_t get(const Bytes& b,size_t& p){check(p+4<=b.size(),"test malformed offset failed");uint32_t n{};for(unsigned j=0;j<4;++j)n|=uint32_t{b[p++]}<<(8*j);return n;}
void put(Bytes& b,size_t p,uint32_t n){check(p+4<=b.size(),"test mutation offset failed");for(unsigned j=0;j<4;++j)b[p+j]=static_cast<uint8_t>(n>>(8*j));}
void list(const Bytes& b,size_t& p){auto n=get(b,p);check(n<MaxInsts&&p+4ull*n<=b.size(),"test list budget");p+=4ull*n;}
size_t first_opcode(const Bytes& b){size_t p=24;auto nb=get(b,p);get(b,p);p+=44;list(b,p);list(b,p);for(unsigned j=0;j<nb;++j){p+=12;list(b,p);list(b,p);list(b,p);auto n=get(b,p);p+=16ull*n;check(p<=b.size(),"test cache offset failed");}return p;}
void rejected(const Bytes& bytes,const char* name,const std::filesystem::path& dir){bool failed=false;std::string reason;try{auto replay=Decode(bytes);}catch(const std::exception& e){failed=true;reason=e.what();}check(failed,name);write(dir/(std::string(name)+".invalid.bin"),bytes);std::ofstream f(dir/(std::string(name)+".reason.txt"));f<<reason<<'\n';}
void negatives(const std::filesystem::path& dir){
    auto good=read(dir/"scalar.pre.bin");auto b=good;b.pop_back();rejected(b,"truncated",dir);b=good;b.push_back(0);rejected(b,"trailing",dir);b=good;b[0]^=0x20;rejected(b,"bad-magic",dir);b=good;put(b,8,2);rejected(b,"bad-version",dir);b=good;b[12]^=1;rejected(b,"enum-fingerprint",dir);b=good;put(b,20,2);rejected(b,"bad-phase",dir);b=good;put(b,24,MaxBlocks+1);rejected(b,"block-budget",dir);b=good;put(b,28,MaxInsts+1);rejected(b,"instruction-budget",dir);b=good;put(b,first_opcode(b),0xffffffff);rejected(b,"bad-opcode",dir);b=good;put(b,first_opcode(b)+16,6);rejected(b,"bad-arg-count",dir);
    b.assign(MaxBytes+1,0);bool failed=false;try{auto r=Decode(b);}catch(const std::exception&){failed=true;}check(failed,"oversized input accepted");std::ofstream f(dir/"byte-budget.reason.txt");f<<"MaxBytes+1 rejected; invalid32MiB+1 artifact intentionally not written\n";
}
void env(const char* name,const std::string& value){check(SetEnvironmentVariableA(name,value.empty()?nullptr:value.c_str())!=0,"test environment failed");}
std::string textfile(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);check(bool(f),"metadata missing");return {std::istreambuf_iterator<char>(f),std::istreambuf_iterator<char>()};}
void capture(const std::filesystem::path& dir){
    auto root=std::filesystem::absolute(dir/"capture");check(!std::filesystem::exists(root),"capture test output must be fresh");
    env("NXEMU_NATIVE_SHADER_DIAG_DIR",root.string());env("NXEMU_DEBUG_RUN_ID","unit-run");env("NXEMU_DEBUG_GENERATION","3");env("NXEMU_PIPELINE_FIXTURE_KEY","0000000000001234");env("NXEMU_SSA_FIXTURES","0");
    Fixture f;auto* b=f.block();f.order({b});f.emit(b,Opcode::SetRegister,{Value(Reg::R0),Value(u32{39})});f.emit(b,Opcode::GetRegister,{Value(Reg::R0)});
    {Scope scope(0x1234,17);CapturePre(f.p);CapturePost(f.p);}check(!std::filesystem::exists(root),"disabled capture wrote files");
    env("NXEMU_SSA_FIXTURES","1");{Scope scope(0x9999,17);CapturePre(f.p);CapturePost(f.p);}check(!std::filesystem::exists(root),"filter mismatch wrote files");
    IdentityMap ids;auto expected_pre=Encode(f.p,ids,0);
    {Scope scope(0x1234,17);CapturePre(f.p);check(scope.state!=nullptr,"capture was not admitted");check(std::filesystem::exists(root/"ssa-fixtures/ssa-0000000000001234-g3-1.pending.json"),"pending publication missing");check(!std::filesystem::exists(root/"ssa-fixtures/ssa-0000000000001234-g3-1.json"),"ready published before post");Optimization::SsaRewritePass(f.p);CapturePost(f.p);}
    auto expected_post=Encode(f.p,ids,1);auto prefix=root/"ssa-fixtures/ssa-0000000000001234-g3-1";
    same(expected_pre,read(prefix.string()+".pre.bin"),"capture pre bytes differ");same(expected_post,read(prefix.string()+".post.bin"),"capture post bytes differ");
    auto metadata=textfile(prefix.string()+".json");check(metadata.find("\"status\":\"ready\"")!=std::string::npos&&metadata.find("\"pre_complete\":true")!=std::string::npos&&metadata.find("\"post_complete\":true")!=std::string::npos,"ready metadata incorrect");
    size_t files{};for(auto& entry:std::filesystem::directory_iterator(root/"ssa-fixtures")){check(entry.path().extension()!=".tmp","unpublished temp remains");++files;}
    env("NXEMU_DEBUG_GENERATION","5");{Scope scope(0x1234,17);CapturePre(f.p);CapturePost(f.p);check(!scope.state,"reload reset single-pair budget");}
    size_t final_files{};for(auto& entry:std::filesystem::directory_iterator(root/"ssa-fixtures"))++final_files;check(final_files==files,"reload created extra pair");
    env("NXEMU_SSA_FIXTURES","");env("NXEMU_NATIVE_SHADER_DIAG_DIR","");
}
void unit(const std::filesystem::path& dir){std::filesystem::create_directories(dir);scalar(dir);diamond(dir);loop(dir);detached_cache(dir);pseudo(dir);negatives(dir);encoder_negatives(dir);capture(dir);std::cout<<"{\"passed\":true,\"mode\":\"unit\",\"actual_production_SSA\":true,\"synthetic_IR\":true,\"GPU_executed\":false,\"paired_SSA_cases\":4,\"codec_only_cases\":1,\"negative_decode_cases\":11,\"negative_encode_cases\":2,\"byte_exact_roundtrip\":true,\"byte_exact_SSA_replay\":true,\"pseudo_associations\":true,\"capture_off_filter_ready_reload_budget\":true,\"known_loop_write_defect\":\"reproduced7/self; not fixed\"}\n";}
void replay(const std::filesystem::path& pre,const std::filesystem::path& post,const std::filesystem::path& out){
    std::filesystem::create_directories(out);auto before=read(pre),expected=read(post);roundtrip(before);roundtrip(expected);auto r=Decode(before);auto captured=Decode(expected);check(r->phase==0&&captured->phase==1,"paired phase mismatch");size_t count=r->ids.inst_ids.size();Optimization::SsaRewritePass(r->program);auto actual=Encode(r->program,r->ids,1);write(out/"replayed.post.bin",actual);
    auto first=mismatch(expected,actual);bool ok=expected==actual;std::cout<<"{\"passed\":"<<(ok?"true":"false")<<",\"mode\":\"replay\",\"actual_production_SSA\":true,\"synthetic_IR\":null,\"input_provenance\":\"caller-supplied paired snapshots; provenance not authenticated\",\"GPU_executed\":false,\"SSA_only\":true,\"paired_comparison_performed\":true,\"pre_blocks\":"<<r->ids.block_ids.size()<<",\"pre_instructions\":"<<count<<",\"post_instructions\":"<<r->ids.inst_ids.size()<<",\"pre_bytes\":"<<before.size()<<",\"post_bytes\":"<<expected.size()<<",\"replay_bytes\":"<<actual.size()<<",\"byte_exact_roundtrip\":true,\"byte_exact_SSA_replay\":"<<(ok?"true":"false")<<",\"first_mismatch\":"<<(ok?"null":std::to_string(first))<<",\"expected_content_fnv1a64\":"<<hash(expected)<<",\"actual_content_fnv1a64\":"<<hash(actual)<<"}\n";
    if(!ok)throw std::runtime_error("paired actualSSA output mismatch");
}
void preonly(const std::filesystem::path& pre,const std::filesystem::path& out){
    std::filesystem::create_directories(out);auto before=read(pre);roundtrip(before);auto r=Decode(before);check(r->phase==0,"pre-only input phase mismatch");auto blocks=r->ids.block_ids.size(),insts=r->ids.inst_ids.size();Optimization::SsaRewritePass(r->program);auto actual=Encode(r->program,r->ids,1);roundtrip(actual);write(out/"replayed.post.bin",actual);
    std::cout<<"{\"passed\":true,\"mode\":\"preonly\",\"actual_production_SSA\":true,\"synthetic_IR\":null,\"input_provenance\":\"caller-supplied snapshot; provenance not authenticated\",\"GPU_executed\":false,\"SSA_only\":true,\"pre_blocks\":"<<blocks<<",\"pre_instructions\":"<<insts<<",\"post_instructions\":"<<r->ids.inst_ids.size()<<",\"pre_bytes\":"<<before.size()<<",\"replay_bytes\":"<<actual.size()<<",\"byte_exact_roundtrip\":true,\"paired_comparison_performed\":false,\"byte_exact_SSA_replay\":null,\"replayed_post_codec_roundtrip\":true,\"actual_content_fnv1a64\":"<<hash(actual)<<"}\n";
}
int main(int argc,char** argv){try{if(argc==3&&std::string(argv[1])=="unit")unit(argv[2]);else if(argc==5&&std::string(argv[1])=="replay")replay(argv[2],argv[3],argv[4]);else if(argc==4&&std::string(argv[1])=="preonly")preonly(argv[2],argv[3]);else throw std::runtime_error("usage: unit OUT | replay PRE POST OUT | preonly PRE OUT");return 0;}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}catch(...){std::cerr<<"nonstandard exception\n";return 2;}}
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--library', type=Path, required=True)
    ap.add_argument('--out', type=Path, required=True)
    ap.add_argument('--source-root', type=Path)
    ap.add_argument('--ssa-source', type=Path)
    ap.add_argument('--codec-source', type=Path)
    ap.add_argument('--pre', type=Path)
    ap.add_argument('--post', type=Path)
    a = ap.parse_args()
    if a.post and not a.pre:
        ap.error('--post requires --pre; --pre without --post runs unpaired SSA-only replay')
    root = (a.source_root or Path(__file__).resolve().parents[1]).resolve(strict=True)
    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    library = a.library.resolve(strict=True)
    ssa = (a.ssa_source or root/'src/yuzu_shader_recompiler/ir_opt/ssa_rewrite_pass.cpp').resolve(strict=True)
    codec = (a.codec_source or root/'src/yuzu_shader_recompiler/ssa_fixture.cpp').resolve(strict=True)
    capture_source = root/'src/yuzu_shader_recompiler/ssa_fixture_capture.cpp'
    cpp = out/'ssa-fixture-test.cpp'
    cpp.write_text(CPP, encoding='utf-8')
    exe = out/'ssa-fixture-test.exe'
    objects = out/'objects'
    objects.mkdir(exist_ok=True)
    build = root/'build/windows-ninja'
    libs = [library, build/'src/common/Release/common.lib', build/'src/yuzu_common/Release/yuzu_common.lib', build/'external/fmt/Release/fmt.lib']
    args = ['cl', '/nologo', '/std:c++20', '/utf-8', '/EHsc', '/MT', '/O2',
            '/I'+str(root/'src'), '/I'+str(root/'external/boost'), '/I'+str(root/'external/fmt/include'), '/I'+str(build/'src'),
            str(cpp), str(codec), str(ssa), str(capture_source), '/Fe:'+str(exe), '/Fo:'+str(objects)+'\\', *map(str, libs),
            '/link', '/OPT:REF', 'ws2_32.lib', 'winmm.lib', 'bcrypt.lib', 'crypt32.lib', 'iphlpapi.lib', 'psapi.lib']
    cmd = out/'build-ssa-fixture.cmd'
    cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(args)+'\n', encoding='utf-8')
    sha = lambda p: hashlib.sha256(p.read_bytes()).hexdigest()
    headers = [root/'src/yuzu_shader_recompiler'/p for p in ['ssa_fixture.h', 'frontend/ir/value.h', 'frontend/ir/basic_block.h', 'frontend/ir/program.h', 'frontend/ir/opcodes.h', 'frontend/ir/opcodes.inc', 'frontend/ir/type.h']]
    files = [cpp, codec, ssa, capture_source, *libs, *headers, *([a.pre.resolve(strict=True)] if a.pre else []), *([a.post.resolve(strict=True)] if a.post else [])]
    hashes = {str(p): sha(p) for p in files}
    report = {'command': subprocess.list2cmdline([sys.executable, '-B', str(Path(__file__).resolve()), *sys.argv[1:]]),
              'compiler_command': subprocess.list2cmdline(args), 'source_binding': 'actual codec+SSA sources explicitly compiled; production library supplies actual IR support',
              'input': ('caller-supplied paired pre/post SSA graphs' if a.post else 'caller-supplied preSSA graph; no captured postSSA available for comparison' if a.pre else 'handcrafted scalar/diamond/actualPostOrder-loop/cache graph; malformed bounded decoder inputs'),
              'hashes': hashes, 'GPU_executed': False}
    b = subprocess.run(['cmd', '/c', str(cmd)], capture_output=True, text=True, timeout=180)
    (out/'build.log').write_text(b.stdout+b.stderr, encoding='utf-8')
    if b.returncode:
        report.update(stage='compile', literal_stdout=b.stdout, literal_stderr=b.stderr, exit_status=b.returncode)
        (out/'FAILURE.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(b.stdout+b.stderr)
        raise SystemExit(b.returncode)
    native = ([str(exe), 'replay', str(a.pre.resolve()), str(a.post.resolve()), str(out/'fixtures')] if a.post else
              [str(exe), 'preonly', str(a.pre.resolve()), str(out/'fixtures')] if a.pre else [str(exe), 'unit', str(out/'fixtures')])
    r = subprocess.run(native, capture_output=True, text=True, timeout=180)
    (out/'run.log').write_text(r.stdout+r.stderr, encoding='utf-8')
    unchanged = all(sha(Path(p)) == h for p, h in hashes.items())
    report.update(native_command=subprocess.list2cmdline(native), literal_stdout=r.stdout, literal_stderr=r.stderr, exit_status=r.returncode,
                  inputs_unchanged=unchanged, executable_sha256=sha(exe), fixture_hashes={str(p): sha(p) for p in (out/'fixtures').glob('*') if p.is_file()})
    if r.returncode or not unchanged:
        report['stage'] = 'run' if r.returncode else 'hash_binding'
        (out/'FAILURE.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
        print(r.stdout+r.stderr)
        raise SystemExit(r.returncode or 1)
    result = json.loads(r.stdout)
    if not result.get('passed'):
        raise RuntimeError('test returned false success')
    report['result'] = result
    (out/'RESULT.json').write_text(json.dumps(report, indent=2), encoding='utf-8')
    print(r.stdout, end='')


if __name__ == '__main__':
    main()
