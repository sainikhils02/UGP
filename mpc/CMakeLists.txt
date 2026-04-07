cmake_minimum_required(VERSION 3.15)
project(mpc_async LANGUAGES CXX)

# ------------------------------------------------------------------
# Language & global compile flags
# ------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_compile_options(
    -march=native
    -O3
    -g
    -Wall
    -pedantic
    -Wno-ignored-attributes
    -fopenmp
    -msse2
    -mssse3
    -maes
    -mpclmul
    -DBOOST_ERROR_CODE_HEADER_ONLY
)

# ------------------------------------------------------------------
# Dependencies
# ------------------------------------------------------------------
find_package(libOTe REQUIRED COMPONENTS std_20 boost)
find_package(Boost REQUIRED COMPONENTS system)
find_package(OpenMP REQUIRED)
find_package(Threads REQUIRED)

# libbsd has no official CMake target
find_library(BSD_LIB bsd REQUIRED)

# ------------------------------------------------------------------
# Helper function (avoids repetition)
# ------------------------------------------------------------------
function(add_mpc_executable name)
    add_executable(${name} ${ARGN})

    target_link_libraries(${name}
        oc::libOTe
        Boost::system
        OpenMP::OpenMP_CXX
        Threads::Threads
        ${BSD_LIB}
    )

    target_compile_definitions(${name} PRIVATE
        BOOST_ASIO_HAS_CO_AWAIT
    )
endfunction()

# ------------------------------------------------------------------
# Main binary
# ------------------------------------------------------------------
add_mpc_executable(mpc_async
    main.cpp
)

# ------------------------------------------------------------------
# OT benchmark executable
# ------------------------------------------------------------------
add_mpc_executable(ot_tests
    ot_tests.cpp
)

# ------------------------------------------------------------------
# Tests
# ------------------------------------------------------------------
add_mpc_executable(mpcops_test
    mpcops_test.cpp
)

add_mpc_executable(test_shares
    shares_test.cpp
)

add_mpc_executable(locoram_test
    locoram_test.cpp
)

add_mpc_executable(dpf_test
    dpf_test.cpp
)
