cmake_minimum_required(VERSION 3.10)
project(SpinnakerAcquisition VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ---------------------------
# 源文件
# ---------------------------
set(APP_SOURCES
    main.cpp
    acquisition.cpp
    async_writer.cpp
    camera_setup.cpp
    config.cpp
    console_ctrl.cpp
    file_naming.cpp
    folder_dialog.cpp
    overlay.cpp
    text_utils.cpp
    text_render.cpp
)
foreach(_src IN LISTS APP_SOURCES)
    if(NOT EXISTS "${CMAKE_SOURCE_DIR}/${_src}")
        message(FATAL_ERROR "Required source not found: ${CMAKE_SOURCE_DIR}/${_src}")
    endif()
endforeach()
add_executable(${PROJECT_NAME}
    ${APP_SOURCES}
    "app.rc"           # 图标资源
)

# 源码含中文，MSVC 必须按 UTF-8 编译（只对 C++ 文件生效，不影响 .rc 资源文件）
if(MSVC)
    target_compile_options(${PROJECT_NAME} PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/utf-8>)
endif()

# ---------------------------
# OpenCV
# ---------------------------
# Windows 默认路径，可根据实际修改
if(WIN32)
    set(OpenCV_DIR "D:/CppLibrary/opencv/build")  # 指向 OpenCVConfig.cmake 所在目录
endif()

find_package(OpenCV REQUIRED)

if(OpenCV_FOUND)
    message(STATUS "Found OpenCV version ${OpenCV_VERSION}")
    target_include_directories(${PROJECT_NAME} PRIVATE ${OpenCV_INCLUDE_DIRS})
    target_link_libraries(${PROJECT_NAME} PRIVATE ${OpenCV_LIBS})
else()
    message(FATAL_ERROR "OpenCV not found!")
endif()

# ---------------------------
# Spinnaker SDK
# ---------------------------
if(WIN32)
    # Windows 路径
    set(SPINNAKER_DIR "${CMAKE_SOURCE_DIR}/lib64/vs2015")  # 放置 .lib 文件
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${CMAKE_SOURCE_DIR}/include"
        "${CMAKE_SOURCE_DIR}/include/Spinnaker"
        "${CMAKE_SOURCE_DIR}/include/SpinGenApi"
    )
    file(GLOB SPINNAKER_LIBS "${SPINNAKER_DIR}/*.lib")
    if(SPINNAKER_LIBS)
        target_link_libraries(${PROJECT_NAME} PRIVATE ${SPINNAKER_LIBS})
    else()
        message(WARNING "No Spinnaker .lib files found in ${SPINNAKER_DIR}")
    endif()
else()
    # Linux 路径
    set(SPINNAKER_DIR "/opt/spinnaker")   # 根据实际安装修改
    target_include_directories(${PROJECT_NAME} PRIVATE
        "${SPINNAKER_DIR}/include"
        "${SPINNAKER_DIR}/include/Spinnaker"
        "${SPINNAKER_DIR}/include/SpinGenApi"
    )
    link_directories("${SPINNAKER_DIR}/lib")
    file(GLOB SPINNAKER_LIBS
        "${SPINNAKER_DIR}/lib/libSpinnaker.so*"
        "${SPINNAKER_DIR}/lib/libGenApi*.so*"
    )
    target_link_libraries(${PROJECT_NAME} PRIVATE ${SPINNAKER_LIBS} pthread)
endif()

# 选择文件夹对话框（folder_dialog.cpp）需要的系统库
if(WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE ole32 shell32 uuid)
endif()

# ---------------------------
# 输出路径
# ---------------------------
set_target_properties(${PROJECT_NAME} PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)

# ---------------------------
# 构建后自动复制运行所需文件（仅 Windows）
#   1) config.ini    ：目标目录里没有才复制，不会覆盖同学改过的配置
#   2) OpenCV DLL    ：Debug 用 opencv_world*d.dll，其余配置用 opencv_world*.dll
#   3) Spinnaker DLL ：自动在下列位置查找，找不到可手动指定：
#                      cmake .. -DSPINNAKER_BIN_DIR="C:/Program Files/.../bin64/vs2015"
# 目标目录用 $<TARGET_FILE_DIR:...>，VS 多配置生成器下会自动落到 bin/Release 或 bin/Debug
# ---------------------------
set(SPINNAKER_BIN_DIR "" CACHE PATH "Spinnaker DLL 所在目录（留空则自动查找）")

if(WIN32)
    # ---- 1) config.ini ----
    set(CONFIG_SRC "${CMAKE_SOURCE_DIR}/config.ini")
    if(EXISTS "${CONFIG_SRC}")
        # 生成一个小脚本：目标文件不存在才复制
        file(WRITE "${CMAKE_BINARY_DIR}/copy_if_missing.cmake"
"if(NOT EXISTS \"\${DST}\")
    execute_process(COMMAND \"${CMAKE_COMMAND}\" -E copy \"\${SRC}\" \"\${DST}\")
endif()
")
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND}
                "-DSRC=${CONFIG_SRC}"
                "-DDST=$<TARGET_FILE_DIR:${PROJECT_NAME}>/config.ini"
                -P "${CMAKE_BINARY_DIR}/copy_if_missing.cmake"
            VERBATIM)
    else()
        message(WARNING "config.ini not found next to CMakeLists.txt, it will not be copied. "
                        "(The program still generates a default one on first run.)")
    endif()

    # ---- 2) OpenCV DLL ----
    set(_ocv_bin_candidates "")
    if(OpenCV_ARCH AND OpenCV_RUNTIME)
        list(APPEND _ocv_bin_candidates "${OpenCV_DIR}/${OpenCV_ARCH}/${OpenCV_RUNTIME}/bin")
    endif()
    if(OpenCV_LIB_PATH)
        list(APPEND _ocv_bin_candidates "${OpenCV_LIB_PATH}/../bin")
    endif()
    list(APPEND _ocv_bin_candidates
        "${OpenCV_DIR}/x64/vc16/bin"
        "${OpenCV_DIR}/x64/vc17/bin"
        "${OpenCV_DIR}/bin")

    set(_ocv_dll_release "")
    set(_ocv_dll_debug "")
    foreach(_dir IN LISTS _ocv_bin_candidates)
        file(GLOB _ocv_all "${_dir}/opencv_world*.dll")
        if(_ocv_all)
            set(_ocv_rel ${_ocv_all})
            set(_ocv_dbg ${_ocv_all})
            list(FILTER _ocv_rel EXCLUDE REGEX "[0-9]d\\.dll$")
            list(FILTER _ocv_dbg INCLUDE REGEX "[0-9]d\\.dll$")
            if(_ocv_rel)
                list(GET _ocv_rel 0 _ocv_dll_release)
            endif()
            if(_ocv_dbg)
                list(GET _ocv_dbg 0 _ocv_dll_debug)
            endif()
            break()
        endif()
    endforeach()

    if(_ocv_dll_release)
        if(NOT _ocv_dll_debug)
            message(WARNING "OpenCV debug DLL (opencv_world*d.dll) not found; Debug builds will copy the release DLL and may fail to run.")
            set(_ocv_dll_debug "${_ocv_dll_release}")
        endif()
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<IF:$<CONFIG:Debug>,${_ocv_dll_debug},${_ocv_dll_release}>"
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            VERBATIM)
        message(STATUS "OpenCV DLL will be copied: ${_ocv_dll_release}")
    else()
        message(WARNING "opencv_world*.dll not found near ${OpenCV_DIR}; OpenCV DLLs will NOT be copied.")
    endif()

    # ---- 3) Spinnaker DLL ----
    set(_spin_candidates
        ${SPINNAKER_BIN_DIR}
        "${CMAKE_SOURCE_DIR}/bin64/vs2015"
        "${CMAKE_SOURCE_DIR}/lib64/vs2015"
        "C:/Program Files/Teledyne/Spinnaker/bin64/vs2015"
        "C:/Program Files/FLIR Systems/Spinnaker/bin64/vs2015")

    set(_spin_dlls "")
    foreach(_dir IN LISTS _spin_candidates)
        if(EXISTS "${_dir}")
            # 覆盖 Spinnaker_v140.dll / SpinnakerC_v140.dll 以及 GenApi/GCBase/Log/MathParser/NodeMapData/XmlParser_*_Spinnaker.dll
            file(GLOB _found "${_dir}/*Spinnaker*.dll")
            if(_found)
                set(_spin_dlls ${_found})
                set(_spin_dll_dir "${_dir}")
                break()
            endif()
        endif()
    endforeach()

    if(_spin_dlls)
        add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                ${_spin_dlls}
                "$<TARGET_FILE_DIR:${PROJECT_NAME}>"
            VERBATIM)
        message(STATUS "Spinnaker DLLs will be copied from: ${_spin_dll_dir}")
    else()
        message(WARNING "Spinnaker DLLs (*Spinnaker*.dll) not found. Spinnaker DLLs will NOT be copied. "
                        "Re-run with -DSPINNAKER_BIN_DIR=<folder containing Spinnaker_v140.dll>")
    endif()
endif()

message("***************************")
message("******Done!!!!!!!!!!!******")
message("***************************")