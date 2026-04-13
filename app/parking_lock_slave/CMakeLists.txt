cmake_minimum_required(VERSION 3.20.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(parking_lock_slave)

target_sources(app PRIVATE src/main.cpp)
