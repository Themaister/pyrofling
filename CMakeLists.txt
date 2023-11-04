cmake_minimum_required(VERSION 3.5)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_C_STANDARD 99)
project(pyrofling LANGUAGES CXX C)

if (CMAKE_COMPILER_IS_GNUCXX OR (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang"))
    set(PYROFLING_CXX_FLAGS -Wshadow -Wall -Wextra -Wno-comment -Wno-missing-field-initializers -Wno-empty-body -fvisibility=hidden)
    if (${CMAKE_CXX_COMPILER_ID} MATCHES "Clang")
        set(PYROFLING_CXX_FLAGS ${PYROFLING_CXX_FLAGS} -Wno-backslash-newline-escape)
    endif()
    if (NOT (${CMAKE_BUILD_TYPE} MATCHES "Release"))
        message("Enabling frame pointer for profiling/debug.")
        set(PYROFLING_CXX_FLAGS ${PYROFLING_CXX_FLAGS} -fno-omit-frame-pointer)
    endif()
    if (CMAKE_SYSTEM_PROCESSOR MATCHES "(x86)|(X86)|(amd64)|(AMD64)")
        message("Enabling SSE3 support.")
        set(PYROFLING_CXX_FLAGS ${PYROFLING_CXX_FLAGS} -msse3)
    endif()
elseif (MSVC)
    set(PYROFLING_CXX_FLAGS /D_CRT_SECURE_NO_WARNINGS /wd4267 /wd4244 /wd4309 /wd4005 /MP)
endif()

option(PYROFLING_LAYER_ONLY "Only build capture layer." OFF)

include(GNUInstallDirs)

if (NOT WIN32)
    add_subdirectory(ipc)
endif()

# Disable stuff we don't need. We'll use slangmosh to precompile the two shaders we care about.
set(GRANITE_VULKAN_SYSTEM_HANDLES ON CACHE BOOL "" FORCE)
set(GRANITE_RENDERER ON CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SHADER_COMPILER_OPTIMIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SHADER_MANAGER_RUNTIME_COMPILER OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_FOSSILIZE OFF CACHE BOOL "" FORCE)
set(GRANITE_VULKAN_SPIRV_CROSS OFF CACHE BOOL "" FORCE)
set(GRANITE_SHIPPING ON CACHE BOOL "" FORCE)

if (NOT PYROFLING_LAYER_ONLY)
    set(GRANITE_FFMPEG ON CACHE BOOL "" FORCE)
    set(GRANITE_AUDIO ON CACHE BOOL "" FORCE)
endif()

add_library(granite-application-interface-query STATIC pyrofling_app_query.cpp)

add_subdirectory(Granite EXCLUDE_FROM_ALL)

add_subdirectory(proto)

add_library(pyrofling-socket STATIC simple_socket.cpp simple_socket.hpp)
target_include_directories(pyrofling-socket PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(pyrofling-socket PRIVATE ${PYROFLING_CXX_FLAGS})
target_link_libraries(pyrofling-socket PRIVATE pyro-protocol)

if (NOT WIN32)
    add_subdirectory(pyro-server)
endif()
add_subdirectory(pyro-client)

if (NOT PYROFLING_LAYER_ONLY)
    if (NOT WIN32)
        add_subdirectory(examples)
        add_executable(pyrofling pyrofling.cpp virtual_gamepad.cpp virtual_gamepad.hpp)
        target_compile_options(pyrofling PRIVATE ${PYROFLING_CXX_FLAGS})
        target_link_libraries(pyrofling PRIVATE
                pyro-protocol pyrofling-ipc granite-threading granite-vulkan granite-video granite-audio pyro-server)
        install(TARGETS pyrofling)
    endif()

	add_blob_archive_target(viewer-fonts viewer_fonts
        ${CMAKE_CURRENT_SOURCE_DIR}/Granite/assets/fonts/font.ttf fonts/font.ttf)

    add_granite_application(pyrofling-viewer pyrofling_viewer.cpp)
    target_link_libraries(pyrofling-viewer PRIVATE granite-video granite-audio pyro-client viewer-fonts)
    if (WIN32)
            target_link_libraries(pyrofling-viewer PRIVATE winmm)
    endif()
    install(TARGETS pyrofling-viewer)
endif()

if (NOT WIN32)
	add_subdirectory(layer-util)
	add_subdirectory(capture-layer)
	add_subdirectory(cross-wsi-layer)
endif()
