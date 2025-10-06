# Finds all source files in the current directory
# and save the name to the DIR_ALL_SRCS variable
aux_source_directory(. DIR_ALL_SRCS)

include_directories(../lib/Config)
include_directories(../lib/GUI)
include_directories(../lib/Infrared)
include_directories(../lib/LCD)
include_directories(../lib/Icm20948)

# create an executable file
add_executable(main ${DIR_ALL_SRCS})

# enable usb output, disable uart output
pico_enable_stdio_usb(main 1)
pico_enable_stdio_uart(main 0)

# create map/bin/hex/uf2 file etc.
pico_add_extra_outputs(main)

target_include_directories(main PRIVATE ${CMAKE_CURRENT_LIST_DIR}) # for our common lwipopts

target_compile_definitions(main PRIVATE
        WIFI_SSID=\"${WIFI_SSID}\"
        WIFI_PASSWORD=\"${WIFI_PASSWORD}\"
        )

target_link_libraries(main
    LCD
    Infrared
    Icm20948
    GUI
    Fonts
    Config
    pico_stdlib
    hardware_clocks
    hardware_spi
    pico_cyw43_arch_lwip_threadsafe_background
    )
