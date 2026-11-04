// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Fixed-size CPU-only optional measurements. No shader/GPU semantics or shared locks.
#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <utility>
namespace Shader::TranslationTrace {
enum class Pass : unsigned {
    BuildASL, GenerateBlocks, PostOrder, StageSetup, RemoveUnreachable,
    LowerFp64, LowerFp16, LowerInt64, ConditionalBarrier, SsaRewrite,
    ConstantPropagation, Position, GlobalMemory, Texture, Rescaling,
    DeadCode, Verification, CollectInfo, Layer, Vendor, Interpolation, NVN,
    Count
};
constexpr std::size_t Count=static_cast<std::size_t>(Pass::Count);
constexpr const char* Names[Count]={"BuildASL","GenerateBlocks","PostOrder","StageSetup","RemoveUnreachable","LowerFp64","LowerFp16","LowerInt64","ConditionalBarrier","SsaRewrite","ConstantPropagation","Position","GlobalMemory","Texture","Rescaling","DeadCode","Verification","CollectInfo","Layer","Vendor","Interpolation","NVN"};
struct Counter {std::uint64_t wall_ns{},calls{},completed{},exceptions{};};
enum class Subpass : unsigned { BuildGoto,BuildTranslate,SSAVisit,SSAFinalize,Count };
constexpr std::size_t SubCount=static_cast<std::size_t>(Subpass::Count);
constexpr const char* SubNames[SubCount]={"BuildGoto","BuildTranslate","SSAVisit","SSAFinalize"};
enum class Work : unsigned {
    SSABlocks,SSAVisitInstructions,SSAFinalPhis,SSAOpaquePhis,
    SSATrivialCandidates,SSATrivialRemoved,SSAPrefixScans,SSAPrefixChecks,SSAPrefixMaxChecks,
    SSATypeCalls,SSATypePops,SSATypeEnqueues,SSATypeMaxQueue,
    BuildGotos,BuildOutwardMoves,BuildInwardMoves,BuildLifts,
    SSAReadCalls,SSARootHits,SSARootMisses,SSAFinalInstructions,SSAFinalPhiArgs,SSAFinalPhiMaxArgs,Count
};
constexpr std::size_t WorkCount=static_cast<std::size_t>(Work::Count);
constexpr const char* WorkNames[WorkCount]={"ssa_blocks","ssa_visit_instructions","ssa_final_phis","ssa_opaque_phis","ssa_trivial_candidates","ssa_trivial_removed","ssa_prefix_scans","ssa_prefix_checks","ssa_prefix_max_checks","ssa_type_calls","ssa_type_pops","ssa_type_enqueues","ssa_type_max_queue","build_gotos","build_outward_moves","build_inward_moves","build_lifts","ssa_read_calls","ssa_root_hits","ssa_root_misses","ssa_final_instructions","ssa_final_phi_args","ssa_final_phi_max_args"};
struct Record {std::array<Counter,Count> passes{};std::array<Counter,SubCount> subpasses{};std::array<std::uint64_t,WorkCount> work{};bool work_overflow{};};
inline thread_local Record* current{};
inline void Add(Work field,std::uint64_t value=1) noexcept {
    if(!current)return;auto& count=current->work[static_cast<std::size_t>(field)];
    if(UINT64_MAX-count<value){count=UINT64_MAX;current->work_overflow=true;}else count+=value;
}
inline void Max(Work field,std::uint64_t value) noexcept {
    if(current){auto& count=current->work[static_cast<std::size_t>(field)];if(value>count)count=value;}
}
class Capture {
    Record* previous;
public:
    explicit Capture(Record* record) noexcept:previous(current){current=record;}
    ~Capture(){current=previous;}
    Capture(const Capture&)=delete;Capture& operator=(const Capture&)=delete;
};
class Timer {
    Counter* counter{};
    std::chrono::steady_clock::time_point started{};
    int exceptions{};
protected:
    explicit Timer(Counter* row) noexcept:counter(row) {
        if(counter){exceptions=std::uncaught_exceptions();started=std::chrono::steady_clock::now();++counter->calls;}
    }
public:
    explicit Timer(Pass pass) noexcept:Timer(current?&current->passes[static_cast<std::size_t>(pass)]:nullptr) {}

    ~Timer() noexcept {
        if(counter){counter->wall_ns+=std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now()-started).count();
            if(std::uncaught_exceptions()>exceptions)++counter->exceptions;else ++counter->completed;}
    }
    Timer(const Timer&)=delete;Timer& operator=(const Timer&)=delete;
};
class SubTimer:public Timer {
public:explicit SubTimer(Subpass pass) noexcept:Timer(current?&current->subpasses[static_cast<std::size_t>(pass)]:nullptr){}
};
template<class F> decltype(auto) Measure(Pass pass,F&& fn){Timer timer(pass);return std::forward<F>(fn)();}
} // namespace Shader::TranslationTrace
