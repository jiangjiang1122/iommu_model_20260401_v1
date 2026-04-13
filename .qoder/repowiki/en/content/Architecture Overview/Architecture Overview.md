# Architecture Overview

<cite>
**Referenced Files in This Document**
- [main.cpp](file://main.cpp)
- [Makefile](file://Makefile)
- [iommu_top.hh](file://iommu/iommu_top.hh)
- [iommu_top.cc](file://iommu/iommu_top.cc)
- [param_trans_def.hh](file://iommu/param_trans_def.hh)
- [iommu_req_rsp.hh](file://iommu/iommu_req_rsp.hh)
- [iommu_struct.hh](file://iommu/iommu_struct.hh)
- [iommu_translate.cc](file://iommu/iommu_translate.cc)
- [iommu_command_queue.cc](file://iommu/iommu_command_queue.cc)
- [iommu_interrupt.hh](file://iommu/iommu_interrupt.hh)
- [iommu_fault.hh](file://iommu/iommu_fault.hh)
- [iommu_registers.hh](file://iommu/iommu_registers.hh)
- [test_rp.hh](file://rp/test_rp.hh)
- [test_rp_func.cc](file://rp/test_rp_func.cc)
- [test_pcienoc.hh](file://pcienoc/test_pcienoc.hh)
- [test_ddr.hh](file://ddr/test_ddr.hh)
</cite>

## Table of Contents
1. [Introduction](#introduction)
2. [Project Structure](#project-structure)
3. [Core Components](#core-components)
4. [Architecture Overview](#architecture-overview)
5. [Detailed Component Analysis](#detailed-component-analysis)
6. [Dependency Analysis](#dependency-analysis)
7. [Performance Considerations](#performance-considerations)
8. [Troubleshooting Guide](#troubleshooting-guide)
9. [Conclusion](#conclusion)

## Introduction
This document describes the architecture of the IOMMU SystemC model. The IOMMU is modeled as a transaction-level SystemC simulation with a central top-level module coordinating interactions among RP (Root Complex/ATS endpoint), DDR memory, and PCIe NOC (Network-on-Chip) interfaces. The design uses TLM 2.0 sockets for inter-module communication, enabling event-driven simulation of address translation, ATS messaging, and DMA traffic. Cross-cutting concerns include timing via delays, synchronization via events, and resource management via internal queues and caches.

## Project Structure
The repository organizes code by functional domain:
- Top-level simulation and binding in main.cpp and Makefile
- IOMMU core in iommu/: translation engine, registers, command queue, interrupts, faults, and supporting data structures
- RP (Root Complex/ATS endpoint) in rp/: test harness and translation request generators
- PCIe NOC in pcienoc/: minimal interface for AHB traffic
- DDR in ddr/: simple AXI memory model

```mermaid
graph TB
subgraph "SystemC Simulation"
MAIN["main.cpp<br/>SC_MAIN entry"]
TOP["iommu_top<br/>Central coordinator"]
RP["RP_Module<br/>ATS/translation tests"]
NOC["PCIENOC_Module<br/>AHB master"]
DDR["DDR_Module<br/>AXI memory"]
end
MAIN --> TOP
MAIN --> RP
MAIN --> NOC
MAIN --> DDR
TOP -- "AXI initiator" --> DDR
TOP -- "AXI initiator" --> RP
TOP -- "AHB initiator" --> NOC
RP -- "AXI target" --> TOP
NOC -- "AHB target" --> TOP
DDR -- "AXI target" --> TOP
```

**Diagram sources**
- [main.cpp:37-79](file://main.cpp#L37-L79)
- [iommu_top.hh:19-56](file://iommu/iommu_top.hh#L19-L56)
- [test_rp.hh:54-112](file://rp/test_rp.hh#L54-L112)
- [test_pcienoc.hh:10-21](file://pcienoc/test_pcienoc.hh#L10-L21)
- [test_ddr.hh:15-61](file://ddr/test_ddr.hh#L15-L61)

**Section sources**
- [main.cpp:37-79](file://main.cpp#L37-L79)
- [Makefile:1-98](file://Makefile#L1-L98)

## Core Components
- iommu_top: Central SystemC module exposing multiple TLM sockets for AXI and AHB traffic, hosting the IOMMU state machine, and driving command queue monitoring.
- RP_Module: Generates translation requests and validates responses; acts as an ATS endpoint and AXI master to the IOMMU.
- PCIENOC_Module: Minimal AHB master module used to simulate NOC-side traffic.
- DDR_Module: Simulated AXI memory with three target sockets for different IOMMU access domains.
- IOMMU internals: Translation engine, register file, command queue, ATS/ATC, interrupts, and fault handling.

Key sockets and roles:
- AXI streams: Stream-to-CMN-RND, Master-0 to PCIe NOC, Master-1 to CMN-RND, Master-2 to PCIe NOC (ATS).
- AXI slaves: From PCIe NOC (AXI), From PCIe NOC (AHB).
- Event-driven command queue monitoring thread.

**Section sources**
- [iommu_top.hh:19-56](file://iommu/iommu_top.hh#L19-L56)
- [iommu_top.cc:6-36](file://iommu/iommu_top.cc#L6-L36)
- [test_rp.hh:54-112](file://rp/test_rp.hh#L54-L112)
- [test_pcienoc.hh:10-21](file://pcienoc/test_pcienoc.hh#L10-L21)
- [test_ddr.hh:15-61](file://ddr/test_ddr.hh#L15-L61)

## Architecture Overview
The IOMMU operates as a transaction-level model with explicit separation of concerns:
- iommu_top coordinates incoming transactions, performs address translation, and forwards traffic to appropriate destinations.
- RP generates requests and validates outcomes; it also handles ATS messages.
- PCIe NOC provides an AHB interface for legacy or auxiliary traffic.
- DDR provides AXI memory backing for DMA and register accesses.

```mermaid
graph TB
RP["RP_Module<br/>ATS/Translation Generator"] --> |"AXI master"| TOP["iommu_top<br/>Coordinator"]
NOC["PCIENOC_Module<br/>AHB master"] --> |"AHB master"| TOP
TOP --> |"AXI target"| DDR["DDR_Module<br/>AXI memory"]
subgraph "IOMMU Internal"
TRANS["Translation Engine<br/>iommu_translate_*"]
REG["Registers<br/>iommu_registers.hh"]
CMDQ["Command Queue<br/>iommu_command_queue.cc"]
INT["Interrupts<br/>iommu_interrupt.hh"]
FLT["Faults<br/>iommu_fault.hh"]
end
TOP --> TRANS
TOP --> REG
TOP --> CMDQ
TOP --> INT
TOP --> FLT
```

**Diagram sources**
- [iommu_top.cc:77-180](file://iommu/iommu_top.cc#L77-L180)
- [iommu_translate.cc:8-11](file://iommu/iommu_translate.cc#L8-L11)
- [iommu_command_queue.cc:7-13](file://iommu/iommu_command_queue.cc#L7-L13)
- [iommu_interrupt.hh:23-25](file://iommu/iommu_interrupt.hh#L23-L25)
- [iommu_fault.hh:87-89](file://iommu/iommu_fault.hh#L87-L89)
- [iommu_registers.hh:1-200](file://iommu/iommu_registers.hh#L1-L200)

## Detailed Component Analysis

### iommu_top: Central Coordinator
Responsibilities:
- Registers AXI/AHB b_transport handlers for slave sockets.
- Implements translation logic for AXI slave traffic and ATS message handling for AHB slave.
- Forwards translated traffic to DDR via AXI sockets or streams to RP/CMN-RND via dedicated sockets.
- Drives command queue monitoring via an SC_THREAD sensitive to an event.

```mermaid
sequenceDiagram
participant RP as "RP_Module"
participant TOP as "iommu_top"
participant IOMMU as "Translation Engine"
participant DDR as "DDR_Module"
participant RP2 as "RP_Module (ATS)"
RP->>TOP : "AXI master b_transport(IOVA, attrs)"
TOP->>TOP : "Parse PayloadExtention"
TOP->>IOMMU : "iommu_translate_iova(req, rsp)"
IOMMU-->>TOP : "rsp {status, PPN, S, is_msi, dest_mrif_addr}"
alt "MSI translation"
TOP->>TOP : "set_address(dest_mrif_addr)"
TOP->>RP2 : "stream socket b_transport()"
else "Regular translation"
TOP->>TOP : "compute PA from PPN"
TOP->>DDR : "AXI master b_transport(PA)"
end
```

**Diagram sources**
- [iommu_top.cc:77-180](file://iommu/iommu_top.cc#L77-L180)
- [iommu_translate.cc:8-11](file://iommu/iommu_translate.cc#L8-L11)
- [param_trans_def.hh:12-97](file://iommu/param_trans_def.hh#L12-L97)

**Section sources**
- [iommu_top.hh:19-56](file://iommu/iommu_top.hh#L19-L56)
- [iommu_top.cc:6-36](file://iommu/iommu_top.cc#L6-L36)
- [iommu_top.cc:77-180](file://iommu/iommu_top.cc#L77-L180)

### Translation Pipeline and Data Structures
The translation engine consumes requests with device/process attributes and address type, consults device/process contexts, and produces a physical address or MSI redirection. It tracks transaction types, permissions, and faults.

```mermaid
flowchart TD
Start(["iommu_translate_iova"]) --> CheckMode["Check IOMMU mode (Off/Bare)"]
CheckMode --> |Off| FaultOff["Report fault (cause=256)"]
CheckMode --> |Bare| BarePath["Direct passthrough (pa=iova)"]
CheckMode --> |Other| LocateDC["Locate device context"]
LocateDC --> DCValid{"Device context found?"}
DCValid --> |No| StopFault["Stop and report fault"]
DCValid --> |Yes| CheckATSType["Check ATS/Translated/Untranslated"]
CheckATSType --> |ATS| ATSPath["Handle ATS translation"]
CheckATSType --> |Translated| TranslatedPath["Translated passthrough"]
CheckATSType --> |Untranslated| VStage["VS-stage walk"]
VStage --> GStage["GS-stage walk (optional)"]
GStage --> Output["Compute PA/PPN, set flags"]
BarePath --> Output
ATSPath --> Output
TranslatedPath --> Output
FaultOff --> End(["Return"])
StopFault --> End
Output --> End
```

**Diagram sources**
- [iommu_translate.cc:8-11](file://iommu/iommu_translate.cc#L8-L11)
- [iommu_translate.cc:96-141](file://iommu/iommu_translate.cc#L96-L141)
- [iommu_req_rsp.hh:29-101](file://iommu/iommu_req_rsp.hh#L29-L101)

**Section sources**
- [iommu_translate.cc:8-11](file://iommu/iommu_translate.cc#L8-L11)
- [iommu_req_rsp.hh:29-101](file://iommu/iommu_req_rsp.hh#L29-L101)

### Command Queue and Event-Driven Processing
The command queue is polled by an SC_THREAD sensitive to an event. The monitor checks readiness (enabled, no faults, no stalls) and executes commands from memory-mapped queue entries.

```mermaid
sequenceDiagram
participant TOP as "iommu_top"
participant MON as "CQ_Monitor_Process_Thread"
participant Q as "Command Queue Memory"
participant INTF as "Interrupts/Faults"
TOP->>MON : "notify cq_process_evt"
MON->>Q : "Read next command (cqh)"
Q-->>MON : "command"
MON->>MON : "Decode opcode/func3"
alt "Valid command"
MON->>Q : "Execute and advance cqh"
else "Invalid/illegal"
MON->>INTF : "Set cmd_ill/cmd_to/cqmf"
MON->>TOP : "Generate interrupt"
end
```

**Diagram sources**
- [iommu_top.hh:32-33](file://iommu/iommu_top.hh#L32-L33)
- [iommu_top.cc:185-192](file://iommu/iommu_top.cc#L185-L192)
- [iommu_command_queue.cc:7-13](file://iommu/iommu_command_queue.cc#L7-L13)

**Section sources**
- [iommu_top.cc:185-192](file://iommu/iommu_top.cc#L185-L192)
- [iommu_command_queue.cc:7-13](file://iommu/iommu_command_queue.cc#L7-L13)

### RP Module: Translation Requests and Validation
The RP module constructs TLM transactions with PayloadExtention metadata, sends them to iommu_top, and validates responses and faults via register reads and memory inspection.

```mermaid
sequenceDiagram
participant RP as "RP_Module"
participant TOP as "iommu_top"
participant IOMMU as "Translation Engine"
participant MEM as "DDR/Registers"
RP->>RP : "Build tlm_generic_payload + PayloadExtention"
RP->>TOP : "axi_master_to_pcie_noc_0_socket.b_transport()"
TOP->>IOMMU : "iommu_translate_iova(req, rsp)"
IOMMU-->>TOP : "rsp"
TOP-->>RP : "response via socket"
RP->>MEM : "Read FQ/CQ registers/memory"
RP-->>RP : "Validate cause/status"
```

**Diagram sources**
- [test_rp_func.cc:23-67](file://rp/test_rp_func.cc#L23-L67)
- [test_rp_func.cc:112-157](file://rp/test_rp_func.cc#L112-L157)
- [param_trans_def.hh:12-97](file://iommu/param_trans_def.hh#L12-L97)

**Section sources**
- [test_rp.hh:54-112](file://rp/test_rp.hh#L54-L112)
- [test_rp_func.cc:23-67](file://rp/test_rp_func.cc#L23-L67)
- [test_rp_func.cc:112-157](file://rp/test_rp_func.cc#L112-L157)

### PCIe NOC and DDR Modules
- PCIENOC_Module exposes an AHB master socket bound to iommu_top’s AHB slave.
- DDR_Module exposes three AXI target sockets and implements b_transport for read/write with bounds checking.

```mermaid
graph TB
NOC["PCIENOC_Module"] --> |"AHB master"| TOP["iommu_top"]
TOP --> |"AXI target"| DDR["DDR_Module"]
RP["RP_Module"] --> |"AXI master"| TOP
```

**Diagram sources**
- [test_pcienoc.hh:10-21](file://pcienoc/test_pcienoc.hh#L10-L21)
- [test_ddr.hh:15-61](file://ddr/test_ddr.hh#L15-L61)
- [iommu_top.hh:27-28](file://iommu/iommu_top.hh#L27-L28)

**Section sources**
- [test_pcienoc.hh:10-21](file://pcienoc/test_pcienoc.hh#L10-L21)
- [test_ddr.hh:15-61](file://ddr/test_ddr.hh#L15-L61)

## Dependency Analysis
The build system compiles all IOMMU sources and links against SystemC. The main binds modules and wires sockets according to the intended topology.

```mermaid
graph TB
MAIN["main.cpp"] --> TOP["iommu_top.cc/.hh"]
MAIN --> RP["rp/test_rp_func.cc"]
MAIN --> NOC["pcienoc/test_pcienoc.cc"]
MAIN --> DDR["ddr/test_ddr.cc"]
TOP --> CORE["iommu/*.cc/.hh"]
CORE --> HDR["iommu/*.hh"]
```

**Diagram sources**
- [Makefile:30-50](file://Makefile#L30-L50)
- [main.cpp:7-12](file://main.cpp#L7-L12)

**Section sources**
- [Makefile:30-50](file://Makefile#L30-L50)
- [main.cpp:7-12](file://main.cpp#L7-L12)

## Performance Considerations
- Transaction-level modeling: Uses TLM 2.0 to abstract memory latency behind delays, enabling coarse-grained timing without cycle-accurate DRAM models.
- Event-driven scheduling: Threads and sockets minimize busy-waiting; command queue processing occurs only when signaled.
- Caching: Internal caches (TLB, ATC, DDT/PDT) reduce repeated translation overhead.
- Pipeline stages: Translation engine separates device/process context lookup, stage walks, and permission checks to support pipelined request handling.

## Troubleshooting Guide
Common areas to inspect:
- Translation failures: Validate device/process contexts, IOMMU mode, and ATS enablement; check fault queue records and causes.
- Command queue stalls: Inspect readiness flags, memory faults, and ITAG tracker availability.
- Socket mismatches: Verify bind order and directionality (initiator/target) in main.cpp.
- Address errors: Confirm address ranges and alignment for AXI transactions.

**Section sources**
- [iommu_fault.hh:52-89](file://iommu/iommu_fault.hh#L52-L89)
- [iommu_command_queue.cc:48-73](file://iommu/iommu_command_queue.cc#L48-L73)
- [test_ddr.hh:44-58](file://ddr/test_ddr.hh#L44-L58)
- [main.cpp:52-65](file://main.cpp#L52-L65)

## Conclusion
The IOMMU SystemC model employs a modular, event-driven design centered on iommu_top. TLM sockets enable clean separation between RP, PCIe NOC, and DDR while preserving transaction semantics. The translation engine, command queue, interrupts, and faults form a cohesive subsystem that supports realistic IOMMU behavior for testing and validation.