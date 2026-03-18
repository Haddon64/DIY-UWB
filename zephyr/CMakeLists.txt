cmake_minimum_required(VERSION 3.13.1)
include($ENV{ZEPHYR_BASE}/cmake/app/boilerplate.cmake NO_POLICY_SCOPE)
project(DIY-UWB)

# ── dwt_uwb_driver (Qorvo DW3110 low-level driver) ───────────────────────────
set(DWT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../lib/dwt_uwb_driver)

target_sources(app PRIVATE
    # Application sources
    ../src/main.c

    # DW3xxx core driver
    ${DWT_DIR}/deca_interface.c
    ${DWT_DIR}/deca_compat.c
    ${DWT_DIR}/deca_rsl.c

    # DW3000/QM33110 chip-specific driver
    ${DWT_DIR}/dw3000/dw3000_device.c

    # Math library (used internally by the driver)
    ${DWT_DIR}/lib/qmath/src/qmath.c
)

target_include_directories(app PRIVATE
    ${DWT_DIR}
    ${DWT_DIR}/dw3000
    ${DWT_DIR}/lib/qmath/include
)
