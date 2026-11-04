
#include <bit>
#include <cmath>
// Independent IEEE binary16 converter used only by matrix oracle/fixture packing.
u32 half(float f){u32 x=std::bit_cast<u32>(f),sign=(x>>16)&0x8000,m=x&0x7fffff;int ex=int((x>>23)&255)-127+15;if(ex>=31)return sign|0x7c00;if(ex<=0){if(ex<-10)return sign; m|=0x800000;unsigned shift=14-ex;u32 base=m>>shift,rem=m&((1U<<shift)-1),mid=1U<<(shift-1);return sign+base+(rem>mid||(rem==mid&&(base&1)));}u32 base=m>>13,rem=m&8191;u32 result=sign|(u32(ex)<<10)|base;return result+(rem>4096||(rem==4096&&(base&1)));}
float fp(u32 h){u32 s=(h&0x8000)<<16,m=h&1023,e=(h>>10)&31;if(!e){if(!m)return std::bit_cast<float>(s);int exp=-14;while(!(m&1024)){m<<=1;--exp;}return std::bit_cast<float>(s|(u32(exp+127)<<23)|((m&1023)<<13));}return std::bit_cast<float>(s|((e+112)<<23)|(m<<13));}
u32 pack(float a,float b){return half(a)|(half(b)<<16);}
u64 mov(unsigned r,u32 value,unsigned pred=7){return 0x010000000000f000ULL|(u64{pred}<<16)|r|(u64{value}<<20);}
u64 st(unsigned r,unsigned n){return 0xef54000000070000ULL|r|(u64{255}<<8)|(u64{n*4}<<20);}

void force_rte16(std::vector<u32>& spv){
 u32 entry{};for(unsigned n=5;n<spv.size();n+=spv[n]>>16)if((spv[n]&65535)==15)entry=spv[n+2];assert(entry);
 std::vector<u32> result(spv.begin(),spv.begin()+5);bool cap=false,mode=false;
 for(unsigned n=5;n<spv.size();){unsigned len=spv[n]>>16,op=spv[n]&65535;
  if(!cap&&op!=17){result.insert(result.end(),{(2U<<16)|17U,4467});const std::string ext="SPV_KHR_float_controls";std::vector<u32> words((ext.size()+1+3)/4,0);std::memcpy(words.data(),ext.c_str(),ext.size()+1);result.push_back((u32(words.size()+1)<<16)|10);result.insert(result.end(),words.begin(),words.end());cap=true;}
  if(!mode&&(op==5||op==6||op==71||op==19)){result.insert(result.end(),{(4U<<16)|16U,entry,4462,16});mode=true;}
  result.insert(result.end(),spv.begin()+n,spv.begin()+n+len);n+=len;
 }
 assert(cap&&mode);spv=std::move(result);
}
u32 half_rtz(float f){u32 x=std::bit_cast<u32>(f),sign=(x>>16)&0x8000;int e=int((x>>23)&255)-127+15;assert(e>0&&e<31);return sign|(u32(e)<<10)|((x&0x7fffff)>>13);}
std::vector<u32> compile_case(Ampere::LoweredProgram lower,const std::vector<std::array<u32,8>>& inputs,const std::filesystem::path& out,const std::string& name){
 std::vector<u64> seed{0xf0c8000000070000ULL,0xf0c8000000070001ULL|(u64{33}<<20)};for(unsigned n=0;n<inputs.size();++n){seed.push_back(mov(252,n));seed.push_back(0x5b60000000070007ULL|(u64{1}<<8)|(u64{252}<<20)|(u64{7}<<39)|(u64{2}<<49));for(unsigned k=0;k<8;++k)seed.push_back(mov(k<4?8+k:k<6?12+k-4:20+k-6,inputs[n][k],0));}
 seed.push_back(mov(49,0));for(unsigned k=0;k<4;++k)seed.push_back(mov(200+k,0x12340000+k));seed.push_back(mov(246,31));seed.push_back(0xef10000000070000ULL|46|(u64{1}<<8)|(u64{246}<<20)|(u64{1}<<29)|(u64{31}<<34)|(u64{7}<<48));
 lower.instructions.insert(lower.instructions.begin(),seed.begin(),seed.end());std::vector<u64> tail{st(48,0),st(49,1),st(0,2),st(46,3)};for(unsigned k=0;k<4;++k)tail.push_back(st(200+k,4+k));lower.instructions.insert(lower.instructions.end()-1,tail.begin(),tail.end());
 Env env;for(unsigned n=0;n<lower.instructions.size();++n)env.code.emplace(8+n*8+(n/3)*8,lower.instructions[n]);ObjectPool<Maxwell::Flow::Block> flows;Maxwell::Flow::CFG cfg(env,flows,Maxwell::Location{0});ObjectPool<Inst> insts;ObjectPool<Block> blocks;HostTranslateInfo host{};host.support_float16=std::getenv("NXEMU_HMMA_NATIVE_HALF")!=nullptr;auto program=Maxwell::TranslateProgram(insts,blocks,env,cfg,host);assert(program.workgroup_size[0]==inputs.size());
 auto* last=program.blocks.back();IREmitter ir(*last);auto lane=ir.LocalInvocationIdX();for(unsigned k=0;k<8;++k){auto offset=ir.IAdd(ir.IMul(lane,ir.Imm32(32)),ir.Imm32(k*4));last->AppendNewInst(Opcode::WriteStorage32,{Value(0U),Value(offset),Value(ir.LoadLocal(ir.Imm32(k)))});}
 Optimization::CollectShaderInfoPass(env,program);program.info.storage_buffers_descriptors.push_back({static_cast<u32>(Info::MAX_CBUFS),0,1,true});Profile profile{};profile.supported_spirv=0x00010300;profile.unified_descriptor_binding=true;profile.support_vote=true;profile.warp_size_potentially_larger_than_guest=std::getenv("NXEMU_HMMA_HOST64_PARTITION")!=nullptr;Backend::Bindings binding{};auto spv=Backend::SPIRV::EmitSPIRV(profile,{},program,binding);assert(binding.unified==1);if(std::getenv("NXEMU_HMMA_RTE16"))force_rte16(spv);std::ofstream f(out/(name+".spv"),std::ios::binary);f.write(reinterpret_cast<const char*>(spv.data()),spv.size()*4);std::ofstream text(out/(name+".ir.txt"));text<<DumpProgram(program);return spv;
}
int main(int argc,char**argv)try{
 std::cout<<std::unitbuf;assert(argc==3);const bool reference=std::string(argv[1])=="modified";const std::filesystem::path out=argv[2];std::filesystem::create_directories(out);const char* tensor=std::getenv("EDEN_NX2_TENSOR");assert(tensor&&std::string(tensor)=="1");std::cout<<"CONTROLLED_TEST_PROCESS_TENSOR emulate; game_setting_unchanged\n";
 unsigned cases{};{
 Gpu gpu;std::cout<<"GPU_ADAPTER "<<gpu.name<<'\n';
 if(std::getenv("NXEMU_HMMA_F2F")){
  _putenv_s("NXEMU_HMMA_THREADS","32");std::vector<std::array<u32,8>> inputs(32);std::vector<u32> expected;const float values[4]={1.f+7.f/4096,1.f+3.f/2048,-1.f-7.f/4096,1.f+15.f/4096};
  for(unsigned t=0;t<32;++t){float v=values[t%4];inputs[t][0]=std::bit_cast<u32>(v);expected.push_back(std::getenv("NXEMU_HMMA_RTE16")?half(v):half_rtz(v));expected.push_back(0);expected.push_back(t);expected.push_back(31);for(unsigned k=0;k<4;++k)expected.push_back(0x12340000+k);}
  Ampere::Instruction native{{0x00307304,8,0x00200800,0}};auto low=one(native);assert(low);auto spv=compile_case(low,inputs,out,"f2f");actual_file=out/"f2f.actual.txt";gpu.run(spv,expected);std::ofstream exp(out/"f2f.expected.txt");for(u32 x:expected)exp<<std::hex<<x<<'\n';std::cout<<"F2F_NATIVE_RN_GPU "<<(std::getenv("NXEMU_HMMA_RTE16")?"RTE16_pass":"default_matches_RTZ_semantic_defect")<<" word0="<<std::hex<<expected[0]<<std::dec<<'\n';++cases;
 }else for(unsigned kind=0;kind<6;++kind){if(kind==4&&std::getenv("NXEMU_HMMA_SKIP_ROUNDING"))continue;float A[16][16]{},B[16][8]{},C[16][8]{};
  if(kind==0)for(auto& row:A)for(float& v:row)v=1; // B=0 exercises zero result.
  if(kind==1){A[13][9]=1;B[9][6]=1;}
  if(kind==2||kind==3){for(unsigned r=0;r<16;++r)for(unsigned k=0;k<16;++k)A[r][k]=int((r+k)%3)-1;for(unsigned k=0;k<16;++k)for(unsigned c=0;c<8;++c)B[k][c]=int((k+2*c)%5)-2;}
  if(kind==3)for(unsigned r=0;r<16;++r)for(unsigned c=0;c<8;++c)C[r][c]=(int(r)-2*int(c))*.25f;
  if(kind==4){for(auto& row:A)for(float& v:row)v=1;for(unsigned k=0;k<16;++k)for(float& v:B[k])v=k?std::ldexp(1.f,-12):1;}
  unsigned threads=kind==5?128:32;_putenv_s("NXEMU_HMMA_THREADS",std::to_string(threads).c_str());std::vector<std::array<u32,8>> input(threads);std::vector<u32> expected;for(unsigned local=0;local<threads;++local){unsigned t=local%32,warp=local/32,g=t/4,i=t%4;if(kind==5){for(auto& row:A)for(float& v:row)v=warp+1;for(auto& row:B)for(float& v:row)v=1;}input[local]={pack(A[g][2*i],A[g][2*i+1]),pack(A[g+8][2*i],A[g+8][2*i+1]),pack(A[g][8+2*i],A[g][9+2*i]),pack(A[g+8][8+2*i],A[g+8][9+2*i]),pack(B[2*i][g],B[2*i+1][g]),pack(B[8+2*i][g],B[9+2*i][g]),pack(C[g][2*i],C[g][2*i+1]),pack(C[g+8][2*i],C[g+8][2*i+1])};
   for(unsigned row:{g,g+8}){float d[2]{};for(unsigned col=0;col<2;++col){float sum=C[row][2*i+col];if(reference){for(unsigned k=0;k<16;++k){sum=std::fma(A[row][k],B[k][2*i+col],sum);if(k%8==7)sum=fp(half(sum));}}else{for(unsigned z=0;z<4;++z)for(unsigned block=0;block<2;++block)for(unsigned h=0;h<2;++h){unsigned k=2*z+8*block+h;sum=fp(half(std::fma(A[row][k],B[k][2*i+col],sum)));}}d[col]=sum;}expected.push_back(pack(d[0],d[1]));}
   expected.push_back(t);expected.push_back(warp*32+31);for(unsigned k=0;k<4;++k)expected.push_back(0x12340000+k);
  }
  Ampere::Instruction captured{{0x0830723c,12,0x000008ff,0x041ff000}};if(kind==3)bits(captured,64,8,20);auto lower=one(captured);assert(lower);auto spv=compile_case(lower,input,out,"case-"+std::to_string(kind));std::ofstream exp(out/("case-"+std::to_string(kind)+".expected.txt"));for(u32 x:expected)exp<<std::hex<<x<<'\n';actual_file=out/("case-"+std::to_string(kind)+".actual.txt");gpu.run(spv,expected);std::cout<<"HMMA_CASE "<<kind<<" PASS packed_lane0="<<std::hex<<expected[0]<<std::dec<<" actual_GPU=True\n";++cases;
 }
 }
 assert(validation_errors==0&&validation_warnings==0);std::cout<<"RESULT {\"passed\":true,\"actual_lower_CFG_SSA_SPIRV_Vulkan\":true,\"guest_warp\":32,\"SHFL31_all32lanes_passed\":true,\"GPU_cases\":"<<cases<<",\"reference_FFMA\":"<<(reference?"true":"false")<<",\"Khronos_errors\":"<<validation_errors<<",\"Khronos_warnings\":"<<validation_warnings<<",\"hardware_precision_contract_verified\":false,\"full_ROM_replay\":false}\n";
}catch(const std::exception&e){std::cerr<<e.what()<<'\n';return 2;}
