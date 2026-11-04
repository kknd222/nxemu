// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Opt-in CPU-side exact SPIR-V archive; no GPU queries, dispatch or wait changes.
#include <chrono>
#include <stdexcept>
#include <atomic>
#include <array>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <string_view>

namespace VideoCommon::PipelineFixture {
constexpr std::size_t MaxBlobBytes=4*1024*1024, MaxTotalBytes=16*1024*1024, MaxCases=16;
std::string Environment(std::string_view name);
inline bool Enabled(){return Environment("NXEMU_PIPELINE_FIXTURES")=="1" && !Environment("NXEMU_NATIVE_SHADER_DIAG_DIR").empty();}
inline std::string Hex(std::uint64_t v){std::ostringstream s;s<<std::hex<<std::setw(16)<<std::setfill('0')<<v;return s.str();}
inline std::string Q(std::string_view input){std::ostringstream s;s<<'"';for(unsigned char c:input){if(c=='"'||c=='\\')s<<'\\'<<char(c);else if(c<32)s<<"\\u00"<<std::hex<<std::setw(2)<<std::setfill('0')<<unsigned(c)<<std::dec;else s<<char(c);}s<<'"';return s.str();}
inline std::uint64_t ContentId(std::span<const std::uint32_t> code){std::uint64_t h=14695981039346656037ull;for(auto w:code)for(unsigned i=0;i<4;++i){h^=(w>>(8*i))&255;h*=1099511628211ull;}return h;}
// Fixed, module-load lifetime counters. No per-game reset races or event allocation.
enum class Outcome : std::size_t {
 Attempted, MissingDirectory, Filtered, InvalidCode, InvalidSession, LockContention,
 CaseBudget, ByteBudget, Reserved, DirectoryFailure, BlobFailure, PendingFailure,
 CaptureException, BlobWritten, PendingWritten, ReturnedHandle, RecipeCalls,
 RecipeInvalid, RecipeWriteFailure, RecipeBuildException, RecipeReady, ReleasedWithoutRecipe, RecipeException,
 Count
};
inline constexpr std::array<const char*,static_cast<std::size_t>(Outcome::Count)> OutcomeNames{
 "attempted","missing_directory","filtered","invalid_code","invalid_session","lock_contention",
 "case_budget","byte_budget","reserved","directory_failure","blob_failure","pending_failure",
 "capture_exception","blob_written","pending_written","returned_handle","recipe_calls",
 "recipe_invalid","recipe_write_failure","recipe_build_exception","recipe_ready","released_without_recipe","recipe_exception"};
inline std::array<std::atomic<std::uint64_t>,static_cast<std::size_t>(Outcome::Count)> outcomes{};
inline void Note(Outcome reason) noexcept {outcomes[static_cast<std::size_t>(reason)].fetch_add(1,std::memory_order_relaxed);}
inline std::atomic<std::uint64_t> capture_active{},recipe_active{};
struct Activity {std::atomic<std::uint64_t>& value;explicit Activity(std::atomic<std::uint64_t>& v):value(v){value.fetch_add(1,std::memory_order_relaxed);}~Activity(){value.fetch_sub(1,std::memory_order_relaxed);}};
struct Handle {
 std::filesystem::path root;std::string id,run,generation,content_id;std::uint64_t key{},unique_hash{},words{},archive_ns{};
 bool tracked{};std::atomic<bool> recipe_ready{};
 ~Handle(){if(tracked&&!recipe_ready.load(std::memory_order_relaxed))Note(Outcome::ReleasedWithoutRecipe);}
};
inline std::mutex mutex;
inline std::string current_run,current_generation;
inline std::uint64_t count{};
// One run's reservations must remain alive while older captures finish IO.
struct RunBudget {std::atomic<std::uint64_t> charged_bytes{};};
inline std::shared_ptr<RunBudget> current_budget;
struct BlobCharge {
 std::shared_ptr<RunBudget> budget;std::uint64_t bytes{};bool committed{};
 BlobCharge(std::shared_ptr<RunBudget> b,std::uint64_t n) noexcept:budget(std::move(b)),bytes(n){}
 BlobCharge(const BlobCharge&)=delete;BlobCharge& operator=(const BlobCharge&)=delete;
 BlobCharge(BlobCharge&&)=delete;BlobCharge& operator=(BlobCharge&&)=delete;
 ~BlobCharge(){if(budget&&!committed)budget->charged_bytes.fetch_sub(bytes,std::memory_order_relaxed);}
};
inline std::atomic<std::uint64_t> reservation_lock_ns{},reservation_lock_max_ns{};
inline std::atomic<std::uint64_t> drops{},errors{};
// Publication is provided by VideoManager, no GPU calls or waits.
void Publish() noexcept;
inline std::string CountersJson(){
 std::ostringstream s;s<<"{\"schema\":1,\"counter_scope\":\"video_module_load\",\"snapshot_transactional\":false,\"disabled_calls_counted\":false,\"coverage\":\"compute SPIRV capture hook only; not graphics, cache hits or translation failures before Capture\",\"configuration_enabled\":"<<(Environment("NXEMU_PIPELINE_FIXTURES")=="1"?"true":"false")<<",\"filter\":"<<Q(Environment("NXEMU_PIPELINE_FIXTURE_KEY"))<<",\"capture_active\":"<<capture_active.load(std::memory_order_relaxed)<<",\"recipe_active\":"<<recipe_active.load(std::memory_order_relaxed)<<",\"counters\":{";
 for(std::size_t i=0;i<OutcomeNames.size();++i){if(i)s<<',';s<<Q(OutcomeNames[i])<<':'<<outcomes[i].load(std::memory_order_relaxed);}s<<"},\"legacy_drops\":"<<drops.load()<<",\"legacy_errors\":"<<errors.load();
 // Do not take the archive mutex: sampling must not cause new Capture rejections.
 s<<",\"budget_totals_not_sampled\":true,\"budget_scope\":\"run_id; generation reload does not reset\"";
 s<<",\"reservation_only_lock\":true,\"admitted_reservation_body_ns\":"<<reservation_lock_ns.load(std::memory_order_relaxed)<<",\"admitted_reservation_body_max_best_effort_ns\":"<<reservation_lock_max_ns.load(std::memory_order_relaxed)<<",\"reservation_sample_excludes_accounting_unlock_rejections\":true,\"byte_budget_includes_inflight\":true";
 s<<",\"limits\":{\"cases\":16,\"blob_bytes\":4194304,\"total_bytes\":16777216},\"recipe_ready_is_not_driver_success\":true}";return s.str();
}
inline std::uint64_t Now(){return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();}
inline void Write(const std::filesystem::path& path,const char* bytes,std::size_t size){
 auto tmp=path;tmp+=".tmp";std::ofstream f(tmp,std::ios::binary|std::ios::trunc);f.write(bytes,static_cast<std::streamsize>(size));f.close();if(!f)throw std::runtime_error("pipeline fixture write failed");
 std::error_code ec;std::filesystem::rename(tmp,path,ec);
 if(ec){std::filesystem::remove(tmp);throw std::runtime_error("pipeline fixture publish failed");}
}
inline std::string Base(const Handle& h){return "\"schema\":1,\"run_id\":"+Q(h.run)+",\"module_generation\":"+Q(h.generation)+",\"id\":"+Q(h.id)+",\"pipeline_hash\":"+Q(std::to_string(h.key))+",\"shader_unique_hash\":"+Q(std::to_string(h.unique_hash))+",\"spirv_file\":"+Q(h.id+".spv")+",\"spirv_words\":"+std::to_string(h.words)+",\"spirv_bytes\":"+std::to_string(h.words*4)+",\"content_id\":"+Q(h.content_id)+",\"hash_algorithm\":\"fnv1a64-nonauthenticating\",\"byte_order\":\"little\",\"archive_ns\":"+std::to_string(h.archive_ns);}
inline std::shared_ptr<Handle> Capture(std::uint64_t key,std::uint64_t unique_hash,std::span<const std::uint32_t> code) noexcept {
 Outcome failure=Outcome::CaptureException;
 try {
  if(Environment("NXEMU_PIPELINE_FIXTURES")!="1")return {};
  Note(Outcome::Attempted);Activity activity(capture_active);
  if(Environment("NXEMU_NATIVE_SHADER_DIAG_DIR").empty()){Note(Outcome::MissingDirectory);return {};}
  auto filter=Environment("NXEMU_PIPELINE_FIXTURE_KEY");if(!filter.empty()&&filter!=Hex(key)){Note(Outcome::Filtered);return {};}
  const auto started=Now();if(std::endian::native!=std::endian::little || code.size()<5 || code.size()>MaxBlobBytes/4 || code[0]!=0x07230203){++errors;Note(Outcome::InvalidCode);return {};}
  auto run=Environment("NXEMU_DEBUG_RUN_ID"),gen=Environment("NXEMU_DEBUG_GENERATION");if(run.empty()||gen.empty()||gen.size()>20||gen.find_first_not_of("0123456789")!=std::string::npos){++errors;Note(Outcome::InvalidSession);return {};}
  // Hashing, allocation, formatting and all disk IO stay OUTSIDE admission lock.
  auto h=std::make_shared<Handle>();h->run=run;h->generation=gen;h->key=key;h->unique_hash=unique_hash;h->words=code.size();h->content_id=Hex(ContentId(code));h->id="pipeline-"+Hex(key)+"-"+h->content_id+"-g"+gen+"-";
  h->root=std::filesystem::u8path(Environment("NXEMU_NATIVE_SHADER_DIAG_DIR"))/"pipeline-fixtures";
  auto candidate=std::make_shared<RunBudget>();std::shared_ptr<RunBudget> budget;std::uint64_t ordinal{};
  {
   std::unique_lock lock(mutex,std::try_to_lock);if(!lock.owns_lock()){++drops;Note(Outcome::LockContention);return {};}
   const auto lock_started=Now();
   if(run!=current_run){current_run.swap(run);count=0;current_budget.swap(candidate);}
   current_generation.swap(gen); // reload does not reset the run-wide disk budget
   if(count>=MaxCases){++drops;Note(Outcome::CaseBudget);return {};}
   if(current_budget->charged_bytes.load(std::memory_order_relaxed)+code.size_bytes()>MaxTotalBytes){++drops;Note(Outcome::ByteBudget);return {};}
   budget=current_budget;budget->charged_bytes.fetch_add(code.size_bytes(),std::memory_order_relaxed);ordinal=++count;Note(Outcome::Reserved);
   const auto elapsed=Now()-lock_started;reservation_lock_ns.fetch_add(elapsed,std::memory_order_relaxed);auto maximum=reservation_lock_max_ns.load(std::memory_order_relaxed);
   // Best-effort max, no retry loop or extra blocking on the diagnostic path.
   if(elapsed>maximum)reservation_lock_max_ns.compare_exchange_strong(maximum,elapsed,std::memory_order_relaxed);
  }
  BlobCharge charge{std::move(budget),code.size_bytes()};h->id+=std::to_string(ordinal);
  failure=Outcome::DirectoryFailure;
  std::filesystem::create_directories(h->root);
  failure=Outcome::BlobFailure;Write(h->root/(h->id+".spv"),reinterpret_cast<const char*>(code.data()),code.size_bytes());charge.committed=true;Note(Outcome::BlobWritten);h->archive_ns=Now()-started;
  failure=Outcome::CaptureException;const auto pending="{"+Base(*h)+",\"status\":\"awaiting_recipe\",\"replay_ready\":false}";failure=Outcome::PendingFailure;Write(h->root/(h->id+".pending.json"),pending.data(),pending.size());Note(Outcome::PendingWritten);h->tracked=true;Note(Outcome::ReturnedHandle);return h;
 }catch(...){++errors;Note(failure);return {};}
}
inline void Recipe(const std::shared_ptr<Handle>& h,const std::string& recipe) noexcept {
 if(!h)return;
 Note(Outcome::RecipeCalls);Activity activity(recipe_active);
 Outcome failure=Outcome::RecipeException;
 try {
  if(recipe.empty()||recipe.size()>192*1024){++errors;Note(Outcome::RecipeInvalid);return;}
  const auto text="{"+Base(*h)+",\"status\":\"ready\",\"entry_point\":\"main\",\"recipe\":"+recipe+",\"limits\":{\"cases\":16,\"blob_bytes\":4194304,\"total_bytes\":16777216},\"capture_drops\":"+std::to_string(drops.load())+",\"capture_errors\":"+std::to_string(errors.load())+",\"scope\":\"create-only inputs; not dispatch resources or GPU numeric replay\"}";
  failure=Outcome::RecipeWriteFailure;Write(h->root/(h->id+".json"),text.data(),text.size());
  if(!h->recipe_ready.exchange(true,std::memory_order_relaxed))Note(Outcome::RecipeReady);
 }catch(...){++errors;Note(failure);}
}
} // namespace VideoCommon::PipelineFixture
