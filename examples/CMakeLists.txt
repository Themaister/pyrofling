add_executable(example-echo echo.cpp)
target_compile_options(example-echo PRIVATE ${PYROFLING_CXX_FLAGS})
target_link_libraries(example-echo PRIVATE pyrofling-ipc)

add_executable(example-pyro pyro.cpp)
target_compile_options(example-pyro PRIVATE ${PYROFLING_CXX_FLAGS})
target_link_libraries(example-pyro PRIVATE pyrofling-ipc pyrofling-socket pyro-protocol granite-util)

