cmake_minimum_required(VERSION 3.25)

# Прошивка устройства стенда CONTR (ТЗ 19, план 22). Одно дерево, две сборки:
#   target — STM32F767ZI через cmake/arm-none-eabi.cmake (пресет `target`);
#   host   — логика core/ с заглушками hal/host: тесты ctest и процесс contr-host
#            для прогонщика диалогов (пресет `host`).
# Версия проекта — версия прошивки (*IDN?, заголовок образа).
project(contr_fw VERSION 0.2.5 LANGUAGES C ASM)

option(CONTR_HIL_HANG_AFTER_INIT "HIL ONLY: hang after boot check to test watchdog rollback" OFF)

option(CONTR_BRINGUP "Наладочная сборка с командами DBG:* (план 22, шаг 1)" OFF)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON) # strcasecmp, fileno

set(BOARD_DIR ${CMAKE_SOURCE_DIR}/board/rev1)
set(GEN_DIR ${CMAKE_SOURCE_DIR}/gen)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    set(CONTR_TARGET ON)
    add_compile_definitions(CONTR_TARGET=1)
else()
    set(CONTR_TARGET OFF)
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wundef)
add_compile_definitions(
    FW_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
    FW_VERSION_MINOR=${PROJECT_VERSION_MINOR}
    FW_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)
if(CONTR_BRINGUP)
    add_compile_definitions(CONTR_BRINGUP=1)
endif()

# ---- логика, общая для обеих сборок (А2: core компилируется без изменений) ----------
set(CORE_SOURCES
    core/core.c
    core/cmd.c
    core/errlog.c
    core/cfg.c
    core/netcfg.c
    core/update.c
    core/safe.c
    core/sr.c
    core/signals.c
    core/dbg.c
    ${GEN_DIR}/relay_table.c
    ${GEN_DIR}/signal_table.c
)

function(contr_core_library name hal_target)
    add_library(${name} STATIC ${CORE_SOURCES})
    target_include_directories(${name} PUBLIC core hal ${GEN_DIR})
    target_link_libraries(${name} PUBLIC ${hal_target})
endfunction()

if(CONTR_TARGET)
    # ---- сборка для платы -----------------------------------------------------
    set(CMSIS_CORE ${CMAKE_SOURCE_DIR}/third_party/cmsis_core/CMSIS/Core/Include)
    set(CMSIS_F7 ${CMAKE_SOURCE_DIR}/third_party/cmsis_device_f7)
    set(LWIP_DIR ${CMAKE_SOURCE_DIR}/third_party/lwip)
    if(NOT EXISTS ${CMSIS_CORE}/core_cm7.h OR NOT EXISTS ${CMSIS_F7}/Include/stm32f7xx.h
       OR NOT EXISTS ${LWIP_DIR}/src/Filelists.cmake)
        message(FATAL_ERROR "Подмодули не получены: git submodule update --init")
    endif()

    # LwIP: ядро, IPv4, Ethernet — raw API, NO_SYS=1 (А14)
    include(${LWIP_DIR}/src/Filelists.cmake)
    add_library(lwip STATIC ${lwipcore_SRCS} ${lwipcore4_SRCS} ${LWIP_DIR}/src/netif/ethernet.c)
    target_include_directories(lwip SYSTEM PUBLIC ${LWIP_DIR}/src/include hal/target/net)
    target_compile_options(lwip PRIVATE -w)

    add_library(contr_hal STATIC
        hal/target/init.c
        hal/target/clock.c
        hal/target/gpio.c
        hal/target/sr.c
        hal/target/spia.c
        hal/target/i2c.c
        hal/target/wdt.c
        hal/target/console.c
        hal/target/flash.c
        hal/target/net/eth.c
        hal/target/net/net.c
        ${CMSIS_F7}/Source/Templates/system_stm32f7xx.c
        ${CMSIS_F7}/Source/Templates/gcc/startup_stm32f767xx.s
    )
    target_include_directories(contr_hal PUBLIC hal hal/target hal/target/net)
    target_include_directories(contr_hal SYSTEM PUBLIC ${CMSIS_CORE} ${CMSIS_F7}/Include)
    # ROM dual-boot leaves VTOR pointing at system memory. Relocate before IRQs.
    target_compile_definitions(contr_hal PUBLIC STM32F767xx USER_VECT_TAB_ADDRESS)
    set_source_files_properties(${CMSIS_F7}/Source/Templates/system_stm32f7xx.c PROPERTIES COMPILE_OPTIONS "-w")
    target_link_libraries(contr_hal PUBLIC lwip)

    contr_core_library(contr_core contr_hal)
    target_link_libraries(contr_hal PUBLIC contr_core) # колбэки core_net_* из core

    if(CONTR_HIL_HANG_AFTER_INIT)
        message(WARNING "HIL fault image: intentionally hangs after core_init; never deploy as production")
        add_executable(contr-fw tests/hil/hang_main.c)
    else()
        add_executable(contr-fw app/main.c)
    endif()
    set_target_properties(contr-fw PROPERTIES SUFFIX ".elf")
    target_link_libraries(contr-fw PRIVATE contr_core contr_hal lwip)
    target_link_options(contr-fw PRIVATE
        -T${BOARD_DIR}/STM32F767ZITx_FLASH.ld
        -Wl,-Map=${CMAKE_BINARY_DIR}/contr-fw.map
        -Wl,--gc-sections
        -Wl,--no-warn-rwx-segments
        -Wl,--print-memory-usage
    )
    set_property(TARGET contr-fw APPEND PROPERTY LINK_DEPENDS ${BOARD_DIR}/STM32F767ZITx_FLASH.ld)

    add_custom_command(TARGET contr-fw POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O ihex $<TARGET_FILE:contr-fw> ${CMAKE_BINARY_DIR}/contr-fw.hex
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:contr-fw> ${CMAKE_BINARY_DIR}/contr-fw.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:contr-fw>
        COMMAND ${CMAKE_COMMAND} -E env python ${CMAKE_SOURCE_DIR}/tools/mkimage.py
                ${CMAKE_BINARY_DIR}/contr-fw.bin ${CMAKE_BINARY_DIR}/contr-fw.upd
        COMMENT "contr-fw.hex / contr-fw.bin / contr-fw.upd"
    )
else()
    # ---- сборка для хоста: заглушки и тесты (FW-246) ---------------------------
    add_library(contr_hal_mem STATIC hal/host/stubs.c hal/host/mem_channels.c)
    target_include_directories(contr_hal_mem PUBLIC hal hal/host)
    contr_core_library(contr_core_mem contr_hal_mem)
    target_link_libraries(contr_hal_mem PUBLIC contr_core_mem)

    add_library(contr_hal_sock STATIC hal/host/stubs.c hal/host/sock_channels.c)
    target_include_directories(contr_hal_sock PUBLIC hal hal/host)
    contr_core_library(contr_core_sock contr_hal_sock)
    target_link_libraries(contr_hal_sock PUBLIC contr_core_sock)
    if(WIN32)
        target_link_libraries(contr_hal_sock PUBLIC ws2_32)
    endif()

    add_executable(contr-host app/host_main.c)
    target_link_libraries(contr-host PRIVATE contr_core_sock contr_hal_sock)

    enable_testing()
    file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/host/test_*.c)
    foreach(src IN LISTS TEST_SOURCES)
        get_filename_component(name ${src} NAME_WE)
        add_executable(${name} ${src})
        target_include_directories(${name} PRIVATE tests/host)
        target_link_libraries(${name} PRIVATE contr_core_mem contr_hal_mem)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endif()
