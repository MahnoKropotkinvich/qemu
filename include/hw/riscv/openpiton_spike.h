#ifndef HW_RISCV_OPENPITON_SPIKE_H
#define HW_RISCV_OPENPITON_SPIKE_H

#include "hw/core/boards.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/core/sysbus.h"

#define TYPE_OPENPITON_SPIKE_MACHINE MACHINE_TYPE_NAME("openpiton-spike")
typedef struct OpenPitonSpikeState OpenPitonSpikeState;
DECLARE_INSTANCE_CHECKER(OpenPitonSpikeState, OPENPITON_SPIKE_MACHINE,
                         TYPE_OPENPITON_SPIKE_MACHINE)

struct OpenPitonSpikeState {
    /*< private >*/
    MachineState parent;

    /*< public >*/
    RISCVHartArrayState soc;
    DeviceState *plic;
};

enum {
    OPENPITON_MROM,
    OPENPITON_CLINT,
    OPENPITON_PLIC,
    OPENPITON_UART0,
    OPENPITON_DRAM,
};

/* OpenPiton RV64 platform constants */
#define OPENPITON_TIMEBASE_FREQ       390625
#define OPENPITON_UART0_IRQ           1
#define OPENPITON_PLIC_NUM_SOURCES    2
#define OPENPITON_PLIC_NUM_PRIO       7
#define OPENPITON_PLIC_PRIO_BASE      0x00
#define OPENPITON_PLIC_PENDING_BASE   0x1000
#define OPENPITON_PLIC_ENABLE_BASE    0x2000
#define OPENPITON_PLIC_ENABLE_STRIDE  0x80
#define OPENPITON_PLIC_CONTEXT_BASE   0x200000
#define OPENPITON_PLIC_CONTEXT_STRIDE 0x1000

#endif
