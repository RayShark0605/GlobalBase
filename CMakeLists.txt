cmake_minimum_required(VERSION 3.20)

# 解决方案名
project(GlobalBaseAndTest LANGUAGES CXX)

# 可改：C++ 标准（默认 17）
#set(PROJECT_CXX_STANDARD 17 CACHE STRING "C++ standard (e.g. 17/20)")
set(CMAKE_CXX_STANDARD ${PROJECT_CXX_STANDARD})
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# 统一输出目录（多配置生成器 VS/MSBuild 也覆盖）
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
foreach(cfg IN ITEMS Debug Release RelWithDebInfo MinSizeRel)
  string(TOUPPER "${cfg}" cfgU)
  set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${cfgU} ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_LIBRARY_OUTPUT_DIRECTORY_${cfgU} ${CMAKE_BINARY_DIR}/bin)
  set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY_${cfgU} ${CMAKE_BINARY_DIR}/lib)
endforeach()

# Windows 可选：自动导出符号（若源码未写 dllexport）
option(EXPORT_ALL_SYMBOLS "Export all symbols for SHARED libs on Windows" OFF)
set_property(GLOBAL PROPERTY USE_FOLDERS ON)

# VS 下把启动项目设为 Test（F5 直接跑）
if (MSVC)
  set_property(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR} PROPERTY VS_STARTUP_PROJECT Test)
endif()

add_subdirectory(GlobalBase)
add_subdirectory(Test)
