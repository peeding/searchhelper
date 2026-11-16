cmake_minimum_required(VERSION 3.21)
project(SearchHelper VERSION 3.0 LANGUAGES CXX)

add_executable(SearchHelper WIN32 src/main.cpp)
target_compile_features(SearchHelper PRIVATE cxx_std_17)
target_compile_definitions(SearchHelper PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_link_libraries(SearchHelper PRIVATE comctl32 uxtheme)

if(MSVC)
    target_compile_options(SearchHelper PRIVATE /W4 /permissive- /utf-8)
endif()
