project(tsr CXX)
cmake_minimum_required(VERSION 3.10)
add_library(tsr)

# # Sources
target_sources(tsr PRIVATE
    src/udta.cpp
    src/transfer.cpp
)
target_include_directories(tsr PUBLIC include)
