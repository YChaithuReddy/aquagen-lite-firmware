# AquaGen Lite — ESP32 Modbus → Azure IoT Hub flow-meter gateway
# Top-level project CMake. Build with: idf.py set-target esp32 && idf.py build
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(aquagen_lite)
