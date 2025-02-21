cmake_minimum_required(VERSION 3.8)

project(ONET)

add_executable(ONET src/main.cpp)

target_include_directories(ONET PUBLIC ${ONET_INCLUDES})
