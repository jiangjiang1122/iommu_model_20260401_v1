#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1 || exit 1
# 用python删除那个奇怪文件名的文件
python3 -c "
import os, glob
for f in glob.glob('./h*'):
    if 'repowiki' in f or 'wiki' in f:
        os.remove(f)
        print(f'removed: {f}')
"
rm -f tmp/cleanup.sh
git status --short
