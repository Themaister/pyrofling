set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_library(pyrofling-ipc STATIC
        listener.cpp listener.hpp
        messages.cpp messages.hpp
        client.cpp client.hpp
        file_handle.cpp file_handle.hpp)
target_include_directories(pyrofling-ipc PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(pyrofling-ipc PRIVATE ${PYROFLING_CXX_FLAGS})

if (NOT WIN32)
    target_link_libraries(pyrofling-ipc PUBLIC -pthread)
endif()

