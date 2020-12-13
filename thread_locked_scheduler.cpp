
//      Copyright Oliver Kowalke 2015, CommitThis 2020
// Distributed under the Boost Software License, Version 1.0.
//    (See accompanying file LICENSE_1_0.txt or copy at
//          http://www.boost.org/LICENSE_1_0.txt)


#include "thread_locked_scheduler.hpp"

#include <cstddef>
#include <iterator>


std::mutex utility::print_mtx{};


std::mutex thread_locked_scheduler::s_mutex{};
std::atomic<std::size_t> thread_locked_scheduler::s_current_scheduler{0};

thread_locked_scheduler::scheduler_list_t
        thread_locked_scheduler::s_schedulers{};


