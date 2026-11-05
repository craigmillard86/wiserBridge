# WiserBridge sniffer - SEPARATE firmware: passive IEEE 802.15.4 capture.
# It replaces the router firmware on the board while it is flashed.
cmake_minimum_required(VERSION 3.16)
set(PROJECT_VER "0.1.0")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(wiser_bridge_sniffer)
