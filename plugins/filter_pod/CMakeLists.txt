# Filter Pod Plugin
set(src
        filter_pod.c
        pod_meta.c
)

FLB_PLUGIN(filter_pod "${src}" "")

target_include_directories(flb-plugin-filter_pod PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
)