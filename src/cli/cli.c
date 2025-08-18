#include "class.h"
#include "common.h"
#include <stdio.h>
#include <string.h>
#include "obj_string.h"
#include "vm.h"
#include "core.h"

// 可选flag：DIS_ASM_CHUNK DIS_ASM_CHUNK_WHEN_CALL OUTPUT_GC_INFO

static void run_file(const char* path) {
    const char* last_slash = strrchr(path, '/');
    if (last_slash != NULL) {
        char* root = (char*)malloc(last_slash - path + 1);
        memcpy(root, path, last_slash - path + 1);
        root[last_slash - path + 1] = '\0';
        root_dir = root;
    }

    VM* vm = vm_new();
    const char* src = read_file(path);

    execute_module(vm, OBJ_TO_VALUE(objstring_new(vm, path, strlen(path))), src);

    vm_free(vm);
}

int main(int argc, char* argv[]) {
    switch (argc) {
        case 2:
            run_file(argv[1]);
            break;
        default:
            fprintf(stderr, "usage: %s <file-name>\n", argv[0]);
    }
    return 0;
}
