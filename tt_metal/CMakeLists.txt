
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/common)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/jit_build)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/llrt)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/tools)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/impl)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/detail)

set(TT_METAL_OBJECTS
    ${CMAKE_CURRENT_SOURCE_DIR}/tt_metal.cpp
    $<TARGET_OBJECTS:profiler>
    $<TARGET_OBJECTS:common>
    $<TARGET_OBJECTS:jit_build>
    $<TARGET_OBJECTS:llrt>
    $<TARGET_OBJECTS:impl>
    $<TARGET_OBJECTS:detail>
)

add_library(tt_metal ${TT_METAL_OBJECTS})

target_link_libraries(tt_metal PUBLIC umd_device metal_common_libs)

target_precompile_headers(tt_metal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/magic_enum/magic_enum.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/tracy/public/tracy/Tracy.hpp
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fmt/fmt/core.h
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fmt/fmt/format.h
    <functional>
    <map>
    <memory>
    <unordered_map>
    <variant>
    <vector>
)

target_link_libraries(tt_metal PUBLIC compiler_flags linker_flags metal_header_directories $<$<BOOL:$ENV{ENABLE_TRACY}>:TracyClient>)  # linker_flags = -rdynamic if tracy enabled
target_link_directories(tt_metal PUBLIC ${PROJECT_BINARY_DIR}/lib)    # required so tt_metal can find device library
target_include_directories(tt_metal PUBLIC
    ${UMD_HOME}
    ${PROJECT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${PROJECT_SOURCE_DIR}/tt_metal/third_party/fmt
)
target_compile_options(tt_metal PUBLIC -Wno-int-to-pointer-cast)
add_dependencies(tt_metal hw_toolchain)

set_target_properties(tt_metal PROPERTIES
    INSTALL_RPATH "${PROJECT_BINARY_DIR}/lib"
    ADDITIONAL_CLEAN_FILES "${PROJECT_BINARY_DIR}/lib;${PROJECT_BINARY_DIR}/obj"
)
