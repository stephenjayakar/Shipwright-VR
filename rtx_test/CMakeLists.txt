cmake_minimum_required(VERSION 3.20)
project(rtx_test LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)

add_executable(rtx_test rtx_test.cpp)
target_link_libraries(rtx_test PRIVATE d3d12 dxgi dxguid)
