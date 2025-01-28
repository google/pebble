/*
 * This linker script must be passed to the linker argument after the
 * MCU-specific linker script.
 *
 * The MCU-specific linker script must define the following memory regions:
 *   - FLASH
 *   - KERNEL_RAM
 *   - APP_RAM
 *   - WORKER_RAM
 * The APP_RAM and WORKER_RAM regions are special: the firmware will overwrite
 * the entire regions' contents at runtime without consideration for any data or
 * variables that have been statically linked into those regions. Therefore no
 * sections may be assigned to these regions, and these regions must not alias
 * each other.
 *
 * The MCU-specific linker script must also define the following memory regions,
 * which may be aliases (using the REGION_ALIAS command) to KERNEL_RAM or
 * another memory region.
 *   - REGION_ISR_STACK
 *   - REGION_KERNEL_STACKS
 *   - REGION_KERNEL_HEAP
 */

ASSERT(ORIGIN(KERNEL_RAM) != ORIGIN(APP_RAM),
       "APP_RAM cannot alias KERNEL_RAM");
ASSERT(ORIGIN(KERNEL_RAM) != ORIGIN(WORKER_RAM),
       "WORKER_RAM cannot alias KERNEL_RAM");
ASSERT(ORIGIN(APP_RAM) != ORIGIN(WORKER_RAM),
       "APP_RAM cannot alias WORKER_RAM");

/* PBL-40376: Temp hack: put .rocky_bss at end of APP_RAM:
   Interim solution until all statics are removed from applib & jerry */
__ROCKY_BSS_size__ = 1512;

__SRAM_size__ = LENGTH(KERNEL_RAM) + LENGTH(WORKER_RAM) + LENGTH(APP_RAM);

__WORKER_RAM__ = ORIGIN(WORKER_RAM);
__WORKER_RAM_end__ = __WORKER_RAM__ + LENGTH(WORKER_RAM);

__APP_RAM__ = ORIGIN(APP_RAM);
__APP_RAM_end__ = __APP_RAM__ + LENGTH(APP_RAM);

__KERNEL_RAM_start__ = ORIGIN(KERNEL_RAM);
__KERNEL_RAM_size__ = LENGTH(KERNEL_RAM);

/*
Our MPU only supports powers of two for region sizes. This means we have to upfront define
how big our privileged regions are, since there's no way of doing "Align to power of two"
in a linker script.

If a section is too small, you'll see an error like this:
fw_common.ld:99 cannot move location counter backwards (from 0000000008011754 to 0000000008010800)
If you see this, you need to bump the size to the next power of two.
*/
__unpriv_ro_bss_size__ = 2K;
__stack_guard_size__ = 32;


MEMORY {
    /* Allocate log strings here for the console. Not loaded to memory. */
    LOG_STRINGS (r) : ORIGIN = 0xC0000000, LENGTH = 512K
}

/* Section concepts!

VMA = Virtual Memory Address - The final address for the section.
      This could be either RAM or FLASH.
LMA = Load Memory Address - The initial address in ROM that the section is loaded from.
      This is always a location in FLASH.

We lay out our LMAs like the following (some sections are skipped because they're not important)...
 .text
 .kernel_data
 .app_data
 .fw_version
    This must be last since we read it out of SPI flash before we load the rest of the image.

We take that image and load part of it into RAM. That lay out looks like...
 .kernel_data
 .kernel_bss (A small portion of which is read only from unprivileged code)
 stacks (not all of our stacks are fixed, some are on the heap and some are statically located here.
 <gap for our heap>
 .pbl_app, fixed at 0x801a000
   .app_data
   .app_bss
   .app_stack
   <gap for loaded 3rd party app code+data+bss>
   <gap for app heap>
*/

SECTIONS {
    .isr_vector : {
        ASSERT(. == ORIGIN(FLASH), "Error: Vector table is offset from the start of the FLASH region");
        PROVIDE(__ISR_VECTOR_TABLE__ = .);
        KEEP(*(.isr_vector))            /* Startup code */
    } >FLASH

    /* Exception handling sections. "contains index entries for section unwinding"
     * We don't actually use this or know what it is, but it seems happy to go here. */
    .ARM.exidx :
    {
        . = ALIGN(4);
        *(.ARM.exidx)
        . = ALIGN(4);
    } >FLASH

    /* the program code is stored in the .text section, which goes to Flash */
    .text : {
        __syscall_text_start__ = .;
        *(.syscall_text.*)           /* System call functions */
        __syscall_text_end__ = .;
        *(.text)                   /* remaining code */
        *(.text.*)                 /* remaining code */
        *(.rodata)                 /* read-only data (constants) */
        *(.rodata*)
        *(.constdata)              /* read-only data (constants) */
        *(.constdata*)
        *(.conststring)
        *(i.*)

        . = ALIGN(8);
    } >FLASH

    /* PBL-40376: Temp hack: put .rocky_bss at end of APP_RAM:
       Interim solution until all statics are removed from applib & jerry */
    .app_ram (NOLOAD) : {
        . = . + LENGTH(APP_RAM) - __ROCKY_BSS_size__;
        . = ALIGN(4);
        __ROCKY_BSS__ = ABSOLUTE(.);
        src/fw/vendor/jerryscript/libjerry_core.a:(.bss .bss.*)
        . = ALIGN(4);
        *(.rocky_bss)
    } >APP_RAM

    .kernel_data : ALIGN(8) {
        __data_start = .; /* This is used by the startup in order to initialize the .data section */

        *(.data)
        *(.data.*)

        . = ALIGN(8);
        __data_end = .;
    } >KERNEL_RAM AT>FLASH
    __data_load_start = LOADADDR(.kernel_data);

    .kernel_ro_bss (NOLOAD) : {
        __bss_start = .; /* This is used by the startup in order to initialize the .bss section */

        . = ALIGN(__unpriv_ro_bss_size__);
        __unpriv_ro_bss_start__ = .;

        *:lib_a-timelocal.o(.bss.*)
        *:lib_a-tzvars.o(.bss.*)

        *(.kernel_unpriv_ro_bss)

        . = __unpriv_ro_bss_start__ + __unpriv_ro_bss_size__;
    } >KERNEL_RAM

    .kernel_bss (NOLOAD) : {
        *(.freertos_privileged_data)

        *(.bss)
        *(.bss.*)

        *(COMMON)

        __bss_end = .;
    } >KERNEL_RAM

    .stack (NOLOAD) : {
        . = ALIGN(__stack_guard_size__);
        __isr_stack_start__ = .;

        . = . + 1024;

        _estack = .;
    } >REGION_ISR_STACK

    .kernel_main_stack (NOLOAD) : {
        . = ALIGN(__stack_guard_size__);
        __kernel_main_stack_start__ = .;

        . = . + 2048;
    } >REGION_KERNEL_STACKS
    __kernel_main_stack_size__ = . - __kernel_main_stack_start__;

    .kernel_bg_stack (NOLOAD) : {
        . = ALIGN(__stack_guard_size__);
        __kernel_bg_stack_start__ = .;

        . = . + 1536;
    } >REGION_KERNEL_STACKS
    __kernel_bg_stack_size__ = . - __kernel_bg_stack_start__;

    .kernel_heap (NOLOAD) : {
        _heap_start = .;
        . = ORIGIN(REGION_KERNEL_HEAP) + LENGTH(REGION_KERNEL_HEAP);
        _heap_end = .;
    } >REGION_KERNEL_HEAP

    /* GNU build id: This is a hash of parts of the binary that uniquely
     * identifies the binary. This hash gets inserted by the linker;
     * we're passing the flag --build-id=sha1 to do this.
     * The variable TINTIN_BUILD_ID is provided, so that the values can be used
     * in the firmware code. See src/core/util/version.c for its use. */
    .note.gnu.build-id : {
        PROVIDE(TINTIN_BUILD_ID = .);
        KEEP(*(.note.gnu.build-id))
    } >FLASH

    /* Struct with version information of the firmware. This must be the last thing that goes
     * into FLASH that has a non-zero size. */
    .fw_version : {
        KEEP(*(.pbl_fw_version))
    } >FLASH

    /* Unloaded section containing our log strings. */
    .log_strings (INFO) : {
        KEEP(*(.log_string.header))
        KEEP(*(.log_strings))
    } >LOG_STRINGS

    /* after that it's only debugging information. */
    /* remove the debugging information from the standard libraries */
    DISCARD : {
        libgcc.a ( * )
    }

    /* Stabs debugging sections.  */
    .stab          0 : { *(.stab) }
    .stabstr       0 : { *(.stabstr) }
    .stab.excl     0 : { *(.stab.excl) }
    .stab.exclstr  0 : { *(.stab.exclstr) }
    .stab.index    0 : { *(.stab.index) }
    .stab.indexstr 0 : { *(.stab.indexstr) }
    .comment       0 : { *(.comment) }
    /* DWARF debug sections.
       Symbols in the DWARF debugging sections are relative to the beginning
       of the section so we begin them at 0.  */
    /* DWARF 1 */
    .debug          0 : { *(.debug) }
    .line           0 : { *(.line) }
    /* GNU DWARF 1 extensions */
    .debug_srcinfo  0 : { *(.debug_srcinfo) }
    .debug_sfnames  0 : { *(.debug_sfnames) }
    /* DWARF 1.1 and DWARF 2 */
    .debug_aranges  0 : { *(.debug_aranges) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    /* DWARF 2 */
    .debug_info     0 : { *(.debug_info .gnu.linkonce.wi.*) }
    .debug_abbrev   0 : { *(.debug_abbrev) }
    .debug_line     0 : { *(.debug_line) }
    .debug_frame    0 : { *(.debug_frame) }
    .debug_str      0 : { *(.debug_str) }
    .debug_loc      0 : { *(.debug_loc) }
    .debug_macinfo  0 : { *(.debug_macinfo) }
    /* SGI/MIPS DWARF 2 extensions */
    .debug_weaknames 0 : { *(.debug_weaknames) }
    .debug_funcnames 0 : { *(.debug_funcnames) }
    .debug_typenames 0 : { *(.debug_typenames) }
    .debug_varnames  0 : { *(.debug_varnames) }
}

  /* vim: set filetype=ld: */
