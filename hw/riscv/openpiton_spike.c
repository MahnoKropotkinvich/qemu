#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qapi/error.h"
#include "hw/core/boards.h"
#include "hw/core/loader.h"
#include "hw/core/sysbus.h"
#include "hw/char/serial-mm.h"
#include "target/riscv/cpu.h"
#include "hw/riscv/riscv_hart.h"
#include "hw/riscv/openpiton_spike.h"
#include "hw/riscv/boot.h"
#include "hw/intc/riscv_aclint.h"
#include "hw/intc/sifive_plic.h"
#include "chardev/char.h"
#include "system/device_tree.h"
#include "system/system.h"

#include <libfdt.h>

// Memory map

static const MemMapEntry openpiton_memmap[] = {
    [OPENPITON_MROM]  = {        0x1000,        0xf000 },
    [OPENPITON_CLINT] = { 0xfff1020000ULL,     0xc0000 },
    [OPENPITON_PLIC]  = { 0xfff1100000ULL,   0x4000000 },
    [OPENPITON_UART0] = { 0xfff0c2c000ULL,      0x1000 },
    [OPENPITON_DRAM]  = { 0x80000000,              0x0 },
};

// FDT generation

static void create_fdt(OpenPitonSpikeState *s, const MemMapEntry *memmap,
                       bool is_32_bit)
{
    MachineState *ms = MACHINE(s);
    uint64_t mem_base = memmap[OPENPITON_DRAM].base;
    uint64_t mem_size = ms->ram_size;
    void *fdt;
    uint32_t phandle = 1, intc_phandle, plic_phandle;
    g_autofree char *plic_name = NULL;
    g_autofree char *clint_name = NULL;
    g_autofree char *uart_name = NULL;
    g_autofree char *mem_name = NULL;
    g_autofree uint32_t *plic_cells = NULL;
    g_autofree uint32_t *clint_cells = NULL;
    g_autofree char *plic_hart_config = NULL;

    int fdt_size;
    fdt = ms->fdt = create_device_tree(&fdt_size);
    if (!fdt) {
        error_report("create_device_tree() failed");
        exit(1);
    }

    qemu_fdt_setprop_string(fdt, "/", "model", "openpiton-spike");
    qemu_fdt_setprop_string(fdt, "/", "compatible", "openpiton-spike");
    qemu_fdt_setprop_cell(fdt, "/", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/", "#address-cells", 0x2);

    /* /soc */
    qemu_fdt_add_subnode(fdt, "/soc");
    qemu_fdt_setprop(fdt, "/soc", "ranges", NULL, 0);
    qemu_fdt_setprop_string(fdt, "/soc", "compatible", "simple-bus");
    qemu_fdt_setprop_cell(fdt, "/soc", "#size-cells", 0x2);
    qemu_fdt_setprop_cell(fdt, "/soc", "#address-cells", 0x2);

    /* /cpus */
    qemu_fdt_add_subnode(fdt, "/cpus");
    qemu_fdt_setprop_cell(fdt, "/cpus", "timebase-frequency",
                          OPENPITON_TIMEBASE_FREQ);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#size-cells", 0x0);
    qemu_fdt_setprop_cell(fdt, "/cpus", "#address-cells", 0x1);

    /* single hart */
    qemu_fdt_add_subnode(fdt, "/cpus/cpu@0");
    if (is_32_bit) {
        qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "mmu-type", "riscv,sv32");
    } else {
        qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "mmu-type", "riscv,sv39");
    }
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "riscv,isa",
                            is_32_bit ? "rv32imafdc" : "rv64imafdc");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "compatible", "riscv");
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "status", "okay");
    qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0", "reg", 0);
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0", "device_type", "cpu");

    intc_phandle = phandle++;
    qemu_fdt_add_subnode(fdt, "/cpus/cpu@0/interrupt-controller");
    qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0/interrupt-controller",
                          "#interrupt-cells", 1);
    qemu_fdt_setprop(fdt, "/cpus/cpu@0/interrupt-controller",
                     "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_string(fdt, "/cpus/cpu@0/interrupt-controller",
                            "compatible", "riscv,cpu-intc");
    qemu_fdt_setprop_cell(fdt, "/cpus/cpu@0/interrupt-controller",
                          "phandle", intc_phandle);

    /* /memory */
    mem_name = g_strdup_printf("/memory@%"PRIx64, mem_base);
    qemu_fdt_add_subnode(fdt, mem_name);
    qemu_fdt_setprop_sized_cells(fdt, mem_name, "reg", 2, mem_base, 2, mem_size);
    qemu_fdt_setprop_string(fdt, mem_name, "device_type", "memory");

    /* CLINT */
    clint_cells = g_new0(uint32_t, 4);
    clint_cells[0] = cpu_to_be32(intc_phandle);
    clint_cells[1] = cpu_to_be32(IRQ_M_SOFT);
    clint_cells[2] = cpu_to_be32(intc_phandle);
    clint_cells[3] = cpu_to_be32(IRQ_M_TIMER);

    clint_name = g_strdup_printf("/soc/clint@%"PRIx64,
                                 (uint64_t)memmap[OPENPITON_CLINT].base);
    qemu_fdt_add_subnode(fdt, clint_name);
    qemu_fdt_setprop_string(fdt, clint_name, "compatible", "riscv,clint0");
    qemu_fdt_setprop_sized_cells(fdt, clint_name, "reg",
        2, memmap[OPENPITON_CLINT].base,
        2, memmap[OPENPITON_CLINT].size);
    qemu_fdt_setprop(fdt, clint_name, "interrupts-extended",
                     clint_cells, 4 * sizeof(uint32_t));

    /* PLIC */
    plic_phandle = phandle++;
    plic_cells = g_new0(uint32_t, 4);
    plic_cells[0] = cpu_to_be32(intc_phandle);
    plic_cells[1] = cpu_to_be32(IRQ_M_EXT);
    plic_cells[2] = cpu_to_be32(intc_phandle);
    plic_cells[3] = cpu_to_be32(IRQ_S_EXT);

    plic_name = g_strdup_printf("/soc/plic@%"PRIx64,
                                (uint64_t)memmap[OPENPITON_PLIC].base);
    qemu_fdt_add_subnode(fdt, plic_name);
    qemu_fdt_setprop_string(fdt, plic_name, "compatible", "riscv,plic0");
    qemu_fdt_setprop(fdt, plic_name, "interrupt-controller", NULL, 0);
    qemu_fdt_setprop_cell(fdt, plic_name, "#interrupt-cells", 1);
    qemu_fdt_setprop_cell(fdt, plic_name, "#address-cells", 0);
    qemu_fdt_setprop(fdt, plic_name, "interrupts-extended",
                     plic_cells, 4 * sizeof(uint32_t));
    qemu_fdt_setprop_sized_cells(fdt, plic_name, "reg",
        2, memmap[OPENPITON_PLIC].base,
        2, memmap[OPENPITON_PLIC].size);
    qemu_fdt_setprop_cell(fdt, plic_name, "riscv,ndev",
                          OPENPITON_PLIC_NUM_SOURCES - 1);
    qemu_fdt_setprop_cell(fdt, plic_name, "phandle", plic_phandle);

    /* UART */
    uart_name = g_strdup_printf("/soc/serial@%"PRIx64,
                                (uint64_t)memmap[OPENPITON_UART0].base);
    qemu_fdt_add_subnode(fdt, uart_name);
    qemu_fdt_setprop_string(fdt, uart_name, "compatible", "ns16550");
    qemu_fdt_setprop_sized_cells(fdt, uart_name, "reg",
        2, memmap[OPENPITON_UART0].base,
        2, memmap[OPENPITON_UART0].size);
    qemu_fdt_setprop_cell(fdt, uart_name, "clock-frequency", 3686400);
    qemu_fdt_setprop_cell(fdt, uart_name, "interrupt-parent", plic_phandle);
    qemu_fdt_setprop_cell(fdt, uart_name, "interrupts", OPENPITON_UART0_IRQ);

    qemu_fdt_add_subnode(fdt, "/chosen");
    qemu_fdt_setprop_string(fdt, "/chosen", "stdout-path", uart_name);

    qemu_fdt_add_subnode(fdt, "/aliases");
    qemu_fdt_setprop_string(fdt, "/aliases", "serial0", uart_name);
}

/* Machine init                                                        */

static void openpiton_spike_machine_init(MachineState *machine)
{
    OpenPitonSpikeState *s = OPENPITON_SPIKE_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *mask_rom = g_new(MemoryRegion, 1);
    const MemMapEntry *memmap = openpiton_memmap;
    RISCVBootInfo boot_info;
    // 0x100000 size is reserved for M-mode stub in openPiton verilator
    hwaddr firmware_load_addr = memmap[OPENPITON_DRAM].base + 0x100000;
    uint64_t kernel_entry = 0;
    uint64_t fdt_load_addr;
    g_autofree char *plic_hart_config = NULL;

    /* Single-socket, single or few harts */
    object_initialize_child(OBJECT(machine), "soc", &s->soc,
                            TYPE_RISCV_HART_ARRAY);
    object_property_set_str(OBJECT(&s->soc), "cpu-type",
                            machine->cpu_type, &error_abort);
    object_property_set_int(OBJECT(&s->soc), "hartid-base", 0, &error_abort);
    object_property_set_int(OBJECT(&s->soc), "num-harts", machine->smp.cpus,
                            &error_abort);
    sysbus_realize(SYS_BUS_DEVICE(&s->soc), &error_fatal);

    /* CLINT: SWI + MTIMER */
    riscv_aclint_swi_create(memmap[OPENPITON_CLINT].base,
                            0, machine->smp.cpus, false);
    riscv_aclint_mtimer_create(
        memmap[OPENPITON_CLINT].base + RISCV_ACLINT_SWI_SIZE,
        RISCV_ACLINT_DEFAULT_MTIMER_SIZE,
        0, machine->smp.cpus,
        RISCV_ACLINT_DEFAULT_MTIMECMP,
        RISCV_ACLINT_DEFAULT_MTIME,
        OPENPITON_TIMEBASE_FREQ, true);

    /* PLIC */
    plic_hart_config = riscv_plic_hart_config_string(machine->smp.cpus);
    s->plic = sifive_plic_create(
        memmap[OPENPITON_PLIC].base,
        plic_hart_config, machine->smp.cpus, 0,
        OPENPITON_PLIC_NUM_SOURCES,
        OPENPITON_PLIC_NUM_PRIO,
        OPENPITON_PLIC_PRIO_BASE,
        OPENPITON_PLIC_PENDING_BASE,
        OPENPITON_PLIC_ENABLE_BASE,
        OPENPITON_PLIC_ENABLE_STRIDE,
        OPENPITON_PLIC_CONTEXT_BASE,
        OPENPITON_PLIC_CONTEXT_STRIDE,
        memmap[OPENPITON_PLIC].size);

    /* DRAM */
    memory_region_add_subregion(system_memory, memmap[OPENPITON_DRAM].base,
                                machine->ram);

    /* Boot ROM */
    memory_region_init_rom(mask_rom, NULL, "openpiton-spike.mrom",
                           memmap[OPENPITON_MROM].size, &error_fatal);
    memory_region_add_subregion(system_memory, memmap[OPENPITON_MROM].base,
                                mask_rom);

    /* UART 16550 */
    serial_mm_init(system_memory, memmap[OPENPITON_UART0].base, 0,
                   qdev_get_gpio_in(s->plic, OPENPITON_UART0_IRQ),
                   3686400, serial_hd(0), DEVICE_LITTLE_ENDIAN);

    /* Firmware */
    riscv_find_and_load_firmware(machine,
        riscv_default_firmware_name(&s->soc),
        &firmware_load_addr, NULL);

    /* FDT */
    create_fdt(s, memmap, riscv_is_32bit(&s->soc));

    /* Kernel */
    riscv_boot_info_init(&boot_info, &s->soc);
    if (machine->kernel_filename) {
        vaddr kernel_start_addr = riscv_calc_kernel_start_addr(
            &boot_info, firmware_load_addr);
        riscv_load_kernel(machine, &boot_info, kernel_start_addr, true, NULL);
        kernel_entry = boot_info.image_low_addr;
    }

    /* Load FDT into RAM */
    fdt_load_addr = riscv_compute_fdt_addr(memmap[OPENPITON_DRAM].base,
                                           memmap[OPENPITON_DRAM].size,
                                           machine, &boot_info);
    riscv_load_fdt(fdt_load_addr, machine->fdt);

    /* Reset vector in MROM */
    riscv_setup_rom_reset_vec(machine, &s->soc, firmware_load_addr,
                              memmap[OPENPITON_MROM].base,
                              memmap[OPENPITON_MROM].size,
                              kernel_entry, fdt_load_addr);
}

static void openpiton_spike_machine_instance_init(Object *obj)
{
}

static void openpiton_spike_machine_class_init(ObjectClass *oc,
                                               const void *data)
{
    MachineClass *mc = MACHINE_CLASS(oc);

    mc->desc = "RISC-V OpenPiton-spike board (OpenPiton address map)";
    mc->init = openpiton_spike_machine_init;
    mc->max_cpus = 1;
    mc->default_cpus = 1;
    mc->default_cpu_type = TYPE_RISCV_CPU_BASE;
    mc->default_ram_id = "openpiton-spike.ram";
}

static const TypeInfo openpiton_spike_machine_typeinfo = {
    .name       = TYPE_OPENPITON_SPIKE_MACHINE,
    .parent     = TYPE_MACHINE,
    .class_init = openpiton_spike_machine_class_init,
    .instance_init = openpiton_spike_machine_instance_init,
    .instance_size = sizeof(OpenPitonSpikeState),
};

static void openpiton_spike_machine_init_register_types(void)
{
    type_register_static(&openpiton_spike_machine_typeinfo);
}

type_init(openpiton_spike_machine_init_register_types)
