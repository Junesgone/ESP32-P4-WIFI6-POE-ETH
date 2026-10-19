# Self-contained copy for ESP32-P4-WIFI6-POE-ETH.
# The upstream project pulled in ../../../config/esp32p4_local_defaults.cmake from
# the ESP32-P4-Platform repo; the ESP32-P4 rev v3.x profile is set in
# sdkconfig.defaults here instead.
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
set(COMPONENTS main)
project(sd_card)
