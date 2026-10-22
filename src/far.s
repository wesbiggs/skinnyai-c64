; "Far" memory: the 12K of RAM under the I/O area and the KERNAL ROM,
; $D000-$FFF7, which holds the chat history. Reaching it means switching
; both out ($01 = $34) with interrupts off; the NMI that brings in serial
; bytes still arrives, through the RAM vector at $FFFA, and acia.s's
; handler switches I/O back in for itself.

        .export _far_init, _far_peek, _far_poke, _far_next, _far_move
        .import nmi_entry, popax, popa
        .importzp ptr1, ptr2, ptr3, tmp1

ALLRAM  = $34

        .code

; void far_init(void): RAM vectors for when the KERNAL is switched out.
_far_init:
        lda     #<nmi_entry
        sta     $FFFA
        lda     #>nmi_entry
        sta     $FFFB
        lda     #<irq_stub
        sta     $FFFE
        lda     #>irq_stub
        sta     $FFFF
        rts

irq_stub:
        rti

; unsigned char __fastcall__ far_peek(unsigned int addr)
_far_peek:
        sta     ptr1
        stx     ptr1+1
        ldy     #0
        sei
        lda     $01
        tax
        lda     #ALLRAM
        sta     $01
        lda     (ptr1),y
        stx     $01
        cli
        ldx     #0
        rts

; void __fastcall__ far_poke(unsigned int addr, unsigned char c)
_far_poke:
        sta     tmp1
        jsr     popax
        sta     ptr1
        stx     ptr1+1
        ldy     #0
        sei
        lda     $01
        tax
        lda     #ALLRAM
        sta     $01
        lda     tmp1
        sta     (ptr1),y
        stx     $01
        cli
        rts

; unsigned int __fastcall__ far_next(unsigned int addr): the address after
; the 0 that ends the string at addr.
_far_next:
        sta     ptr1
        stx     ptr1+1
        ldy     #0
        sei
        lda     $01
        sta     tmp1
        lda     #ALLRAM
        sta     $01
@loop:  lda     (ptr1),y
        beq     @end
        iny
        bne     @loop
        inc     ptr1+1
        bne     @loop
@end:   lda     tmp1
        sta     $01
        cli
        iny                     ; past the 0
        tya
        clc
        adc     ptr1
        ldx     ptr1+1
        bcc     @out
        inx
@out:   rts

; void __fastcall__ far_move(unsigned int dst, unsigned int src, unsigned int n)
; Copies downward (dst < src), so overlapping is fine.
_far_move:
        sta     ptr3
        stx     ptr3+1
        jsr     popax
        sta     ptr1
        stx     ptr1+1
        jsr     popax
        sta     ptr2
        stx     ptr2+1
        sei
        lda     $01
        sta     tmp1
        lda     #ALLRAM
        sta     $01
        ldy     #0
        ldx     ptr3+1
        beq     @rest
@page:  lda     (ptr1),y
        sta     (ptr2),y
        iny
        bne     @page
        inc     ptr1+1
        inc     ptr2+1
        dex
        bne     @page
@rest:  ldx     ptr3
        beq     @done
@byte:  lda     (ptr1),y
        sta     (ptr2),y
        iny
        dex
        bne     @byte
@done:  lda     tmp1
        sta     $01
        cli
        rts
