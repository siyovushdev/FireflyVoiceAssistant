#pragma once
#include "Json.h"
#include "firefly/config/ConfigStore.h"
#include "firefly/admin/AdminCopilot.h"
#include "firefly/admin/AdminDraft.h"
#include "firefly/admin/AdminRequestPolicy.h"
#include "firefly/admin/RuntimeGeneration.h"
#include "firefly/config/ConfigLoader.h"
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>
#include <atomic>
#include <sys/random.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <fstream>
#include <charconv>
#include <cstdlib>

namespace firefly::admin {
// One server-owned draft for conversation and manual edits. Never executes actions.
class SetupSession final {
public:
    using Call = std::function<Json(const Json&)>;
    SetupSession(config::ConfigStore& store, Call call) : store_(store), call_(std::move(call)), path_(store.directory()+"/setup-draft.json") {
        disconnectSeconds_=limit("FIREFLY_SETUP_DISCONNECT_SECONDS",120,30,1800);
        inactivitySeconds_=limit("FIREFLY_SETUP_INACTIVITY_SECONDS",1800,300,7200);
        const int fd=::open(path_.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW);
        if(fd>=0) {
            struct stat info{};
            std::string bytes;
            if(::fstat(fd,&info)==0 && S_ISREG(info.st_mode) && info.st_size>=0 && info.st_size<=1024*1024) {
                bytes.resize(static_cast<std::size_t>(info.st_size));
                std::size_t offset=0;
                while(offset<bytes.size()) {
                    const auto count=::read(fd,bytes.data()+offset,bytes.size()-offset);
                    if(count<0 && errno==EINTR)continue;
                    if(count<=0){bytes.clear();break;}
                    offset+=static_cast<std::size_t>(count);
                }
            }
            ::close(fd);
            if(!bytes.empty())try {
                data_=Json::parse(bytes);data_["active"]=false;
                // A crash can leave a valid but partial configuration between
                // dialogue steps. Recover the message checkpoint, never accept
                // that prefix as a completed request. Also fail closed for old
                // drafts persisted in planning before checkpoints existed.
                const auto status=data_.value("status",std::string());
                if(data_.contains("pending_message") || status=="planning" || status=="planning_failed")
                    data_["message_incomplete"]=true;
                if(data_.value("message_incomplete",false)) {
                    if(data_.contains("pending_message")) {
                        data_["configuration"]=data_["pending_message"].at("configuration");
                        data_["focus"]=data_["pending_message"].at("focus");
                    }
                    data_["questions"]=Json::array({"Обработка просьбы не завершена. Повторите её полностью или начните новую настройку."});
                }
                if(data_.value("status",std::string())!="applied")data_["status"]="resume_required";
                owner_.clear();
            } catch(...) {data_=Json::object();}
        }
        std::atomic_store(&published_,std::make_shared<const Json>(view()));
    }
    ~SetupSession() { if(entryWorker_.joinable())entryWorker_.join(); }
    bool active() const { return active_; }
    Json handle(std::string_view route, const Json& request) {
        if(route=="status") {
            const auto snapshot=std::atomic_load(&published_);
            return snapshot?*snapshot:Json{{"ok",true},{"active",false},{"status","idle"}};
        }
        if(route!="start" && route!="resume") {
            const auto owner=std::atomic_load(&publishedOwner_);
            if(!active_ || !owner || request.value("session_id",std::string())!=*owner)
                throw std::runtime_error("setup_session_conflict");
            if(route=="cancel")std::atomic_store(&cancelOwner_,owner);
        }
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
        if (!lock.owns_lock()) {
            if(route=="cancel")return {{"ok",true},{"active",true},{"status","cancel_pending"}};
            throw std::runtime_error("setup_busy");
        }
        if (route == "status") return view();
        if (route == "start" || route == "resume") {
            if (active_) throw std::runtime_error("setup_busy: другая сессия уже настраивает ассистента");
            const bool asyncEntry=request.is_object() && request.value("async_enter",false);
            const auto base = store_.current();
            if (route == "resume") {
                if(data_.value("status",std::string())=="applied")throw std::runtime_error("no_pending_setup_draft");
                if (data_.empty() || data_.at("base_hash") != base->contentHash() || data_.at("base_revision") != base->configuration().revision) throw std::runtime_error("stale_setup_base");
            } else data_ = {{"base_hash",base->contentHash()},{"base_revision",base->configuration().revision},
                {"configuration",Json::parse(base->canonicalJson())},{"messages",Json::array()},
                {"focus",Json::array()},{"version",0},{"last_request",""},{"questions",Json::array()},{"trace",Json::array()}};
            owner_ = token(); std::atomic_store(&publishedOwner_,std::make_shared<const std::string>(owner_));std::atomic_store(&cancelOwner_,std::shared_ptr<const std::string>()); active_ = true; data_["status"]="entering";
            heartbeat_ = activity_ = Clock::now(); persist();
            if(asyncEntry) {
                // Publish ownership immediately. The joined server worker owns
                // entry even if the initiating HTTP connection is closed/reloaded.
                if(entryWorker_.joinable())entryWorker_.join();
                entryRunning_=true;
                try { entryWorker_=std::thread([this]{ enterInBackground(); }); }
                catch (...) { entryRunning_=false;active_=false;data_["status"]="entry_failed";persist();throw; }
                auto result=view();result["session_id"]=owner_;return result;
            }
            try { invoke({{"method","setup_enter"}}); data_["status"]=data_.value("message_incomplete",false)?"planning_failed":"editing"; }
            catch (...) { active_=false; data_["status"]="entry_failed"; persist(); throw; }
            heartbeat_=Clock::now(); persist(); auto result=view(); result["session_id"]=owner_; return result;
        }
        if (!active_ || request.value("session_id",std::string()) != owner_)
            throw std::runtime_error("setup_session_conflict");
        heartbeat_ = Clock::now();
        if (route == "heartbeat") return view();
        if(entryRunning_) {
            if(route=="cancel") { data_["status"]="cancel_pending";persist();return view(); }
            throw std::runtime_error("setup_entering");
        }
        if (route == "authorize") return view();
        activity_ = Clock::now();
        if (route == "cancel") { restore(); return view(); }
        if (route == "message" && request.value("request_id",std::string()) == data_.value("last_request",std::string()) && !request.value("request_id",std::string()).empty()) return view();
        if (request.value("version",-1) != data_.at("version").get<int>()) throw std::runtime_error("stale_draft_version");
        checkBase();
        if (route == "draft") {
            if(data_.value("message_incomplete",false)) throw std::runtime_error("incomplete_setup_plan: повторите незавершённую просьбу полностью");
            auto catalog=config::ConfigLoader::fromJson(request.at("configuration").dump());
            RuntimeGeneration admission(catalog);
            data_["configuration"]=Json::parse(catalog->canonicalJson());
            data_["questions"]=Json::array(); data_["unresolved_text"]="";
            // A manual checkpoint restore must not retain references to rejected creations.
            Json focus=Json::array();
            for(const auto& id:data_["focus"]) {
                bool present=false;
                for(const auto* category:{"rooms","devices"})for(const auto& entity:data_["configuration"].at(category))if(entity.at("id")==id)present=true;
                if(present && std::find(focus.begin(),focus.end(),id)==focus.end())focus.push_back(id);
            }
            data_["focus"]=focus; data_["status"]="editing"; advance();
        } else if (route == "message") {
            const auto text=request.at("text").get<std::string>(); validateMessage(text);
            if(data_.value("message_incomplete",false) &&
               (!data_.contains("pending_message") || data_["pending_message"].at("text")!=text))
                throw std::runtime_error("incomplete_setup_plan: повторите незавершённую просьбу полностью");
            const auto id=request.at("request_id").get<std::string>();
            if (id.empty() || id.size()>80) throw std::invalid_argument("invalid_request_id");
            if (id==data_.at("last_request")) return view();
            if (data_["messages"].size()>=32) throw std::runtime_error("setup_message_limit");
            const auto unresolved=data_.value("unresolved_text",std::string());
            const auto plannedText=unresolved.empty()?text:unresolved+"\nОтвет на уточнение: "+text;
            data_["messages"].push_back(text); data_["last_request"]=id;
            data_["message_incomplete"]=true;
            data_["pending_message"]={{"text",text},{"configuration",data_["configuration"]},{"focus",data_["focus"]}};
            data_["status"]="planning"; persist();
            const auto original=data_["configuration"];
            const auto originalFocus=data_["focus"];
            try {
                // V2 proposals keep a short request and its final capability restriction
                // in one validation unit. Legacy clients retain sentence stepping.
                const bool atomicMessage=request.value("atomic_message",false);
                if(atomicMessage && plannedText.size()>1800)throw std::runtime_error("admin_step_budget: сократите просьбу до одного небольшого предложения");
                auto steps=unresolved.empty() && !atomicMessage?split(plannedText):std::vector<std::string>{plannedText};
                if(!unresolved.empty() && plannedText.size()>1800)throw std::runtime_error("admin_step_budget: уточните незавершённую просьбу меньшими частями");
                if (steps.size()>16) throw std::runtime_error("setup_step_limit: максимум 16 связанных шагов");
                data_["questions"]=Json::array();
                const auto begin=Clock::now();
                int languageCalls=0,stepNumber=0;std::string contextLocation;
                data_["steps"]=Json::array();
                for (const auto& step:steps) {
                    data_["progress"]={{"step",++stepNumber},{"total_steps",steps.size()},{"phase","planning"}};
                    data_["steps"].push_back({{"message",data_["messages"].size()},{"source",step},{"status","pending"}});
                    persist();
                    if(cancellationRequested())throw std::runtime_error("setup_cancelled");
                    if (Clock::now()-begin>std::chrono::minutes(10)) throw std::runtime_error("setup_planning_timeout");
                    const auto reply=invoke({{"method","dialogue"},{"configuration",data_["configuration"]},{"text",step},{"focus",data_["focus"]},{"context_location",contextLocation}});

                    const auto proposal=reply.at("suggestion");
                    data_["steps"].back()["status"]=proposal.value("status",std::string("proposed"));
                    data_["steps"].back()["summary"]=proposal.value("summary",std::string());
                    if (!proposal.contains("configuration") || proposal.value("rejected",false)) {
                        data_["questions"].push_back(proposal.value("summary",std::string("Уточните просьбу")));
                        data_["status"]="clarification_required"; data_["unresolved_text"]=plannedText; break;
                    }
                    const auto accepted=Json::parse(acceptCopilotProposal(data_["configuration"].dump(),proposal.dump()));
                    const auto previous=data_["configuration"];
                    data_["configuration"]=accepted.at("configuration");
                    for (const auto* category:{"rooms","devices"}) for (const auto& entity:data_["configuration"].at(category)) {
                        bool existed=false;
                        for (const auto& old:previous.at(category)) if (old.at("id")==entity.at("id")) existed=true;
                        if (!existed) {data_["focus"].push_back(entity.at("id"));if(std::string(category)=="rooms")contextLocation=entity.at("id").get<std::string>();}
                    }
                    if(data_["focus"].size()>12) throw std::runtime_error("setup_focus_limit: уточните объекты");
                    // New language remains editable draft data; never overwrite an
                    // existing entity's manually reviewed forms.
                    for (const auto* category:{"rooms","devices","actions"}) {
                        const auto snapshot=data_["configuration"];
                        for (const auto& entity:snapshot.at(category)) {
                            bool existed=false;
                            for(const auto& old:previous.at(category)) if(old.at("id")==entity.at("id")) existed=true;
                            if(existed)continue;
                            if(cancellationRequested())throw std::runtime_error("setup_cancelled");
                            if(Clock::now()-begin>std::chrono::minutes(10)) throw std::runtime_error("setup_planning_timeout");
                            if(++languageCalls>10) { data_["language_warning"]="Лимит предложений языка: остальные поля доступны вручную."; break; }
                            try {
                                data_["progress"]["phase"]="language_profile";persist();
                                auto language=invoke({{"method","language_profile"},{"configuration",data_["configuration"]},
                                    {"entity",std::string(category)=="rooms"?"location":std::string(category)=="devices"?"device":"action"},{"id",entity.at("id")},
                                    {"initial_location",std::string(category)=="rooms"}});

                                auto next=data_["configuration"];
                                for(auto& target:next.at(category)) if(target.at("id")==entity.at("id")) {
                                    const auto& profile=language.at("suggestion");
                                    if(std::string(category)=="rooms") {
                                        target["speech"]["mentions"]=profile.at("mentions");target["speech"]["locations"]=profile.at("locations");
                                        target["metadata"]["aliases"]=profile.at("aliases");
                                    } else if(std::string(category)=="devices") target["spoken_aliases"]=profile.at("aliases");
                                    else target["speech_aliases"]=profile.at("aliases");
                                }
                                RuntimeGeneration checked(config::ConfigLoader::fromJson(next.dump()));
                                data_["configuration"]=next;
                            } catch(const std::exception&) { data_["language_warning"]="Часть языковых предложений отклонена проверкой; сохранены исходные формы."; }
                        }
                    }
                    data_["summary"]=proposal.at("summary");
                }
                if (data_["questions"].empty()) { data_["status"]="editing"; data_["unresolved_text"]=""; }
                else { data_["configuration"]=original; data_["focus"]=originalFocus; } // No partial message presented as complete.
                data_["message_incomplete"]=false; data_.erase("pending_message");
                advance();
            } catch (...) { data_["configuration"]=original; data_["focus"]=originalFocus; data_["status"]="planning_failed"; advance(); throw; }
        } else if (route == "apply") {
            if (data_.value("message_incomplete",false) || data_.at("status")!="editing" || !data_["questions"].empty()) throw std::runtime_error("incomplete_setup_plan");
            for(const auto* category:{"rooms","devices"})for(auto& entity:data_["configuration"].at(category))
                if(entity.contains("metadata") && entity["metadata"].value("origin",std::string())=="local_ai")entity["metadata"]["confirmed"]=true;
            const auto candidate=config::ConfigLoader::fromJson(data_.at("configuration").dump());
            RuntimeGeneration admission(candidate);
            const auto old=store_.current();
            data_["status"]="restoring_for_admission"; persist();
            config::Catalog saved;
            try {
                invoke({{"method","setup_restore"}});
                invoke({{"method","preflight"},{"configuration",data_["configuration"]}});
                const auto next=store_.save(candidate,{old->configuration().revision,old->contentHash()}); saved=next;
                invoke({{"method","apply"},{"configuration",Json::parse(next->canonicalJson())}});
                store_.recordLastKnownGood(next);
                invoke({{"method","setup_finish"}});
                data_["configuration"]=Json::parse(next->canonicalJson());
                data_["base_hash"]=next->contentHash();data_["base_revision"]=next->configuration().revision;
                active_=false; data_["status"]="applied";
            } catch (...) {
                const auto failure=std::current_exception();
                try {
                    if (saved) {
                        const auto rollback=store_.compensateSave(
                            {saved->configuration().revision,saved->contentHash()},
                            {old->configuration().revision,old->contentHash()});
                        invoke({{"method","apply"},{"configuration",Json::parse(rollback->canonicalJson())}});
                        store_.recordLastKnownGood(rollback);
                    }
                    invoke({{"method","setup_finish"}}); active_=false;
                    data_["status"]="apply_failed_restored";
                } catch (...) { data_["status"]="restart_required"; }
                persist(); std::rethrow_exception(failure);
            }
            persist();
        } else throw std::invalid_argument("unknown_setup_route");
        return view();
    }
    void tick(std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        std::unique_lock<std::mutex> lock(mutex_,std::try_to_lock);
        if (!lock.owns_lock() || !active_ || data_.value("status",std::string())=="restart_required") return;
        if (cancellationRequested() || now-heartbeat_>std::chrono::seconds(disconnectSeconds_) || now-activity_>std::chrono::seconds(inactivitySeconds_)) {
            if(entryRunning_) {
                std::atomic_store(&cancelOwner_,std::atomic_load(&publishedOwner_));
                data_["status"]="cancel_pending";persist();
            } else try { restore(); } catch (...) { data_["status"]="restart_required"; persist(); }
        }
    }
private:
    int disconnectSeconds_=120, inactivitySeconds_=1800;
    using Clock=std::chrono::steady_clock;
    config::ConfigStore& store_; Call call_; std::string path_,owner_;
    mutable std::mutex mutex_; std::atomic<bool> active_{false};
    std::thread entryWorker_;
    bool entryRunning_=false; // Protected by mutex_; heavy runtime entry holds no session lock.
    std::shared_ptr<const Json> published_;
    std::shared_ptr<const std::string> publishedOwner_;
    std::shared_ptr<const std::string> cancelOwner_;
    bool cancellationRequested() const {
        const auto requested=std::atomic_load(&cancelOwner_);
        // A stale client's racing cancellation cannot cancel a newer session.
        return requested && requested==std::atomic_load(&publishedOwner_);
    }
    Json data_=Json::object(); Clock::time_point heartbeat_=Clock::now(),activity_=Clock::now();
    void enterInBackground() {
        // call_ returns only runtime data; unlike invoke it does not mutate the
        // draft while heartbeats/cancellation access it under mutex_.
        bool entered=false;
        try { entered=call_({{"method","setup_enter"}}).value("ok",false); }
        catch (...) { /* Runtime entry already owns its failure restoration. */ }
        std::lock_guard<std::mutex> lock(mutex_);
        entryRunning_=false;
        try {
            if(!entered) { active_=false;data_["status"]="entry_failed";persist();return; }
            const auto now=Clock::now();
            if(cancellationRequested() || now-heartbeat_>std::chrono::seconds(disconnectSeconds_) ||
                now-activity_>std::chrono::seconds(inactivitySeconds_))restore();
            else { data_["status"]=data_.value("message_incomplete",false)?"planning_failed":"editing";persist(); }
        } catch (...) { data_["status"]="restart_required";try { persist(); } catch (...) {} }
    }
    static int limit(const char* key,int fallback,int low,int high) {
        const char* value=std::getenv(key);if(!value)return fallback;
        const std::string text(value);int result=0;
        auto parsed=std::from_chars(text.data(),text.data()+text.size(),result);
        if(parsed.ec!=std::errc{}||parsed.ptr!=text.data()+text.size()||result<low||result>high)
            throw std::invalid_argument(std::string("Invalid setup limit: ")+key);
        return result;
    }
    Json invoke(const Json& request) {
        auto result=call_(request);
        for(const auto& trace:result.value("trace",Json::array())) {
            Json record=Json::object();
            for(const auto* key:{"attempt","group","stage","metrics","error_code"})
                if(trace.contains(key))record[key]=trace.at(key);
            data_["trace"].push_back(record);
        }
        if(result.contains("trace"))persist();
        if(!result.value("ok",false)) throw std::runtime_error(result.value("errors",Json::array()).dump());
        return result;
    }
    void checkBase() {
        const auto current=store_.current();
        if(current->contentHash()!=data_.at("base_hash") || current->configuration().revision!=data_.at("base_revision"))
            throw std::runtime_error("stale_setup_base");
    }
    static std::string token() {
        unsigned char bytes[24];
        if(::getrandom(bytes,sizeof(bytes),0)!=sizeof(bytes)) throw std::runtime_error("session_entropy_unavailable");
        std::string out;const char* hex="0123456789abcdef";
        for(auto byte:bytes){out+=hex[byte>>4];out+=hex[byte&15];}return out;
    }
    Json view() const {
        Json result={{"ok",true},{"active",active_.load()},{"status",data_.value("status",std::string("idle"))}};
        for(const auto* key:{"configuration","version","questions","summary","language_warning","progress","message_incomplete"}) if(data_.contains(key)) result[key]=data_.at(key);
        result["disconnect_seconds"]=disconnectSeconds_; result["inactivity_seconds"]=inactivitySeconds_;
        result["connection_note"]="Действия настроены программно. Без Binding физическое подключение не задано.";
        return result;
    }
    void advance() { heartbeat_=activity_=Clock::now(); data_["version"]=data_.at("version").get<int>()+1; persist(); }
    void restore() {
        data_["status"]="restoring";persist();
        try { invoke({{"method","setup_restore"}});invoke({{"method","setup_finish"}}); }
        catch (...) { data_["status"]="restart_required";persist();throw; }
        active_=false;data_["status"]="cancelled_draft_saved";persist();
    }
    static std::vector<std::string> split(const std::string& text) {
        // Ordered sentence steps share the same evolving draft and references.
        // Nothing is applied until ALL steps, including final exclusions, succeed.
        std::vector<std::string> out; std::string step;
        std::size_t start=0;int sentences=0;
        for(std::size_t i=0;i<=text.size();++i) if(i==text.size()||text[i]=='.'||text[i]=='\n'||text[i]==';') {
            auto sentence=text.substr(start,i-start+(i<text.size()?1:0));start=i+1;
            if(sentence.size()>1800) throw std::runtime_error("admin_step_budget: сократите одно предложение или разделите связанную просьбу на предложения");
            if(step.size()+sentence.size()>1800 || sentences==1){if(step.find_first_not_of(" \t\r\n")!=std::string::npos)out.push_back(step);step.clear();sentences=0;}
            step+=sentence; if(sentence.find_first_not_of(" \t\r\n")!=std::string::npos)++sentences;
        }
        if(step.find_first_not_of(" \t\r\n")!=std::string::npos) out.push_back(step);
        return out;
    }
    void persist() {
        data_["active"]=active_.load();
        std::atomic_store(&published_,std::make_shared<const Json>(view()));
        const auto bytes=data_.dump();
        if(bytes.size()>1024*1024) throw std::runtime_error("setup_private_storage_limit");
        const auto temp=path_+".tmp";
        int fd=::open(temp.c_str(),O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC|O_NOFOLLOW,0600);
        if(fd<0)throw std::runtime_error("setup_draft_write_failed");
        if(::fchmod(fd,0600)!=0){::close(fd);throw std::runtime_error("setup_draft_permissions_failed");}
        std::size_t offset=0;
        while(offset<bytes.size()) {
            auto n=::write(fd,bytes.data()+offset,bytes.size()-offset);
            if(n<0&&errno==EINTR)continue;
            if(n<=0){::close(fd);throw std::runtime_error("setup_draft_write_failed");}offset+=n;
        }
        const bool synced=::fsync(fd)==0;::close(fd);
        if(!synced||::rename(temp.c_str(),path_.c_str())!=0)throw std::runtime_error("setup_draft_publish_failed");
        int directory=::open(store_.directory().c_str(),O_RDONLY|O_DIRECTORY|O_CLOEXEC|O_NOFOLLOW);
        if(directory<0)throw std::runtime_error("setup_directory_sync_failed");
        const bool durable=::fsync(directory)==0;::close(directory);
        if(!durable)throw std::runtime_error("setup_directory_sync_failed");
    }
};
}
