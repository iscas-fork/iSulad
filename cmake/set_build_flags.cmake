# set common FLAGS
set(CMAKE_C_FLAGS "-fPIC -fstack-protector-all -D_FORTIFY_SOURCE=2 -O2 -Wall -fPIE")
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -D__FILENAME__='\"$(subst ${CMAKE_SOURCE_DIR}/,,$(abspath $<))\"'")

if (GRPC_CONNECTOR)
    include(CheckCXXCompilerFlag)
    CHECK_CXX_COMPILER_FLAG("-std=c++17" COMPILER_SUPPORTS_CXX17)
    if (COMPILER_SUPPORTS_CXX17)
        message(STATUS "The compiler ${CMAKE_CXX_COMPILER} has C++17 support.")
        set(CMAKE_CXX_VERSION "-std=c++17")
    else()
        message(STATUS "The compiler ${CMAKE_CXX_COMPILER} has no C++17 support. Use C++11.")
        set(CMAKE_CXX_VERSION "-std=c++11")
    endif()
    set(CMAKE_CXX_FLAGS "-fPIC ${CMAKE_CXX_VERSION} -fstack-protector-all -D_FORTIFY_SOURCE=2 -O2 -Wall -Wno-error=deprecated-declarations")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -D__FILENAME__='\"$(subst ${CMAKE_SOURCE_DIR}/,,$(abspath $<))\"'")
endif()
set(CMAKE_SHARED_LINKER_FLAGS "-Wl,-E -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wtrampolines -shared -pthread")
set(CMAKE_EXE_LINKER_FLAGS "-Wl,-E -Wl,-z,relro -Wl,-z,now -Wl,-z,noexecstack -Wtrampolines -pie -rdynamic")

if (NOT DISABLE_WERROR)
    set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Werror")
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -Werror")
endif()

if (ISULAD_GCOV)
    set(CMAKE_C_FLAGS_DEBUG "-Wall -fprofile-arcs -ftest-coverage")
    set(CMAKE_CXX_FLAGS_DEBUG "-Wall -fprofile-arcs -ftest-coverage -Wno-error=deprecated-declarations")
    message("-----CXXFLAGS: " ${CMAKE_CXX_FLAGS_DEBUG})
    message("------compile with gcov-------------")
    message("-----CFLAGS: " ${CMAKE_C_FLAGS_DEBUG})
    message("------------------------------------")
endif()
