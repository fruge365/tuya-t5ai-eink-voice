##
# @file CMakeLists.txt
# @brief eink_chat - eink AI chat app for TUYA_T5AI_EINK_NFC
#/

# APP_PATH
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# APP_NAME
get_filename_component(APP_NAME ${APP_PATH} NAME)

# APP_SRCS
aux_source_directory(${APP_PATH}/src APP_SRCS)

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRCS}
    )

# Beken bk_cli（日志口 shell 的命令注册接口）+ libopus 头文件（小智协议编解码）
target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${CMAKE_SOURCE_DIR}/platform/T5AI/t5_os/ap/components/bk_cli/include/bk_private
        ${CMAKE_SOURCE_DIR}/platform/T5AI/t5_os/ap/components/bk_websocket/include
        ${CMAKE_SOURCE_DIR}/platform/T5AI/tuyaos/tuyaos_adapter/include/opus/include
    )
