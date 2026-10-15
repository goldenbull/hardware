cmake_minimum_required(VERSION 3.16)

# 板级驱动在上一级 components/ 里，几个工程共用
set(EXTRA_COMPONENT_DIRS
    ../components/board
    ../components/display
    ../components/mic
    ../components/sht4x
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lcd-clock)
