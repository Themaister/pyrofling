add_executable(example-echo echo.cpp)
target_compile_options(example-echo PRIVATE ${PYROFLING_CXX_FLAGS})
target_link_libraries(example-echo PRIVATE pyrofling-ipc)
