cmake_minimum_required(VERSION 3.31.2)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

add_definitions(-DDEBUG_ASSERT_ON -g)

project(spr C)

set(SRC ${PROJECT_SOURCE_DIR}/src)

aux_source_directory(${SRC}             SRC_CODE)
aux_source_directory(${SRC}/cli         CLI_CODE)
aux_source_directory(${SRC}/compiler    COMPILER_CODE)
aux_source_directory(${SRC}/include     INCLUDE_CODE)
aux_source_directory(${SRC}/object      OBJ_CODE)
aux_source_directory(${SRC}/parser      PARSER_CODE)
aux_source_directory(${SRC}/vm          VM_CODE)
aux_source_directory(${SRC}/disassemble DIS_ASM_CODE)
aux_source_directory(${SRC}/gc          GC_CODE)

set(ALL_CODE ${COMPILER_CODE} ${SRC_CODE} ${GC_CODE} ${INCLUDE_CODE} ${VM_CODE} ${PARSER_CODE} ${OBJ_CODE} ${CLI_CODE} ${DIS_ASM_CODE})

include_directories(
    ${SRC}/cli
    ${SRC}/compiler
    ${SRC}/include
    ${SRC}/object
    ${SRC}/parser
    ${SRC}/vm
    ${SRC}/gc
    ${SRC}/disassemble
)

add_executable(${PROJECT_NAME} ${ALL_CODE})
