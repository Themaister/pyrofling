add_library(pyro-server STATIC pyro_server.cpp pyro_server.hpp)
target_include_directories(pyro-server PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(pyro-server PRIVATE ${PYROFLING_CXX_FLAGS})
target_link_libraries(pyro-server PUBLIC pyro-protocol pyrofling-ipc granite-util lt-codec)