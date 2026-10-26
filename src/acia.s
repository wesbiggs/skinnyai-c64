; SwiftLink (6551 ACIA at $DE00) driver, as emulated by the TeensyROM.
;
; Received bytes arrive by NMI into a 256-byte ring at $CF00. When the ring
; is 3/4 full the handler raises RTS (command register bits 3-2 = 00), which
; makes the TeensyROM hold further bytes in its own large buffer until the
; program has caught up; acia_get lowers it again below 1/4. So nothing is
; lost however long the screen takes to scroll or a pager waits for a key.
;
; On a TeensyROM (net.c asks it with ATI) the receive interrupt is turned
; off instead and the chip is polled: the TeensyROM hands over the next
; byte only once the last has been read, so nothing is lost there either,
; and the ~100 cycles an NMI costs each byte are saved. Polled, the rate
; only sets the least gap between bytes, so all of Turbo232's rates work. A real
; 6551 receives at the line rate whether or not it has been read, so it
; needs the NMI. The ring is still read first (a RESTORE NMI may have put
; a byte there).

        .export _acia_init, _acia_done, _acia_get, _acia_put
        .export _acia_count, _acia_flush, _ticks
        .export _acia_speed, _acia_baud, _acia_setpoll, _acia_poll
        .export _charset_init, _reset, nmi_entry
        .export _acia_head := head, _acia_tail := tail, _acia_held := held

ACIA_DATA   = $DE00
ACIA_STATUS = $DE01
ACIA_CMD    = $DE02
ACIA_CTRL   = $DE03
ACIA_TURBO  = $DE07     ; Turbo232's enhanced speed register
RING        = $CF00
NMIVEC      = $0318

CMD_ON      = $09       ; DTR on, receive NMI on, RTS low: send to us
CMD_HOLD    = $01       ; DTR on, receive NMI on, RTS high: hold
CMD_OFF     = $02       ; receive NMI off
CMD_POLL    = $0B       ; DTR on, receive NMI off, RTS low: polled
CTRL_38400  = $1F       ; 8N1, internal clock, 38400 baud on a SwiftLink
CTRL_TURBO  = $10       ; 8N1, internal clock, rate from ACIA_TURBO

HIGH_WATER  = 192
LOW_WATER   = 64

        .data
_acia_baud: .byte 0     ; 0: 38400, 1: 57600, 2: 115200, 3: 230400
_acia_poll: .byte 0     ; receiving by polling, not NMI
cmdon:  .byte CMD_ON    ; the command for receiving: CMD_ON or CMD_POLL

        .rodata
turbo:  .byte   $02, $01, $00   ; ACIA_TURBO for 57600, 115200, 230400

        .bss
want:   .res 1
head:   .res 1
tail:   .res 1
held:   .res 1
oldnmi: .res 2

        .code

; void acia_init(void)
_acia_init:
        sta     ACIA_STATUS     ; programmed reset: receive interrupts off
        lda     #0
        sta     head
        sta     tail
        sta     held
        sei
        lda     NMIVEC
        sta     oldnmi
        lda     NMIVEC+1
        sta     oldnmi+1
        lda     #<nmi
        sta     NMIVEC
        lda     #>nmi
        sta     NMIVEC+1
        jsr     _acia_speed
        lda     cmdon
        sta     ACIA_CMD
        lda     ACIA_STATUS
        lda     ACIA_DATA
        cli
        rts

; void acia_setpoll(unsigned char on): receive by polling (a TeensyROM) or
; by NMI.
_acia_setpoll:
        sta     _acia_poll
        ldx     #CMD_ON
        cmp     #0
        beq     @set
        ldx     #CMD_POLL
@set:   stx     cmdon
        stx     ACIA_CMD
        lda     #0
        sta     held
        rts

; unsigned char acia_speed(void): sets the rate acia_baud names, returning
; false (and going back to 38400) if it can't. net.c sets the faster ones
; only when polling a TeensyROM: by NMI, the TeensyROM's gap between bytes
; at 115200 and 230400 (68 and 34 cycles) is less than the handler takes,
; so each NMI would interrupt the one before until the stack overflowed.
; Above 38400 is Turbo232's: the control register's rate bits at 0 to say "use the speed
; register", then the speed register, which a Turbo232 (and VICE) only
; accepts in that order; the TeensyROM takes either. A Turbo232 reads the
; speed register back with bit 2 set (enhanced speed on). A plain SwiftLink
; has no such register (open bus, or a mirror of another one) and no
; external clock for those rate bits, so to tell, two different values must
; each read back exactly, and if not it goes back to 38400.
_acia_speed:
        ldx     _acia_baud
        beq     @std
        lda     #CTRL_TURBO
        sta     ACIA_CTRL
        lda     #3              ; first a value that sets no rate
        jsr     @try
        bne     @plain
        ldx     _acia_baud
        lda     turbo-1,x
        jsr     @try
        bne     @plain
        lda     #1
        ldx     #0
        rts
@try:   sta     ACIA_TURBO      ; Z set if it reads back with bit 2 added
        ora     #$04
        sta     want
        lda     ACIA_TURBO
        and     #$07
        cmp     want
        rts
@plain: lda     #0
        sta     _acia_baud
        lda     #CTRL_38400
        sta     ACIA_CTRL
        lda     #0
        tax
        rts
@std:   lda     #CTRL_38400
        sta     ACIA_CTRL
        lda     #1
        ldx     #0
        rts

; void acia_done(void)
_acia_done:
        lda     #CMD_OFF
        sta     ACIA_CMD
        sei
        lda     oldnmi
        sta     NMIVEC
        lda     oldnmi+1
        sta     NMIVEC+1
        cli
        rts

; Entered from the KERNAL's NMI entry ($FE43: SEI, JMP ($0318)), or
; straight from the RAM vector at $FFFA while far.s has the KERNAL and I/O
; switched out, with no registers saved. NMIs that aren't the ACIA's
; (RESTORE) are ignored.
;
; Reading the data register lets a TeensyROM send the next byte, which it
; does as soon as the rate's gap since the last one has passed (136 cycles
; at 57600). Badlines and the title bar's sprites can stretch this handler
; past that, so the next NMI may arrive before it returns. So the byte's
; slot is taken (head moved on) and the hold decided before the data is
; read: an NMI that interrupts after that point stores its byte in the
; next slot, and this one's lands in its own before the program sees it.
nmi:
nmi_entry:
        pha
        lda     $01
        pha
        lda     #$35            ; I/O in
        sta     $01
        lda     ACIA_STATUS     ; also acknowledges the interrupt
        and     #$08            ; receive register full?
        beq     @out
        txa
        pha
        ldx     head
        inx
        cpx     tail
        beq     @full           ; ring full: drop (shouldn't happen)
        stx     head
        txa
        sec
        sbc     tail
        cmp     #HIGH_WATER
        bcc     @read
        lda     #CMD_HOLD
        sta     ACIA_CMD
        sta     held
@read:  dex
        lda     ACIA_DATA
        sta     RING,x
        pla
        tax
@out:   pla
        sta     $01
        pla
        rti
@full:  lda     ACIA_DATA
        pla
        tax
        pla
        sta     $01
        pla
        rti

; int acia_get(void): the next byte, or -1 if none has arrived.
_acia_get:
        ldx     tail
        cpx     head
        beq     @none
        lda     RING,x
        inx
        stx     tail
        ldx     held
        bne     @check
        ldx     #0
        rts
@check: pha
        lda     head
        sec
        sbc     tail
        cmp     #LOW_WATER
        bcs     @keep
        lda     #0
        sta     held
        lda     cmdon
        sta     ACIA_CMD
@keep:  pla
        ldx     #0
        rts
@none:  lda     _acia_poll
        beq     @nil
        lda     ACIA_STATUS
        and     #$08            ; receive register full?
        beq     @nil
        lda     ACIA_DATA
        ldx     #0
        rts
@nil:   lda     #$FF
        tax
        rts

; void acia_put(unsigned char c): waits until the transmitter is free.
_acia_put:
        pha
        lda     held            ; a held line can't transmit: release it
        beq     @wait
        lda     #0
        sta     held
        lda     cmdon
        sta     ACIA_CMD
@wait:  lda     ACIA_STATUS
        and     #$10
        beq     @wait
        pla
        sta     ACIA_DATA
        rts

; unsigned char acia_count(void)
_acia_count:
        lda     head
        sec
        sbc     tail
        ldx     #0
        rts

; void acia_flush(void): discard whatever has been received (polled, the
; byte waiting in the chip; the TeensyROM's buffer goes on sending).
_acia_flush:
        lda     _acia_poll
        beq     @ring
        lda     ACIA_STATUS
        and     #$08
        beq     @ring
        lda     ACIA_DATA
@ring:  lda     head
        sta     tail
        lda     held
        beq     @ok
        lda     #0
        sta     held
        lda     cmdon
        sta     ACIA_CMD
@ok:    rts

; unsigned int ticks(void): the low 16 bits of the jiffy clock (1/60 s).
_ticks:
        sei
        lda     $A2
        ldx     $A1
        cli
        rts

; void charset_init(void): copies the ROM's lower/upper case character set
; to $0800 (over the BASIC line that started the program, which has done
; its job) and points the VIC at it. The screen stays at $0400.
_charset_init:
        sei
        lda     $01
        pha
        lda     #$32            ; character ROM in at $D000
        sta     $01
        lda     #$D8            ; second half of the ROM: lower case set
        sta     @src+2
        lda     #$08
        sta     @dst+2
        ldx     #8
        ldy     #0
@src:   lda     $D800,y
@dst:   sta     $0800,y
        iny
        bne     @src
        inc     @src+2
        inc     @dst+2
        dex
        bne     @src
        pla
        sta     $01
        cli
        lda     #$12            ; screen $0400, characters $0800
        sta     $D018
        lda     #$80            ; SHIFT+C= must not switch character sets
        sta     $0291
        rts

; void reset(void): leaves the way a cc65 program must when its memory
; has been reused (it can't be RUN again): a reset, which on a TeensyROM
; goes back to its menu.
_reset:
        sei
        jmp     ($FFFC)

; unsigned char rx_pump(void): feeds received bytes to json_feed (the plain
; body of a response) until the ring is empty (returns 0), "NO CARRIER"
; has arrived (1), or 256 bytes have gone by (2), so the caller can look at
; the keyboard. rx_nc is how much of "NO CARRIER\r" has matched so far, and
; rx_lo/rx_hi count the bytes.

        .export _rx_pump, _rx_nc, _rx_lo, _rx_hi
        .import _json_feed

        .bss
_rx_nc: .res 1
_rx_lo: .res 1
_rx_hi: .res 2
count:  .res 1

        .rodata
nocarrier:
        .byte   $4E,$4F,$20,$43,$41,$52,$52,$49,$45,$52,$0D

        .code
_rx_pump:
        lda     #0
        sta     count
@loop:  ldx     tail
        cpx     head
        beq     @chip
        lda     RING,x
        inx
        stx     tail
        ldy     held
        beq     @match
        pha                     ; release the hold below the low water mark
        lda     head
        sec
        sbc     tail
        cmp     #LOW_WATER
        bcs     @keep
        lda     #0
        sta     held
        lda     cmdon
        sta     ACIA_CMD
@keep:  pla
@match: ldy     _rx_nc
        cmp     nocarrier,y
        bne     @no
        iny
        sty     _rx_nc
        cpy     #11
        bne     @feed
        lda     #1
        ldx     #0
        rts
@no:    ldy     #0
        cmp     #$4E            ; N
        bne     @set
        iny
@set:   sty     _rx_nc
@feed:  inc     _rx_lo
        bne     @call
        inc     _rx_hi
        bne     @call
        inc     _rx_hi+1
@call:  jsr     _json_feed
        inc     count
        bne     @loop
        lda     #2
        ldx     #0
        rts
@chip:  lda     _acia_poll      ; polled: straight from the chip
        beq     @empty
        lda     ACIA_STATUS
        and     #$08
        beq     @empty
        lda     ACIA_DATA
        jmp     @match
@empty: lda     #0
        tax
        rts
