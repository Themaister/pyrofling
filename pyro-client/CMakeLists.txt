add_library(pyro-client STATIC pyro_client.cpp pyro_client.hpp)
target_include_directories(pyro-client PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(pyro-client PRIVATE ${PYROFLING_CXX_FLAGS})
target_link_libraries(pyro-client PUBLIC pyro-protocol granite-util pyrofling-socket)
