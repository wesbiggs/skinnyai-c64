; Code that only runs at start-up (the BOOT segment: main.c's first-run
; questions, the welcome, and the rest of what comes before the first
; prompt) runs in the line editor's recall buffer, which is empty until the
; first line is typed, so it takes no memory after that. It's loaded with
; the rest, after the ONCE segment, where the BSS goes; this constructor,
; which runs before the BSS is cleared, copies it to the buffer.

        .export _recall
        .constructor copy_boot
        .import __BOOT_LOAD__, __BOOT_RUN__, __BOOT_SIZE__, __RECALL_START__
        .import _memcpy, pushax

_recall = __RECALL_START__              ; input.c's char recall[]

        .segment "ONCE"

copy_boot:
        lda     #<__BOOT_RUN__
        ldx     #>__BOOT_RUN__
        jsr     pushax
        lda     #<__BOOT_LOAD__
        ldx     #>__BOOT_LOAD__
        jsr     pushax
        lda     #<__BOOT_SIZE__
        ldx     #>__BOOT_SIZE__
        jmp     _memcpy
