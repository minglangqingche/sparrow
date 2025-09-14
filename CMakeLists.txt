cmake_minimum_required(VERSION 3.31.2)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# debug时为了避免优化对代码结构的改变，使用无优化版本 -O0
# 实测发现 -O2 和 -O3 都不能使代码通过测试，使用 -O1 可以通过测试
add_definitions(-g -O1)

project(spr C)

set(SRC ${PROJECT_SOURCE_DIR}/src)
set(DEV ${PROJECT_SOURCE_DIR}/dev)

aux_source_directory(${SRC}             SRC_CODE)
aux_source_directory(${SRC}/cli         CLI_CODE)
aux_source_directory(${SRC}/compiler    COMPILER_CODE)
aux_source_directory(${SRC}/include     INCLUDE_CODE)
aux_source_directory(${SRC}/object      OBJ_CODE)
aux_source_directory(${SRC}/parser      PARSER_CODE)
aux_source_directory(${SRC}/vm          VM_CODE)
aux_source_directory(${SRC}/disassemble DIS_ASM_CODE)
aux_source_directory(${SRC}/gc          GC_CODE)
aux_source_directory(${SRC}/ast         AST_CODE)

aux_source_directory(${DEV}             DEV_CODE)

set(ALL_CODE ${COMPILER_CODE} ${SRC_CODE} ${GC_CODE} ${INCLUDE_CODE} ${VM_CODE} ${PARSER_CODE} ${OBJ_CODE} ${CLI_CODE} ${DIS_ASM_CODE} ${DEV_CODE} ${AST_CODE})

include_directories(
    ${SRC}/cli
    ${SRC}/compiler
    ${SRC}/include
    ${SRC}/object
    ${SRC}/parser
    ${SRC}/vm
    ${SRC}/gc
    ${SRC}/disassemble
    ${SRC}/ast
    ${DEV}
)

add_executable(${PROJECT_NAME} ${ALL_CODE})
