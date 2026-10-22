; "Far" memory for the chat history, addressed by offset from 0. It is
; either:
;
; - an REU (RAM Expansion Unit, registers at $DF00), if far_init finds
;   one: its first 64K, reached by DMA through a 128-byte cache line in the
;   cassette buffer; or
; - the 12K of RAM under the I/O area and the KERNAL ROM, $D000-$FFF7.
;   Reaching it means switching both out ($01 = $34) with interrupts off;
;   the NMI that brings in serial bytes still arrives, through the RAM
;   vector at $FFFA, and acia.s's handler switches I/O back in for itself.

        .export _far_init, _far_peek, _far_poke, _far_move, _reu_banks
        .import nmi_entry, popax
        .importzp ptr1, ptr2, ptr3, tmp1

ALLRAM  = $34
BASE    = $D000                 ; the RAM history
CACHE   = $0380                 ; the REU cache line, $0380-$03FF
REU     = $DF00
STASH   = $90                   ; execute now, C64 to REU
FETCH   = $91                   ; execute now, REU to C64

        .bss
_reu_banks: .res 1              ; 64K banks in the REU (0: none, 255: 16M)
cline:  .res 2                  ; REU offset of the cached line ($FFFF: none)
dirty:  .res 1                  ; the cache line has changed
xlen:   .res 1                  ; transfer length
marker: .res 1

        .code

; unsigned char far_init(void): sets up the RAM vectors for when the KERNAL
; is switched out, and looks for an REU; returns its size in 64K banks.
_far_init:
        lda     #<nmi_entry
        sta     $FFFA
        lda     #>nmi_entry
        sta     $FFFB
        lda     #<irq_stub
        sta     $FFFE
        lda     #>irq_stub
        sta     $FFFF
        lda     #$FF
        sta     cline
        sta     cline+1
        lda     #0
        sta     dirty
        sta     _reu_banks
        lda     #128
        sta     xlen
        ; an REU's address registers keep what's written to them
        ldx     #2
@w:     txa
        sta     REU,x
        inx
        cpx     #6
        bne     @w
        ldx     #2
@r:     txa
        cmp     REU,x
        bne     @done
        inx
        cpx     #6
        bne     @r
        ; its size: put each bank's number in its first byte, highest
        ; first, so a smaller REU's wrapped-around banks are overwritten
        ldx     #255
@mark:  stx     marker
        lda     #STASH
        jsr     xfer_byte
        dex
        cpx     #255
        bne     @mark
        ldx     #0              ; count the banks that kept their number
@count: lda     #FETCH
        jsr     xfer_byte
        cpx     marker
        bne     @size
        inx
        bne     @count
        dex                     ; 256 banks: report 255
@size:  stx     _reu_banks
@done:  lda     _reu_banks
        ldx     #0
        rts

; One byte between marker and offset 0 of REU bank X; A = command.
xfer_byte:
        pha
        lda     #<marker
        sta     REU+2
        lda     #>marker
        sta     REU+3
        lda     #0
        sta     REU+4
        sta     REU+5
        sta     REU+8
        sta     REU+10
        stx     REU+6
        lda     #1
        sta     REU+7
        pla
        sta     REU+1
        rts

irq_stub:
        rti

; xlen bytes between the cache and REU offset cline; A = command.
xfer:
        pha
        lda     #<CACHE
        sta     REU+2
        lda     #>CACHE
        sta     REU+3
        lda     cline
        sta     REU+4
        lda     cline+1
        sta     REU+5
        lda     #0
        sta     REU+6
        sta     REU+8
        sta     REU+10
        lda     xlen
        sta     REU+7
        pla
        sta     REU+1
        rts

; Brings the line holding offset ptr1 into the cache; Y = its place there.
seek:
        lda     ptr1
        and     #$80
        cmp     cline
        bne     @load
        lda     ptr1+1
        cmp     cline+1
        beq     @hit
@load:  lda     dirty
        beq     @fetch
        lda     #STASH          ; write the old line back first
        jsr     xfer
@fetch: lda     ptr1
        and     #$80
        sta     cline
        lda     ptr1+1
        sta     cline+1
        lda     #FETCH
        jsr     xfer
        lda     #0
        sta     dirty
@hit:   lda     ptr1
        and     #$7F
        tay
        rts

; unsigned char __fastcall__ far_peek(unsigned int off)
_far_peek:
        sta     ptr1
        stx     ptr1+1
        ldy     _reu_banks
        beq     @ram
        jsr     seek
        lda     CACHE,y
        ldx     #0
        rts
@ram:   txa
        clc
        adc     #>BASE
        sta     ptr1+1
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

; void __fastcall__ far_poke(unsigned int off, unsigned char c)
_far_poke:
        sta     tmp1
        jsr     popax
        sta     ptr1
        stx     ptr1+1
        ldy     _reu_banks
        beq     @ram
        jsr     seek
        lda     tmp1
        sta     CACHE,y
        lda     #1
        sta     dirty
        rts
@ram:   txa
        clc
        adc     #>BASE
        sta     ptr1+1
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

; void __fastcall__ far_move(unsigned int dst, unsigned int src, unsigned int n)
; Copies downward (dst < src), so overlapping is fine.
_far_move:
        sta     ptr3
        stx     ptr3+1
        jsr     popax
        sta     ptr1            ; src
        stx     ptr1+1
        jsr     popax
        sta     ptr2            ; dst
        stx     ptr2+1
        lda     _reu_banks
        bne     reu_move
        lda     ptr1+1
        clc
        adc     #>BASE
        sta     ptr1+1
        lda     ptr2+1
        clc
        adc     #>BASE
        sta     ptr2+1
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

; In the REU: through the cache, up to 128 bytes at a time.
reu_move:
        lda     dirty
        beq     @loop
        lda     #STASH          ; save the cached line first
        jsr     xfer
@loop:  lda     ptr3
        ora     ptr3+1
        beq     @done
        lda     #128
        ldx     ptr3+1
        bne     @len
        cmp     ptr3
        bcc     @len
        lda     ptr3
@len:   sta     xlen
        lda     ptr1
        sta     cline
        lda     ptr1+1
        sta     cline+1
        lda     #FETCH
        jsr     xfer
        lda     ptr2
        sta     cline
        lda     ptr2+1
        sta     cline+1
        lda     #STASH
        jsr     xfer
        lda     ptr1
        clc
        adc     xlen
        sta     ptr1
        bcc     @s
        inc     ptr1+1
@s:     lda     ptr2
        clc
        adc     xlen
        sta     ptr2
        bcc     @d
        inc     ptr2+1
@d:     lda     ptr3
        sec
        sbc     xlen
        sta     ptr3
        bcs     @loop
        dec     ptr3+1
        jmp     @loop
@done:  lda     #128            ; the cache now holds nothing
        sta     xlen
        lda     #$FF
        sta     cline
        sta     cline+1
        lda     #0
        sta     dirty
        rts
