
//      Copyright Oliver Kowalke 2015, CommitThis 2020
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)

#pragma once

#include <condition_variable>
#include <chrono>
#include <deque>
#include <mutex>
#include <thread>

#include <boost/config.hpp>

#include <boost/fiber/algo/algorithm.hpp>
#include <boost/fiber/context.hpp>
#include <boost/fiber/detail/config.hpp>
#include <boost/fiber/scheduler.hpp>

#include <boost/thread/barrier.hpp>


using boost::fibers::context;
using boost::fibers::scheduler;
using boost::fibers::algo::algorithm_with_properties;


/* Utility functions for making printing a bit easier */
namespace utility {

template <typename Lockable>
inline auto make_unique_lock(Lockable & lockable) -> std::unique_lock<Lockable>
{
    return std::unique_lock<Lockable>(lockable);
}

extern std::mutex print_mtx;// = {};

template <typename ... Args>
inline auto locked_print(Args && ... args) -> void
{
    auto lock = make_unique_lock(print_mtx);
    (std::cout << ... << args);
}

}




/*  Exposes custom property for each fiber; if the fiber has awakened for the
    first time, `m_previously_awakened` will be false. The idea is that the
    scheduler sets this after it's first been awakened */
class thread_locked_props : public boost::fibers::fiber_properties
{
public:
    thread_locked_props(boost::fibers::context * ctx)
        : fiber_properties{ctx}
        , m_previously_awakened(false) 
    {
    }
    auto was_previously_awakened() -> bool
    {
        return m_previously_awakened;
    }
    auto set_previously_awakened() -> void
    {
        if (!m_previously_awakened) {
            m_previously_awakened = true;
            /*  Notify isn't really needed as the change wouldn't be used */
            notify();
        }
    }
private:
    bool m_previously_awakened;
};



/*  Thread locked scheduler. The aim of this class is to ensure that once a
    fiber has been started, it remains on the thread it was started from. This
    is achieved by giving the schedulers each their own ready queue, and 
    populating them in a round-robin fashion.

    Because of the use of statics for the scheduler list, much like the other
    Boost schedulers, only one set of schedulers participating in the same
    "pool of work" can exist.
*/
class thread_locked_scheduler : public 
        algorithm_with_properties<thread_locked_props>
{
    using local_queue_t = scheduler::ready_queue_type;

    /*  an intrusive pointer is used because it scheduler objects maintain their
        reference counts, without having to use a shared_ptr. This makes 
        assignment from `this` possible. It is called intrusive because the
        reference count "intrudes" on the class's definition. */
    using scheduler_list_t = std::vector<
            boost::intrusive_ptr<thread_locked_scheduler>>;

public:
    thread_locked_scheduler(std::size_t thread_count, bool main_scheduler = false)
        : m_local_queue{}
        , m_condition{}
        , m_flag{false}
        , m_suspend{false}
    {
        static boost::barrier barrier{static_cast<std::uint32_t>(thread_count)};

        /*  The first scheduler that is created is responsible for the creation
            of the vector of other schedulers. `call_once` safely manages this
            across multiple threads. */
        static std::once_flag flag;
        std::call_once(flag, [thread_count](){
            scheduler_list_t{thread_count - 1, nullptr}.swap(s_schedulers);
        });

        /*  In this case, I do not want the main-fiber to participate in the work,
            so it is free to handle other things */
        if (!main_scheduler) {
            s_schedulers[s_current_scheduler++] = this;
        }

        /*  We wait for each scheduler to finish initialising, the main fiber's
            and worker's schedulers will be constructed in a non-deterministic
            fashion. (i.e, when the thread gets around to it), if we didn't wait, a
            fiber may awake on a partially constructed object. This UB and
            consequently your computer might turn into a unicorn and fly away. */
        barrier.wait();
    }


    /*  Used to accept a context from another thread */
    auto accept(context * ctx) -> void 
    {
        auto lock = std::lock_guard<std::mutex>{ s_mutex };
        m_local_queue.push_back(*ctx);
    }
    

    /*  Informs scheduler that fiber `ctx` is ready to run; the ready fiber will
        be resumed once `pick_next` gets around to it.
        When a fiber is newly awakened, it will enter this function from the main
        fiber/thread. When this happens, it will be placed into one of the other
        schedulers. If previously awakened, it will placed in this schedulers
        ready queue. */
    auto awakened(context * ctx, thread_locked_props & props) noexcept -> void
    {
        auto lock = std::unique_lock<std::mutex>{s_mutex};
        if (ctx->is_context( boost::fibers::type::pinned_context) ) { 
            m_local_queue.push_back(*ctx);
        } 
        else {
            ctx->detach();
            if (props.was_previously_awakened()) {
                m_local_queue.push_back(*ctx);
            } 
            else {
                s_current_scheduler = ++s_current_scheduler % 
                        std::size(s_schedulers);
                lock.unlock();
                props.set_previously_awakened();
                auto next = s_schedulers[s_current_scheduler];
                next->accept(ctx);
            }
        }
    }

    /* Returns the fiber to be resumed next */
    auto pick_next() noexcept -> context *
    {
        context * ctx = nullptr;
        auto lock = std::unique_lock<std::mutex>{ s_mutex };

        if ( ! m_local_queue.empty() ) {
            ctx = & m_local_queue.front();
            m_local_queue.pop_front();
            lock.unlock();

            if (!ctx->is_context(boost::fibers::type::pinned_context)) {
                context::active()->attach(ctx);
            }
        }

        return ctx;
    }


    /* Do we have any fibers ready to run? */
    auto has_ready_fibers() const noexcept -> bool
    {
        auto lock = std::lock_guard<std::mutex>{s_mutex};
        return ! m_local_queue.empty();
    }


    auto suspend_until(std::chrono::steady_clock::time_point const& time_point)
        noexcept -> void
    {
        if ( (std::chrono::steady_clock::time_point::max)() == time_point) {
            auto lock = std::unique_lock<std::mutex>{ s_mutex };
            m_condition.wait( lock, [this](){ return m_flag; });
            m_flag = false;
        } else {
            auto lock = std::unique_lock<std::mutex>{ s_mutex };
            m_condition.wait_until(lock, time_point, [this](){ return m_flag; });
            m_flag = false;
        }
    }

    auto notify() noexcept -> void
    {
        auto lock = std::unique_lock<std::mutex>{ s_mutex };
        m_flag = true;
        lock.unlock();
        m_condition.notify_all();
    }

private:
    static std::atomic<std::size_t> s_current_scheduler;
    static scheduler_list_t         s_schedulers;
    static std::mutex               s_mutex;

    local_queue_t           m_local_queue;
    std::condition_variable m_condition;
    bool                    m_flag;
    bool                    m_suspend;

};







