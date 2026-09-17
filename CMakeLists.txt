cmake_minimum_required(VERSION 3.25)

# Прошивка устройства стенда CONTR (ТЗ 19, план 22). Одно дерево, две сборки:
#   target — STM32F767ZI через cmake/arm-none-eabi.cmake (пресет `target`);
#   host   — логика core/ с заглушками hal/host и тестами ctest (пресет `host`).
project(contr_fw VERSION 0.0.1 LANGUAGES C ASM)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

set(BOARD_DIR ${CMAKE_SOURCE_DIR}/board/rev1)
set(GEN_DIR ${CMAKE_SOURCE_DIR}/gen)

if(CMAKE_SYSTEM_NAME STREQUAL "Generic")
    set(CONTR_TARGET ON)
else()
    set(CONTR_TARGET OFF)
endif()

add_compile_options(-Wall -Wextra -Wshadow -Wconversion -Wundef)

# ---- логика, общая для обеих сборок ------------------------------------------
add_library(contr_core STATIC
    core/blink_echo.c
    ${GEN_DIR}/relay_table.c
)
target_include_directories(contr_core PUBLIC core hal ${GEN_DIR})

if(CONTR_TARGET)
    # ---- сборка для платы -----------------------------------------------------
    set(CMSIS_CORE ${CMAKE_SOURCE_DIR}/third_party/cmsis_core/CMSIS/Core/Include)
    set(CMSIS_F7 ${CMAKE_SOURCE_DIR}/third_party/cmsis_device_f7)
    if(NOT EXISTS ${CMSIS_CORE}/core_cm7.h OR NOT EXISTS ${CMSIS_F7}/Include/stm32f7xx.h)
        message(FATAL_ERROR "Подмодули не получены: git submodule update --init")
    endif()

    add_library(contr_hal STATIC
        hal/target/hal_target.c
        ${CMSIS_F7}/Source/Templates/system_stm32f7xx.c
        ${CMSIS_F7}/Source/Templates/gcc/startup_stm32f767xx.s
    )
    target_include_directories(contr_hal PUBLIC hal)
    target_include_directories(contr_hal SYSTEM PUBLIC ${CMSIS_CORE} ${CMSIS_F7}/Include)
    target_compile_definitions(contr_hal PUBLIC STM32F767xx)
    # шаблоны ST собираются с предупреждениями, которые не наши
    set_source_files_properties(${CMSIS_F7}/Source/Templates/system_stm32f7xx.c
        PROPERTIES COMPILE_OPTIONS "-w")
    target_link_libraries(contr_core PUBLIC contr_hal)

    add_executable(contr-fw app/main.c)
    set_target_properties(contr-fw PROPERTIES SUFFIX ".elf")
    target_link_libraries(contr-fw PRIVATE contr_core contr_hal)
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
        COMMENT "contr-fw.hex / contr-fw.bin"
    )
else()
    # ---- сборка для хоста: заглушки и тесты (FW-246) ---------------------------
    add_library(contr_hal STATIC hal/host/hal_host.c)
    target_include_directories(contr_hal PUBLIC hal hal/host)
    target_link_libraries(contr_core PUBLIC contr_hal)

    enable_testing()
    foreach(name IN ITEMS test_core test_relay_table)
        add_executable(${name} tests/host/${name}.c)
        target_link_libraries(${name} PRIVATE contr_core)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endif()
