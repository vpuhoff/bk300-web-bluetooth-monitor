# Top-level project file for ESP-IDF.
# В IDF "project name" задаёт имя бинаря и используется в build-каталоге.
cmake_minimum_required(VERSION 3.16)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(bk300_monitor)
