# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Junesgone

cmake_minimum_required(VERSION 3.16)

# Firmware version reported in the boot banner ("App version") and used for the
# GitHub release tag v0.0.1.
set(PROJECT_VER "0.0.1")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(voice_logger)
