file(GLOB_RECURSE Test_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.inl")

add_executable(Test ${Test_SOURCES})

target_link_libraries(Test PRIVATE ThirdParty::libcurl)
target_link_libraries(Test PRIVATE GlobalBase)

target_include_directories(Test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

set_property(TARGET Test PROPERTY FOLDER "apps")

if (WIN32)
  add_custom_command(TARGET Test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:GlobalBase>
            $<TARGET_FILE_DIR:Test>)
endif()

install(TARGETS Test RUNTIME DESTINATION bin)
