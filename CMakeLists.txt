# ============================================================================
# voice_player_learn —— 学习沙盒
#
# M1：Qt6 骨架 + 空窗口
# M2：引入 FFmpeg（av_utils.h 需要它），并新增一个独立测试目标
#     之后 M7 会再引入 SDL3
# ============================================================================

# 3.21 是 Qt6 官方推荐的 CMake 下限；用太老的 CMake 会缺 target 相关特性。
cmake_minimum_required(VERSION 3.21)

# LANGUAGES CXX：本工程只用 C++。
# （FFmpeg 是纯头文件 + 预编译库，不需要 C 编译器，所以保持 CXX 即可）
project(voice_player_learn VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)   # 编译器不支持 C++17 就直接报错，别偷偷降级
set(CMAKE_CXX_EXTENSIONS OFF)         # 关闭 gnu++17 之类的编译器扩展，保证可移植

# AUTOMOC：扫描带 Q_OBJECT 的头文件，自动生成 moc_xxx.cpp。
set(CMAKE_AUTOMOC ON)
# AUTORCC：自动把 .qrc 资源文件编译进可执行文件（M13 用图标时才会生效）。
set(CMAKE_AUTORCC ON)
# 注意：这里【没有】AUTOUIC —— 界面全部用 C++ 代码手写，不需要 uic。

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "构建类型" FORCE)
endif()

# ---------------------------------------------------------------------------
# 1. Qt6
# ---------------------------------------------------------------------------
if(DEFINED ENV{QT_ROOT})
    list(APPEND CMAKE_PREFIX_PATH "$ENV{QT_ROOT}")
endif()
find_package(Qt6 6.2 REQUIRED COMPONENTS Widgets)

# ===========================================================================
# 2. FFmpeg（★ M2 新增）
# ===========================================================================
# 目标：把 FFmpeg 的头文件目录和 5 个库找出来，供所有目标链接。
#
# 为什么不 find_package(FFmpeg)？
#   FFmpeg 官方【不提供 CMake 配置文件】（没有 FFmpegConfig.cmake），
#   所以 find_package 根本用不了，只能自己用 find_path/find_library 手工探测。
#   这是 FFmpeg 在 CMake 项目里的常态，几乎所有工程都这么干。
# ---------------------------------------------------------------------------

# 允许三种方式指定 FFmpeg 位置，优先级从高到低：
#   ① cmake 命令行 -DFFMPEG_ROOT=...
#   ② 环境变量 FFMPEG_ROOT
#   ③ vcpkg 的默认安装路径（本机就是这里）
set(FFMPEG_ROOT "" CACHE PATH "FFmpeg 开发包根目录（含 include/、lib/、bin/）")

if(NOT FFMPEG_ROOT AND DEFINED ENV{FFMPEG_ROOT})
    set(FFMPEG_ROOT "$ENV{FFMPEG_ROOT}")
endif()

if(NOT FFMPEG_ROOT AND
   EXISTS "C:/vcpkg/installed/x64-windows/include/libavformat/avformat.h")
    set(FFMPEG_ROOT "C:/vcpkg/installed/x64-windows")
endif()

# 找不到就立刻失败，并明确告诉用户该怎么做——而不是留一堆看不懂的编译错误。
if(NOT FFMPEG_ROOT)
    message(FATAL_ERROR
        "未找到 FFmpeg 开发包。请安装 FFmpeg 7+（如 vcpkg install ffmpeg），\n"
        "并用 -DFFMPEG_ROOT=<dev包根目录> 指向它（目录下需有 include/、lib/、bin/）。")
endif()
message(STATUS "FFmpeg 根目录: ${FFMPEG_ROOT}")

# 本项目用到的 5 个 FFmpeg 组件：
#   avformat    解复用/封装（打开文件、读包）
#   avcodec     编解码（解码音视频帧）
#   avutil      公共工具（内存、时间、数学、错误码）
#   swresample  音频重采样（采样率/采样格式/声道布局转换）
#   swscale     图像缩放与像素格式转换
set(_FFMPEG_COMPONENTS avformat avcodec avutil swresample swscale)

set(_FFMPEG_LIBS "")
foreach(_comp IN LISTS _FFMPEG_COMPONENTS)
    # 头文件：确认 lib<组件>/<组件>.h 存在（如 libavformat/avformat.h）
    find_path(_FFMPEG_${_comp}_INC NAMES lib${_comp}/${_comp}.h
              HINTS "${FFMPEG_ROOT}/include" NO_DEFAULT_PATH)
    # Release 库
    find_library(_FFMPEG_${_comp}_REL NAMES ${_comp}
                 HINTS "${FFMPEG_ROOT}/lib" NO_DEFAULT_PATH)
    # Debug 库（可选）：vcpkg 默认布局不含 debug/lib，所以通常找不到
    find_library(_FFMPEG_${_comp}_DBG NAMES ${_comp}
                 HINTS "${FFMPEG_ROOT}/debug/lib" NO_DEFAULT_PATH)

    if(NOT _FFMPEG_${_comp}_INC OR NOT _FFMPEG_${_comp}_REL)
        message(FATAL_ERROR "未找到 FFmpeg 组件 ${_comp}（检查 FFMPEG_ROOT 下的 include/lib）。")
    endif()

    # 有 Debug 库就按构建类型二选一，没有就统一用 Release。
    # 这是 M1 里说的"Debug/Release 混用"坑的正解：
    #   $<IF:$<CONFIG:Debug>,<Debug时用>,<否则用>>
    # 是 CMake 的"生成器表达式"，在生成构建脚本时才求值。
    if(_FFMPEG_${_comp}_DBG)
        list(APPEND _FFMPEG_LIBS
             "$<IF:$<CONFIG:Debug>,${_FFMPEG_${_comp}_DBG},${_FFMPEG_${_comp}_REL}>")
    else()
        list(APPEND _FFMPEG_LIBS "${_FFMPEG_${_comp}_REL}")
    endif()
endforeach()

message(STATUS "FFmpeg 组件: ${_FFMPEG_COMPONENTS}")

# ★ 关键一步（我第一次就漏了）：
#   find_path 只是"把找到的路径存进一个变量"，它【不会】自动把头文件目录
#   加进编译命令。要让编译器真的 -I 上，必须显式声明。
#
# 这里用一个 INTERFACE 库把"头文件目录 + 5 个库"打包成一个逻辑目标 vpl_ffmpeg：
#   INTERFACE 目标不产出任何文件，只用来"携带使用要求(usage requirements)"。
#   好处是以后新增目标时只写一句 target_link_libraries(x PRIVATE vpl_ffmpeg)，
#   头文件路径和库都会自动带上，不会像现在这样漏一处就编译不过。
add_library(vpl_ffmpeg INTERFACE)
target_include_directories(vpl_ffmpeg INTERFACE "${FFMPEG_ROOT}/include")
target_link_libraries(vpl_ffmpeg INTERFACE ${_FFMPEG_LIBS})

# ===========================================================================
# 3. 公共编译选项（抽成函数，避免每个目标重复写一遍）
# ===========================================================================
function(vpl_apply_common_options tgt)
    if(MSVC)
        target_compile_options(${tgt} PRIVATE /utf-8 /W3 /permissive-)
        target_compile_definitions(${tgt} PRIVATE
            _CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
    endif()
endfunction()

# 构建后把 FFmpeg 的运行时 DLL 拷到 exe 旁边。
# 不拷的话程序一运行就弹"找不到 avcodec-63.dll"。
function(vpl_copy_runtime_dlls tgt)
    if(WIN32)
        file(GLOB _FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
        if(_FFMPEG_DLLS)
            add_custom_command(TARGET ${tgt} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        ${_FFMPEG_DLLS} "$<TARGET_FILE_DIR:${tgt}>"
                COMMENT "拷贝 FFmpeg 运行时 DLL 到 ${tgt} 输出目录")
        endif()
    endif()
endfunction()

# ===========================================================================
# 4. 主程序
# ===========================================================================
# 仍然没有 WIN32 关键字 —— 保留控制台看日志。
add_executable(voice_player_learn
    src/main.cpp
)
target_include_directories(voice_player_learn PRIVATE src)
target_link_libraries(voice_player_learn PRIVATE
    Qt6::Widgets
    vpl_ffmpeg
)
vpl_apply_common_options(voice_player_learn)
vpl_copy_runtime_dlls(voice_player_learn)

# ===========================================================================
# 5. 测试目标（★ M2 新增，学习沙盒专用）
# ===========================================================================
# 说明：最终工程里没有这个目标。它只是学习阶段的"实验台"——
#       让每个基础设施模块都能脱离 GUI 单独跑起来验证。
#       M3/M4/M5 的队列和时钟，都会各自加一个这样的目标。
add_executable(test_av_utils
    tests/test_av_utils.cpp
)
target_include_directories(test_av_utils PRIVATE src)
target_link_libraries(test_av_utils PRIVATE vpl_ffmpeg)
vpl_apply_common_options(test_av_utils)
vpl_copy_runtime_dlls(test_av_utils)

# ===========================================================================
# 6. M3 测试目标：线程安全队列
# ===========================================================================
# packet_queue.cpp / frame_queue.cpp 是真正的实现文件，必须一起编译进来。
# （主程序 voice_player_learn 现在还不需要它们，等到 M8 才会加上。）
add_executable(test_queues
    tests/test_queues.cpp
    src/packet_queue.cpp
    src/frame_queue.cpp
)
target_include_directories(test_queues PRIVATE src)
target_link_libraries(test_queues PRIVATE vpl_ffmpeg)
vpl_apply_common_options(test_queues)
vpl_copy_runtime_dlls(test_queues)

# ===========================================================================
# 7. M4 测试目标：内核消息机制
# ===========================================================================
# message_queue 不依赖 FFmpeg，但仍链接 vpl_ffmpeg —— 保持所有测试目标
# 配置一致，省得以后加东西时漏改。
add_executable(test_message_queue
    tests/test_message_queue.cpp
    src/message_queue.cpp
)
target_include_directories(test_message_queue PRIVATE src)
target_link_libraries(test_message_queue PRIVATE vpl_ffmpeg)
vpl_apply_common_options(test_message_queue)
vpl_copy_runtime_dlls(test_message_queue)

# ===========================================================================
# 8. M5 测试目标：同步时钟
# ===========================================================================
# clock.h 是 header-only（实现在类体内），所以不需要额外的 .cpp。
# 但要链接 vpl_ffmpeg —— 因为它用了 av_gettime_relative()（在 libavutil 里）。
add_executable(test_clock
    tests/test_clock.cpp
)
target_include_directories(test_clock PRIVATE src)
target_link_libraries(test_clock PRIVATE vpl_ffmpeg)
vpl_apply_common_options(test_clock)
vpl_copy_runtime_dlls(test_clock)

# ===========================================================================
# 9. 工具目标（★ M6 新增）：测试素材生成器
# ===========================================================================
# 课程目录里没有任何音视频素材，vcpkg 也没带 ffmpeg.exe。
# M6 之后每个模块都需要真实文件，所以用 FFmpeg 的编码 API 现场造一个。
# 它不属于最终工程，纯粹是开发期的辅助工具。
add_executable(make_test_media
    tools/make_test_media.cpp
)
target_include_directories(make_test_media PRIVATE src)
target_link_libraries(make_test_media PRIVATE vpl_ffmpeg)
vpl_apply_common_options(make_test_media)
vpl_copy_runtime_dlls(make_test_media)

# ===========================================================================
# 10. M6 测试目标：解码器封装
# ===========================================================================
# decoder.cpp 需要 packet_queue.cpp（上游）和 frame_queue.cpp（abort 时发信号）。
add_executable(test_decoder
    tests/test_decoder.cpp
    src/decoder.cpp
    src/packet_queue.cpp
    src/frame_queue.cpp
)
target_include_directories(test_decoder PRIVATE src)
target_link_libraries(test_decoder PRIVATE vpl_ffmpeg)
vpl_apply_common_options(test_decoder)
vpl_copy_runtime_dlls(test_decoder)

# ===========================================================================
# 11. SDL3（★ M7 新增）
# ===========================================================================
# 本目录内置了一份 SDL3 的 VC 开发包（third_party/SDL3），所以我们【不用】
# 系统安装的 SDL3，直接指向内置的那份，保证换台机器也能编译。
#
# 为什么不像 Qt6 那样用 find_package(SDL3)？
#   内置包里的 SDL3Config.cmake 没有做"架构探测"（它不知道要挑
#   lib/x64 还是 lib/x86），直接 find_package 会因为找不到对应架构的
#   库而失败。所以这里按目录结构【手工创建】一个导入目标 SDL3::SDL3 ——
#   最土但最稳，也顺便让你看清"导入一个预编译库"到底需要哪几样东西：
#     ① 头文件目录  ② 链接用的 .lib  ③ 运行时用的 .dll
# ---------------------------------------------------------------------------
set(SDL3_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/SDL3"
    CACHE PATH "SDL3 开发包根目录")

# 按目标架构挑库目录。MSVC 在 ARM64 上不会定义 CMAKE_SIZEOF_VOID_P，
# 所以先用 MSVC_CXX_ARCHITECTURE_ID 判一次，再退回按指针宽度判。
if(MSVC_CXX_ARCHITECTURE_ID MATCHES "ARM64")
    set(_SDL3_ARCH arm64)
elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(_SDL3_ARCH x64)
else()
    set(_SDL3_ARCH x86)
endif()

set(_SDL3_INCLUDE_DIR "${SDL3_ROOT}/include")
set(_SDL3_IMPLIB      "${SDL3_ROOT}/lib/${_SDL3_ARCH}/SDL3.lib")
set(_SDL3_DLL         "${SDL3_ROOT}/lib/${_SDL3_ARCH}/SDL3.dll")

if(NOT EXISTS "${_SDL3_IMPLIB}")
    message(FATAL_ERROR "未找到 ${_SDL3_IMPLIB}，请检查 third_party/SDL3 目录。")
endif()

add_library(SDL3::SDL3 SHARED IMPORTED)
set_target_properties(SDL3::SDL3 PROPERTIES
    INTERFACE_INCLUDE_DIRECTORIES "${_SDL3_INCLUDE_DIR}"
    IMPORTED_IMPLIB               "${_SDL3_IMPLIB}"
    IMPORTED_LOCATION             "${_SDL3_DLL}")
message(STATUS "SDL3 (${_SDL3_ARCH}): ${_SDL3_IMPLIB}")

# 构建后把 SDL3.dll 拷到 exe 旁边（否则运行时报缺 DLL）
function(vpl_copy_sdl3_dll tgt)
    if(WIN32)
        add_custom_command(TARGET ${tgt} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${_SDL3_DLL}" "$<TARGET_FILE_DIR:${tgt}>"
            COMMENT "拷贝 SDL3.dll")
    endif()
endfunction()

# ===========================================================================
# 12. M7 测试目标：图像转换器
# ===========================================================================
add_executable(test_imagescaler
    tests/test_imagescaler.cpp
    src/imagescaler.cpp
    src/decoder.cpp
    src/packet_queue.cpp
    src/frame_queue.cpp
)
target_include_directories(test_imagescaler PRIVATE src)
target_link_libraries(test_imagescaler PRIVATE vpl_ffmpeg)
vpl_apply_common_options(test_imagescaler)
vpl_copy_runtime_dlls(test_imagescaler)

# ===========================================================================
# 13. M7 测试目标：音频输出设备
# ===========================================================================
add_executable(test_audiodevice
    tests/test_audiodevice.cpp
    src/audiodevice.cpp
)
target_include_directories(test_audiodevice PRIVATE src)
target_link_libraries(test_audiodevice PRIVATE
    SDL3::SDL3
    vpl_ffmpeg
)
vpl_apply_common_options(test_audiodevice)
vpl_copy_runtime_dlls(test_audiodevice)
vpl_copy_sdl3_dll(test_audiodevice)

# ===========================================================================
# 14. M8 测试目标：FFPlayer 解复用阶段
# ===========================================================================
# ⚠ M9 起本目标也需要 SDL3：因为 ffplayer.h 现在 include 了 audiodevice.h。
#   教训：**改了头文件的依赖，要检查所有引用它的目标**，否则那个目标会
#   编译失败 —— 而如果它是个独立可执行文件，你会不小心跑到上一次的
#   旧产物上，得到一个"假通过"。
add_executable(test_ffplayer
    tests/test_ffplayer_demux.cpp
    src/ffplayer.cpp
    src/audiodevice.cpp
    src/decoder.cpp
    src/packet_queue.cpp
    src/frame_queue.cpp
    src/message_queue.cpp
)
target_include_directories(test_ffplayer PRIVATE src)
target_link_libraries(test_ffplayer PRIVATE
    SDL3::SDL3
    vpl_ffmpeg
)
vpl_apply_common_options(test_ffplayer)
vpl_copy_runtime_dlls(test_ffplayer)
vpl_copy_sdl3_dll(test_ffplayer)

# ===========================================================================
# 15. M9 测试目标：音频链路 + 音频主时钟
# ===========================================================================
# 这一轮开始需要 SDL3（音频输出）和 swresample（在 vpl_ffmpeg 里）。
add_executable(test_ffplayer_audio
    tests/test_ffplayer_audio.cpp
    src/ffplayer.cpp
    src/audiodevice.cpp
    src/decoder.cpp
    src/packet_queue.cpp
    src/frame_queue.cpp
    src/message_queue.cpp
)
target_include_directories(test_ffplayer_audio PRIVATE src)
target_link_libraries(test_ffplayer_audio PRIVATE
    SDL3::SDL3
    vpl_ffmpeg
)
vpl_apply_common_options(test_ffplayer_audio)
vpl_copy_runtime_dlls(test_ffplayer_audio)
vpl_copy_sdl3_dll(test_ffplayer_audio)
