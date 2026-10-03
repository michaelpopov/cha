# Small source fixes for the pinned dependencies. Only write changed files so
# repeated configuration does not force a rebuild.
set(libuv_common_source "${libuv_SOURCE_DIR}/src/uv-common.c")
file(READ "${libuv_common_source}" original)
# The public model pointer is const; its owned allocation must still be freed.
string(REPLACE "uv__free(cpu_infos[i].model);"
    "uv__free((void*) cpu_infos[i].model);" patched "${original}")
if(NOT patched STREQUAL original)
    file(WRITE "${libuv_common_source}" "${patched}")
endif()
