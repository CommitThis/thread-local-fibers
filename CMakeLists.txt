project(ThreadLocalFiber)
cmake_minimum_required(VERSION 3.12)

find_package(Boost REQUIRED COMPONENTS fiber context system)

add_library(tlfiber SHARED
    thread_locked_scheduler.cpp
    )
target_compile_features(tlfiber PUBLIC cxx_std_17)
target_link_libraries(tlfiber PUBLIC Boost::fiber Boost::context)


add_executable(example example.cpp)
target_link_libraries(example PRIVATE tlfiber pthread)
