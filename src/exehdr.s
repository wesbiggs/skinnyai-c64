; Load address and the BASIC line "10 SYS4096" that starts the program.

        .segment "LOADADDR"
        .word   $0801

        .segment "EXEHDR"
        .word   @next           ; link to the next line
        .word   10              ; line number
        .byte   $9E             ; SYS
        .byte   "4096"
        .byte   0
@next:  .word   0               ; end of program
