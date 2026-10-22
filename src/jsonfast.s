; The JSON tokenizer's hot path. Most bytes of a streamed reply are plain
; characters inside keys and values ("created_at":"2026-..."), so those are
; stored here; anything else goes to json_feed_slow in json.c.

        .export _json_feed
        .import _json_feed_slow
        .import _jstate, _jis_key, _jstreaming, _jklen, _jvlen
        .import _json_key, _json_val

J_STR   = 2

; void __fastcall__ json_feed(unsigned char c)
_json_feed:
        ldx     _jstate
        cpx     #J_STR
        bne     slow
        cmp     #$22            ; "
        beq     slow
        cmp     #$5C            ; backslash
        beq     slow
        cmp     #$20
        bcc     slow
        cmp     #$80
        bcs     slow
        ldx     _jis_key
        bne     key
        ldx     _jstreaming
        bne     slow
        ldx     _jvlen
        cpx     #63
        bcs     done
        sta     _json_val,x
        inc     _jvlen
done:   rts
key:    ldx     _jklen
        cpx     #23
        bcs     done
        sta     _json_key,x
        inc     _jklen
        rts
slow:   jmp     _json_feed_slow
