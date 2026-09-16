#!/usr/bin/env python3
"""Patch iommu_top.cc: injection delay + prefetch enable for MSI device"""
import sys

def patch_injection_delay():
    path = "iommu/iommu_top.cc"
    with open(path, "r") as f:
        content = f.read()

    old = """    if (phase == tlm::BEGIN_REQ) {
        // ===== Bandwidth control: slave port accept delay =====
        // delay_ns = 1000 * length * 8 / bandwidth_mbps
        unsigned int data_len = trans.get_data_length();
        slave_0_total_bytes += data_len;  // [STAT] 入口字节计数
        double slave_bw_delay_ns = 1000.0 * data_len * 8 / AXI_SLAVE_0_BANDWIDTH_MBPS;
        wait(slave_bw_delay_ns, SC_NS);

        // 1. Extract PayloadExtention
        PayloadExtention* ext = nullptr;
        trans.get_extension(ext);"""

    new = """    if (phase == tlm::BEGIN_REQ) {
        // 1. Extract PayloadExtention FIRST (needed for delay calculation)
        PayloadExtention* ext = nullptr;
        trans.get_extension(ext);

        // ===== Bandwidth control: slave port accept delay =====
        // [v5] 512B data -> 4ns (128GB/s), ctrl(SQ/CQ/MSI) -> 2ns fixed
        unsigned int data_len = trans.get_data_length();
        slave_0_total_bytes += data_len;  // [STAT] 入口字节计数
        double slave_bw_delay_ns;
        bool is_ctrl_packet = (ext && ext->is_ctrl);
        if (is_ctrl_packet) {
            slave_bw_delay_ns = 2.0;  // 控制包固定2ns
        } else {
            slave_bw_delay_ns = 1000.0 * data_len * 8 / AXI_SLAVE_0_BANDWIDTH_MBPS;
        }
        wait(slave_bw_delay_ns, SC_NS);"""

    if old in content:
        content = content.replace(old, new)
        with open(path, "w") as f:
            f.write(content)
        print("OK: injection delay patched")
    else:
        print("WARN: old injection text not found, may already be patched")

def patch_prefetch_enable():
    path = "iommu/iommu_top.cc"
    with open(path, "r") as f:
        content = f.read()

    old = """    // [MSI] MSI使能设备禁用预取: MSI命中后跳过S2与PT回填, 预取组/dedup链无法正确flush
    if (!task->walk_ctx.prefetch_enabled && PT_DEDUP_PREFETCH_DEPTH > 0 &&
        task->DC.msiptp.MODE == MSIPTP_Off) {"""

    new = """    // [v5] 场景13: MSI设备也开启预取(PTW在S1后识别MSI并跳过S2, 预取不影响MSI识别)
    if (!task->walk_ctx.prefetch_enabled && PT_DEDUP_PREFETCH_DEPTH > 0) {"""

    if old in content:
        content = content.replace(old, new)
        with open(path, "w") as f:
            f.write(content)
        print("OK: prefetch enable patched")
    else:
        print("WARN: old prefetch text not found, may already be patched")

if __name__ == "__main__":
    patch_injection_delay()
    patch_prefetch_enable()
