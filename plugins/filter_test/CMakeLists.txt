# Filter Test Plugin
set(src
        filter_test.c
)

# 添加插件到构建系统
FLB_PLUGIN(filter_test "${src}" "")

# 设置编译选项
target_include_directories(filter_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)