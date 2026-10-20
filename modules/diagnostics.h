#pragma once
#include "model.h"
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>
namespace music_tracker {
// Workers only enqueue. The host supplies a scheduler that drains on its main thread.
class Diagnostics {
public:
    using Task=std::function<void()>;
    using Writer=std::function<void(const std::string&)>;
    using Scheduler=std::function<void(Task)>;
    using Now=std::function<MonoClock::time_point()>;
    Diagnostics(Writer writer={},Scheduler scheduler={},Now now={}):state_(std::make_shared<State>(std::move(writer),std::move(scheduler),std::move(now))){}
    void set_verbose(bool value) const noexcept {state_->verbose=value;}
    bool verbose() const noexcept {return state_->verbose;}
    void info(const std::string& scope,const std::string& text) const noexcept {emit(false,scope,text);}
    void detail(const std::string& scope,const std::string& text) const noexcept {if(verbose())emit(true,scope,text);}
private:
    struct Entry {std::string text;bool verbose;};
    struct State {
        Writer writer;Scheduler scheduler;Now now;
        std::atomic<bool> verbose{false};
        std::mutex mutex;
        std::deque<Entry> queue;
        std::map<std::string,MonoClock::time_point> recent;
        bool pending=false;unsigned dropped=0;
        State(Writer w,Scheduler s,Now n):writer(std::move(w)),scheduler(std::move(s)),now(std::move(n)){}
    };
    std::shared_ptr<State> state_;
    static void drain(const std::shared_ptr<State>& state) noexcept {
        std::deque<Entry> entries;unsigned dropped=0;
        {std::lock_guard lock(state->mutex);entries.swap(state->queue);dropped=state->dropped;state->dropped=0;state->pending=false;}
        for(const auto& entry:entries)if(!entry.verbose||state->verbose)try{state->writer(entry.text);}catch(...){}
        if(dropped)try{state->writer("[discord-foobar-rpc] Diagnostics queue full; "+std::to_string(dropped)+" messages omitted");}catch(...){}
    }
    void emit(bool verbose,const std::string& scope,const std::string& text) const noexcept {
        try {
            const auto state=state_;
            if(!state->writer||!state->scheduler)return;
            const auto line="[discord-foobar-rpc] ["+bounded_text(scope,180)+"] "+bounded_text(text,1536);
            bool schedule=false;
            {
                std::lock_guard lock(state->mutex);
                const auto now=state->now?state->now():MonoClock::now();
                if(auto hit=state->recent.find(line);hit!=state->recent.end()&&now-hit->second<std::chrono::seconds(30))return;
                if(state->queue.size()>=256){++state->dropped;return;}
                if(state->recent.size()>=128)state->recent.clear();
                state->recent[line]=now;state->queue.push_back({line,verbose});
                if(!state->pending){state->pending=true;schedule=true;}
            }
            if(schedule)try{state->scheduler([state]{drain(state);});}
            catch(...){std::lock_guard lock(state->mutex);state->pending=false;state->queue.clear();}
        } catch(...){} // Diagnostics must never affect playback or lookup results.
    }
};
}
