# Small Windows DLL for the PE plugin pipeline (tests/pe_fixture.py links it
# like MSVC: base relocations for every absolute address, a switch table in
# the code section). Built with clang -target i386-pc-windows-msvc.
.intel_syntax noprefix
.text
.globl pe_address
pe_address:                                # relocated immediate
    mov eax, offset value
    ret

.globl pe_load
pe_load:                                   # relocated displacement + internal direct call
    mov eax, dword ptr [value]
    call helper
    ret
helper:
    add eax, 1
    ret

.globl pe_switch
pe_switch:                                 # MSVC switch: relocated table inside .text
    mov eax, dword ptr [esp + 4]
    cmp eax, 2
    ja 9f
    jmp dword ptr [4*eax + table]
1:  mov eax, 10
    ret
2:  mov eax, 20
    ret
3:  mov eax, 30
    ret
9:  mov eax, -1
    ret
    .p2align 2
table:
    .long 1b, 2b, 3b

.globl pe_indirect
pe_indirect:                               # indirect call through a relocated code pointer
    mov ecx, offset helper
    mov eax, 5
    call ecx
    ret

callback:                                  # reachable only through a pointer in .data
    mov eax, 77
    ret 4                                  # stdcall

.data
value:
    .long 41
callbacks:
    .long callback
