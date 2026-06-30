#!/bin/bash
cd /mnt/d/Qoder_proj/iommu_model_20260401_v1
timeout 120 ./iommu_model 2>&1 | tee /mnt/d/Qoder_proj/iommu_model_20260401_v1/sim_twostage_full.log | grep -E "IOMMU STAT|^=|Completed|IOPS|Sim Time|Steady|Efficiency|slave_|master_|Peak|Global|PTW:|DDR |xDTW|Collector|Output|Phase|responses|e2e|Bandwidth|Avg|Max task|Min task|DDR access|DDR lat|task lat|Update|PTWC|NONE|total|window|peak|util" | head -100
