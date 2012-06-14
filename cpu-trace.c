void cpu_trace_instruction()
{
    printf("%c%s", cpu_op_in_base_instruction_set[op_code] ? ' ' : '*', cpu_op_name[op_code]);

    if (cpu_op_address_mode[op_code] == cpu_address_implied) {
        if (cpu_op_handler[op_code] == cpu_op_asla
            || cpu_op_handler[op_code] == cpu_op_lsra
            || cpu_op_handler[op_code] == cpu_op_rola
            || cpu_op_handler[op_code] == cpu_op_rora)
            printf(" A                           ");
        else
            printf("                             ");
    }
    else if (cpu_op_address_mode[op_code] == cpu_address_absolute) {
        if (!(cpu_op_handler[op_code] == cpu_op_jmp
            || cpu_op_handler[op_code] == cpu_op_jsr))
            printf(" $%04X = %02X                  ", op_address, op_value);
        else
            printf(" $%04X                       ", op_address);
    }
    else if (cpu_op_address_mode[op_code] == cpu_address_absolute_x) {
        printf(" $%04X,X @ %04X = %02X         ", memory_readw(cpu.PC - 2), (memory_readw(cpu.PC - 2) + cpu.X) & 0xFFFF, op_value);
    }
    else if (cpu_op_address_mode[op_code] == cpu_address_absolute_y) {
        printf(" $%04X,Y @ %04X = %02X         ", memory_readw(cpu.PC - 2), (memory_readw(cpu.PC - 2) + cpu.Y) & 0xFFFF, op_value);
    }
    else if (cpu_op_address_mode[op_code] == cpu_address_immediate) {
        printf(" #$%02X                        ", op_value);
    }
    else if (cpu_op_address_mode[op_code] == cpu_address_zero_page) {
        printf(" $%02X = %02X                    ", op_address, op_value);
    }
    else if (cpu_op_address_mode[op_code] == cpu_address_zero_page_x) {
        printf(" $%02X,X @ %02X = %02X             ", memory_readb(cpu.PC - 1), (memory_readb(cpu.PC - 1) + cpu.X) & 0xFF, op_value);
    }
    else if (cpu_op_address_mode[op_code] == cpu_address_zero_page_y) {
        printf(" $%02X,Y @ %02X = %02X             ", memory_readb(cpu.PC - 1), (memory_readb(cpu.PC - 1) + cpu.Y) & 0xFF, op_value);
    }
    else if (cpu_op_address_mode[op_code] == cpu_address_relative) {
        printf(" $%04X                       ", op_address);
    }
    else if (cpu_op_address_mode[op_code] == cpu_address_indirect) {
        printf(" ($%04X) = %04X              ", memory_readw(cpu.PC - 2), memory_readw(memory_readw(cpu.PC - 2)));
    }
    else if (cpu_op_address_mode[op_code] == cpu_address_indirect_x) {
        printf(" ($%02X,X) @ %02X = %04X = %02X    ", memory_readb(cpu.PC - 1), (memory_readb(cpu.PC - 1) + cpu.X) & 0xFF, op_address, op_value);
    }
    else if (cpu_op_address_mode[op_code] == cpu_address_indirect_y) {
        byte arg_addr = memory_readb(cpu.PC - 1);
        word trace_addr = (memory_readb((arg_addr + 1) & 0xFF) << 8) | memory_readb(arg_addr);

        printf(" ($%02X),Y = %04X @ %04X = %02X  ", memory_readb(cpu.PC - 1), trace_addr, (trace_addr + cpu.Y) & 0xFFF, op_value);
    }

    printf("A:%02X X:%02X Y:%02X P:%02X SP:%02X", cpu.A, cpu.X, cpu.Y, cpu.P, cpu.SP);
#if STACK_TRACE
    printf("    ");
    for (int i = 0x1FF; i > 0x100 + cpu.SP; i--) {
        printf("%02X ", memory_readb(i));
    }
#endif
    printf("\n");
}

