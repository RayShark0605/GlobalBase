# 建议显式列出源文件；若想省事，可用 GLOB（已加 CONFIGURE_DEPENDS）
file(GLOB_RECURSE GlobalBase_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.inl")

add_library(GlobalBase STATIC ${GlobalBase_SOURCES})

target_include_directories(GlobalBase
  PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
)

target_link_libraries(GlobalBase PRIVATE ThirdParty::libcurl)

target_compile_definitions(GlobalBase PRIVATE GLOBALBASE_EXPORTS)


if (WIN32 AND EXPORT_ALL_SYMBOLS)
  set_target_properties(GlobalBase PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS ON)
endif()


set_property(TARGET GlobalBase PROPERTY FOLDER "libs")


install(TARGETS GlobalBase
        RUNTIME DESTINATION bin
        LIBRARY DESTINATION bin
        ARCHIVE DESTINATION lib)
