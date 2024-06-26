cmake_minimum_required (VERSION 2.8)
project (isulad)
add_definitions(-DENABLE_CRI_API_V1=1)
include(cmake/helper.cmake)

include(cmake/options.cmake)

include(cmake/set_build_flags.cmake)

#set(CMAKE_C_COMPILER "gcc" CACHE PATH "c compiler")

set(GIT_COMMIT_HASH "5ebca976dd591a5676527be1bde950e5ce93eac0")
message("--  commit id: " ${GIT_COMMIT_HASH})
add_definitions(-DISULAD_GIT_COMMIT="${GIT_COMMIT_HASH}")

execute_process(
    COMMAND sh -c "date --rfc-3339 ns | sed -e 's/ /T/'"
    OUTPUT_VARIABLE BUILD_DATE
    OUTPUT_STRIP_TRAILING_WHITESPACE
)
message("--  build time: " ${BUILD_DATE})
add_definitions(-DISULAD_BUILD_TIME="${BUILD_DATE}")

if (NOT ISULAD_ROOT_PATH)
add_definitions(-DISULAD_ROOT_PATH="/var/lib/isulad")
endif()
if (NOT ISULAD_STATE_PATH)
add_definitions(-DISULAD_STATE_PATH="/var/run/isulad")
endif()

if (LIB_INSTALL_DIR)
  set(LIB_INSTALL_DIR_DEFAULT ${LIB_INSTALL_DIR})
else()
  set(LIB_INSTALL_DIR_DEFAULT "lib")
endif()

# check depends library and headers
message("---------------Check libraries and headers-----------------------")
include(cmake/checker.cmake)
if (CHECKER_RESULT)
    message(FATAL_ERROR "ERROR: Check libraries and headers failed")
endif()

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/config.h.in"
    "${CMAKE_BINARY_DIR}/conf/config.h"
)

configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/isulad.pc.in"
    "${CMAKE_BINARY_DIR}/conf/isulad.pc"
)

# parse .proto files
message("---------------Generate GRPC iSulad proto-----------------------")
include(cmake/protoc.cmake)

# unit test and coverage
SET(CMAKE_VERBOSE_MAKEFILE OFF)
IF(ENABLE_COVERAGE)
    IF(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        message(WARNING "Debugging mode should be turned on when code coverage reports are being generated; otherwise coverage may be inaccurate.")
    ENDIF()
    MESSAGE(STATUS "Enable coverage compiler option")
    SET(COVERAGE_C_OPTION "${COVERAGE_OPTION} -fprofile-arcs -ftest-coverage -fkeep-static-functions -fkeep-inline-functions")
    SET(COVERAGE_CXX_OPTION "${COVERAGE_OPTION} -fprofile-arcs -ftest-coverage")
ENDIF(ENABLE_COVERAGE)

IF(ENABLE_ASAN)
    MESSAGE(STATUS "Enable asan compiler option")
    SET(ASAN_OPTIONS "${ASAN_OPTION} -fsanitize=address -fsanitize-recover=address -fno-omit-frame-pointer")
ENDIF(ENABLE_ASAN)

IF(CMAKE_BUILD_TYPE STREQUAL Debug)
    SET(CMAKE_C_FLAGS_DEBUG "${CMAKE_C_FLAGS_DEBUG} ${COVERAGE_C_OPTION} ${ASAN_OPTIONS}")
    SET(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} ${COVERAGE_CXX_OPTION} ${ASAN_OPTIONS}")
ENDIF()

add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/src)
IF(ENABLE_UT)
    include(CTest)
    include(Dart)
    find_program(MEMORYCHECK_COMMAND NAMES valgrind)
    set(MEMORYCHECK_COMMAND_OPTIONS "--trace-children=yes --leak-check=full")
    enable_testing()
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/test)
ENDIF(ENABLE_UT)

IF(ENABLE_FUZZ AND (NOT ENABLE_UT))
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/test)
ENDIF(ENABLE_FUZZ AND (NOT ENABLE_UT))

# install all files
install(FILES ${CMAKE_BINARY_DIR}/conf/isulad.pc
  DESTINATION ${LIB_INSTALL_DIR_DEFAULT}/pkgconfig PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ GROUP_WRITE)
if (EANBLE_IMAGE_LIBARAY)
    install(FILES src/daemon/modules/api/image_api.h
        DESTINATION include/isulad)
endif()

# install config files
set(conf_prefix "/etc")
if (CMAKE_INSTALL_SYSCONFDIR)
    set(conf_prefix ${CMAKE_INSTALL_SYSCONFDIR})
endif()

if (ANDROID)
    install(FILES src/contrib/config/container_contexts
        DESTINATION ${conf_prefix}/isulad PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ GROUP_WRITE)
endif()

install(FILES src/contrib/config/daemon.json
    DESTINATION ${conf_prefix}/isulad PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ GROUP_WRITE)
install(FILES src/contrib/config/daemon_constants.json
    DESTINATION ${conf_prefix}/isulad PERMISSIONS OWNER_READ GROUP_READ)
install(FILES src/contrib/config/config.json src/contrib/config/systemcontainer_config.json
    DESTINATION ${conf_prefix}/default/isulad PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ GROUP_WRITE)
install(FILES src/contrib/config/seccomp_default.json
    DESTINATION ${conf_prefix}/isulad PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ GROUP_WRITE)
install(FILES src/contrib/config/hooks/default.json
    DESTINATION ${conf_prefix}/default/isulad/hooks PERMISSIONS OWNER_READ OWNER_WRITE GROUP_READ GROUP_WRITE)
install(FILES src/contrib/sysmonitor/isulad-check.sh
    DESTINATION ${conf_prefix}/default/isulad PERMISSIONS OWNER_READ OWNER_EXECUTE GROUP_READ GROUP_EXECUTE)
install(FILES src/contrib/sysmonitor/isulad-monit
    DESTINATION ${conf_prefix}/sysmonitor/process PERMISSIONS OWNER_READ OWNER_WRITE)

# uninstall
if(NOT TARGET uninstall)
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake_uninstall.cmake.in"
    "${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake"
    IMMEDIATE @ONLY)

  add_custom_target(uninstall
    COMMAND ${CMAKE_COMMAND} -P ${CMAKE_CURRENT_BINARY_DIR}/cmake_uninstall.cmake)
endif()
