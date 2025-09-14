#!/bin/zsh

# 使用数组存储测试文件路径，支持含空格的文件名
test_files=()

# 通过find命令查找测试文件，并安全地存入数组
# 使用while循环确保每个文件名作为单独元素处理
while IFS= read -r -d '' file; do
    test_files+=("$file")
done < <(find . -maxdepth 1 -type f \( -name "test_*.sp" -o -name "*_test.sp" \) -print0)

# 检查是否找到测试文件
if [ ${#test_files[@]} -eq 0 ]; then
    echo "未找到任何测试文件"
    exit 0
fi

# 统计测试文件数量
count=${#test_files[@]}
echo "找到 $count 个测试文件，开始运行..."
echo "----------------------------------------"

# 遍历数组中的每个测试文件
# 使用"${test_files[@]}"确保文件名被正确解析（包括空格）
for file in "${test_files[@]}"; do
    echo "正在运行测试文件: $file"
    
    # 运行测试文件
    if ~/sparrow/build/spr "$file"; then
        echo "✅ $file 测试通过"
    else
        echo "❌ $file 测试失败"
        # 如需测试失败即停止，取消下面一行的注释
        # exit 1
    fi
    echo "----------------------------------------"
done

echo "所有测试运行完毕"
