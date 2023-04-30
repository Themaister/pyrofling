add_library(pyrofling-layer-util dispatch_helper.cpp dispatch_helper.hpp dispatch_wrapper.hpp)
target_include_directories(pyrofling-layer-util PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(pyrofling-layer-util PRIVATE ${PYROFLING_CXX_FLAGS})
