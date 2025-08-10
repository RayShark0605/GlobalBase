# 建议显式列出源文件；若想省事，可用 GLOB（已加 CONFIGURE_DEPENDS）
file(GLOB_RECURSE GlobalBase_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.inl")

add_library(GlobalBase SHARED ${GlobalBase_SOURCES})

# 头文件搜索路径：若有 include 目录，改成 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
target_include_directories(GlobalBase
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

# 若你的头里用 GLOBALBASE_EXPORTS 切换 dllexport/dllimport，这里给库自身定义
target_compile_definitions(GlobalBase PRIVATE GLOBALBASE_EXPORTS)

# Windows 下可选自动导出（当未显式写 dllexport 时）
if (WIN32 AND EXPORT_ALL_SYMBOLS)
  set_target_properties(GlobalBase PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()

# 归组到 VS 解决方案文件夹（可选）
set_property(TARGET GlobalBase PROPERTY FOLDER "libs")

# 可选安装规则
install(TARGETS GlobalBase
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin
        ARCHIVE DESTINATION lib)
