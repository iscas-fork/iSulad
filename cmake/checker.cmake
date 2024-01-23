include(CheckIncludeFile)

# check depends library and headers
find_package(PkgConfig REQUIRED)

# check python3
find_program(CMD_PYTHON python3)
_CHECK(CMD_PYTHON "CMD_PYTHON-NOTFOUND" "python3")

# check tools
find_program(CMD_TAR tar)
_CHECK(CMD_TAR "CMD_TAR-NOTFOUND" "tar")
find_program(CMD_SHA256 sha256sum)
_CHECK(CMD_SHA256 "CMD_SHA256-NOTFOUND" "sha256sum")
find_program(CMD_GZIP gzip)
_CHECK(CMD_GZIP "CMD_GZIP-NOTFOUND" "gzip")

# check std headers ctype.h sys/param.h sys/capability.h
find_path(STD_HEADER_CTYPE ctype.h)
_CHECK(STD_HEADER_CTYPE "STD_HEADER_CTYPE-NOTFOUND" "ctype.h")

find_path(STD_HEADER_SYS_PARAM sys/param.h)
_CHECK(STD_HEADER_SYS_PARAM "STD_HEADER_SYS_PARAM-NOTFOUND" "sys/param.h")

CHECK_INCLUDE_FILE(sys/capability.h HAVE_LIBCAP)
if (HAVE_LIBCAP)
    message("--  found linux capability.h --- works")
    add_definitions(-DHAVE_LIBCAP_H=1)
else()
    message("--  found linux capability.h --- no")
endif()

# check libcapability
pkg_check_modules(PC_LIBCAP REQUIRED "libcap")
find_library(CAP_LIBRARY cap
    HINTS ${PC_LIBCAP_LIBDIR} ${PC_CAP_LIBRARY_DIRS})
_CHECK(CAP_LIBRARY "CAP_LIBRARY-NOTFOUND" "libcap.so")

# check zlib
pkg_check_modules(PC_ZLIB "zlib>=1.2.8")
find_path(ZLIB_INCLUDE_DIR zlib.h
    HINTS ${PC_ZLIB_INCLUDEDIR} ${PC_ZLIB_INCLUDE_DIRS})
_CHECK(ZLIB_INCLUDE_DIR "ZLIB_INCLUDE_DIR-NOTFOUND" "zlib.h")
find_library(ZLIB_LIBRARY z
  HINTS ${PC_ZLIB_LIBDIR} ${PC_ZLIB_LIBRARY_DIRS})
_CHECK(ZLIB_LIBRARY "ZLIB_LIBRARY-NOTFOUND" "libz.so")

# check libyajl
pkg_check_modules(PC_LIBYAJL REQUIRED "yajl>=2")
find_path(LIBYAJL_INCLUDE_DIR yajl/yajl_tree.h
	HINTS ${PC_LIBYAJL_INCLUDEDIR} ${PC_LIBYAJL_INCLUDE_DIRS})
_CHECK(LIBYAJL_INCLUDE_DIR "LIBYAJL_INCLUDE_DIR-NOTFOUND" "yajl/yajl_tree.h")
find_library(LIBYAJL_LIBRARY yajl
    HINTS ${PC_LIBYAJL_LIBDIR} ${PC_LIBYAJL_LIBRARY_DIRS})
_CHECK(LIBYAJL_LIBRARY "LIBYAJL_LIBRARY-NOTFOUND" "libyajl.so")

# check libcrypto
pkg_check_modules(PC_CRYPTO REQUIRED "libcrypto")
find_library(CRYPTO_LIBRARY crypto
    HINTS ${PC_CRYPTO_LIBDIR} ${PC_LIBCRYPTO_LIBRARY_DIRS})
_CHECK(CRYPTO_LIBRARY "CRYPTO_LIBRARY-NOTFOUND" "libcrypto.so")

if (ANDROID OR MUSL)
    # check libssl
    find_library(LIBSSL_LIBRARY ssl)
    _CHECK(CRYPTO_LIBRARY "LIBSSL_LIBRARY-NOTFOUND" "libssl.so")
endif()

find_path(HTTP_PARSER_INCLUDE_DIR http_parser.h)
_CHECK(HTTP_PARSER_INCLUDE_DIR "HTTP_PARSER_INCLUDE_DIR-NOTFOUND" "http_parser.h")
find_library(HTTP_PARSER_LIBRARY http_parser)
_CHECK(HTTP_PARSER_LIBRARY "HTTP_PARSER_LIBRARY-NOTFOUND" "libhttp_parser.so")

pkg_check_modules(PC_CURL "libcurl>=7.4.0")
find_path(CURL_INCLUDE_DIR "curl/curl.h"
    HINTS ${PC_CURL_INCLUDEDIR} ${PC_CURL_INCLUDE_DIRS})
_CHECK(CURL_INCLUDE_DIR "CURL_INCLUDE_DIR-NOTFOUND" "curl/curl.h")
find_library(CURL_LIBRARY curl
	HINTS ${PC_CURL_LIBDIR} ${PC_CURL_LIBRARY_DIRS})
_CHECK(CURL_LIBRARY "CURL_LIBRARY-NOTFOUND" "libcurl.so")

if (SYSTEMD_NOTIFY)
    # check systemd
    find_path(SYSTEMD_INCLUDE_DIR systemd/sd-daemon.h)
    _CHECK(SYSTEMD_INCLUDE_DIR "SYSTEMD_INCLUDE_DIR-NOTFOUND" "systemd/sd-daemon.h")
    find_library(SYSTEMD_LIBRARY systemd)
    _CHECK(SYSTEMD_LIBRARY "SYSTEMD_LIBRARY-NOTFOUND" "libsystemd.so")
endif()

if (ENABLE_SELINUX)
    pkg_check_modules(PC_SELINUX "libselinux>=2.0")
    find_path(SELINUX_INCLUDE_DIR "selinux/selinux.h"
        HINTS ${PC_SELINUX_INCLUDEDIR} ${PC_SELINUX_INCLUDE_DIRS})
    _CHECK(SELINUX_INCLUDE_DIR "SELINUX_INCLUDE_DIR-NOTFOUND" "selinux/selinux.h")
    find_library(SELINUX_LIBRARY selinux
        HINTS ${PC_SELINUX_LIBDIR} ${PC_SELINUX_LIBRARY_DIRS})
    _CHECK(SELINUX_LIBRARY "SELINUX_LIBRARY-NOTFOUND" "libselinux.so")
endif()

# check iSula libutils
pkg_check_modules(PC_ISULA_LIBUTILS REQUIRED "libisula")
find_path(ISULA_LIBUTILS_INCLUDE_DIR isula_libutils/log.h
	HINTS ${PC_ISULA_LIBUTILS_INCLUDEDIR} ${PC_ISULA_LIBUTILS_INCLUDE_DIRS})
_CHECK(ISULA_LIBUTILS_INCLUDE_DIR "ISULA_LIBUTILS_INCLUDE_DIR-NOTFOUND" "isula_libutils/log.h")

find_library(ISULA_LIBUTILS_LIBRARY isula_libutils
	HINTS ${PC_ISULA_LIBUTILS_LIBDIR} ${PC_ISULA_LIBUTILS_LIBRARY_DIRS})
_CHECK(ISULA_LIBUTILS_LIBRARY "ISULA_LIBUTILS_LIBRARY-NOTFOUND" "libisula_libutils.so")

find_library(ISULAD_SHIM_LIBUTILS_LIBRARY isulad_shim_libutils
    HINTS ${PC_ISULA_LIBUTILS_LIBDIR} ${PC_ISULA_LIBUTILS_LIBRARY_DIRS})
_CHECK(ISULAD_SHIM_LIBUTILS_LIBRARY "ISULAD_SHIM_LIBUTILS_LIBRARY-NOTFOUND" "libisulad_shim_libutils.a")

if (ENABLE_SHIM_V2)
    find_path(LIBSHIM_V2_INCLUDE_DIR shim_v2.h)
    _CHECK(LIBSHIM_V2_INCLUDE_DIR "LIBSHIM_V2_INCLUDE_DIR-NOTFOUND" "shim_v2.h")
    find_library(LIBSHIM_V2_LIBRARY shim_v2)
    _CHECK(LIBSHIM_V2_LIBRARY "LIBSHIM_V2_LIBRARY-NOTFOUND" "libshim_v2.so")
endif()

if (OPENSSL_VERIFY)
    find_path(OPENSSL_INCLUDE_DIR openssl/x509.h)
    _CHECK(OPENSSL_INCLUDE_DIR "OPENSSL_INCLUDE_DIR-NOTFOUND" "openssl/x509.h")
endif()

if (GRPC_CONNECTOR)
    # check protobuf
    if (ENABLE_CRI_API_V1)
        pkg_check_modules(PC_PROTOBUF "protobuf>=3.14.0")
    else()
        pkg_check_modules(PC_PROTOBUF "protobuf>=3.1.0")
    endif()
    find_library(PROTOBUF_LIBRARY protobuf
        HINTS ${PC_PROTOBUF_LIBDIR} ${PC_PROTOBUF_LIBRARY_DIRS})
    _CHECK(PROTOBUF_LIBRARY "PROTOBUF_LIBRARY-NOTFOUND" "libprotobuf.so")

    find_program(CMD_PROTOC protoc)
    _CHECK(CMD_PROTOC "CMD_PROTOC-NOTFOUND" "protoc")
    find_program(CMD_GRPC_CPP_PLUGIN grpc_cpp_plugin)
    _CHECK(CMD_GRPC_CPP_PLUGIN "CMD_GRPC_CPP_PLUGIN-NOTFOUND" "grpc_cpp_plugin")

    # check grpc
    if (ENABLE_CRI_API_V1)
        pkg_check_modules(PC_GRPC++ "grpc++>=1.41.0")
    endif()
    find_path(GRPC_INCLUDE_DIR grpc/grpc.h)
    _CHECK(GRPC_INCLUDE_DIR "GRPC_INCLUDE_DIR-NOTFOUND" "grpc/grpc.h")
    find_library(GRPC_PP_REFLECTION_LIBRARY grpc++_reflection)
    _CHECK(GRPC_PP_REFLECTION_LIBRARY "GRPC_PP_REFLECTION_LIBRARY-NOTFOUND" "libgrpc++_reflection.so")
    find_library(GRPC_PP_LIBRARY grpc++)
    _CHECK(GRPC_PP_LIBRARY "GRPC_PP_LIBRARY-NOTFOUND" "libgrpc++.so")
    find_library(GRPC_LIBRARY grpc)
    _CHECK(GRPC_LIBRARY "GRPC_LIBRARY-NOTFOUND" "libgrpc.so")
    find_library(GPR_LIBRARY gpr)
    _CHECK(GPR_LIBRARY "GPR_LIBRARY-NOTFOUND" "libgpr.so")
    # no check

    # The use of absl libraries depends on the version of protobuf and grpc.
    # Versions of protobuf before v22.0 do not require absl libraries at all. 
    # However, versions after v22.0 require the support of absl libraries. 
    # As a result, we skip the check for absl libraries in order to accommodate different protobuf and grpc versions.
    set(ISULAD_ABSL_USED_TARGETS)
    find_library(ABSL_SYNC_LIB absl_synchronization)
    if (ABSL_SYNC_LIB)
        set(ISULAD_ABSL_USED_TARGETS
            ${ISULAD_ABSL_USED_TARGETS}
            ${ABSL_SYNC_LIB}
        )
    endif()
    
    find_library(ABSL_CORD_LIB absl_cord)
    if (ABSL_CORD_LIB)
        set(ISULAD_ABSL_USED_TARGETS
            ${ISULAD_ABSL_USED_TARGETS}
            ${ABSL_CORD_LIB}
        )
    endif()
    
    find_library(ABSL_CORDZ_FUNCTIONS_LIB absl_cordz_functions)
    if (ABSL_CORDZ_FUNCTIONS_LIB)
        set(ISULAD_ABSL_USED_TARGETS
            ${ISULAD_ABSL_USED_TARGETS}
            ${ABSL_CORDZ_FUNCTIONS_LIB}
        )
    endif()
    
    find_library(ABSL_CORDZ_INFO_LIB absl_cordz_info)
    if (ABSL_CORDZ_INFO_LIB)
        set(ISULAD_ABSL_USED_TARGETS
            ${ISULAD_ABSL_USED_TARGETS}
            ${ABSL_CORDZ_INFO_LIB}
        )
    endif()
    
    find_library(ABSL_HASH_LIB absl_hash)
    if (ABSL_HASH_LIB)
        set(ISULAD_ABSL_USED_TARGETS
            ${ISULAD_ABSL_USED_TARGETS}
            ${ABSL_HASH_LIB}
        )
    endif()
    
    find_library(ABSL_LOG_INTERNAL_CHECK_OP_LIB absl_log_internal_check_op)
    if (ABSL_LOG_INTERNAL_CHECK_OP_LIB)
        set(ISULAD_ABSL_USED_TARGETS
            ${ISULAD_ABSL_USED_TARGETS}
            ${ABSL_LOG_INTERNAL_CHECK_OP_LIB}
        )
    endif()
    
    find_library(ABSL_LOG_INTERNAL_MESSAGE_LIB absl_log_internal_message)
    if (ABSL_LOG_INTERNAL_MESSAGE_LIB)
        set(ISULAD_ABSL_USED_TARGETS
            ${ISULAD_ABSL_USED_TARGETS}
            ${ABSL_LOG_INTERNAL_MESSAGE_LIB}
        )
    endif()
    
    find_library(ABSL_LOG_INTERNAL_NULLGUARD_LIB absl_log_internal_nullguard)
    if (ABSL_LOG_INTERNAL_NULLGUARD_LIB)
        set(ISULAD_ABSL_USED_TARGETS
            ${ISULAD_ABSL_USED_TARGETS}
            ${ABSL_LOG_INTERNAL_NULLGUARD_LIB}
        )
    endif()
    
    find_library(ABSL_STATUS_LIB absl_status)
    if (ABSL_STATUS_LIB)
        set(ISULAD_ABSL_USED_TARGETS
            ${ISULAD_ABSL_USED_TARGETS}
            ${ABSL_STATUS_LIB}
        )
    endif()

    # check websocket
    find_path(WEBSOCKET_INCLUDE_DIR libwebsockets.h)
    _CHECK(WEBSOCKET_INCLUDE_DIR "WEBSOCKET_INCLUDE_DIR-NOTFOUND" libwebsockets.h)
    find_library(WEBSOCKET_LIBRARY websockets)
    _CHECK(WEBSOCKET_LIBRARY "WEBSOCKET_LIBRARY-NOTFOUND" "libwebsockets.so")

    # check libncurses
    pkg_check_modules(PC_LIBNCURSES REQUIRED "ncurses")
    find_path(NCURSES_INCLUDE_DIR curses.h
        HINTS ${PC_NCURSES_INCLUDEDIR} ${PC_NCURSES_INCLUDE_DIRS})
    _CHECK(NCURSES_INCLUDE_DIR "NCURSES_INCLUDE_DIR-NOTFOUND" "curses.h")

    find_library(NCURSES_LIBRARY ncurses
        HINTS ${PC_NCURSES_LIBDIR} ${PC_NCURSES_LIBRARY_DIRS})
    _CHECK(NCURSES_LIBRARY "NCURSES_LIBRARY-NOTFOUND" "libncurses.so")
endif()

if ((NOT GRPC_CONNECTOR) OR (GRPC_CONNECTOR AND ENABLE_METRICS))
    pkg_check_modules(PC_EVENT "event>=2.1.8")
    find_path(EVENT_INCLUDE_DIR event.h
        HINTS ${PC_EVENT_INCLUDEDIR} ${PC_EVENT_INCLUDE_DIRS})
    _CHECK(EVENT_INCLUDE_DIR "EVENT_INCLUDE_DIR-NOTFOUND" "event.h")
    find_library(EVENT_LIBRARY event
        HINTS ${PC_EVENT_LIBDIR} ${PC_EVENT_LIBRARY_DIRS})
    _CHECK(EVENT_LIBRARY "EVENT_LIBRARY-NOTFOUND" "libevent.so")

    pkg_check_modules(PC_EVHTP "evhtp>=1.2.16")
    find_path(EVHTP_INCLUDE_DIR evhtp/evhtp.h
        HINTS ${PC_EVHTP_INCLUDEDIR} ${PC_EVHTP_INCLUDE_DIRS})
    _CHECK(EVHTP_INCLUDE_DIR "EVHTP_INCLUDE_DIR-NOTFOUND" "evhtp/evhtp.h")
    find_library(EVHTP_LIBRARY evhtp
        HINTS ${PC_EVHTP_LIBDIR} ${PC_EVHTP_LIBRARY_DIRS})
    _CHECK(EVHTP_LIBRARY "EVHTP_LIBRARY-NOTFOUND" "libevhtp.so")
endif()

if (ENABLE_OCI_IMAGE)
    # check devmapper
    find_path(DEVMAPPER_INCLUDE_DIR libdevmapper.h)
    _CHECK(DEVMAPPER_INCLUDE_DIR "DEVMAPPER_INCLUDE_DIR-NOTFOUND" "libdevmapper.h")
    find_library(DEVMAPPER_LIBRARY devmapper)
    _CHECK(DEVMAPPER_LIBRARY "DEVMAPPER_LIBRARY-NOTFOUND" "libdevmapper.so")

    # check libarchive
    pkg_check_modules(PC_LIBARCHIVE REQUIRED "libarchive>=3.4")
    find_path(LIBARCHIVE_INCLUDE_DIR archive.h
        HINTS ${PC_LIBARCHIVE_INCLUDEDIR} ${PC_LIBARCHIVE_INCLUDE_DIRS})
    _CHECK(LIBARCHIVE_INCLUDE_DIR "LIBARCHIVE_INCLUDE_DIR-NOTFOUND" "archive.h")
    find_library(LIBARCHIVE_LIBRARY archive
        HINTS ${PC_LIBARCHIVE_LIBDIR} ${PC_LIBARCHIVE_LIBRARY_DIRS})
    _CHECK(LIBARCHIVE_LIBRARY "LIBARCHIVE_LIBRARY-NOTFOUND" "libarchive.so")
endif()

if (ENABLE_EMBEDDED_IMAGE)
    pkg_check_modules(PC_SQLITE3 "sqlite3>=3.7.17")
    find_path(SQLIT3_INCLUDE_DIR sqlite3.h
        HINTS ${PC_SQLITE3_INCLUDEDIR} ${PC_SQLITE3_INCLUDE_DIRS})
    _CHECK(SQLIT3_INCLUDE_DIR "SQLIT3_INCLUDE_DIR-NOTFOUND" "sqlite3.h")
    find_library(SQLITE3_LIBRARY sqlite3
        HINTS ${PC_SQLITE3_LIBDIR} ${PC_SQLITE3_LIBRARY_DIRS})
    _CHECK(SQLITE3_LIBRARY "SQLITE3_LIBRARY-NOTFOUND" "libsqlite3.so")
endif()
