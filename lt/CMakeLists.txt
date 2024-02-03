add_library(lt-codec lt_decode.cpp lt_decode.hpp lt_encode.cpp lt_encode.hpp lt_shuffle.cpp lt_shuffle.hpp)
target_include_directories(lt-codec PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(lt-codec PRIVATE ${PYROFLING_CXX_FLAGS})

add_executable(lt-test lt_test.cpp)
target_link_libraries(lt-test PRIVATE lt-codec)
target_compile_options(lt-test PRIVATE ${PYROFLING_CXX_FLAGS})
