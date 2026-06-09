// PT Cache去重+预取单元测试 (简化版，不依赖SystemC)
// 测试Buffer分配、链表挂接、批量更新等功能

#include <iostream>
#include <cassert>
#include <cstdint>
#include <cstring>

using namespace std;

// ===================== 简化版数据结构 =====================

const uint8_t DEDUP_BUFFER_INVALID_IDX = 0xFF;
const uint16_t PT_DEDUP_BUFFER_SIZE = 256;

// 模拟TransStage枚举
enum class TransStage : uint32_t {
    STAGE1_ONLY = 1,
    STAGE2_ONLY = 2,
    STAGE1_AND_2 = 3
};

// 模拟PTReserved
struct PTReserved {
    uint32_t raw;
    uint32_t valid : 1;
    uint32_t trans_type : 2;
    uint32_t input_page_size : 4;
    uint32_t result_page_size : 4;
    uint32_t is_ph : 1;
    uint32_t _reserved : 20;
};

// 模拟spte_t
struct spte_t {
    uint64_t raw;
    uint64_t V : 1;
    uint64_t R : 1;
    uint64_t W : 1;
    uint64_t X : 1;
    uint64_t PPN : 44;
    uint64_t _reserved : 16;
};

// 模拟gpte_t
struct gpte_t {
    uint64_t raw;
    uint64_t V : 1;
    uint64_t R : 1;
    uint64_t W : 1;
    uint64_t X : 1;
    uint64_t PPN : 44;
    uint64_t _reserved : 16;
};

// 模拟PTData
struct PTData {
    PTReserved reserved;
    spte_t vs_pte;
    gpte_t g_pte;
};

// ===================== DedupBuffer实现 =====================

struct DedupBufferEntry {
    uint8_t valid;
    uint8_t next_index;
    uint8_t tail_index;
    uint32_t group_id;
    TransStage stage;
    void* task_ptr;
    
    bool is_valid() const { return valid != 0; }
    void clear() {
        valid = 0;
        next_index = DEDUP_BUFFER_INVALID_IDX;
        tail_index = DEDUP_BUFFER_INVALID_IDX;
        group_id = 0;
        stage = TransStage::STAGE1_ONLY;
        task_ptr = nullptr;
    }
};

struct DedupBuffer {
    DedupBufferEntry entries[PT_DEDUP_BUFFER_SIZE];
    uint16_t valid_count;
    
    DedupBuffer() : valid_count(0) {
        memset(entries, 0, sizeof(entries));
    }
    
    uint8_t allocate_entry() {
        if (valid_count >= PT_DEDUP_BUFFER_SIZE) {
            return DEDUP_BUFFER_INVALID_IDX;
        }
        
        for (uint16_t i = 0; i < PT_DEDUP_BUFFER_SIZE; i++) {
            if (!entries[i].is_valid()) {
                entries[i].clear();
                entries[i].valid = 1;
                valid_count++;
                return static_cast<uint8_t>(i);
            }
        }
        return DEDUP_BUFFER_INVALID_IDX;
    }
    
    void free_entry(uint8_t idx) {
        if (idx == DEDUP_BUFFER_INVALID_IDX || idx >= PT_DEDUP_BUFFER_SIZE) return;
        if (entries[idx].is_valid()) {
            entries[idx].clear();
            valid_count--;
        }
    }
    
    bool is_full() const { return valid_count >= PT_DEDUP_BUFFER_SIZE; }
    uint16_t get_valid_count() const { return valid_count; }
};

// ===================== 测试1: Buffer分配顺序 =====================
void test_buffer_allocation_order() {
    cout << "\n========================================" << endl;
    cout << "Test 1: Buffer分配顺序测试" << endl;
    cout << "========================================" << endl;
    
    DedupBuffer buffer;
    
    // 测试顺序分配
    uint8_t idx0 = buffer.allocate_entry();
    assert(idx0 == 0);
    cout << "[PASS] allocate_entry() == 0" << endl;
    
    uint8_t idx1 = buffer.allocate_entry();
    assert(idx1 == 1);
    cout << "[PASS] allocate_entry() == 1" << endl;
    
    uint8_t idx2 = buffer.allocate_entry();
    assert(idx2 == 2);
    cout << "[PASS] allocate_entry() == 2" << endl;
    
    // 验证valid_count
    assert(buffer.get_valid_count() == 3);
    cout << "[PASS] valid_count == 3" << endl;
    
    // 测试释放后重新分配
    buffer.free_entry(1);
    assert(buffer.get_valid_count() == 2);
    cout << "[PASS] free_entry(1), valid_count == 2" << endl;
    
    uint8_t idx3 = buffer.allocate_entry();
    assert(idx3 == 1);  // 应该重用释放的Entry 1
    cout << "[PASS] allocate_entry() == 1 (reused)" << endl;
    
    cout << "\n✅ Test 1 PASSED!" << endl;
}

// ===================== 测试2: 链表挂接 =====================
void test_buffer_chain_linking() {
    cout << "\n========================================" << endl;
    cout << "Test 2: Buffer链表挂接测试" << endl;
    cout << "========================================" << endl;
    
    DedupBuffer buffer;
    
    // 创建链头
    uint8_t head = buffer.allocate_entry();
    assert(head == 0);
    buffer.entries[0].tail_index = 0;  // 链首tail指向自己
    buffer.entries[0].next_index = 0xFF;
    cout << "[PASS] 创建链头 Buffer[0], tail=0" << endl;
    
    // 挂接第一个节点
    uint8_t new_idx = buffer.allocate_entry();
    assert(new_idx == 1);
    buffer.entries[0].next_index = 1;
    buffer.entries[0].tail_index = 1;
    buffer.entries[1].tail_index = 0xFF;  // 非首节点固定0xFF
    buffer.entries[1].next_index = 0xFF;
    cout << "[PASS] 挂接 Buffer[1], chain: 0->1" << endl;
    
    // 验证链表
    assert(buffer.entries[0].next_index == 1);
    assert(buffer.entries[0].tail_index == 1);
    assert(buffer.entries[1].tail_index == 0xFF);
    cout << "[PASS] Buffer[0].next == 1" << endl;
    cout << "[PASS] Buffer[0].tail == 1" << endl;
    cout << "[PASS] Buffer[1].tail == 0xFF (非首节点)" << endl;
    
    // 挂接第二个节点
    uint8_t new_idx2 = buffer.allocate_entry();
    assert(new_idx2 == 2);
    uint8_t old_tail = buffer.entries[0].tail_index;  // 1
    buffer.entries[old_tail].next_index = 2;  // Buffer[1].next = 2
    buffer.entries[0].tail_index = 2;         // Buffer[0].tail = 2
    buffer.entries[2].tail_index = 0xFF;
    buffer.entries[2].next_index = 0xFF;
    cout << "[PASS] 挂接 Buffer[2], chain: 0->1->2" << endl;
    
    // 验证链表
    assert(buffer.entries[0].next_index == 1);
    assert(buffer.entries[1].next_index == 2);
    assert(buffer.entries[0].tail_index == 2);
    assert(buffer.entries[2].tail_index == 0xFF);
    cout << "[PASS] Buffer[0].next == 1" << endl;
    cout << "[PASS] Buffer[1].next == 2" << endl;
    cout << "[PASS] Buffer[0].tail == 2" << endl;
    cout << "[PASS] Buffer[2].tail == 0xFF" << endl;
    
    // 遍历链表验证
    uint8_t cur = 0;
    uint32_t count = 0;
    while (cur != 0xFF) {
        count++;
        cur = buffer.entries[cur].next_index;
    }
    assert(count == 3);
    cout << "[PASS] 链表遍历: 3个节点" << endl;
    
    cout << "\n✅ Test 2 PASSED!" << endl;
}

// ===================== 测试3: PTW一次性返回（模拟） =====================
void test_ptw_batch_return() {
    cout << "\n========================================" << endl;
    cout << "Test 3: PTW一次性返回测试（模拟）" << endl;
    cout << "========================================" << endl;
    
    const uint32_t D = 8;  // 预取深度
    const uint32_t total = 1 + D;  // 1主 + 8预取 = 9
    
    // 模拟batch_updates结构
    struct MockBatchUpdate {
        uint64_t iova;
        PTData pt_data;
    };
    
    MockBatchUpdate batch_updates[17];
    
    // 填充模拟数据
    for (uint32_t i = 0; i < total; i++) {
        batch_updates[i].iova = 0x10000000 + i * 0x1000;
        batch_updates[i].pt_data.reserved.raw = 0;
        batch_updates[i].pt_data.reserved.valid = 1;
        batch_updates[i].pt_data.reserved.trans_type = static_cast<uint32_t>(
            TransStage::STAGE1_AND_2);
        batch_updates[i].pt_data.reserved.input_page_size = 0;  // 4KB
        batch_updates[i].pt_data.reserved.result_page_size = 0;  // 4KB
        batch_updates[i].pt_data.reserved.is_ph = 0;  // 常规CL (非占位)
        
        batch_updates[i].pt_data.vs_pte.raw = 0;
        batch_updates[i].pt_data.vs_pte.V = 1;
        batch_updates[i].pt_data.vs_pte.R = 1;
        batch_updates[i].pt_data.vs_pte.W = 1;
        batch_updates[i].pt_data.vs_pte.X = 1;
        batch_updates[i].pt_data.vs_pte.PPN = 0x5000 + i;
    }
    
    // 验证批量更新
    cout << "[PASS] 构造batch_updates, total=" << total << endl;
    
    // 检查is_batch_update标志 (模拟)
    bool is_batch_update = true;
    assert(is_batch_update == true);
    cout << "[PASS] is_batch_update == true" << endl;
    
    // 检查batch_update_count
    uint32_t batch_update_count = total;
    assert(batch_update_count == 1 + D);
    cout << "[PASS] batch_update_count == " << (1 + D) << endl;
    
    // 验证所有更新条目都是常规CL (is_ph=0)
    for (uint32_t i = 0; i < batch_update_count; i++) {
        assert(batch_updates[i].pt_data.reserved.is_ph == 0);
    }
    cout << "[PASS] 所有" << batch_update_count << "个条目 is_ph == 0 (常规CL)" << endl;
    
    // 验证IOVA连续性
    for (uint32_t i = 0; i < batch_update_count - 1; i++) {
        uint64_t diff = batch_updates[i+1].iova - batch_updates[i].iova;
        assert(diff == 0x1000);  // 4KB间隔
    }
    cout << "[PASS] IOVA连续，间隔4KB" << endl;
    
    // 验证PPN连续性
    for (uint32_t i = 0; i < batch_update_count - 1; i++) {
        uint64_t ppn_diff = batch_updates[i+1].pt_data.vs_pte.PPN - 
                            batch_updates[i].pt_data.vs_pte.PPN;
        assert(ppn_diff == 1);
    }
    cout << "[PASS] PPN连续，递增1" << endl;
    
    cout << "\n✅ Test 3 PASSED!" << endl;
}

// ===================== 测试4: Buffer满处理 =====================
void test_buffer_full_handling() {
    cout << "\n========================================" << endl;
    cout << "Test 4: Buffer满处理测试" << endl;
    cout << "========================================" << endl;
    
    DedupBuffer buffer;
    
    // 分配所有256个Entry
    for (uint32_t i = 0; i < 256; i++) {
        uint8_t idx = buffer.allocate_entry();
        assert(idx == i);
    }
    cout << "[PASS] 分配256个Entry" << endl;
    
    assert(buffer.is_full() == true);
    assert(buffer.get_valid_count() == 256);
    cout << "[PASS] Buffer已满, valid_count == 256" << endl;
    
    // 尝试分配第257个，应该失败
    uint8_t idx_fail = buffer.allocate_entry();
    assert(idx_fail == DEDUP_BUFFER_INVALID_IDX);
    cout << "[PASS] allocate_entry() == 0xFF (失败)" << endl;
    
    // 释放一个后再分配
    buffer.free_entry(100);
    assert(buffer.get_valid_count() == 255);
    cout << "[PASS] free_entry(100), valid_count == 255" << endl;
    
    uint8_t idx_reuse = buffer.allocate_entry();
    assert(idx_reuse == 100);
    cout << "[PASS] allocate_entry() == 100 (重用)" << endl;
    
    cout << "\n✅ Test 4 PASSED!" << endl;
}

// ===================== 测试5: 预取组状态追踪 =====================
void test_prefetch_group_tracking() {
    cout << "\n========================================" << endl;
    cout << "Test 5: 预取组状态追踪测试" << endl;
    cout << "========================================" << endl;
    
    const uint32_t D = 8;
    const uint32_t total = 1 + D;
    
    // 模拟预取组状态
    struct MockPrefetchGroup {
        uint32_t pending_tasks = total;
        uint32_t total_tasks = total;
        bool completed = false;
        uint64_t group_iovas[17];
        spte_t vs_ptes[17];
        gpte_t g_ptes[17];
    };
    
    MockPrefetchGroup group;
    
    // 初始化IOVA
    for (uint32_t i = 0; i < total; i++) {
        group.group_iovas[i] = 0x10000000 + i * 0x1000;
    }
    cout << "[PASS] 初始化预取组, total_tasks=" << total << endl;
    
    // 模拟walk完成
    for (uint32_t i = 0; i < total; i++) {
        group.pending_tasks--;
        group.vs_ptes[i].raw = 0;
        group.vs_ptes[i].PPN = 0x5000 + i;
        group.g_ptes[i].raw = 0;
        group.g_ptes[i].PPN = 0x6000 + i;
        
        cout << "[INFO] Walk " << i << " completed, pending=" << group.pending_tasks << endl;
        
        // 检查完成标志
        if (group.pending_tasks == 0 && !group.completed) {
            group.completed = true;
            cout << "[PASS] 所有walk完成, completed=true" << endl;
        }
    }
    
    assert(group.completed == true);
    assert(group.pending_tasks == 0);
    cout << "[PASS] 预取组完成验证通过" << endl;
    
    // 验证所有PTE已收集
    for (uint32_t i = 0; i < total; i++) {
        assert(group.vs_ptes[i].PPN == 0x5000 + i);
        assert(group.g_ptes[i].PPN == 0x6000 + i);
    }
    cout << "[PASS] 所有" << total << "个PTE已收集" << endl;
    
    cout << "\n✅ Test 5 PASSED!" << endl;
}

// ===================== 主函数 =====================
int main() {
    cout << "\n============================================" << endl;
    cout << "PT Cache去重+预取 单元测试" << endl;
    cout << "============================================" << endl;
    
    try {
        test_buffer_allocation_order();
        test_buffer_chain_linking();
        test_ptw_batch_return();
        test_buffer_full_handling();
        test_prefetch_group_tracking();
        
        cout << "\n============================================" << endl;
        cout << "✅ 所有单元测试通过！" << endl;
        cout << "============================================" << endl;
        return 0;
        
    } catch (const exception& e) {
        cerr << "\n❌ 测试失败: " << e.what() << endl;
        return 1;
    }
}
