file(GLOB_RECURSE Test_SOURCES CONFIGURE_DEPENDS
     "${CMAKE_CURRENT_SOURCE_DIR}/*.c"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cc"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cxx"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.h"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.hpp"
     "${CMAKE_CURRENT_SOURCE_DIR}/*.inl")

add_executable(Test ${Test_SOURCES})

# 链接到 GlobalBase 动态库
target_link_libraries(Test PRIVATE GlobalBase)

# 若有专门的头目录可加到这里
target_include_directories(Test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# 归组到 VS 解决方案文件夹（可选）
set_property(TARGET Test PROPERTY FOLDER "apps")

# Windows：构建后把 GlobalBase.dll 复制到 Test.exe 同目录，便于直接运行
if (WIN32)
  add_custom_command(TARGET Test POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:GlobalBase>
            $<TARGET_FILE_DIR:Test>)
endif()

# 可选安装规则
install(TARGETS Test RUNTIME DESTINATION bin)
