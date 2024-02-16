cmake_minimum_required(VERSION 3.8)

project(CHAT_CORE)

add_executable(CHAT_CORE
src/main.cpp
)

target_include_directories(CHAT_CORE PUBLIC headers)