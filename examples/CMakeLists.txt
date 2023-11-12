add_executable(example-echo echo.cpp)
target_compile_options(example-echo PRIVATE ${PYROFLING_CXX_FLAGS})
target_link_libraries(example-echo PRIVATE pyrofling-ipc)

add_executable(example-pyro pyro.cpp)
target_compile_options(example-pyro PRIVATE ${PYROFLING_CXX_FLAGS})
target_link_libraries(example-pyro PRIVATE pyrofling-ipc pyro-server pyro-client granite-util)

add_executable(encode-test vulkan_encode.cpp)
target_compile_options(encode-test PRIVATE ${PYROFLING_CXX_FLAGS})
target_link_libraries(encode-test PRIVATE granite-vulkan)

