"""Compile and run actual NXEmu SSA IR regressions against an explicit production library.
Synthetic IR tests are not ROM replay, full frontend tests or GPU numerical validation.
"""
from pathlib import Path
import argparse,hashlib,json,subprocess,sys

CPP=r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <memory>
#include <set>
#include <string>
#include <vector>
#include "yuzu_shader_recompiler/frontend/ir/program.h"
#include "yuzu_shader_recompiler/frontend/ir/post_order.h"
#include "yuzu_shader_recompiler/ir_opt/passes.h"
#include "yuzu_shader_recompiler/translation_trace.h"
using namespace Shader;
using namespace Shader::IR;

struct Fixture {
    ObjectPool<Inst> instructions;
    ObjectPool<Block> blocks;
    Program program;
    Block* make() {auto* b=blocks.Create(instructions);b->SetOrder(static_cast<u32>(program.blocks.size()));program.blocks.push_back(b);return b;}
    Inst* emit(Block* b,Opcode op,std::initializer_list<Value> args={}) {b->AppendNewInst(op,args);return &b->back();}
    void order(std::initializer_list<Block*> forward){program.post_order_blocks.assign(forward);std::reverse(program.post_order_blocks.begin(),program.post_order_blocks.end());}
};
std::uint64_t hash_text(const std::string& text){std::uint64_t h=1469598103934665603ULL;for(unsigned char c:text){h^=c;h*=1099511628211ULL;}return h;}
void invariant(const Fixture& f){
    for(auto* b:f.program.blocks){bool nonphi=false;for(const auto& inst:b->Instructions()){
        if(inst.GetOpcode()!=Opcode::Phi){nonphi=true;continue;}
        assert(!nonphi);assert(inst.Type()!=Type::Opaque);
        assert(inst.NumArgs()==b->ImmPredecessors().size());
        u32 previous=0;bool first=true;std::set<const Block*> seen;
        for(std::size_t i=0;i<inst.NumArgs();++i){const auto* pred=inst.PhiBlock(i);assert(std::ranges::find(b->ImmPredecessors(),pred)!=b->ImmPredecessors().end());assert(seen.insert(pred).second);assert(first||previous<pred->GetOrder());first=false;previous=pred->GetOrder();}
    }}
}
void immediate(Inst* query,u32 expected){assert(query->GetOpcode()==Opcode::Identity);auto v=Value(query).Resolve();assert(v.IsImmediate()&&v.Type()==Type::U32&&v.U32()==expected);}
void boolean(Inst* query,bool expected){assert(query->GetOpcode()==Opcode::Identity);auto v=Value(query).Resolve();assert(v.IsImmediate()&&v.Type()==Type::U1&&v.U1()==expected);}
std::vector<std::string> dumps,raw_dumps;
std::string canonical_dump(const std::string& raw){
    std::string out;std::size_t start=0;
    while(start<raw.size()){
        const auto end=raw.find('\n',start);const auto length=(end==std::string::npos?raw.size():end)-start;
        std::size_t skip=0;
        if(length>=19&&raw[start]=='['&&raw[start+17]==']'&&raw[start+18]==' '){bool hex=true;for(unsigned i=1;i<=16;++i)hex=hex&&std::isxdigit(static_cast<unsigned char>(raw[start+i]));if(hex)skip=19;}
        out.append(raw,start+skip,length-skip);if(end==std::string::npos)break;out+='\n';start=end+1;
    }
    return out;
}
std::uint64_t finish(Fixture& f){invariant(f);const auto raw=DumpProgram(f.program);assert(raw.find("$<unknown block")==std::string::npos);const auto dump=canonical_dump(raw);dumps.push_back(dump);raw_dumps.push_back(raw);return hash_text(dump);}
std::uint64_t scalar_case(){
    Fixture f;auto* b=f.make();f.order({b});
    f.emit(b,Opcode::SetRegister,{Value(Reg::R0),Value(u32{42})});
    std::vector<Inst*> r;for(unsigned i=0;i<32;++i)r.push_back(f.emit(b,Opcode::GetRegister,{Value(Reg::R0)}));
    f.emit(b,Opcode::SetPred,{Value(Pred::P0),Value(true)});auto* p=f.emit(b,Opcode::GetPred,{Value(Pred::P0)});
    f.emit(b,Opcode::SetZFlag,{Value(true)});auto* z=f.emit(b,Opcode::GetZFlag);
    f.emit(b,Opcode::SetSFlag,{Value(false)});auto* s=f.emit(b,Opcode::GetSFlag);
    f.emit(b,Opcode::SetCFlag,{Value(true)});auto* c=f.emit(b,Opcode::GetCFlag);
    f.emit(b,Opcode::SetOFlag,{Value(false)});auto* o=f.emit(b,Opcode::GetOFlag);
    f.emit(b,Opcode::SetGotoVariable,{Value(u32{100000}),Value(true)});auto* g=f.emit(b,Opcode::GetGotoVariable,{Value(u32{100000})});
    f.emit(b,Opcode::SetIndirectBranchVariable,{Value(u32{77})});auto* ind=f.emit(b,Opcode::GetIndirectBranchVariable);
    Optimization::SsaRewritePass(f.program);for(auto* q:r)immediate(q,42);boolean(p,true);boolean(z,true);boolean(s,false);boolean(c,true);boolean(o,false);boolean(g,true);immediate(ind,77);return finish(f);
}
std::uint64_t redefine_case(){
    Fixture f;auto* b=f.make();f.order({b});
    f.emit(b,Opcode::SetRegister,{Value(Reg::R0),Value(u32{5})});auto* first=f.emit(b,Opcode::GetRegister,{Value(Reg::R0)});
    f.emit(b,Opcode::SetRegister,{Value(Reg::R1),Value(first)});f.emit(b,Opcode::SetRegister,{Value(Reg::R0),Value(u32{9})});
    auto* after=f.emit(b,Opcode::GetRegister,{Value(Reg::R0)});auto* alias=f.emit(b,Opcode::GetRegister,{Value(Reg::R1)});
    f.emit(b,Opcode::SetPred,{Value(Pred::P0),Value(true)});auto* before_pred=f.emit(b,Opcode::GetPred,{Value(Pred::P0)});f.emit(b,Opcode::SetPred,{Value(Pred::P0),Value(false)});auto* after_pred=f.emit(b,Opcode::GetPred,{Value(Pred::P0)});
    f.emit(b,Opcode::SetGotoVariable,{Value(u32{900}),Value(true)});auto* before_goto=f.emit(b,Opcode::GetGotoVariable,{Value(u32{900})});f.emit(b,Opcode::SetGotoVariable,{Value(u32{900}),Value(false)});auto* after_goto=f.emit(b,Opcode::GetGotoVariable,{Value(u32{900})});
    Optimization::SsaRewritePass(f.program);immediate(first,5);immediate(after,9);immediate(alias,5);boolean(before_pred,true);boolean(after_pred,false);boolean(before_goto,true);boolean(after_goto,false);return finish(f);
}
std::uint64_t undef_case(){
    Fixture f;auto* b=f.make();f.order({b});auto* r=f.emit(b,Opcode::GetRegister,{Value(Reg::R4)});auto* repeat=f.emit(b,Opcode::GetRegister,{Value(Reg::R4)});auto* p=f.emit(b,Opcode::GetPred,{Value(Pred::P1)});auto* g=f.emit(b,Opcode::GetGotoVariable,{Value(u32{123})});
    Optimization::SsaRewritePass(f.program);auto vr=Value(r).Resolve();assert(!vr.IsImmediate()&&vr.Inst()->GetOpcode()==Opcode::UndefU32);assert(Value(repeat).Resolve()==vr);assert(Value(p).Resolve().Inst()->GetOpcode()==Opcode::UndefU1);assert(Value(g).Resolve().Inst()->GetOpcode()==Opcode::UndefU1);return finish(f);
}
std::uint64_t diamond_case(bool different){
    Fixture f;auto* entry=f.make();auto* left=f.make();auto* right=f.make();auto* merge=f.make();entry->AddBranch(left);entry->AddBranch(right);right->AddBranch(merge);left->AddBranch(merge);f.order({entry,left,right,merge});
    f.emit(left,Opcode::SetRegister,{Value(Reg::R0),Value(u32{11})});f.emit(right,Opcode::SetRegister,{Value(Reg::R0),Value(u32{different?22u:11u})});auto* query=f.emit(merge,Opcode::GetRegister,{Value(Reg::R0)});
    Optimization::SsaRewritePass(f.program);
    if(!different)immediate(query,11);else {auto v=Value(query).Resolve();assert(!v.IsImmediate());auto* phi=v.Inst();assert(phi->GetOpcode()==Opcode::Phi&&phi->NumArgs()==2&&phi->Type()==Type::U32);assert(phi->PhiBlock(0)==left&&phi->PhiBlock(1)==right);assert(phi->Arg(0).Resolve().U32()==11&&phi->Arg(1).Resolve().U32()==22);}
    return finish(f);
}
std::uint64_t chain_case(){
    Fixture f;auto* a=f.make();auto* b=f.make();auto* c=f.make();a->AddBranch(b);b->AddBranch(c);f.order({a,b,c});f.emit(a,Opcode::SetRegister,{Value(Reg::R1),Value(u32{19})});std::vector<Inst*> queries;for(unsigned i=0;i<64;++i)queries.push_back(f.emit(c,Opcode::GetRegister,{Value(Reg::R1)}));Optimization::SsaRewritePass(f.program);for(auto* q:queries)immediate(q,19);return finish(f);
}
std::uint64_t loop_case(){
    Fixture f;auto* entry=f.make();auto* header=f.make();auto* body=f.make();auto* exit=f.make();entry->AddBranch(header);header->AddBranch(body);body->AddBranch(header);header->AddBranch(exit);f.order({entry,header,body,exit});
    f.emit(entry,Opcode::SetRegister,{Value(Reg::R0),Value(u32{7})});auto* h=f.emit(header,Opcode::GetRegister,{Value(Reg::R0)});auto* e=f.emit(exit,Opcode::GetRegister,{Value(Reg::R0)});Optimization::SsaRewritePass(f.program);
    auto v=Value(h).Resolve();if(v.IsImmediate())assert(v.U32()==7);else {auto* phi=v.Inst();assert(phi->GetOpcode()==Opcode::Phi&&phi->NumArgs()==2);assert(phi->Arg(0).Resolve().IsImmediate()&&phi->Arg(0).Resolve().U32()==7);assert(phi->Arg(1).Resolve()==v);}
    assert(Value(e).Resolve()==v);return finish(f);
}
std::uint64_t benchmark(unsigned reads){
    Fixture f;auto* b=f.make();f.order({b});f.emit(b,Opcode::SetRegister,{Value(Reg::R0),Value(u32{99})});Inst* last=nullptr;
    for(unsigned i=0;i<reads;++i)last=f.emit(b,Opcode::GetRegister,{Value(Reg::R0)});
    TranslationTrace::Capture disabled(nullptr);const auto start=std::chrono::steady_clock::now();Optimization::SsaRewritePass(f.program);const auto elapsed=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();immediate(last,99);return elapsed;
}


std::uint64_t multi_phi_tail_case(){
    Fixture f;auto* entry=f.make();auto* left=f.make();auto* right=f.make();auto* merge=f.make();auto* empty=f.make();entry->AddBranch(left);entry->AddBranch(right);right->AddBranch(merge);left->AddBranch(merge);merge->AddBranch(empty);f.order({entry,left,right,merge,empty});
    std::vector<Inst*> queries;
    for(unsigned r=0;r<32;++r){const auto reg=static_cast<Reg>(r);f.emit(left,Opcode::SetRegister,{Value(reg),Value(u32{100+r})});f.emit(right,Opcode::SetRegister,{Value(reg),Value(u32{r%2?200+r:100+r})});queries.push_back(f.emit(merge,Opcode::GetRegister,{Value(reg)}));}
    for(unsigned n=0;n<4096;++n)f.emit(merge,Opcode::GetRegister,{Value(Reg::R0)});
    Optimization::SsaRewritePass(f.program);unsigned count=0;
    for(unsigned r=0;r<32;++r){auto v=Value(queries[r]).Resolve();if(r%2){assert(!v.IsImmediate()&&v.Inst()->GetOpcode()==Opcode::Phi);auto* phi=v.Inst();assert(phi->Type()==Type::U32&&phi->PhiBlock(0)==left&&phi->PhiBlock(1)==right);assert(phi->Arg(0).Resolve().U32()==100+r&&phi->Arg(1).Resolve().U32()==200+r);++count;}else immediate(queries[r],100+r);}
    assert(count==16&&empty->Instructions().empty());return finish(f);
}


std::uint64_t late_write_successor_case(){
    Fixture f;auto* a=f.make();auto* b=f.make();auto* c=f.make();a->AddBranch(b);b->AddBranch(c);f.order({a,b,c});
    f.emit(a,Opcode::SetRegister,{Value(Reg::R2),Value(u32{11})});f.emit(a,Opcode::SetPred,{Value(Pred::P2),Value(true)});f.emit(a,Opcode::SetGotoVariable,{Value(u32{7}),Value(true)});f.emit(a,Opcode::SetIndirectBranchVariable,{Value(u32{101})});
    std::vector<Opcode> setflags{Opcode::SetZFlag,Opcode::SetSFlag,Opcode::SetCFlag,Opcode::SetOFlag};std::vector<Opcode> getflags{Opcode::GetZFlag,Opcode::GetSFlag,Opcode::GetCFlag,Opcode::GetOFlag};
    for(auto op:setflags)f.emit(a,op,{Value(true)});
    auto* before_r=f.emit(b,Opcode::GetRegister,{Value(Reg::R2)});auto* before_p=f.emit(b,Opcode::GetPred,{Value(Pred::P2)});auto* before_g=f.emit(b,Opcode::GetGotoVariable,{Value(u32{7})});auto* before_i=f.emit(b,Opcode::GetIndirectBranchVariable);std::vector<Inst*> before_flags;for(auto op:getflags)before_flags.push_back(f.emit(b,op));
    f.emit(b,Opcode::SetRegister,{Value(Reg::R2),Value(u32{22})});f.emit(b,Opcode::SetPred,{Value(Pred::P2),Value(false)});f.emit(b,Opcode::SetGotoVariable,{Value(u32{7}),Value(false)});f.emit(b,Opcode::SetIndirectBranchVariable,{Value(u32{202})});for(auto op:setflags)f.emit(b,op,{Value(false)});
    auto* after_r=f.emit(c,Opcode::GetRegister,{Value(Reg::R2)});auto* after_p=f.emit(c,Opcode::GetPred,{Value(Pred::P2)});auto* after_g=f.emit(c,Opcode::GetGotoVariable,{Value(u32{7})});auto* after_i=f.emit(c,Opcode::GetIndirectBranchVariable);std::vector<Inst*> after_flags;for(auto op:getflags)after_flags.push_back(f.emit(c,op));
    Optimization::SsaRewritePass(f.program);immediate(before_r,11);immediate(after_r,22);boolean(before_p,true);boolean(after_p,false);boolean(before_g,true);boolean(after_g,false);immediate(before_i,101);immediate(after_i,202);for(auto* i:before_flags)boolean(i,true);for(auto* i:after_flags)boolean(i,false);return finish(f);
}
std::uint64_t alias_successor_case(){
    Fixture f;auto* a=f.make();auto* b=f.make();auto* c=f.make();a->AddBranch(b);b->AddBranch(c);f.order({a,b,c});f.emit(a,Opcode::SetRegister,{Value(Reg::R0),Value(u32{73})});auto* old=f.emit(b,Opcode::GetRegister,{Value(Reg::R0)});f.emit(b,Opcode::SetRegister,{Value(Reg::R1),Value(old)});f.emit(b,Opcode::SetRegister,{Value(Reg::R0),Value(u32{99})});auto* alias=f.emit(c,Opcode::GetRegister,{Value(Reg::R1)});auto* latest=f.emit(c,Opcode::GetRegister,{Value(Reg::R0)});Optimization::SsaRewritePass(f.program);immediate(alias,73);immediate(latest,99);return finish(f);
}
std::uint64_t undef_successor_case(){
    Fixture f;auto* a=f.make();auto* b=f.make();auto* c=f.make();a->AddBranch(b);b->AddBranch(c);f.order({a,b,c});auto* r=f.emit(b,Opcode::GetRegister,{Value(Reg::R4)});auto* p=f.emit(b,Opcode::GetPred,{Value(Pred::P1)});auto* next_r=f.emit(c,Opcode::GetRegister,{Value(Reg::R4)});auto* next_p=f.emit(c,Opcode::GetPred,{Value(Pred::P1)});Optimization::SsaRewritePass(f.program);auto vr=Value(r).Resolve();auto vp=Value(p).Resolve();assert(!vr.IsImmediate()&&vr.Inst()->GetOpcode()==Opcode::UndefU32);assert(!vp.IsImmediate()&&vp.Inst()->GetOpcode()==Opcode::UndefU1);assert(Value(next_r).Resolve()==vr&&Value(next_p).Resolve()==vp);return finish(f);
}
std::uint64_t loop_write_case(){
    Fixture f;auto* e=f.make();auto* h=f.make();auto* b=f.make();auto* x=f.make();e->AddBranch(h);h->AddBranch(b);b->AddBranch(h);h->AddBranch(x);AbstractSyntaxNode root{};root.type=AbstractSyntaxNode::Type::Block;root.data.block=e;f.program.post_order_blocks=IR::PostOrder(root);std::cerr<<"ACTUAL_POSTORDER_REVERSE:";for(auto it=f.program.post_order_blocks.rbegin();it!=f.program.post_order_blocks.rend();++it)std::cerr<<' '<<(*it)->GetOrder();std::cerr<<'\n';f.emit(e,Opcode::SetRegister,{Value(Reg::R0),Value(u32{7})});auto* head=f.emit(h,Opcode::GetRegister,{Value(Reg::R0)});f.emit(b,Opcode::SetRegister,{Value(Reg::R0),Value(u32{11})});auto* exit=f.emit(x,Opcode::GetRegister,{Value(Reg::R0)});Optimization::SsaRewritePass(f.program);auto v=Value(head).Resolve();assert(!v.IsImmediate()&&v.Inst()->GetOpcode()==Opcode::Phi);auto* phi=v.Inst();assert(phi->NumArgs()==2&&phi->PhiBlock(0)==e&&phi->PhiBlock(1)==b);std::cerr<<DumpProgram(f.program);assert(phi->Arg(0).Resolve().U32()==7&&phi->Arg(1).Resolve().U32()==11);assert(Value(exit).Resolve()==v);return finish(f);
}

struct CostResult {std::uint64_t wall{},visit{},finalize{},before{},after{},identities{},phis{},removed{},final_visits{},checksum{},max_query_identity_depth{};};
CostResult cost_case(const std::string& kind,bool trace,unsigned block_count=512){
    Fixture f;std::vector<Inst*> queries;std::uint64_t expected{};
    const unsigned blocks=kind=="repeated_read"?1:block_count;
    const unsigned regs=kind=="repeated_read"?1:16;
    std::vector<Block*> chain;for(unsigned n=0;n<blocks;++n)chain.push_back(f.make());
    for(unsigned n=1;n<blocks;++n)chain[n-1]->AddBranch(chain[n]);
    f.program.post_order_blocks.assign(chain.rbegin(),chain.rend());
    for(unsigned r=0;r<regs;++r)f.emit(chain[0],Opcode::SetRegister,{Value(static_cast<Reg>(r)),Value(u32{99+r})});
    if(kind=="repeated_read"){
        for(unsigned n=0;n<100000;++n){queries.push_back(f.emit(chain[0],Opcode::GetRegister,{Value(Reg::R0)}));expected+=99;}
    }else if(kind=="phi_dense_chain"){
        for(auto* b:chain)for(unsigned r=0;r<regs;++r){queries.push_back(f.emit(b,Opcode::GetRegister,{Value(static_cast<Reg>(r))}));expected+=99+r;}
    }else{
        for(unsigned r=0;r<regs;++r){queries.push_back(f.emit(chain.back(),Opcode::GetRegister,{Value(static_cast<Reg>(r))}));expected+=99+r;}
    }
    CostResult out;for(auto* b:chain)out.before+=b->Instructions().size();
    TranslationTrace::Record rec;
    {TranslationTrace::Capture capture(trace?&rec:nullptr);const auto start=std::chrono::steady_clock::now();Optimization::SsaRewritePass(f.program);out.wall=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-start).count();}
    out.visit=rec.subpasses[static_cast<unsigned>(TranslationTrace::Subpass::SSAVisit)].wall_ns;
    out.finalize=rec.subpasses[static_cast<unsigned>(TranslationTrace::Subpass::SSAFinalize)].wall_ns;
    out.removed=rec.work[static_cast<unsigned>(TranslationTrace::Work::SSATrivialRemoved)];out.final_visits=rec.work[static_cast<unsigned>(TranslationTrace::Work::SSAFinalInstructions)];
    invariant(f);for(auto* b:chain)for(auto& i:b->Instructions()){++out.after;out.identities+=i.GetOpcode()==Opcode::Identity;out.phis+=i.GetOpcode()==Opcode::Phi;}
    for(auto* q:queries){auto cursor=Value(q);std::uint64_t depth{};while(cursor.IsIdentity()){cursor=cursor.Inst()->Arg(0);++depth;assert(depth<=2*block_count+100000);}out.max_query_identity_depth=std::max(out.max_query_identity_depth,depth);auto v=Value(q).Resolve();assert(v.IsImmediate()&&v.Type()==Type::U32);out.checksum+=v.U32();}assert(out.checksum==expected);return out;
}

void seal_scaling(){
    std::cout<<",\"seal_scaling\":{\"actual_SSA\":true,\"construction_and_census_outside_timed_scope\":true,\"blocks\":[";
    bool comma=false;
    for(unsigned count:{128,256,512,1024}){
        if(comma)std::cout<<',';comma=true;cost_case("phi_dense_chain",false,count);
        std::cout<<"{\"blocks\":"<<count<<",\"registers\":16,\"samples\":[";
        for(unsigned n=0;n<5;++n){if(n)std::cout<<',';auto r=cost_case("phi_dense_chain",true,count);std::cout<<"{\"wall_ns\":"<<r.wall<<",\"visit_ns\":"<<r.visit<<",\"finalize_ns\":"<<r.finalize<<",\"before\":"<<r.before<<",\"after\":"<<r.after<<",\"identities\":"<<r.identities<<",\"max_query_identity_depth\":"<<r.max_query_identity_depth<<",\"checksum\":"<<r.checksum<<'}';}
        std::cout<<"]}";
    }std::cout<<"]}";
}

void trace_ab(){
    std::cout<<",\"trace_ab\":{\"same_executable\":true,\"construction_and_census_outside_timed_scope\":true,\"alternating_order\":true,\"warmup_pairs\":2,\"pairs\":11,\"cases\":[";
    const char* names[]={"repeated_read","phi_dense_chain","sparse_read_chain"};bool first_case=true;
    for(auto* name:names){if(!first_case)std::cout<<',';first_case=false;
        for(unsigned n=0;n<2;++n){cost_case(name,false);cost_case(name,true);}
        std::cout<<"{\"name\":\""<<name<<"\",\"samples\":[";
        for(unsigned n=0;n<11;++n){CostResult off,on;if(n%2){on=cost_case(name,true);off=cost_case(name,false);}else{off=cost_case(name,false);on=cost_case(name,true);}assert(off.before==on.before&&off.after==on.after&&off.identities==on.identities&&off.phis==on.phis&&off.checksum==on.checksum);
            if(n)std::cout<<',';std::cout<<"{\"off_ns\":"<<off.wall<<",\"on_ns\":"<<on.wall<<",\"on_visit_ns\":"<<on.visit<<",\"on_finalize_ns\":"<<on.finalize<<",\"before\":"<<on.before<<",\"after\":"<<on.after<<",\"identities\":"<<on.identities<<",\"phis\":"<<on.phis<<",\"on_removed_events\":"<<on.removed<<",\"on_final_visits\":"<<on.final_visits<<",\"checksum\":"<<on.checksum<<'}';
        }std::cout<<"]}";
    }std::cout<<"]}";
}

int main(int argc,char** argv){
    const bool loop_probe=argc>=3&&std::string(argv[2])=="loop-write-probe";
    TranslationTrace::Record record;std::vector<std::uint64_t> hashes;
    {TranslationTrace::Capture capture(&record);if(loop_probe)hashes={loop_write_case()};else hashes={scalar_case(),redefine_case(),undef_case(),diamond_case(false),diamond_case(true),chain_case(),loop_case(),multi_phi_tail_case(),late_write_successor_case(),alias_successor_case(),undef_successor_case()};}
    const char* names[]={"reg_pred_flags_goto_indirect_repeated","redefinition_and_alias_identity","undef","trivial_diamond","nontrivial_diamond_phi_order","single_predecessor_chain","loop_self_phi","multiple_phi_long_identity_tail_empty_block","late_write_all_types_successor","cross_variable_alias_successor","undef_successor_identity"};if(loop_probe)names[0]="loop_written_nontrivial_phi";
    if(argc>=2){std::filesystem::create_directories(argv[1]);for(unsigned i=0;i<dumps.size();++i){std::ofstream file(std::filesystem::path(argv[1])/(std::string(names[i])+".ir.txt"),std::ios::binary);file<<dumps[i];assert(file);std::ofstream original(std::filesystem::path(argv[1])/(std::string(names[i])+".raw.ir.txt"),std::ios::binary);original<<raw_dumps[i];assert(original);}}
    std::cout<<"{\"actual_production_SSA\":true,\"synthetic_IR\":true,\"GPU_executed\":false,\"passed\":true,\"cases\":[";
    for(unsigned i=0;i<hashes.size();++i){if(i)std::cout<<',';std::cout<<"{\"name\":\""<<names[i]<<"\",\"canonical_IR_fnv1a64\":"<<hashes[i]<<'}';}
    std::cout<<"],\"work\":{";for(unsigned i=0;i<TranslationTrace::WorkCount;++i){if(i)std::cout<<',';std::cout<<'"'<<TranslationTrace::WorkNames[i]<<"\":"<<record.work[i];}
    std::cout<<"},\"benchmark\":{\"reads_per_run\":100000,\"diagnostic_capture_enabled\":false,\"SSA_wall_ns\":[";for(unsigned i=0;i<5;++i){if(i)std::cout<<',';std::cout<<benchmark(100000);}std::cout<<"]}";if(argc>=3){if(std::string(argv[2])=="seal-scaling")seal_scaling();else if(std::string(argv[2])=="trace-ab")trace_ab();}std::cout<<"}\n";
}
'''

def main():
    ap=argparse.ArgumentParser(description=__doc__);ap.add_argument('--library',type=Path,required=True);ap.add_argument('--out',type=Path,required=True);ap.add_argument('--ssa-source',type=Path);ap.add_argument('--loop-write-probe',action='store_true',help='negative loop-write semantic probe; asserts expected incoming7/backedge11, may expose baseline failure');ap.add_argument('--seal-scaling',action='store_true',help='actualSSA dense-chain128/256/512/1024 scaling; postpass Identity depth census');ap.add_argument('--trace-ab',action='store_true',help='alternate enabled/disabled actualSSA over fixed read/Phi workloads in one executable');ap.add_argument('--link-lib',type=Path,action='append',default=[])
    a=ap.parse_args();P=Path(__file__).resolve().parents[2];R=P/'repo';O=a.out.resolve();O.mkdir(parents=True,exist_ok=True);lib=a.library.resolve(strict=True);source=(a.ssa_source or R/'src/yuzu_shader_recompiler/ir_opt/ssa_rewrite_pass.cpp').resolve(strict=True)
    cpp=O/'ssa-production-test.cpp';cpp.write_text(CPP,encoding='utf-8');exe=O/'ssa-production-test.exe';obj=O/'ssa-production-test.obj'
    if a.ssa_source:
        obj=O/'objects';obj.mkdir(exist_ok=True)
    build=R/'build/windows-ninja';libs=[lib,build/'src/common/Release/common.lib',build/'src/yuzu_common/Release/yuzu_common.lib',build/'external/fmt/Release/fmt.lib',*a.link_lib]
    args=['cl','/nologo','/std:c++20','/utf-8','/EHsc','/MT','/O2','/I'+str(R/'src'),'/I'+str(R/'external/boost'),'/I'+str(R/'external/fmt/include'),'/I'+str(build/'src'),str(cpp),*([str(source)] if a.ssa_source else []),'/Fe:'+str(exe),'/Fo:'+(str(obj)+'\\' if a.ssa_source else str(obj)),*[str(p.resolve(strict=True)) for p in libs],'/link','/OPT:REF','ws2_32.lib','winmm.lib','bcrypt.lib','crypt32.lib','iphlpapi.lib','psapi.lib']
    cmd=O/'build-ssa-test.cmd';cmd.write_text('@echo off\ncall "D:\\VSBuildTools2022\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\nif errorlevel 1 exit /b %errorlevel%\n'+subprocess.list2cmdline(args)+'\n',encoding='utf-8')
    sha=lambda p:hashlib.sha256(p.read_bytes()).hexdigest();headers=[R/'src/yuzu_shader_recompiler/translation_trace.h',R/'src/yuzu_shader_recompiler/frontend/ir/value.h',R/'src/yuzu_shader_recompiler/frontend/ir/basic_block.h',R/'src/yuzu_shader_recompiler/frontend/ir/program.h']
    hashes_before={str(p):sha(p) for p in [lib,source,cpp,*libs[1:],*headers]}
    b=subprocess.run(['cmd','/c',str(cmd)],capture_output=True,text=True,timeout=120);(O/'build.log').write_text(b.stdout+b.stderr,encoding='utf-8')
    if b.returncode:print(b.stdout+b.stderr);raise SystemExit(b.returncode)
    r=subprocess.run([str(exe),str(O/'IR-dumps'),*(['loop-write-probe'] if a.loop_write_probe else ['seal-scaling'] if a.seal_scaling else ['trace-ab'] if a.trace_ab else [])],capture_output=True,text=True,timeout=90);(O/'run.log').write_text(r.stdout+r.stderr,encoding='utf-8')
    if r.returncode:
        failure={'stage':'actual_SSA_run','actual_production_SSA':True,'synthetic_IR':True,'GPU_executed':False,'command':subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]]),'native_command':subprocess.list2cmdline([str(exe),str(O/'IR-dumps'),*(['loop-write-probe'] if a.loop_write_probe else ['seal-scaling'] if a.seal_scaling else ['trace-ab'] if a.trace_ab else [])]),'input':('actual PostOrder and actualSSA; entry7/headerread/bodywrite11; expected Phi incoming7/backedge11' if a.loop_write_probe else 'actualSSA handcrafted IR cases'),'literal_stdout':r.stdout,'literal_stderr':r.stderr,'exit_status':r.returncode,'hashes':hashes_before,'expected_failure_probe':a.loop_write_probe}
        (O/'FAILURE.json').write_text(json.dumps(failure,indent=2),encoding='utf-8');print(r.stdout+r.stderr);raise SystemExit(r.returncode)
    result=json.loads(r.stdout);assert result['passed'] and result['actual_production_SSA'];assert all(sha(Path(p))==h for p,h in hashes_before.items())
    for case in result['cases']:
        dump=O/'IR-dumps'/(case['name']+'.ir.txt');case['canonical_IR_sha256']=sha(dump);case['raw_IR_dump_sha256']=sha(O/'IR-dumps'/(case['name']+'.raw.ir.txt'));case['IR_dump_file']=str(dump.resolve())
    evidence={'command':subprocess.list2cmdline([sys.executable,'-B',str(Path(__file__).resolve()),*sys.argv[1:]]),'compiler_command':subprocess.list2cmdline(args),'input':'handcrafted IR reg/pred/flags/goto/undef/diamonds/chain/loop; explicit production static library','literal_stdout':r.stdout,'literal_stderr':r.stderr,'exit_status':r.returncode,'hashes':hashes_before,'source_binding':('explicit SSA source compiled and linked; selected library supplies IR support' if a.ssa_source else 'source/library hashes recorded; caller must verify library build provenance'),'IR_normalization':'only leading [16hexaddress] removed per instruction line; raw dumps separately retained','result':result}
    (O/'RESULT.json').write_text(json.dumps(evidence,indent=2),encoding='utf-8');print(r.stdout,end='')
if __name__=='__main__':main()
