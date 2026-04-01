#!/bin/bash
# GDB调试脚本 for RISC-V IOMMU Model

echo "RISC-V IOMMU Model GDB调试脚本"
echo "==============================="

# 检查可执行文件是否存在
if [ ! -f "./iommu_model" ]; then
    echo "错误: 找不到 iommu_model 可执行文件"
    echo "请先使用 'make' 编译项目"
    exit 1
fi

# 检查GDB是否已安装
if ! command -v gdb &> /dev/null; then
    echo "错误: 未找到 gdb，请先安装 GDB 调试器"
    echo "Ubuntu/Debian: sudo apt-get install gdb"
    echo "CentOS/RHEL: sudo yum install gdb"
    exit 1
fi

echo "启动GDB调试会话..."
echo "提示: 在GDB中可以使用以下命令:"
echo "  - break <function_name> 或 b <function_name> : 在函数处设置断点"
echo "  - break <file:line_number> : 在指定文件的行号设置断点"
echo "  - run 或 r : 运行程序"
echo "  - step 或 s : 单步执行（进入函数）"
echo "  - next 或 n : 单步执行（跳过函数）"
echo "  - continue 或 c : 继续执行"
echo "  - print <variable> 或 p <variable> : 打印变量值"
echo "  - info breakpoints : 显示所有断点"
echo "  - list 或 l : 显示源代码"
echo "  - quit 或 q : 退出GDB"
echo ""

# 启动GDB调试会话
gdb -ex "set confirm off" -ex "directory iommu/" -ex "file ./iommu_model" -ex "set args" -ex "break main" -ex "run" -ex "list" "$@"