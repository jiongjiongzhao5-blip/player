cmake_minimum_required(VERSION 3.21)

project(voice_player_learn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set (CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

if(DEFINED ENV{QT_ROOT})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_ROOT}")
endif()

find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

add_executable(voice_player_learn
    src/main.cpp
)

target_link_libraries(voice_player_learn PRIVATE Qt6::Widgets)

if(MSVC)
    target_compile_options(voice_player_learn PRIVATE
        /utf-8
        /W3
        /permissive-
    )
    target_compile_definitions(voice_player_learn PRIVATE
        _CRT_SECURE_NO_WARNINGS
        NOMINMAX
        WIN32_LEAN_AND_MEAN
    )
endif()