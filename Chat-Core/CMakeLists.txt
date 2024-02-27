cmake_minimum_required(VERSION 3.8)

project(CHAT_CORE)

add_executable(CHAT_CORE
src/main.cpp
 "headers/ONET.h"  "headers/tsQueue.h" "headers/tsVector.h")

target_include_directories(CHAT_CORE PUBLIC 
headers
extern/asio/asio/include
extern/crc/inc
)