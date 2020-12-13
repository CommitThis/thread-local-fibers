Thread Local Fiber Scheduler
============================

This is an implementation of a Boost.Fiber scheduler that pins running fibers
to a specific thread.

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
has fallen out of favour in the past few years<sup>1, 2</sup>. This code, based
on *Boost.Fiber*, may however provide an interesting example for those curious,
particularly in terms of the customisation points of library.


Why a Thread Local Fiber?
-------------------------

Typically, fibers run in a pool of worker threads; when a fiber is suspended and
resumed, it may awaken on any thread in the pool. However, if the fibers require
access to "global" objects, access has to be synchronised across all of the
threads. 

By pinning a fiber to a particular thread, where the fibers in question have 
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



References
----------
1. https://devblogs.microsoft.com/oldnewthing/20191011-00/?p=102989
2. http://www.open-std.org/JTC1/SC22/WG21/docs/papers/2018/p1364r0.pdf
