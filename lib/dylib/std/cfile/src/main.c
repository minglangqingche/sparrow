#include "common.h"
#include "libsprcfile.h"
#include "sparrow.h"

#include <stdio.h>

static SprApi api;
static ObjString* CFile_FILE_classifier = NULL;

static bool prim_CFile_stdin(VM* vm, Value* args) {
    ObjNativePointer* obj = api.create_native_pointer(api.vm, stdin, CFile_FILE_classifier, NULL);
    args[0] = (Value) {.type = VT_OBJ, .header = (ObjHeader*)obj};
    return true;
}

static bool prim_CFile_stdout(VM* vm, Value* args) {
    ObjNativePointer* obj = api.create_native_pointer(api.vm, stdout, CFile_FILE_classifier, NULL);
    args[0] = (Value) {.type = VT_OBJ, .header = (ObjHeader*)obj};
    return true;
}

static bool prim_CFile_stderr(VM* vm, Value* args) {
    ObjNativePointer* obj = api.create_native_pointer(api.vm, stderr, CFile_FILE_classifier, NULL);
    args[0] = (Value) {.type = VT_OBJ, .header = (ObjHeader*)obj};
    return true;
}

static void CFile_FILE_destroy(ObjNativePointer* obj) {
    FILE* fp = api.unpack_native_pointer(obj);
    if (fp != NULL) {
        fclose(fp);
    }
}

static bool prim_CFile_fopen(VM* vm, Value* args) {
    const char* path = NULL;
    u32 _path_len = 0;

    const char* mode = NULL;
    u32 _mode_len = 0;

    if (!api.validate_string(args[1], &path, &_path_len) || !api.validate_string(args[2], &mode, &_mode_len)) {
        api.set_error(&api, "CFile.fopen(path: String, mode: String) -> NativePointer<FILE>;\n");
        return false;
    }
    
    FILE* fp = fopen(path, mode);
    if (fp == NULL) {
        args[0] = (Value) {.type = VT_NULL, .i32val = 0};
        return true;
    }
    
    ObjNativePointer* obj = api.create_native_pointer(api.vm, fp, CFile_FILE_classifier, CFile_FILE_destroy);
    args[0] = (Value) {.type = VT_OBJ, .header = (ObjHeader*)obj};
    return true;
}

static bool prim_CFile_fclose(VM* vm, Value* args) {
    if (!api.validate_native_pointer(&api, args[1], CFile_FILE_classifier)) {
        api.set_error(&api, "CFile.fclose(NativePointer<FILE>);\n");
        return false;
    }

    FILE* fp = api.unpack_native_pointer((ObjNativePointer*)args[1].header);
    if (fp == NULL) {
        return true;
    }

    fclose(fp);

    api.set_native_pointer((ObjNativePointer*)args[1].header, NULL);

    return true;
}

static bool prim_CFile_SEEK_SET(VM* vm, Value* args) {
    args[0] = (Value) {.type = VT_I32, .i32val = SEEK_SET};
    return true;
}

static bool prim_CFile_SEEK_CUR(VM* vm, Value* args) {
    args[0] = (Value) {.type = VT_I32, .i32val = SEEK_CUR};
    return true;
}

static bool prim_CFile_SEEK_END(VM* vm, Value* args) {
    args[0] = (Value) {.type = VT_I32, .i32val = SEEK_END};
    return true;
}

static bool prim_CFile_fseek(VM* vm, Value* args) {
    if (api.validate_native_pointer(&api, args[1], CFile_FILE_classifier) != 0 || args[2].type != VT_I32 || args[3].type != VT_I32) {
        api.set_error(&api, "CFile.fseek(stream: NativePointer<FILE>, whence: i32, offset: i32) -> bool;\n");
        return false;
    }
    FILE* stream = api.unpack_native_pointer((ObjNativePointer*)args[1].header);
    int offset = args[3].i32val;

    if (args[2].i32val > 2 || args[2].i32val < 0) {
        api.set_error(&api, "CFile.fseek: whence must between 0 and 2. please use CFile.SEEK_XXX.\n");
        return false;
    }
    int whence = args[2].i32val;

    int res = fseek(stream, offset, whence);

    args[0] = (Value) {.type = (res == 0) ? VT_TRUE : VT_FALSE};
    return true;
}

static bool prim_CFile_ftell(VM* vm, Value* args) {
    if (api.validate_native_pointer(&api, args[1], CFile_FILE_classifier) != 0) {
        api.set_error(&api, "CFile.ftell(stream: NativePointer<FILE>) -> u32?;\n");
        return false;
    }
    FILE* fp = api.unpack_native_pointer((ObjNativePointer*)args[1].header);

    long res = ftell(fp);
    if (res < 0) {
        args[0] = (Value) {.type = VT_NULL};
        return true;
    }

    args[0] = (Value) {.type = VT_U32, .u32val = res};
    return true;
}

static bool prim_CFile_rewind(VM* vm, Value* args) {
    if (api.validate_native_pointer(&api, args[1], CFile_FILE_classifier) != 0) {
        api.set_error(&api, "CFile.rewind(stream: NativePointer<FILE>);\n");
        return false;
    }
    FILE* fp = api.unpack_native_pointer((ObjNativePointer*)args[1].header);

    rewind(fp);

    args[0] = (Value) {.type = VT_NULL};
    return true;
}

static bool prim_CFile_read_as_string(VM* vm, Value* args) {
    if (api.validate_native_pointer(&api, args[1], CFile_FILE_classifier) != 0 || args[2].type != VT_U32) {
        api.set_error(&api, "CFile.read_as_string(stream: NativePointer<FILE>, u32: max_len) -> String?;\n");
        return false;
    }

    u32 max_len = args[2].u32val;
    if (max_len == 0) {
        args[0] = (Value) {.type = VT_NULL};
        return true;
    }

    FILE* fp = api.unpack_native_pointer((ObjNativePointer*)args[1].header);

    char* buf = malloc(sizeof(char) * (max_len + 1));
    if (buf == NULL) {
        api.set_error(&api, "CFile.read_as_string: memory error when allocate buffer.\n");
        return false;
    }

    int len = fread(buf, sizeof(char), max_len, fp);
    if (len == 0) {
        free(buf);
        args[0] = (Value) {.type = VT_NULL};
        return true;
    }

    buf[len] = '\0';

    ObjString* res = api.create_string(api.vm, buf, len);
    
    free(buf);

    args[0] = (Value) {.type = VT_OBJ, .header = (ObjHeader*)res};
    return true;
}

static bool prim_CFile_read_as_bytes(VM* vm, Value* args) {
    if (api.validate_native_pointer(&api, args[1], CFile_FILE_classifier) != 0 || args[2].type != VT_U32) {
        api.set_error(&api, "CFile.read_as_bytes(stream: NativePointer<FILE>, u32: max_len) -> List<u8>?;\n");
        return false;
    }

    u32 max_len = args[2].u32val;
    if (max_len == 0) {
        args[0] = (Value) {.type = VT_NULL};
        return true;
    }

    FILE* fp = api.unpack_native_pointer((ObjNativePointer*)args[1].header);

    u8* buf = malloc(sizeof(u8) * max_len);
    if (buf == NULL) {
        api.set_error(&api, "CFile.read_as_bytes: memory error when allocate buffer.\n");
        return false;
    }

    int len = fread(buf, sizeof(char), max_len, fp);
    if (len == 0) {
        free(buf);
        args[0] = (Value) {.type = VT_NULL};
        return true;
    }

    ObjList* res = api.create_list(api.vm, len);
    
    int counts = 0;
    Value* elements = api.list_elements(res, &counts);

    for (int i = 0; i < counts; i++) {
        elements[i] = (Value) {.type = VT_U8, .u8val = buf[i]};
    }
    
    free(buf);

    args[0] = (Value) {.type = VT_OBJ, .header = (ObjHeader*)res};
    return true;
}

void pub_spr_dylib_init(SprApi api_in) {
    api = api_in;

    if (CFile_FILE_classifier == NULL) {
        CFile_FILE_classifier = api.create_string(api.vm, "CFILE_FILE*", 11);
        api.push_keep_root(&api, (Value) {.type = VT_OBJ, .header = (ObjHeader*)CFile_FILE_classifier});
    }

    api.register_method(&api, "stdin", prim_CFile_stdin, true);
    api.register_method(&api, "stdout", prim_CFile_stdout, true);
    api.register_method(&api, "stderr", prim_CFile_stderr, true);
    
    api.register_method(&api, "fopen(_,_)", prim_CFile_fopen, true);
    api.register_method(&api, "fclose(_)", prim_CFile_fclose, true);

    api.register_method(&api, "SEEK_SET", prim_CFile_SEEK_SET, true);
    api.register_method(&api, "SEEK_CUR", prim_CFile_SEEK_CUR, true);
    api.register_method(&api, "SEEK_END", prim_CFile_SEEK_END, true);
    api.register_method(&api, "fseek(_,_,_)", prim_CFile_fseek, true);

    api.register_method(&api, "ftell(_)", prim_CFile_ftell, true);
    api.register_method(&api, "rewind(_)", prim_CFile_rewind, true);

    api.register_method(&api, "read_as_string(_,_)", prim_CFile_read_as_string, true);
    api.register_method(&api, "read_as_bytes(_,_)", prim_CFile_read_as_bytes, true);
}
