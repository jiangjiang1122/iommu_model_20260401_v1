#!/bin/bash
# build_wsl.sh - 在WSL本地文件系统编译，然后将可执行文件拷回Windows目录
# 用法: ./build_wsl.sh [TEST_SCENARIO]
# 示例:
#   ./build_wsl.sh rand4k_singlestage   (默认)
#   ./build_wsl.sh seq128k_singlestage
#   ./build_wsl.sh seq128k_twostage
#   ./build_wsl.sh sv48_bare

TEST_SCENARIO=${1:-rand4k_singlestage}
WSL_BUILD_DIR="/tmp/iommu_build"
WIN_PROJ_DIR="/mnt/d/Qoder_proj/iommu_model_20260401_v1"

echo "=========================================="
echo " IOMMU WSL Build Script"
echo " Test Scenario: ${TEST_SCENARIO}"
echo "=========================================="

# Step 1: 拷贝工程到WSL本地文件系统
echo "[1/4] Copying project to WSL native filesystem (${WSL_BUILD_DIR})..."
rm -rf ${WSL_BUILD_DIR}
mkdir -p ${WSL_BUILD_DIR}
cp -r ${WIN_PROJ_DIR}/* ${WSL_BUILD_DIR}/
echo "      Done."

# Step 2: 在WSL本地文件系统编译
echo "[2/4] Compiling in WSL native filesystem (TEST=${TEST_SCENARIO})..."
cd ${WSL_BUILD_DIR}
make clean > /dev/null 2>&1
BUILD_START=$(date +%s)
make TEST=${TEST_SCENARIO} DEBUG=0 2>&1
BUILD_RESULT=$?
BUILD_END=$(date +%s)
BUILD_TIME=$((BUILD_END - BUILD_START))

if [ ${BUILD_RESULT} -ne 0 ]; then
    echo ""
    echo "BUILD FAILED! (exit code: ${BUILD_RESULT})"
    exit ${BUILD_RESULT}
fi
echo "      Build succeeded in ${BUILD_TIME}s."

# Step 3: 拷回可执行文件
echo "[3/4] Copying executable back to Windows directory..."
cp ${WSL_BUILD_DIR}/iommu_model ${WIN_PROJ_DIR}/iommu_model
echo "      Done."

# Step 4: 清理
echo "[4/4] Cleaning WSL build directory..."
rm -rf ${WSL_BUILD_DIR}
echo "      Done."

echo ""
echo "=========================================="
echo " Build Complete!"
echo " Executable: ${WIN_PROJ_DIR}/iommu_model"
echo " Scenario:   ${TEST_SCENARIO}"
echo " Build Time: ${BUILD_TIME}s"
echo "=========================================="
