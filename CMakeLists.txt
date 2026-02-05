cmake_minimum_required(VERSION 3.20)

project(GlobalBaseAndTest LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
foreach(cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${cfg}" cfgU)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfgU} ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfgU} ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfgU} ${CMAKE_BINARY_DIR}/lib)
endforeach()

# ---------------------------
# 3rdParty: libcurl (static)
# ---------------------------
set(libcurlRoot "${CMAKE_CURRENT_SOURCE_DIR}/3rdParty/libcurl_8.18.0")
set(libcurlIncludeDir "${libcurlRoot}/include")
set(libcurlLibDir "${libcurlRoot}/lib/Linux")

set(libcurlLibs
    "${libcurlLibDir}/libcurl.a"
    "${libcurlLibDir}/libnghttp3.a"
    "${libcurlLibDir}/libnghttp2.a"
    "${libcurlLibDir}/libssh2.a"
    "${libcurlLibDir}/libcares.a"
    "${libcurlLibDir}/libidn2.a"
    "${libcurlLibDir}/libunistring.a"
    "${libcurlLibDir}/libpsl.a"
    "${libcurlLibDir}/libzstd.a"
    "${libcurlLibDir}/libz.a"
    "${libcurlLibDir}/libbrotlidec.a"
    "${libcurlLibDir}/libbrotlienc.a"
    "${libcurlLibDir}/libbrotlicommon.a"
    "${libcurlLibDir}/libcrypto.a"
    "${libcurlLibDir}/libssl.a"
)

add_library(ThirdParty_libcurl INTERFACE)
add_library(ThirdParty::libcurl ALIAS ThirdParty_libcurl)

target_include_directories(ThirdParty_libcurl INTERFACE "${libcurlIncludeDir}")

find_package(Threads REQUIRED)
find_package(OpenSSL REQUIRED)

target_link_libraries(ThirdParty_libcurl INTERFACE
    Threads::Threads
    dl
    OpenSSL::SSL
    OpenSSL::Crypto
)

target_link_libraries(ThirdParty_libcurl INTERFACE
    "-Wl,--start-group"
    ${libcurlLibs}
    "-Wl,--end-group"
)

set_target_properties(ThirdParty_libcurl PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "${libcurlIncludeDir}"

  IMPORTED_LOCATION "${libcurlLibRelease}"
  IMPORTED_LOCATION_NOCONFIG "${libcurlLibRelease}"

  IMPORTED_LOCATION_DEBUG "${libcurlLibDebug}"
  IMPORTED_LOCATION_RELEASE "${libcurlLibRelease}"
  IMPORTED_LOCATION_RELWITHDEBINFO "${libcurlLibRelease}"
  IMPORTED_LOCATION_MINSIZEREL "${libcurlLibRelease}"
)

set_property(TARGET ThirdParty_libcurl PROPERTY MAP_IMPORTED_CONFIG_NOCONFIG Release)
set_property(TARGET ThirdParty_libcurl PROPERTY MAP_IMPORTED_CONFIG_RELWITHDEBINFO Release)
set_property(TARGET ThirdParty_libcurl PROPERTY MAP_IMPORTED_CONFIG_MINSIZEREL Release)

add_subdirectory(GlobalBase)
add_subdirectory(Test)

