class CFile {
    // 标准 io
    native stdin -> NativePointer<FILE>;
    native stdout -> NativePointer<FILE>;
    native stderr -> NativePointer<FILE>;

    // 文件操作
    native fopen(path: String, mode: String) -> NativePointer<FILE>;
    native fclose(fp: NativePointer<FILE>);

    // fseek
    native SEEK_SET -> i32;
    native SEEK_CUR -> i32;
    native SEEK_END -> i32;
    native fseek(stream: NativePointer<FILE>, whence: i32, offset: i32) -> bool;

    native ftell(stream: NativePointer<FILE>) -> u32?; // 返回 u32 因此只能最大支持索引 4GB 文件
    native rewind(stream: NativePointer<FILE>);

    native read_as_string(stream: NativePointer<FILE>, u32: max_len) -> String?;
    native read_as_bytes(stream: NativePointer<FILE>, u32: max_len) -> List<u8>?;
}

let dylib_cfile = DyLib.c_dlopen(DyLib.SPR_DYLIB_PATH + "/std/cfile/build/libsprcfile.dylib");
DyLib.bind(dylib_cfile, CFile);

class File {
    let fp: NativePointer<FILE>?;
    let path: String;
    let mode: String;

    new(_path: String, _mode: String) {
        path = _path;
        mode = _mode;
        fp = CFile.fopen(path, mode);
    }

    close() {
        if (fp == null) {
            return;
        }

        CFile.close(self.fp);
        self.fp = null;
    }

    read_as_string() -> String? {
        if fp == null || fp.is_null {
            return null;
        }

        if !CFile.fseek(fp, CFile.SEEK_END, 0) {
            return null;
        }

        let file_size: u32 = CFile.ftell(fp);
        if file_size < 0 {
            return null;
        }

        CFile.rewind(fp);

        return CFile.read_as_string(fp, file_size);
    }

    read_as_bytes() -> List<u8>? {
        if fp == null || fp.is_null {
            return null;
        }

        if !CFile.fseek(fp, CFile.SEEK_END, 0) {
            return null;
        }

        let file_size: u32 = CFile.ftell(fp);
        if file_size < 0 {
            return null;
        }

        CFile.rewind(fp);

        return CFile.read_as_bytes(fp, file_size);
    }
}
