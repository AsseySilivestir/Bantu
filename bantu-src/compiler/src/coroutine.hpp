#pragma once
// ════════════════════════════════════════════════════════════════════════════
//  coroutine.hpp — suspending a handler without suspending the worker.
//
//  THE PROBLEM
//  -----------
//  Each sua worker is one event loop on one thread, and a handler runs to
//  completion on that thread. So while a handler waits on anything — an
//  outbound HTTP call, sleep(), a long query — every other connection on that
//  worker waits with it. SO_REUSEPORT cuts the blast radius to 1/n and
//  sua.http.all collapses a fan-out into one wait, but neither helps a single
//  slow outbound call on a busy worker.
//
//  WHY NOT C++20 COROUTINES
//  ------------------------
//  `co_await` is a COMPILE-TIME transformation: a function that awaits must
//  itself be a coroutine, and so must everything that calls it. Bantu is a
//  tree-walking interpreter — suspension inside a builtin would have to
//  propagate up through evalCall, evalBinary, evalBlock and thirty siblings,
//  turning the whole evaluator into coroutines and putting a heap-allocated
//  frame on the hot path of arithmetic that never suspends. What is needed is
//  STACKFUL suspension: freeze a real C++ call stack mid-recursion, resume it
//  later. ucontext is removed from POSIX and absent on Windows; hand-rolled
//  assembly is a per-architecture commitment forever. Both rejected — see
//  docs/sua-async-design.md §2–3.
//
//  THE MECHANISM: A BATON
//  ----------------------
//  A suspendable handler runs on its own thread, but EXACTLY ONE thread is ever
//  runnable at a time. A single logical baton is held either by the loop or by
//  one task, never by two, and never by none. Handing it over is a condition
//  variable, not a scheduler.
//
//  This is not the thread-per-connection model the event loop deleted:
//
//      thread-per-connection          strict handoff
//      one thread per CONNECTION      one thread per SUSPENDED handler
//      run simultaneously  -> races   never run simultaneously
//      10,000 idle clients = 10,000   10,000 idle clients = 0
//
//  A connection costs nothing until its handler actually suspends. The thread
//  count tracks concurrent outbound I/O — tens, not thousands — and is capped.
//
//  THE ONE PRIMITIVE
//  -----------------
//  yieldFor(work):  park (hand the baton to the loop) → run `work` on this
//  thread with the baton released → ask the loop for the baton back → block
//  until it arrives. Everything else is built on it:
//
//      sleep()        yieldFor([]{ sleep_for(ms); })
//      sua.http.get   yieldFor([]{ curl_easy_perform(...); })
//      sua.http.all   yieldFor([]{ the whole curl_multi loop });
//      sua.sqlite.*   yieldFor([]{ sqlite3_step until done });
//
//  `work` runs while the loop is free, so it MUST NOT touch the interpreter,
//  the connection table, or the backend. Every call above is plain C against
//  its own state, which is what makes this safe.
//
//  Off a task thread — a script with no server, or a handler that ran inline
//  because the pool was full — yieldFor just calls work(). Nothing changes.
//
//  WHY IT IS DEBUGGABLE
//  --------------------
//  A suspended handler is an ordinary thread with an ordinary backtrace in lldb
//  or gdb, and the whole thing is std::thread / std::mutex / condition_variable:
//  C++11, portable to Windows unchanged, and clean under ThreadSanitizer. None
//  of that is true of the alternatives.
//
//  See docs/sua-async-design.md for the full design and the atomicity hazard
//  that made this opt-in per handler rather than global.
// ════════════════════════════════════════════════════════════════════════════

#include "event_loop.hpp"

#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace bantu_co {

// One suspendable handler in flight. Owns the thread it runs on; the thread
// outlives the handler and is returned to the idle pool for the next one.
struct Task {
    std::thread             th;
    std::condition_variable cv;
    std::function<void()>   body;
    bool turn = false;      // this task holds the baton
    bool quit = false;      // pool shutdown
};

class Scheduler {
public:
    // `maxTasks` bounds concurrently SUSPENDED handlers, not requests. Zero
    // disables suspension entirely, which is the correct behaviour outside a
    // server: yieldFor then runs its work inline.
    bool start(int maxTasks) {
        if (started_) return true;
        if (!bantu_loop::makeWakePair(wakeR_, wakeW_)) return false;
        max_ = maxTasks;
        started_ = true;
        return true;
    }

    bool started() const { return started_; }
    int  wakeFd() const  { return wakeR_; }
    int  live() const    { return live_; }        // loop thread only
    int  capacity() const { return max_; }
    uint64_t suspensions() const { return suspensions_.load(); }

    // live_ is written only under m_, and only at moments when the loop is
    // blocked in handOff -- so reading it from the loop thread needs no lock.

    // ── loop thread ────────────────────────────────────────────────────────

    // Run `body` as a suspendable task. Returns false when the pool is at its
    // cap — the caller must then run the handler inline, which is exactly the
    // pre-suspension behaviour and therefore always correct, just not
    // concurrent.
    //
    // Returns once the task has PARKED or FINISHED, so a handler that never
    // suspends behaves indistinguishably from one called directly.
    bool spawn(std::function<void()> body) {
        if (!started_ || live_ >= max_) return false;
        std::unique_lock<std::mutex> l(m_);
        Task* t;
        if (!idle_.empty()) { t = idle_.back(); idle_.pop_back(); }
        else {
            t = new Task();
            all_.push_back(t);
            // Started while m_ is held: the new thread's first act is to lock
            // m_, so it waits until we release it below.
            t->th = std::thread(&Scheduler::taskMain, this, t);
        }
        t->body = std::move(body);
        live_++;
        handOff(l, t);
        return true;
    }

    // Cheap enough to call every loop iteration; lets the caller skip saving
    // its interpreter context when there is nothing to resume.
    bool anyReady() {
        if (!started_) return false;
        std::lock_guard<std::mutex> l(m_);
        return !ready_.empty();
    }

    // Give the baton to every task whose off-baton work has finished, one at a
    // time, each running until it parks again or completes.
    void pump() {
        for (;;) {
            std::unique_lock<std::mutex> l(m_);
            if (ready_.empty()) return;
            Task* t = ready_.front();
            ready_.erase(ready_.begin());
            handOff(l, t);
        }
    }

    // The wake byte has done its job once wait() returned; the queue it
    // announced is drained by pump().
    void drainWake() {
        if (wakeR_ < 0) return;
        char buf[256];
        while (::recv(wakeR_, buf, (int)sizeof(buf), 0) > 0) {}
    }

    // ── task thread ────────────────────────────────────────────────────────

    static bool onTask() { return current_ != nullptr; }

    // Park, run `work` with the baton released, then block until the loop hands
    // the baton back. See the header comment for what `work` may touch.
    void yieldFor(const std::function<void()>& work) {
        Task* t = current_;
        if (!t) { work(); return; }             // not suspendable: run inline

        suspensions_++;
        {
            std::lock_guard<std::mutex> l(m_);
            t->turn = false;
            loopTurn_ = true;
        }
        loopCv_.notify_one();

        work();                                  // off-baton; the loop is free

        {
            std::lock_guard<std::mutex> l(m_);
            ready_.push_back(t);
        }
        wake();
        std::unique_lock<std::mutex> l(m_);
        t->cv.wait(l, [t] { return t->turn; });
    }

    // Ends the pool. Not reached in a server that runs until killed; it exists
    // so the scheduler is not a leak in any other embedding.
    void shutdown() {
        std::vector<Task*> all;
        {
            std::lock_guard<std::mutex> l(m_);
            for (Task* t : idle_) { t->quit = true; t->cv.notify_one(); }
            all.swap(all_);
            idle_.clear();
        }
        for (Task* t : all) { if (t->th.joinable()) t->th.join(); delete t; }
        started_ = false;
    }

private:
    // Hand the baton to `t` and block until it comes back. Called with `l`
    // holding m_; returns still holding it. This is the only place the baton
    // moves loop → task, and the wait is what guarantees the loop runs no
    // interpreter code while a task is running.
    void handOff(std::unique_lock<std::mutex>& l, Task* t) {
        loopTurn_ = false;
        t->turn = true;
        t->cv.notify_one();
        loopCv_.wait(l, [this] { return loopTurn_; });
    }

    void taskMain(Task* t) {
        for (;;) {
            std::unique_lock<std::mutex> l(m_);
            t->cv.wait(l, [t] { return t->turn || t->quit; });
            if (t->quit) return;
            l.unlock();

            current_ = t;
            // A handler that throws must still return the baton, or the loop
            // waits forever. The evaluator reports the error itself; anything
            // reaching here is already unhandled.
            try { t->body(); } catch (...) {}
            t->body = nullptr;
            current_ = nullptr;

            l.lock();
            t->turn = false;
            live_--;
            idle_.push_back(t);
            loopTurn_ = true;
            l.unlock();
            loopCv_.notify_one();
        }
    }

    // One byte is enough: the reader only needs to know the queue is non-empty.
    // A full socket buffer means a wakeup is already pending, so a dropped
    // write loses nothing.
    void wake() {
        if (wakeW_ < 0) return;
        char b = 1;
        (void)::send(wakeW_, &b, 1, 0);
    }

    std::mutex              m_;
    std::condition_variable loopCv_;
    bool                    loopTurn_ = true;    // the loop starts holding it
    std::vector<Task*>      ready_;              // finished their off-baton work
    std::vector<Task*>      idle_;               // threads awaiting a handler
    std::vector<Task*>      all_;
    int                     live_ = 0;
    int                     max_ = 0;
    bool                    started_ = false;
    int                     wakeR_ = -1, wakeW_ = -1;
    std::atomic<uint64_t>   suspensions_{0};   // bumped from task threads

    static thread_local Task* current_;
};

inline thread_local Task* Scheduler::current_ = nullptr;

inline Scheduler& sched() {
    static Scheduler s;
    return s;
}

}  // namespace bantu_co
