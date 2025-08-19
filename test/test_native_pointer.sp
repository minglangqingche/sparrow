let fp: NativePointer<FILE> = CFILE.fopen("/Users/minglangqingche/sparrow/test/test_constant.sp", "r");
System.print(fp);
System.print("fp is null ? %(fp.is_null)");

CFILE.fclose(fp);
System.print(fp);
System.print("fp is null ? %(fp.is_null)");
