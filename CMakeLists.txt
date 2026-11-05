# WiserBridge - ESP32-C6 Zigbee router (range extender) firmware
# ESP-IDF project root. Build with:  idf.py set-target esp32c6 && idf.py build
cmake_minimum_required(VERSION 3.16)

# Firmware version reported over serial and in the Basic cluster SW build ID.
set(PROJECT_VER "0.1.0")

# The sniffer is a separate ESP-IDF project in ./sniffer and is NOT part of this build.
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wiser_bridge_router)
