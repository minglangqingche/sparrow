import std.file for File, CFile;

let f = File.new("test_constant.sp", "r");
System.print(f.read_as_string());

let fp: NativePointer<FILE> = CFile.fopen("/Users/minglangqingche/sparrow/test/", "r");
System.print(fp);
System.print("fp is null ? %(fp.is_null)");

CFile.fclose(fp);
System.print(fp);
System.print("fp is null ? %(fp.is_null)");
