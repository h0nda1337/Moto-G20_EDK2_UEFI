#ifndef __MOTO_G20_PLATFORM_H__
#define __MOTO_G20_PLATFORM_H__

//
// Motorola Moto G20 XT2128-1
// Codename: java / p352
// SoC: Unisoc UMS512 / T700
//

//
// GICv3
//
#define UMS512_GICD_BASE                 0x12000000ULL
#define UMS512_GICD_SIZE                 0x00020000ULL

#define UMS512_GICR_BASE                 0x12040000ULL
#define UMS512_GICR_SIZE                 0x00100000ULL
#define UMS512_GICR_STRIDE               0x00020000ULL
#define UMS512_GICR_REGIONS              1

//
// ARM Generic Timer INTIDs
//
#define UMS512_TIMER_SECURE_INTID        29
#define UMS512_TIMER_NONSECURE_INTID     30
#define UMS512_TIMER_VIRTUAL_INTID       27
#define UMS512_TIMER_HYP_INTID           26

//
// ARM architectural timer frequency.
// Confirmed experimentally from CNTFRQ_EL0.
//
#define MOTO_G20_ARCH_TIMER_FREQ         26000000ULL

//
// PSCI
//
// Device Tree:
//   compatible = "arm,psci-0.2"
//   method     = "smc"
//
#define UMS512_PSCI_USE_SMC              1

//
// UART0
//
#define UMS512_UART0_BASE                0x70000000ULL
#define UMS512_UART0_SIZE                0x00000100ULL
#define UMS512_UART0_IRQ                 34

//
// Boot eMMC / SDHCI
//
#define UMS512_EMMC_BASE                 0x71400000ULL
#define UMS512_EMMC_SIZE                 0x00001000ULL
#define UMS512_EMMC_IRQ                  42

//
// GPU
//
#define UMS512_GPU_BASE                  0x60000000ULL
#define UMS512_GPU_SIZE                  0x00100000ULL
#define UMS512_GPU_IRQ                   92

//
// Physical DRAM as declared by Device Tree.
//
// WARNING:
// The complete declared range is NOT entirely usable RAM.
// Reserved-memory carveouts must be excluded.
//
#define UMS512_DRAM_BASE                 0x80000000ULL
#define UMS512_DRAM_DECLARED_SIZE        0xFFFFF000ULL

//
// Other confirmed interrupt IDs.
//
#define UMS512_GSP_IRQ                   58
#define UMS512_DPU_IRQ                   59
#define UMS512_DSI_INT0_IRQ              62
#define UMS512_DSI_INT1_IRQ              63

//
// Display subsystem MMIO.
//
#define UMS512_DISPLAY_MMIO_BASE         0x20300000ULL
#define UMS512_DISPLAY_MMIO_SIZE         0x00002000ULL

//
// MIPI DSI / DPHY.
//
#define UMS512_DSI_BASE                  0x20400000ULL
#define UMS512_DSI_SIZE                  0x00001000ULL

//
// Live framebuffer / DPU scanout.
//
// CONFIRMED experimentally by:
//   - full white framebuffer test
//   - 4-stripe RGBW geometry/color test
//
// Logical pixel values:
//   0x00RRGGBB
//
// Little-endian memory:
//   B, G, R, X
//
#define MOTO_G20_FRAMEBUFFER_BASE        0x9E000000ULL
#define MOTO_G20_FRAMEBUFFER_RESERVATION 0x009E4000ULL

//
// Confirmed visible display geometry.
//
#define MOTO_G20_DISPLAY_WIDTH           720
#define MOTO_G20_DISPLAY_HEIGHT          1600
#define MOTO_G20_DISPLAY_BITS_PER_PIXEL  32
#define MOTO_G20_DISPLAY_BYTES_PER_PIXEL 4

//
// 720 pixels * 4 bytes.
//
#define MOTO_G20_DISPLAY_STRIDE          0x00000B40ULL

//
// 720 * 1600 * 4 = 4,608,000 bytes.
//
#define MOTO_G20_FRAMEBUFFER_VISIBLE_SIZE 0x00465000ULL

//
// Device Tree logo reservation.
// Same physical region as the confirmed live framebuffer.
//
#define MOTO_G20_LOGO_BUFFER_BASE        0x9E000000ULL
#define MOTO_G20_LOGO_BUFFER_SIZE        0x009E4000ULL

//
// U-Boot splash/BMP source buffer.
//
// IMPORTANT:
// 0x9D000000 is NOT the live DPU scanout framebuffer.
// Writing here did not change the displayed image.
//
#define MOTO_G20_UBOOT_SPLASH_SOURCE_BASE 0x9D000000ULL
#define MOTO_G20_UBOOT_SPLASH_SOURCE_SIZE 0x009E4000ULL

//
// Android/DPU framebuffer-related reserved memory.
//
// This is distinct from the already-active bootloader scanout at
// 0x9E000000 and must not be confused with the initial UEFI GOP buffer.
//
#define MOTO_G20_FRAMEBUFFER_MEM_BASE    0xFD708000ULL
#define MOTO_G20_FRAMEBUFFER_MEM_SIZE    0x01E5A000ULL

//
// DPU writeback reservation.
//
#define MOTO_G20_WRITEBACK_MEM_BASE      0xFF562000ULL
#define MOTO_G20_WRITEBACK_MEM_SIZE      0x00A1E000ULL

//
// Experimentally confirmed U-Boot -> ARM64 handoff.
//
#define MOTO_G20_DRAM_BASE               0x80000000ULL

//
// ARM64 Image runtime base.
//
// Stock Image:
//   DRAM base   = 0x80000000
//   text_offset = 0x00080000
//
#define MOTO_G20_FIRMWARE_LOAD_BASE      0x80080000ULL

//
// U-Boot passes:
//   X0 = 0x82000000
//
// Contents at X0 begin with valid FDT magic:
//   D00DFEED
//
#define MOTO_G20_DTB_HANDOFF_BASE        0x82000000ULL

//
// Initial CPU state confirmed experimentally:
//
//   CurrentEL = EL2
//   MMU       = OFF
//   D-cache   = OFF
//   I-cache   = OFF
//
#define MOTO_G20_BOOT_EXCEPTION_LEVEL    2

#endif
