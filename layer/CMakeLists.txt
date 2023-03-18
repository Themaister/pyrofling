add_library(VkLayer_pyrofling_capture SHARED
        dispatch.cpp dispatch_helper.cpp dispatch_helper.hpp)
target_link_libraries(VkLayer_pyrofling_capture PRIVATE pyrofling-ipc granite-volk-headers)

target_include_directories(VkLayer_pyrofling_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(VkLayer_pyrofling_capture PRIVATE ${PYROFLING_CXX_FLAGS})
set_target_properties(VkLayer_pyrofling_capture PROPERTIES LINK_FLAGS "${PYROFLING_LINK_FLAGS}")
install(TARGETS VkLayer_pyrofling_capture
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})

file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/layer)

if (CMAKE_SIZEOF_VOID_P EQUAL 8)
	set(PYROFLING_ABI_TAG "_64")
else()
	set(PYROFLING_ABI_TAG "_32")
endif()

# First is a JSON file which can be used directly from the build folder.
if (WIN32)
    set(PYROFLING_LAYER_PATH ".\\\\VkLayer_pyrofling_capture${PYROFLING_ABI_TAG}.dll")
else()
    set(PYROFLING_LAYER_PATH "./libVkLayer_pyrofling_capture${PYROFLING_ABI_TAG}.so")
endif()

configure_file(${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_pyrofling_capture.json.in ${CMAKE_BINARY_DIR}/layer/VkLayer_pyrofling_capture${PYROFLING_ABI_TAG}.json @ONLY)

if (WIN32)
    set_target_properties(VkLayer_pyrofling_capture PROPERTIES PREFIX "" LIBRARY_OUTPUT_NAME VkLayer_pyrofling_capture${PYROFLING_ABI_TAG})
    if (CMAKE_COMPILER_IS_GNUCXX)
        if (${CMAKE_BUILD_TYPE} MATCHES "Release")
            set_target_properties(VkLayer_pyrofling_capture PROPERTIES LINK_FLAGS "${PYROFLING_LINK_FLAGS} -Wl,--no-undefined -static -s")
        else()
            set_target_properties(VkLayer_pyrofling_capture PROPERTIES LINK_FLAGS "${PYROFLING_LINK_FLAGS} -Wl,--no-undefined -static")
        endif()
    endif()
    # Normally, we would have to install the layer into the registry, but that is way too annoying for something like this.
    # Just place the layer along the dll on Windows if we're actually bothering to install.
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_pyrofling_capture.json.in ${CMAKE_BINARY_DIR}/VkLayer_pyrofling_capture${PYROFLING_ABI_TAG}.json @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/VkLayer_pyrofling_capture${PYROFLING_ABI_TAG}.json DESTINATION ${CMAKE_INSTALL_BINDIR})
else()
    set_target_properties(VkLayer_pyrofling_capture PROPERTIES LIBRARY_OUTPUT_NAME VkLayer_pyrofling_capture${PYROFLING_ABI_TAG})
    set(PYROFLING_LAYER_PATH "${CMAKE_INSTALL_PREFIX}/${CMAKE_INSTALL_LIBDIR}/libVkLayer_pyrofling_capture${PYROFLING_ABI_TAG}.so")
    configure_file(${CMAKE_CURRENT_SOURCE_DIR}/VkLayer_pyrofling_capture.json.in ${CMAKE_BINARY_DIR}/VkLayer_pyrofling_capture${PYROFLING_ABI_TAG}.json @ONLY)
    install(FILES ${CMAKE_BINARY_DIR}/VkLayer_pyrofling_capture${PYROFLING_ABI_TAG}.json DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/vulkan/implicit_layer.d)
    if (${CMAKE_BUILD_TYPE} MATCHES "Release")
        set_target_properties(VkLayer_pyrofling_capture PROPERTIES LINK_FLAGS "${PYROFLING_LINK_FLAGS} -Wl,--no-undefined -s")
    else()
        set_target_properties(VkLayer_pyrofling_capture PROPERTIES LINK_FLAGS "${PYROFLING_LINK_FLAGS} -Wl,--no-undefined")
    endif()
endif()

set_target_properties(VkLayer_pyrofling_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_BINARY_DIR}/layer")
set_target_properties(VkLayer_pyrofling_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_BINARY_DIR}/layer")
set_target_properties(VkLayer_pyrofling_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_BINARY_DIR}/layer")
set_target_properties(VkLayer_pyrofling_capture PROPERTIES RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_BINARY_DIR}/layer")
