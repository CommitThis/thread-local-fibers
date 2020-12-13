
// Copyright Nat Goodspeed + Oliver Kowalke 2015, CommitThis 2020.
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)


#include "thread_locked_scheduler.hpp"

#include <boost/fiber/all.hpp>
#include <boost/lockfree/spsc_queue.hpp>
#include <boost/thread/barrier.hpp>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <sstream>
#include <string>



using namespace std::chrono_literals;
using namespace std::string_literals;

using boost::fibers::algo::shared_work;
using boost::fibers::algo::work_stealing;


static std::atomic<std::size_t> fiber_count{ 0 };
static std::mutex mtx_count{};
static boost::fibers::condition_variable_any cnd_count{};
typedef std::unique_lock< std::mutex > lock_type;



struct thread_local_object
{
    thread_local_object()
        : m_id{current_id++}
    {}

    auto foo(std::size_t fiber_id) {
        utility::locked_print("thread_local_object::foo: fiber id: ", fiber_id,
                ", object id: ", m_id, "\n");
        return m_id;
    }
    static std::atomic<std::size_t> current_id;
    std::size_t m_id;
};

std::atomic<std::size_t> thread_local_object::current_id = 0ull;


auto get_thread_local() -> thread_local_object&
{
    thread_local static thread_local_object obj{};
    return obj;
}






/*  If declaring a reference to a thread_local object, the fiber function will
    still be able to safely access the object regardless of what thread it is
    on. However, this means that access to these objects must be synchronised
    on a thread basis, rather than a fiber basis. 
    
    If the function for retrieving the thread_local object is used instead of 
    creating a reference, then that will return the object tied to tthat thread.
*/
auto fiber_function(std::size_t fiber_id) -> void
{
    auto my_local_id = ::get_thread_local().foo(fiber_id);
    auto my_thread_id = std::this_thread::get_id();

    for (auto jj = 0ull; jj != 5; ++jj) {
        boost::this_fiber::sleep_for(10ms);
        /*  If using thread local scheduler you can access our variable using a
            static function. */
        auto new_local_id = ::get_thread_local().foo(fiber_id);
        auto new_thread_id = std::this_thread::get_id();

        if (new_thread_id != my_thread_id) {
            my_thread_id = new_thread_id;
            utility::locked_print("WARNING: Fiber migrated thread!\n");
        }

        if (new_local_id != my_local_id) {
            my_local_id = new_local_id;
            utility::locked_print("WARNING: Fiber accessed wrong thread local!\n");
        }
    }
     
    auto lk = utility::make_unique_lock( mtx_count);
    if ( 0 == --fiber_count) {
        lk.unlock();
        cnd_count.notify_all();
    }
}



auto worker_function(boost::barrier & barrier, std::size_t id, std::size_t n_workers)
{
    // boost::fibers::use_scheduling_algorithm<thread_locked_scheduler>(n_workers + 1);
    boost::fibers::use_scheduling_algorithm<shared_work>();
    barrier.wait();

    auto lk = utility::make_unique_lock( mtx_count);
    cnd_count.wait( lk, [](){ return 0 == fiber_count; } );
}


auto main(int argc, char const * argv[]) -> int
{

    auto main_thread_id = std::this_thread::get_id();
    auto n_workers = 16ull;

    /*  This barrier is unnecessary for the `thread_local_scheduler` as the 
        their construction is synchronised internally. This is here for 
        convenience should it want to be compared with the `work_stealing`
        (prior to boost 1.69, where the constructors where not synchronised)
        as well as the `shared_work` scheduler. */
    auto barrier = boost::barrier{static_cast<std::uint32_t>(n_workers + 1)};
    auto workers = std::vector<std::thread>{};

    for (auto ii = 0ull; ii != n_workers; ++ii) {
        auto & worker = workers.emplace_back([&barrier, ii, n_workers](){ 
                worker_function(barrier, ii, n_workers);
        });
    }

    // boost::fibers::use_scheduling_algorithm<thread_locked_scheduler>(n_workers + 1, true);
    boost::fibers::use_scheduling_algorithm<shared_work>();

    for (auto ii = 0ull; ii != 100; ++ii) {
        boost::fibers::fiber([ii]() { fiber_function(ii); }).detach();
        ++fiber_count;
    }

    /*  If using fiber count lock to determine whether processing has been
        completed, launching fibers before waiting on the barrier causes a race
        condition where the fiber count may become 0 as fibers are launched
        and destroyed async, e.g, at a given point more fibers have been 
        destroyed than have been created */
    barrier.wait();

    {
        auto lk = utility::make_unique_lock( mtx_count);
        cnd_count.wait( lk, [](){ return 0 == fiber_count; } );
    }

    for (auto && worker : workers) {
        worker.join();
    }
}