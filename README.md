Thread Local Fibers
===================

This is an implementation of a Boost.Fiber scheduler that pins running fibers
to a specific thread. The full code is available on [GitHub](https://github.com/CommitThis/thread-local-fibers).


Introduction
------------

Fibers are thread like constructs that can suspend and yield to other fibers,
much in the same way that threads can. However, they are more useful than 
threads in the sense that:
1. They avoid the overhead of a context switch between threads -- they store
   all the necessary data for resuming internally;
2. They can be used to make bulk operations to appear as if they are performed
   sequentially;
3. They can be used to make asynchronous operations appear as if they are
   performed in a blocking manner, without actually blocking the thread they
   are running on.

This isn't going to cover the relative merits of using fibers, as usage of such
has fallen out of favour in the past few years <sup>1, 2</sup>. 
This code, based on *Boost.Fiber*, may however provide
an interesting example for those curious, particularly in terms of the
customisation points of library.


Why a Thread Local Fiber?
-------------------------

Typically, fibers run in a pool of worker threads; when a fiber is suspended and
resumed, it may awaken on any thread in the pool. However, if the fibers require
access to "global" objects, access has to be synchronised across all of the
threads. 

By pinning fibers to a particular thread, where the fibers in question have 
access to some resource unique to that thread, you can simplify the 
synchronisation mechanisms; all access to the resource in question is
guaranteed to occur sequentially.

> Curiously, while the Boost library claims that no fibers will migrate
> threads<sup>3</sup>, all of the schedulers they have written move fibers
> around threads:
> 
> _"A fiber can be migrated from one thread to another, though the library
> does not do this by default."_

Admittedly, I imagine the use cases of this to be of limited use, especially
since the Boost fiber synchronisation primitives are more than sufficient to
synchronise fibers across multiple threads.


General Principle
-----------------

The customisation points of `Boost.Fiber` are the scheduling algorithm, and 
adding properties to each fiber.

The general idea is that when a fiber is woken up for the first time it is
assigned to a scheduler that maintains it's own ready queue. When the fiber is
suspended, it is placed back into the local ready queue. Therefore, subsequent
wake ups will continue to occur in the thread where the scheduler is installed.


## Adding Custom Properties

There is no immediately obvious way of telling whether a fiber is newly
launched, or it has come from an existing queue. To that end, we can associate
custom properties with a running fiber. In this case, it is straightforward;
we have a single boolean that stores whether or not it has been previously
awakened:

    #include <boost/fiber/all.hpp>

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
                notify();
            }
        }
    private:
        bool m_previously_awakened;
    };



### The Scheduler

The implementation is most similar to the `work_stealing` algorithm, with a few
differences. The scheduler needs to keep track of the other schedulers in play,
but instead of stealing fibers from their ready queues, it adds to them.

In order to make use of the properties we created earlier, we need to 
inherit from 

    class thread_locked_scheduler : public 
        boost::fibers::algorithm_with_properties<thread_locked_props>
    {
        ...
    }



The constructor is super similar:

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


Here, the schedulers list is initialised once and only once, assigned pointers
to the other schedulers, excluding the main scheduler -- the only fibers that
will awaken here are those newly created.

When a fiber is awakened, we need to figure out whether it is a new fiber, and
if so, drop it into some other scheduler's ready queue:

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
                /*  Determine next scheduler to assign fiber to */
                s_current_scheduler = ++s_current_scheduler % 
                        std::size(s_schedulers);
                lock.unlock();
                props.set_previously_awakened();
                auto next = s_schedulers[s_current_scheduler];
                next->accept(ctx);
            }
        }
    }



It should be fairly easy to see that if the fiber has previously been awakened,
it has already been assigned to a scheduler. The only thing left to do is to
accept the fiber on the receiving scheduler, which is as simple as dropping it
in the queue:

    auto accept(context * ctx) -> void 
    {
        auto lock = std::lock_guard<std::mutex>{ s_mutex };
        m_local_queue.push_back(*ctx);
    }




### Using the Scheduler

Now we have a scheduler that pins fibers to a specific thread. An example of
it's potential usefulness is using it to safely access thread local objects
from within the fiber:

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


Our `foo()` function takes in the id of the fiber, prints the fiber id, the
object id and returns the object id. The return value is so we can check what
object has been used at a given point in the fiber.

Initialisation of our object is done using the Initialise-on-first-use idiom:

    auto get_thread_local() -> thread_local_object&
    {
        thread_local static thread_local_object obj{};
        return obj;
    }



Which can then be used in a running fiber:

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


The fiber function accepts an id which is the number of fibers launched up 
until this point. The id of the original thread local bbject is stored, and then
verified every loop. If the original object id is different, then a warning will
be produced. The same comparison is also made against the thread's id.

Using our scheduler, the output should be something like:

    thread_local_object::foo: fiber id: 3, object id: 0
    thread_local_object::foo: fiber id: 5, object id: 4
    thread_local_object::foo: fiber id: 21, object id: 4
    thread_local_object::foo: fiber id: 37, object id: 4
    thread_local_object::foo: fiber id: 53, object id: 4
    thread_local_object::foo: fiber id: 69, object id: 4
{:class="language-shell"}


However, if we were to drop in `boost::fibers::algo::shared_work`,
our output would look something like this:

    thread_local_object::foo: fiber id: 54, object id: 11
    thread_local_object::foo: fiber id: 5, object id: 1
    thread_local_object::foo: fiber id: 2, object id: 4
    WARNING: Fiber migrated thread!
    WARNING: Fiber accessed wrong thread local!
    thread_local_object::foo: fiber id: 22, object id: 8
    WARNING: Fiber migrated thread!
    WARNING: Fiber accessed wrong thread local!
    thread_local_object::foo: fiber id: 30, object id: 14
{:class="language-shell"}


> If using a different scheduler, you could create a reference to a thread local
> holding it in the fiber, dereferencing that will use the original object. e.g.
> 
>       auto & obj = ::get_thread_local(); obj.foo(); 
> 
> however using the getter directly
> 
>       ::get_thread_local().foo();
> 
> will return the object associated with the current thread, so beware.





Observations & Conclusion
-------------------------

Like I mentioned earlier, I think this has limited usefulness, maybe someone 
will have a good reason for using it. In any case, I was able to observe that
the fibers were not being migrated across threads, and were able to repeatedly
access the local storage of the running thread.

I did, however, notice when I created a lot of fibers and shortened the time
that a fiber would block it's thread, the supplied schedulers performed slightly
better than this. I did wonder whether or not this was down to overheads
involved with handling the properties. However, I have worked enough on this for
now, and probably won't worry myself too much about it.

¯\\(ツ)/¯


References
----------
1. Microsoft: [The Old New Thing Blog](https://devblogs.microsoft.com/oldnewthing/20191011-00/?p=102989)
2. P1264R0, Gor Nishanov [Fibers under the magnifying glass](http://www.open-std.org/JTC1/SC22/WG21/docs/papers/2018/p1364r0.pdf)
3. [Boost Fibers Overview](https://www.boost.org/doc/libs/1_65_0/libs/fiber/doc/html/fiber/overview.html)