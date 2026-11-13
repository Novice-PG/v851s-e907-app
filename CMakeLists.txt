# Copyright (c) 2026
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.28.0)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})

project(v851s_e907_bringup)

target_sources(app PRIVATE
  src/main.c
  src/openamp_slave.c
  src/linkr_bridge.c
  src/board_early.c
)
