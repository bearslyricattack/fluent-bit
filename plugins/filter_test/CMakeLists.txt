# Filter Test Plugin
set(src
        filter_test.c
)

FLB_PLUGIN(filter_test "${src}" "")

target_include_directories(flb-plugin-filter_test PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)
