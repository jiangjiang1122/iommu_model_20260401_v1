#ifndef _PARAM_TRANS_DEF_H_
#define _PARAM_TRANS_DEF_H_

#include "systemc.h"
#include "tlm.h"
#include <string>
#include <cstdint>
using namespace std;

#define BUS_WIDTH 64

struct PayloadExtention: tlm::tlm_extension<PayloadExtention>
{
    PayloadExtention()
    : srcAddr       (0)
    , dstAddr       (0)
    , io_id(-1)
    , sequence_id(-1)
    , is_ctrl(0)
#if TEST_CFG_CPU_INVAL_SCENE
    , nvme_id(0)
#endif
    {
    }

    virtual tlm::tlm_extension_base* clone() const
    {
        PayloadExtention* e = new PayloadExtention();
        e->srcAddr         = srcAddr       ;
        e->dstAddr         = dstAddr       ;
        e->io_id = io_id;
        e->sequence_id =sequence_id;
        e->is_ctrl = is_ctrl;
#if TEST_CFG_CPU_INVAL_SCENE
        e->nvme_id = nvme_id;
#endif
        return e;
    }

    void operator=(const PayloadExtention &e)
    {
        srcAddr = e.srcAddr;
        dstAddr = e.dstAddr;
        io_id = e.io_id;
        sequence_id=e.sequence_id;
        is_ctrl = e.is_ctrl;
#if TEST_CFG_CPU_INVAL_SCENE
        nvme_id = e.nvme_id;
#endif
    }

    virtual void copy_from(tlm::tlm_extension_base const&ext)
    {
        *this = static_cast<PayloadExtention const&>(ext);
    }

    sc_dt::uint64 getSrcAddr       () const { return srcAddr ; }
    sc_dt::uint64 getDstAddr       () const { return dstAddr ; }
    void setSrcAddr       (sc_dt::uint64  x) { srcAddr= x; }
    void setDstAddr       (sc_dt::uint64  x) { dstAddr = x; }

    sc_dt::uint64 srcAddr;
    sc_dt::uint64 dstAddr;
    uint32_t io_id;
    uint32_t sequence_id;

    // common info
    uint32_t msg_type:8;
    uint32_t tag:10;
    uint32_t _rsvd0:14;

    uint32_t requester_id:16; // device_id = requester_id + segment_num
    uint32_t _rsvd1:16;

    uint32_t tc:3;
    uint32_t ido:1;
    uint32_t ep:1;
    uint32_t ro:1;
    uint32_t ns:1;
    uint32_t at:2;
    uint32_t pf_num:3; // not used
    uint32_t first_dw_be:4; //from wstrb
    uint32_t last_dw_be:4; //from wstrb
    uint32_t np:1;
    uint32_t cplsts:3;
    uint32_t _rsvd2:8;

    // msg type info
    uint32_t msg_code:8;
    uint32_t vdm_def:16;
    uint32_t _rsvd3:8;

    uint32_t bus_num:8;
    uint32_t func_num:3;
    uint32_t device_num:5;
    uint32_t vendor_id:16;

    // PASID prefix
    uint32_t pid_valid:1;
    uint32_t no_write:1;
    uint32_t exec_req:1;
    uint32_t priv_req:1;
    uint32_t process_id:28;

    // segment info , not used
    uint16_t segment_num;
    uint8_t ds_valid;
    uint8_t dsegmemt;

    // [场景13] 控制包标记: 1=SQ/CQ/MSI(不计入IOPS), 0=Data(512B, 计入IOPS)
    uint32_t is_ctrl;

#if TEST_CFG_CPU_INVAL_SCENE
    // [缓存失效多核] NVMe 任务组号(= 下发该组的 CPU 核 id / 请求发送进程 id)。
    //   同一组 NVMe 任务(11 包)的每个包都携带相同 nvme_id, 贯穿整个地址翻译流水,
    //   供 IOMMU 出口按 (nvme_id, 4KB页) 记录完成事件, CPU 侧只监控自己 nvme_id 的
    //   任务完成数目。[基线零影响] 仅在 TEST_CFG_CPU_INVAL_SCENE=1 时编译,
    //   保证基线场景 PayloadExtention 结构体大小/堆布局逐位不变。
    uint32_t nvme_id;
#endif
};

class NocTransaction;
typedef NocTransaction* NocTransactionPtr;

std::ostream& operator << (std::ostream &os ,const NocTransaction& trans);

class NocTransaction :public tlm::tlm_generic_payload
{
    public :
        NocTransaction()                         : tlm::tlm_generic_payload() {}
        NocTransaction(tlm::tlm_mm_interface* mm) : tlm::tlm_generic_payload(mm) {}

        sc_dt::uint64 get_src_addr       () const { PayloadExtention* e; get_extension(e); return (e?e->srcAddr    :0); }
        sc_dt::uint64 get_dst_addr       () const { PayloadExtention* e; get_extension(e); return (e?e->dstAddr    :0); }

        void set_src_addr       (sc_dt::uint64  x)       { hookExtension()->srcAddr     = x;      }
        void set_dst_addr       (sc_dt::uint64  x)       { hookExtension()->dstAddr     = x;      }

        static NocTransaction& cast(tlm::tlm_generic_payload& payload)   {   return static_cast<NocTransaction&>(payload); }
    private :
        PayloadExtention* hookExtension()
        {
            PayloadExtention* e;
            get_extension(e);
            if (!e)
            { e = new PayloadExtention();
                set_extension(e);
            }
            return e;
        }
};

#endif