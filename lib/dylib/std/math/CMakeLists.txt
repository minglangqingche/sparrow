cmake_minimum_required(VERSION 3.31.2)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

project(sprmath C)

include_directories(/Users/minglangqingche/sparrow/dev ./include)

set(SRC src/main.c)

add_library(${PROJECT_NAME} SHARED ${SRC})

# 暴露头文件目录（让使用库的项目能找到头文件）
target_include_directories(${PROJECT_NAME} PUBLIC
    ./include/libsprmath.h
)
