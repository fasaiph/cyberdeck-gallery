# ESP32-S3-Touch-AMOLED-1.43 (non-C) - on-flash PNG image swipe gallery
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
add_compile_options("-Wno-format")
project(07_SD_Image_Gallery)

# Build a read-only FAT image from ./flash_data (contains makeup/ and hinge/)
# and flash it into the "storage" partition alongside the app.
fatfs_create_rawflash_image(storage ${CMAKE_SOURCE_DIR}/flash_data FLASH_IN_PROJECT)
