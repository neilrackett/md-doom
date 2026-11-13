; Copyright (C) 2026 Neil Rackett
; SPDX-License-Identifier: GPL-3.0-or-later
;
; userfw.s -- user firmware module.
;
; Entry point: USERFW ($FA0800), reached from main.s either directly
; (early-boot fast path) or via the rom_function dispatcher when the
; RP issues CMD_START.
;
; Pipeline (per VBL):
;   1. Custom VBL handler at $70 (installed once at boot) wakes the
;      main loop by clearing a flag in ST RAM. We DON'T use XBIOS
;      Vsync (trap #14, #37) -- that trips through TOS's GEMDOS-aware
;      dispatch and adds latency / jitter.
;   2. Read FB_FRAME_COUNTER_ADDR ($FA400C). The RP increments this
;      after every fb_render_frame() with a memory barrier. If
;      unchanged since last iteration (D4), the FB has nothing new
;      and we skip the blit + flip entirely.
;   3. Copy the 32 KB cart framebuffer ($FA8300) into the hidden
;      ST screen page selected by A4. The copy is a pure 68000 CPU
;      MOVEM burst expanded inline via FBDRV_INLINE -- same code on
;      plain ST / STE / MegaSTE / TT / Falcon (no _MCH cookie
;      dispatch, no STE blitter path).
;   4. Flip the video base to the screen we just wrote.
;   5. Toggle A4 between SCREEN_A and SCREEN_B for the next frame.
;   6. Poll CMD_MAGIC_SENTINEL_ADDR ($FA4000). The RP-side IKBD demux
;      writes CMD_BOOT_GEM there when it decodes an ESC press. On
;      match, restore vectors / MFP / VBL / screen base and rts back
;      to the cartridge dispatcher.
;
; ST high res (640x400 on a mono monitor) takes the same 32000 bytes
; through the same copy, but its VBLs come at 71 Hz, 14 ms apart, and the
; copy takes ~17.6 ms. So in mono a frame is two VBLs: the first copies
; the first half of the cart FB, the second the rest, then flips. Audio
; and the reports to the RP still happen on every VBL. See UFW_MONO.
;
; IRQ ownership: TOS's HBL ($68), Timer-A/B/C/D
; ($134/$120/$114/$110), and ACIA ($118) handlers are stubbed to
; single-rte dummies and their MFP IERA/IERB bits are cleared so no
; MFP source can fire. Only the custom VBL handler at $70 stays
; active.
;
; IKBD bytes are forwarded interrupt-driven by userfw_acia_irq: on each
; ACIA RX interrupt it reads the byte and emits it via a cart-bus read
; at IKBD_WINDOW_BASE + byte ($FB8200..$FB82FF, md-devops single-byte
; ABI). Reading on interrupt (rather than polling inside the blit) means
; no byte is lost, so multi-byte joystick packets survive. The RP
; captures the read via the commemul PIO+DMA ring (no per-read CPU
; overhead) and runs the IKBD demux from its main loop.
;
; --- Constants ----------------------------------------------------

; Atari ST shifter video base registers (68000-compatible, present
; on every ST/STE/MegaSTE/TT/Falcon). Only HIGH+MID are written;
; the STE-only LOW byte at $FFFF820D stays at TOS's default of 0,
; which matches our 256-byte-aligned hidden screens at $70000 and
; $78000.
VIDEO_BASE_ADDR_HIGH  equ $FFFF8201
VIDEO_BASE_ADDR_MID   equ $FFFF8203
VIDEO_COUNT_MID       equ $FFFF8207          ; video address counter, bits 8..15

; Palette index 0 doubles as the border colour. We poke it at three
; points in the VBL loop so the ST border visualises blit timing
; (cherry-picked from md-sprites-demo). Foreground text in the FB also
; uses idx 0, so during BLIT_MARK_RUNNING the white text momentarily
; becomes black -- harmless since the blit is only a few ms long.
PALETTE_BASE          equ $FFFF8240          ; 16 hardware palette words ($FFFF8240..$FFFF825E)
PALETTE_IDX0          equ PALETTE_BASE
BLIT_MARK_VSYNC       equ $000               ; black: vsync returned, copy not yet started
BLIT_MARK_RUNNING     equ $777               ; white: cart->ST copy in flight
BLIT_MARK_DONE        equ $070               ; green: FBDRV_INLINE returned

; Shifter resolution: 0 = low, 1 = medium, 2 = high (mono). main.s has
; already switched anything but low and high to low, so this tells
; colour from mono.
SHIFTER_RES           equ $FFFF8260
SHIFTER_RES_HIGH      equ 2
; In mono only bit 0 of colour 0 counts: set, and a 0 pixel is white
; and a 1 black, which is what the RP draws (ink bits). Written once at
; boot; the RP's palette slot means nothing in mono and is not copied.
MONO_PAPER_WHITE      equ $777

; FBDRV_DEBUG_MARKS = 1 paints palette-idx-0 with the three border
; band colours above (black/white/green) at vsync / blit-running /
; blit-done. Useful for timing measurement on a CRT but flickers
; any visible content drawn in palette idx 0 (incl. the cart-side
; palette publish below) -- so it is off by default. Set to
; 1 when measuring.
FBDRV_DEBUG_MARKS     equ 0

; Scratch word in TOS's _dskbufp ($4C6..$4C9). userfw does no disk
; I/O, so the slot is fair game while userfw owns the machine.
; .vbl_loop arms this to -1 then `stop`s; userfw_vbl clears it. The
; m68k re-stops on any non-VBL IRQ (the MFP's) and only exits the
; wait when the VBL handler has cleared the flag.
UFW_VBL_FLAG          equ $4C6               ; word: cleared by userfw_vbl, polled after each `stop`

; Per-VBL state area at the end of SCREEN_A's 32 KB allocation.
; Used by FBDRV_INLINE to spill A7 (SP) around the MOVEM-burst that
; includes A7 in its register list; the current-page pointer
; UFW_SCREEN_PAGE and the saved TOS VBL vector / Physbase result
; also live here, as do the three Xpad slots. 28 bytes used from
; $77FD0; SCREEN_A's tail at $77D00 has 768 bytes available (shifter
; only reads 200*160 = 32000 B of each screen page, allocation is
; 32 KB), of which the first 500 are the STE DMA sound buffer.
UFW_SND_LEN           equ $00077F00          ; word: current DMA buffer length in bytes
UFW_SND_PREVPOS       equ $00077F02          ; word: DMA offset within its buffer, last VBL
UFW_SND_TICK          equ $00077F04          ; byte: frame counter, steer on every 4th
UFW_MONO              equ $00077F06          ; byte: 1 = ST high res (mono monitor)
UFW_HALF              equ $00077F07          ; byte: $FF = mono frame half copied, rest next VBL
UFW_YM_SEQ            equ $00077F08          ; word: last music packet written to the YM
UFW_YM_MUSIC          equ $00077F0A          ; byte: $FF = no DMA, and the YM is the music's
UFW_TWO_VBL           equ $00077F0B          ; byte: $FF = a frame is two VBLs (mono, or no DMA sound)
UFW_YM_BANK           equ $00077F0C          ; longword: the YM sample bank this VBL plays
UFW_SP_SAVE           equ $00077F10          ; longword: the stack userfw was entered on
UFW_PAL_LATCH        equ $00077F20          ; 32 B: the palette of the page being copied
UFW_PAL_SHOW          equ $0007FF00          ; 32 B: and of the page on screen (page B's tail)
UFW_HUD_NEXT          equ $00077F40          ; 32 B: status bar palette of the page being copied
UFW_HUD_SHOW          equ $00077F60          ; 32 B: and of the page on screen
UFW_SPLIT_NEXT        equ $00077F80          ; word: non-zero = that page has one
UFW_SPLIT_SHOW        equ $00077F82          ; word: as UFW_SPLIT_NEXT, for the page on screen
UFW_SPLIT_MID         equ $00077F84          ; byte: video counter bits 8..15 at the split
UFW_CPU               equ $00077F86          ; word: the _CPU cookie, 0/10/20/30/40/60
UFW_OLD_CACR          equ $00077F88          ; longword: a 68020 or later's CACR, at start
UFW_FALCON_SND        equ $00077F8C          ; byte: $FF = a Falcon's sound (_SND bit 2, the codec)
UFW_OLD_8900          equ $00077F8D          ; byte: its $FF8900 as TOS left it
UFW_EIGHT             equ $00077F8E          ; byte: 0, or 256 colours: EIGHT_TT / EIGHT_FALCON
UFW_TT                equ $00077F8F          ; byte: SNDCAP_TT on a TT, else 0
UFW_SPK_STATE         equ $00077F90          ; byte: TT speaker setting last applied ($00 off, $FF on, $01 none yet)
UFW_SPK_PORTA         equ $00077F91          ; byte: YM port A as TOS left it (TT: bit 6 put back on exit)
UFW_XPAD_HDR          equ $00077FD0          ; longword: Xpad block base, 0 = no provider
UFW_XPAD_BUF0         equ $00077FD4          ; longword: address of pad 0 in buffer 0
UFW_XPAD_BUF1         equ $00077FD8          ; longword: address of pad 0 in buffer 1
UFW_VBL_VEC_SAVE      equ $00077FE0          ; longword: TOS VBL vector ($70)
UFW_HAS_DMA           equ $00077FE4          ; byte: 1 = STE DMA sound detected
UFW_PHYSBASE_SAVE     equ $00077FE8          ; longword: XBIOS Physbase result
UFW_SCREEN_PAGE       equ $00077FEC          ; longword: current draw page address

; fbdrv iteration arithmetic. Pulled out as equs so the macro body
; below doesn't carry literal magic numbers. FBDRV_TOTAL_BYTES is
; derived from FB_COPY_LINES (defined further down with the other
; framebuffer constants); change FB_COPY_LINES in one place to
; throttle how many ST scanlines the per-VBL copy touches.
;
; FBDRV_TOTAL_BYTES must be divisible by FBDRV_ITER_BYTES (48) so
; the unrolled REPT covers the full byte count without a tail.
; FB_COPY_LINES * 160 byte rows / 48 byte iters: 150*160/48=500,
; 200*160/48=666r32. For values that don't divide evenly the trailing
; bytes are simply not copied (they remain stale on the screen page).
FBDRV_ITER_BYTES      equ 48                            ; 12 longwords: D0-D7 + A1-A4 (A6=src, A5=dst, A0=dedicated audio pointer, A7=SP preserved -- IRQs may fire during the macro).
FBDRV_TOTAL_BYTES     equ (FB_COPY_LINES * FB_ROW_BYTES) ; honours FB_COPY_LINES
FBDRV_MAIN_ITERS      equ (FBDRV_TOTAL_BYTES / FBDRV_ITER_BYTES)
FBDRV_MAIN_BYTES      equ (FBDRV_MAIN_ITERS * FBDRV_ITER_BYTES)
FBDRV_TAIL_BYTES      equ (FBDRV_TOTAL_BYTES - FBDRV_MAIN_BYTES)  ; 20 bytes at FB_COPY_LINES=200 (= 5 longwords)
FBDRV_TAIL_DISP       equ FBDRV_MAIN_BYTES                        ; tail goes at page_start + FBDRV_MAIN_BYTES (= 31980)
; The two parts a mono frame is copied in, one per VBL (see UFW_MONO). In
; colour the second simply follows the first.
FBDRV_ITERS_A         equ (FBDRV_MAIN_ITERS / 2)                  ; 333
FBDRV_ITERS_B         equ (FBDRV_MAIN_ITERS - FBDRV_ITERS_A)      ; 333

;----------------------------------------------------------------
; FBDRV_INLINE -- fully unrolled cart->ST screen framebuffer copy.
;
; Each REPT iteration emits:
;   movem.l (a6)+, d0-d7/a1-a4   ; 4 B, ~108 cycles  -- read 48 B forward
;   movem.l d0-d7/a1-a4, -(a5)   ; 4 B, ~104 cycles  -- predec store
;
; A7 (SP) and A0 are intentionally NOT in the MOVEM list:
;   - A7: keeps the supervisor SP valid so IRQs can fire safely
;     during the macro.
;   - A0: dedicated to the YM sample handler's read pointer.
;     With A0 stable across the macro, the IRQ handler doesn't have
;     to save/restore it (-24 cyc/IRQ * ~125 IRQ/VBL = ~3000 cyc/VBL).
;     The inline IKBD poll uses A1 (which IS in the MOVEM list and
;     gets reloaded each iter) so it never disturbs A0.
;
; Predec mode is 4 cyc faster per iter than d16(a5) displacement
; (8+8n vs 12+8n on 68000). The catch: predec writes each 52-byte
; chunk into the destination ST page in REVERSE order relative to
; the source -- chunks land from the screen-page END (offset 31980)
; down to the START (offset 0). For the displayed image to look
; correct, the RP-side fb_chunky_to_planar pre-reverses chunks in
; the cart FB at $FA8300, so the m68k's reversal restores the
; natural image. See "Framebuffer chunk layout for the m68k MOVEM
; blit" in rp/src/include/cart_shared.h for the full spec.
;
; The macro copies \1 chunks, so the frame can be copied in two parts
; (FBDRV_ITERS_A then FBDRV_ITERS_B), followed by FBDRV_TAIL.
;
; Caller protocol (must be set up BEFORE the first part):
;   A6 = UFW_FB_SRC
;   A5 = destination ST screen page END
;        ($70000 + 31980 or $78000 + 31980; .vbl_loop adds the
;        FBDRV_MAIN_ITERS * FBDRV_ITER_BYTES offset via LEA after
;        loading UFW_SCREEN_PAGE).
;
; Clobbers: D0-D7, A1-A4. A0 and A7 are PRESERVED (A0 for the
; YM sample pointer, A7 for IRQ-safe SP).
; A5 and A6 move \1 chunks: after both parts
; A5 = original SCREEN_PAGE end - (FBDRV_MAIN_ITERS * FBDRV_ITER_BYTES)
; = original page START, which is the value .after_copy expects in A5.
;
; Code size: 8 B per unrolled iteration * FBDRV_MAIN_ITERS (666)
; = ~5 KB inline, plus the small d16(a5) tail MOVEM at the end.
FBDRV_INLINE          macro
    ; IKBD is serviced interrupt-driven (userfw_acia_irq), so the blit
    ; no longer polls the ACIA inline -- it's a straight MOVEM burst.
    rept    \1
    movem.l (a6)+, d0-d7/a1-a4
    movem.l d0-d7/a1-a4, -(a5)
    endr
                      endm

FBDRV_TAIL            macro
    ; Tail: copy the last FBDRV_TAIL_BYTES bytes of the blitted
    ; region that the chunked main loop can't reach (FB_COPY_LINES *
    ; 160 isn't a multiple of FBDRV_ITER_BYTES=48). A6 is at
    ; UFW_FB_SRC + FBDRV_MAIN_BYTES after the REPT; A5 is back at
    ; page_start. The RP-side fb_chunky_to_planar leaves these tail
    ; bytes in NATURAL (non-reversed) order in cart-FB, so this is a
    ; straight forward-direction copy via d16(a5).
    ;
    ; The register list must hold exactly FBDRV_TAIL_BYTES/4 longs.
    ; Current `d0-d7` covers 32 bytes (= FB_COPY_LINES=200, 32000 -
    ; 666*48 = 32). Other useful settings:
    ;   FB_COPY_LINES=180 -> 600*48 = 28800 exactly, 0-byte tail
    ;                       (the `ifne` skips the block entirely).
    ;   FB_COPY_LINES=198 -> 660*48 = 31680 exactly, 0-byte tail.
    ;   FB_COPY_LINES=199 -> 31840 - 663*48 = 16 B tail (d0-d3).
    ;   FB_COPY_LINES=190 -> 30400 - 633*48 = 16 B tail (d0-d3).
    ; For other tail sizes, manually adjust the register list to
    ; cover FBDRV_TAIL_BYTES/4 longwords.
    ifne    FBDRV_TAIL_BYTES
    movem.l (a6)+, d0-d7
    movem.l d0-d7, FBDRV_TAIL_DISP(a5)
    endc
                      endm

; Atari ST VBL interrupt vector. Replacing TOS's handler here drops
; mouse / cursor-blink / keyboard-repeat updates -- harmless for the
; framebuffer template because we own the screen until ESC exit.
VBL_VECTOR            equ $70

; FB dirty-frame counter (lives in cart shared region at $FA400C). The
; RP fills the framebuffer and then writes a new value here as the
; LAST step of the frame. If this matches D4 (last seen) we skip the
; cart->ST blit + video flip entirely.
FB_FRAME_COUNTER      equ $00FA400C

; RP→m68k command sentinel at $FA4000. The RP IKBD demux writes
; CMD_BOOT_GEM here when it decodes an ESC keypress; userfw's main
; loop polls and exits back to GEM on match. Must agree
; with main.s's CMD_MAGIC_SENTINEL_ADDR / CMD_BOOT_GEM equs.
CMD_MAGIC_SENTINEL    equ $00FA4000
CMD_BOOT_GEM          equ 2
CMD_RESET             equ 1

; 16-entry ST palette slot. 32 bytes of palette words published by the
; RP with each frame; .vbl_loop latches them into UFW_PAL_LATCH as it
; copies the frame and applies them to PALETTE_BASE at the next VBL,
; when that page goes on screen. Mirrors main.s PALETTE_ADDR.
PALETTE_ADDR          equ $00FA4040
PALETTE_SIZE          equ 32

; The status bar's own palette (cart_shared.h, CART_SPLIT_OFFSET): a
; word, non-zero when the frame has one, and its 16 colours, switched in
; at SPLIT_LINE by userfw_hud_irq; latched with the frame like the
; palette.
SPLIT_ADDR            equ $00FA4018          ; shared-variable slot 2
HUD_PALETTE_ADDR      equ $00FA4060          ; slots 20..27
SPLIT_LINE            equ 168                ; CART_SPLIT_LINE: the bar's first line
; Cycles to wait after the video counter reaches the split line before
; writing the bar's colours: the counter runs a little ahead of the
; picture, and the last pixels of the line above must not change. The
; blank that follows is ~190 cycles; the wait, the detection loop (up
; to ~20) and the MOVEM (~76) must fit in it.
HUD_SPLIT_NOPS        equ 4
HUD_SPIN_MAX          equ 1000               ; detection loop bound (~2.5 ms)

; Screen pages live just below TOS RAM top (TT-style 256 KB ST RAM
; assumption -- screens land at $70000/$78000, matching md-sprites-demo).
UFW_SCREEN_A          equ $00070000
UFW_SCREEN_B          equ $00078000

; Where main.s copies this module before running it. userfw executes
; from ST RAM rather than in place from the cartridge, so that the RP
; can reclaim the cartridge's 16 KB code area as ordinary SRAM for the
; Doom zone -- see "Memory layout" in AGENTS.md. 16 KB immediately below
; screen page A, which is the block userfw already commandeers; GEMDOS
; has nothing here at CA_INIT time, before any disk boot. (8 KB at
; $6E000 until the 256-colour path outgrew it.)
; KEEP IN STEP with main.s.
UFW_RAM_DEST          equ $0006C000
UFW_RAM_SIZE          equ $4000
; userfw's own stack, at the top of that window above the image. It
; cannot keep the one it was entered on: from the desktop that is the
; top of the program's TPA, which on a 512 KB machine is $77FF8, in the
; middle of the state above, and the interrupt frames pushed there
; overwrote UFW_SCREEN_PAGE. The build fails at the end of this file if
; the image leaves less than UFW_STACK_SIZE.
UFW_STACK_TOP         equ (UFW_RAM_DEST + UFW_RAM_SIZE)
UFW_STACK_SIZE        equ 512
UFW_SCREEN_XOR       equ (UFW_SCREEN_A ^ UFW_SCREEN_B)

UFW_FB_SRC            equ $00FA8300           ; FRAMEBUFFER_ADDR

; --- YM2149 sound chip (single-channel A 4-bit DAC) ----------------
;
; PSG access: write a register number to $FFFF8800 (latch), then
; write data to $FFFF8802. Reg 8 = ch A volume (low 4 bits). We
; configure ch A as a "fake DAC": tone enabled, period = 0 (DC
; clamp above the audio band so the volume register is the only
; thing driving the output). Reg 8 stays latched after boot, so the
; sample handler just writes a single byte to YM_DATA per fire.
YM_SELECT             equ $FFFF8800
YM_DATA               equ $FFFF8802
YM_REG_MIXER          equ 7                  ; tone+noise enables
YM_REG_CHA_VOL        equ 8                  ; channel A volume (low 4 bits)
YM_REG_CHB_VOL        equ 9                  ; channel B volume (low 4 bits)
YM_REG_PORT_A         equ 14                 ; I/O port A: floppy selects, RTS/DTR, printer strobe, bits 6/7 per machine
YM_PORTA_TT_SPEAKER   equ 6                  ; TT: internal speaker enable, active low (0 = speaker off)
YM_MIXER_DAC_CHA      equ $FE                ; tone A enabled, all other tones/noise off, ports out
YM_MIXER_DAC_AB       equ $FC                ; tones A AND B enabled, tone C off, all noise off, ports out (Ghostbusters dual-channel fake DAC)

; Cart-shared audio sample buffer (mirrors AUDIO_BUFFER_ADDR /
; AUDIO_BUFFER_SIZE in main.s and CART_AUDIO_BUFFER_OFFSET in
; rp/src/include/cart_shared.h). 256 bytes of YM ch A volume
; nibbles, filled by the RP and read by the YM sample handler.
AUDIO_BUFFER_ADDR     equ $00FA4100
AUDIO_BUFFER_SIZE     equ 1024
AUDIO_BUFFER_END      equ (AUDIO_BUFFER_ADDR + AUDIO_BUFFER_SIZE)

; YM2149 music registers, in the last 32 bytes of the audio buffer
; (CART_YM_PACKET_OFFSET in cart_shared.h, written by the RP's
; i_mdmusic.c). Words: a sequence number that changes when there is
; something new, a mask of the registers to write (bit n = R n), then
; R0..R13, each in the low byte of its word. Written on DMA-sound
; machines, where the sound effects are DMA samples and the YM is free,
; and without DMA sound when YM_MUSIC_ADDR gives the YM to the music.
YM_PKT_SIZE           equ 32
YM_PKT_ADDR           equ (AUDIO_BUFFER_END - YM_PKT_SIZE)

; The YM sound effects' samples come from two banks, one a VBL:
; userfw_vbl points A0 at the other bank every vsync, and the VBL's ack
; says which one is playing (+VBLSYNC_BANK), so the RP refills the one
; that is not. With a single buffer the refill, which follows the ack,
; overwrote the samples still to play in that VBL with the next VBL's.
; The bank is bit 9 of the address, so the ack's bit is the address's
; mid byte masked (CART_AUDIO_YM_BANK_BYTES in cart_shared.h).
YM_BANK_BYTES         equ $200
    ifne  AUDIO_BUFFER_ADDR&YM_BANK_BYTES
    fail  "AUDIO_BUFFER_ADDR must have bit 9 clear: it names the YM bank"
    endc
; Without DMA sound the YM is the sound effects' DAC, unless this word
; (shared-variable slot 1, CART_YM_MUSIC_OFFSET) is non-zero: then the
; RP has given it to the music (keypad ), saved with the video options).
YM_MUSIC_ADDR         equ $00FA4014

; Number of 320-px lines the cart->ST blit covers per frame. Full ST
; low-res is 200; copying fewer leaves the bottom band of the
; destination ST page untouched (useful for a status row or to bound
; the blitter cost).
FB_COPY_LINES         equ 200         ; M68k copies all 200 lines (32000 bytes = 666 chunks * 48 B + 32-byte tail). Full screen blitted.
FB_ROW_BYTES          equ 160                 ; 320 px * 4 bpp / 8

; --- IKBD ownership ------------------------------------------------

; Keyboard ACIA at $FFFFFC00/02. Status bit 0 = RX-data-ready; bit 1 =
; TX-empty. The MIDI ACIA at $FFFFFC04/06 shares the one MFP ACIA
; interrupt (IERB bit 6), so the interrupt-driven handler must drain it
; too or the level-asserted IRQ line can re-trigger.
ACIA_KBD_STATUS       equ $FFFFFC00
ACIA_KBD_DATA         equ $FFFFFC02
ACIA_MIDI_STATUS      equ $FFFFFC04
ACIA_MIDI_DATA        equ $FFFFFC06
MFP_ACIA_BIT          equ 6                  ; IERB/IMRB bit for keyboard/MIDI ACIA

; MC68901 MFP registers (subset we manipulate).
MFP_IERA              equ $FFFFFA07          ; interrupt enable A (Timer-A = bit 5)
MFP_IERB              equ $FFFFFA09          ; interrupt enable B
MFP_ISRA              equ $FFFFFA0F          ; in-service A (Timer-A ack = bit 5)
MFP_IMRA              equ $FFFFFA13          ; interrupt mask A
MFP_IMRB              equ $FFFFFA15          ; interrupt mask B
MFP_VR                equ $FFFFFA17          ; vector register (high nibble = vector base, bit 3 = S: 1=software EOI, 0=auto-EOI)
MFP_TACR              equ $FFFFFA19          ; Timer-A control register (cleared at boot for safety)
MFP_TADR              equ $FFFFFA1F          ; Timer-A data register
MFP_TIMERA_BIT        equ 5                  ; IERA/IMRA bit for Timer-A
MFP_TIMERC_BIT        equ 5                  ; IERB/IMRB bit for Timer-C
MFP_TBCR              equ $FFFFFA1B          ; Timer-B control register (delay-mode + prescaler)
MFP_TBDR              equ $FFFFFA21          ; Timer-B data register (8-bit countdown)

; MFP timers, highest priority first, the same on every machine:
;   Timer-A  audio: the DMA frame end (event count; the STE wires the
;            DMA sound's frame-end signal to Timer-A's input), or the
;            YM sample clock without DMA sound (delay mode)
;   Timer-B  the status bar's palette split (event count; Timer-B's
;            input is display enable, so only it can count lines)
;   ACIA     keyboard, joystick and mouse bytes
; Priority only decides which of two pending interrupts goes first --
; they all run at IPL 6 and never nest. Audio goes first because a late
; sample is heard and every audio handler is a few dozen cycles; the
; split can start late and still land in the blank (userfw_hud_irq);
; an IKBD byte has 1.28 ms before the next arrives.
;
; YM sample rate. MFP master clock = 2.4576 MHz. We pick a /4
; prescaler with count 110:
;   f = 2.4576 MHz / (4 * 110) = 5,585.45 Hz
; (~10.9% slower than STE-low's 6,258 Hz). The count was raised from
; 98 -> 110 to free ~1500 cyc/VBL for the FB_COPY_LINES=200 macro;
; sample.h is still generated at the older 6,269 Hz rate, so the
; jingle plays back ~11% lower pitch (about 2 semitones down) -- a
; modest but audible detune. Regenerate sample.h at 5585 Hz via
; wav_to_ym4.py if exact pitch matters. PAL VBL = 49.92 Hz so
; ~111.71 samples/VBL. At 2 bytes per sample (dual-ghost LUT) that's
; ~223 bytes/VBL in the cart buffer (audio.c's AUDIO_BYTES_PER_VBL
; = 224 matches this).
YM_TIMER_PRESCALER    equ 1                  ; /4 (delay mode)
YM_TIMER_COUNT        equ 110                ; ~5,585 Hz (~112 samples/PAL VBL)

; IRQ vector slots we take over. $70 (VBL) already handled by the
; original userfw code path (D3 holds the save).
VEC_HBL               equ $68
VEC_TIMERD            equ $110
VEC_TIMERC            equ $114
VEC_ACIA              equ $118
VEC_TIMERB            equ $120
VEC_TIMERA            equ $134

; IKBD cart-bus emit window (ROM3). userfw_acia_irq reads
; (IKBD_WINDOW_BASE + byte).b to forward `byte` to the RP; the RP side filters commemul ring samples whose low 16 bits
; fall in [$8200, $8300) and extracts the IKBD byte from the low 8
; bits.
IKBD_WINDOW_BASE      equ $FB8200

; VBL frame-sync ack. After each blit completes (.after_copy)
; the m68k does a single dummy cart-bus read at VBLSYNC_ADDR to tell
; the RP "the blit is done, the cart framebuffer is free to overwrite".
; The m68k cannot WRITE the shared region (it's ROM from the m68k
; side), so the ack must be a READ captured by the RP's commemul ring
; -- the same mechanism IKBD uses. Distinct high byte ($84) from the
; IKBD window ($82) so the RP can tell the two apart. The value read
; is irrelevant; only the address matters.
VBLSYNC_ADDR          equ $FB8400
; Two-VBL frames only (UFW_TWO_VBL): the ack for the first half of a
; frame. The cart FB is not free yet -- half of it has still to be
; copied -- so the RP must not take it as a frame ack, but it is still
; a VBL and the audio refill keys off it. The RP tells the two apart by
; the low byte.
VBLSYNC_HALF_ADDR     equ (VBLSYNC_ADDR + 1)
; Added to either ack when YM bank 1 is the one playing this VBL.
VBLSYNC_BANK          equ (YM_BANK_BYTES >> 8)

; Display report: the m68k reads (VIDEO_WINDOW_BASE + UFW_MONO) first
; thing every VBL, before it touches the cart FB or the palette slots,
; so the RP knows which picture to draw and how many YM samples a 71 Hz
; VBL takes, and when the frame ack's window has closed. Distinct from
; every other window here.
VIDEO_WINDOW_BASE     equ $FB9000

; --- 256 colours: a TT's TT Low or a Falcon's 8-plane mode ----------
; Doom's own 256 colours from 8 interleaved bitplanes, on a TT or a
; Falcon with a colour monitor (and DMA sound, and the ST RAM for two
; pages above $80000). The picture is 64,000 bytes, twice the cart FB,
; so the RP sends it in two parts of 100 rows (cart_shared.h, "256
; colours"): it writes a part and bumps PART_SEQ_ADDR; the m68k copies
; it, up to UFW8_BUDGET rows a VBL, and acks it at PARTACK_WINDOW_BASE +
; the sequence's low byte, and only then does the RP write the next. The
; page goes on screen when part 1 is in, double-buffered as ever, with
; the palette the RP sent with part 0. A TT shows each row on two lines
; of TT Low's 480 (lines 40..439); a Falcon's VIDEL doubles them itself
; on VGA (320x240, lines 20..219) and an RGB monitor or a TV gets
; 320x200 as it is. The display report says which: VIDEO_WINDOW_BASE +
; EIGHT_TT or EIGHT_FALCON (bit 1, eight planes; bit 2, the Falcon's
; palette format), and ROWS_WINDOW_BASE + the budget, every VBL.
EIGHT_TT              equ 2
EIGHT_FALCON          equ 6
PART_SEQ_ADDR         equ $00FA401C          ; word: bumped by the RP for each part
PART_IDX_ADDR         equ $00FA401E          ; word: 0 = rows 0..99, 1 = rows 100..199
PAL8_SEQ_ADDR         equ $00FA4020          ; word: bumped when PAL8_ADDR has changed
PAL8_ADDR             equ $00FA4300          ; 256 words: YM bank 1's space (see cart_shared.h)
PARTACK_WINDOW_BASE   equ $FB9200
ROWS_WINDOW_BASE      equ $FB9400
CART_EIGHT_ROWS       equ 100                ; rows a part
CART_EIGHT_ROW_BYTES  equ 320                ; 20 groups of 16 pixels, 8 plane words each
CART_PAL8_ENTRIES     equ 256
COOKIE_VDO            equ $5F56444F          ; '_VDO'; high word 2 = TT, 3 = Falcon
VDO_TT                equ 2
VDO_FALCON            equ 3
MFP_GPIP              equ $FFFFFA01
MFP_GPIP_MONO         equ 7                  ; low on a TT with a mono monitor
VIDEO_BASE_ADDR_LOW   equ $FFFF820D
TT_SHIFT              equ $FFFF8262          ; TT shift mode
TT_LOW                equ $0700              ; TT Low, palette bank 0
TT_PALETTE            equ $FFFF8400          ; 256 words, 0000RRRR GGGGBBBB
FALCON_PALETTE        equ $FFFF9800          ; 256 longs, RRRRRR00 GGGGGG00 00000000 BBBBBB00
VM_BPS8               equ $0003              ; VsetMode: 8 planes, 40 columns
VM_VGA                equ $0010
VM_PAL                equ $0020
VM_VERTFLAG           equ $0100              ; on VGA, every line twice
MON_VGA               equ 2                  ; VgetMonitor
UFW8_PAGE_A           equ $00080000
UFW8_SIZE_TT          equ 153600             ; 320x480
UFW8_SIZE_FALCON      equ 76800              ; 320x240; RGB uses the first 64,000
UFW8_FIRST_TT         equ 40*CART_EIGHT_ROW_BYTES
UFW8_FIRST_VGA        equ 20*CART_EIGHT_ROW_BYTES
; State, in the part of screen page A the 8-plane mode does not show
; (after snd_measure's 4 KB of silence), so touched only in that mode.
UFW8_STATE            equ $00071000
UFW8_SEQ              equ UFW8_STATE+0       ; word: the last part copied
UFW8_PART             equ UFW8_STATE+2       ; word: the part under way, 0 or 1
UFW8_ROW              equ UFW8_STATE+4       ; word: its rows copied so far, 0 = none under way
UFW8_LEFT             equ UFW8_STATE+6       ; word: rows this VBL may still copy
UFW8_BUDGET           equ UFW8_STATE+8       ; word: rows a VBL, measured at boot
UFW8_STEP             equ UFW8_STATE+10      ; word: bytes from one picture row to the next
UFW8_PALSEQ           equ UFW8_STATE+12      ; word: the palette last latched
UFW8_OLD_MODE         equ UFW8_STATE+14      ; word: TOS's TT shift mode, or Falcon VsetMode word
UFW8_FLIPPED          equ UFW8_STATE+16      ; byte: a page went up: its palette at the next VBL
UFW8_TWICE            equ UFW8_STATE+17      ; byte: $FF = every row on two lines (a TT)
UFW8_FIRST            equ UFW8_STATE+20      ; longword: offset of the picture's first line
UFW8_XOR              equ UFW8_STATE+24      ; longword: page A ^ page B
UFW8_PAL_LATCH        equ $00071100          ; 1 KB: the palette for the page being copied
UFW8_TOS_PAL          equ $00071500          ; 1 KB: TOS's 256 colours, for the way out

; --- STE/Mega STE DMA sound (auto-detected at runtime) -----------
; STE-class machines play 8-bit SIGNED PCM from ST RAM by DMA with no
; CPU per sample -- a big quality win over the ~5.6 kHz YM beeper
; approximation. The buffers live in the UNUSED TAILS of the two
; screen pages: past the 32000-byte framebuffer, below the userfw
; scratch slots ($77FE0+), never displayed nor touched by the blit --
; no separately-allocated ST RAM. Presence is detected at boot via
; the _SND cookie (bit 1), stored in UFW_HAS_DMA; both the DMA and
; the YM paths compile in and are chosen at runtime, and the
; result is reported to the RP each VBL so it produces the matching
; audio format (signed PCM for DMA, (vA,vB) YM pairs otherwise).
; Byte registers live at ODD addresses in the $FFFF89xx block.
; (Design shared with md-mjpeg's userfw.s.)
STE_DMA_CTRL          equ $FFFF8901          ; bit0 = play, bit1 = loop
; A Falcon routes its sound frames to the MFP only if asked: bit 2 of
; $FF8900 puts the play frames on Timer-A's input, and TOS 4.04 leaves it
; clear, so without it userfw_snd_irq never ran and the chip looped on
; buffer A for good. (The edge comes at the start of a frame there, the
; end on an STE or a TT -- in loop mode the same moment.)
SND_INT_CTRL          equ $FFFF8900
SND_INT_PLAY_TIMERA   equ 2
STE_DMA_START_HI      equ $FFFF8903
STE_DMA_START_MID     equ $FFFF8905
STE_DMA_START_LO      equ $FFFF8907
STE_DMA_END_HI        equ $FFFF890F
STE_DMA_END_MID       equ $FFFF8911
STE_DMA_END_LO        equ $FFFF8913
STE_DMA_MODE          equ $FFFF8921          ; bit7 = mono, bits1-0 = rate
STE_DMA_MODE_VAL      equ $82                ; mono + 25033 Hz (rate = 2)
STE_DMA_CNT_MID       equ $FFFF890B          ; frame COUNTER mid byte (read-only)
STE_DMA_CNT_LO        equ $FFFF890D          ; frame COUNTER low byte (read-only)
; Double buffer in the two screen-page tails. Loop-mode DMA plays one
; while .after_copy refills the OTHER, so the m68k write never crosses
; the DMA read pointer. Which buffer the DMA is in is read from bit 7
; of the frame-counter mid byte: A ($77Dxx -> bit7=0) vs B ($7FDxx ->
; bit7=1).
;
; START/END are written by userfw_snd_irq, not by the VBL loop. The
; chip latches them only when it reaches END, and the frame end pulses
; MFP Timer-A's event input, so the handler runs the moment the chip
; has switched buffers and points START/END at the one it just left --
; a full frame before the next latch. Written from the VBL loop after
; the copy, the six byte writes sat at an arbitrary phase of the DMA
; frame; whenever the chip's slow drift brought its frame end into that
; ~6 us window, it latched a START from one buffer and an END from the
; other and played the 32 KB of screen memory between them: ~1.3 s of
; noise, every few minutes.
STE_SND_BUF_A         equ $00077D00          ; page A tail
STE_SND_BUF_B         equ $0007FD00          ; page B tail
STE_SND_BUF_MASK      equ $7D00              ; low 16 bits of either base, bit 15 cleared

; The DMA plays 25,033 samples a second off its own oscillator; the PAL
; VBL is a different oscillator entirely. So the samples the chip eats
; per frame is not 500, not an integer, and not the same on every
; machine -- it is around 501.5. Handing it a fixed 500 every VBL means
; it runs the buffer dry roughly every 350 frames and replays one, which
; is the ~7-second crackle.
;
; So the length is not fixed. UFW_SND_LEN is steered each VBL by the
; loop in .after_copy until the chip consumes exactly one buffer per
; frame, whatever the true ratio is on this machine. Nothing here has to
; know that ratio, and it cannot drift, because it is measured.
;
; This works only because the RP resamples one VBL of audio onto
; however many samples it is asked for: changing the length changes the
; resampling ratio by a fraction of a percent -- inaudible -- instead of
; dropping or repeating a block of samples. The RP is told the current
; length through SNDLEN_WINDOW_BASE below.
;
; The copy is a fixed STE_SND_COPY bytes so the longword loop stays
; simple; anything past UFW_SND_LEN sits in the buffer unplayed, because
; END is set from the length rather than the copy size.
; END is a byte address, so the length itself need not be a multiple of
; anything -- only the copy does, and that is fixed.
;
; The starting length is measured at boot (snd_measure): the samples
; the chip plays over SND_MEASURE_VBLS VBLs, a quarter of them. A guess
; would have to know the VBL rate, and it is not one rate: 50 Hz on an
; STE in colour, 71 in mono, 60 or 70 on a TT's or a Falcon's VGA
; monitor. A guess far enough out never converges, because the steer
; below takes a drift of more than 64 for a handover and ignores it:
; a TT started from 500 at 60 Hz (~417 a VBL) stayed at 500. The
; minimum covers 71 Hz (~350) and keeps the report inside one byte
; (512 - 320 = 192).
STE_SND_COPY          equ 512                ; bytes copied per VBL (mult of 4)
    ifgt  STE_SND_COPY-(YM_PKT_ADDR-AUDIO_BUFFER_ADDR)
    fail  "the DMA sample copy would reach the YM music packet"
    endc
STE_SND_LEN_MIN       equ 320
STE_SND_LEN_MAX       equ 512                ; must not exceed STE_SND_COPY
; The measurement plays this much silence once, from the start of screen
; page A (which the first frame overwrites): one VBL to line up and four
; timed ones at 50 Hz, the slowest rate, are ~2,500 bytes.
SND_MEASURE_BYTES     equ 4096
SND_MEASURE_VBLS      equ 4                  ; a power of two: the result is a shift
SND_MEASURE_SHIFT     equ 2

; Cookie jar (_p_cookies) + _SND cookie for DMA-sound detection.
COOKIE_JAR_PTR        equ $000005A0          ; longword: cookie jar base (0 = none)
COOKIE_SND            equ $5F534E44          ; '_SND'; value bit 1 = DMA/PCM sound
COOKIE_CPU            equ $5F435055          ; '_CPU'; 0, 10, 20, 30, 40 or 60
COOKIE_MCH            equ $5F4D4348          ; '_MCH'; high word 2 = TT
MCH_TT                equ 2

; Sound-capability report window: the m68k reads (SNDCAP_WINDOW_BASE +
; has_dma + UFW_TT) each VBL; the RP commemul ring decodes the low bit
; and produces the matching audio format, and bit 1 tells it this is a
; TT, whose internal speaker keypad ( switches. Distinct from IKBD
; ($FB8200) and VBLSYNC ($FB8400).
SNDCAP_WINDOW_BASE    equ $FB8600
SNDCAP_TT             equ 2                  ; bit 1: a TT

; The TT's internal speaker (cart_shared.h CART_SPEAKER_OFFSET, slot 5;
; must match): a word the RP sets with keypad (, 0 = off (as the cart
; region starts), else on. Applied by ufw_speaker_apply each VBL, on a
; TT only.
SPEAKER_ADDR          equ $00FA4024

; Relocation heartbeat: the m68k reads (RELOC_WINDOW_BASE + protocol
; version) once per VBL, from the display loop and therefore only ever
; from code running at UFW_RAM_DEST. It tells the RP three things with
; one read: that this firmware relocated itself and the cartridge's code
; area is free to reclaim, that the ST is still alive (the RP watches
; for it to stop), and which protocol to expect. Distinct from IKBD
; ($FB8200), VBLSYNC ($FB8400), SNDCAP ($FB8600) and Xpad ($FB8800/A00).
RELOC_WINDOW_BASE     equ $FB8E00
RELOC_PROTOCOL        equ 1

; Buffer-length report: the m68k reads (SNDLEN_WINDOW_BASE + len -
; STE_SND_LEN_MIN) once per VBL while STE DMA sound is running, so the
; RP produces exactly the number of samples the chip is about to eat.
; The bias keeps the whole range inside one byte.
SNDLEN_WINDOW_BASE    equ $FB8C00

; --- Xpad: extended controller input ------------------------------
; Xpad (https://github.com/neilrackett/atarist-xpad) is an open standard
; for describing a modern gamepad to ST software: a provider publishes a
; shared block and installs the 'XPAD' cookie pointing at it; consumers
; poll it. It is how MD/DOOM reaches strafe, use, run and weapon
; selection from a pad, which a one-button ST joystick cannot express.
;
; This is the CONSUMER half. We locate and validate the block once at
; boot, then read pad 0 every VBL and forward its button mask to the RP.
; Nothing here is required: with no provider the RP falls back to the
; IKBD joystick, which is the standard's own fallback ladder.
;
; Header layout (v1, big-endian, see xpad.h):
;    0  magic       long  'XPAD'
;    4  version     word  major<<8 | minor
;    6  hdr_size    word
;    8  pads_offset word
;   10  pad_size    word
;   12  caps        word
;   14  seq         word   bumped on every commit, BEFORE active flips
;   16  pad_count   byte   1..4
;   17  active      byte   front buffer index, 0 or 1
; XPAD_PAD (12 bytes): 0 buttons(long), 4..9 axes/triggers,
;   10 type (0 = disconnected), 11 flags.
; ABI equates come from the xpad submodule (lib/xpad), generated from
; xpad.h by its own build so they cannot drift from the header. The file
; is staged into src/inc/ by target/atarist/build.sh -- see the note
; there about stcmd mounting only this folder.
    include "inc/xpad.inc"

; Xpad report windows: the low and high bytes of pad 0's button mask,
; emitted every VBL as two cart-bus reads. HIGH is emitted first and the
; RP latches it, committing the pair when the LOW byte lands, so a ring
; drain that falls between the two reads can't tear the value.
XPAD_HI_WINDOW_BASE   equ $FB8800
XPAD_LO_WINDOW_BASE   equ $FB8A00

; XPAD_ENABLE=1 compiles the Xpad consumer in: the boot-time cookie
; walk, the per-VBL report to the RP, and the etv_timer tick that keeps
; an ETV-hooked provider running while userfw owns the machine.
;
; On by default now that there is a way for a provider to exist at all.
; It never could before: MD/DOOM autostarts at CA_INIT bit 27, before
; the AUTO folder, so a provider installed as a TSR was not resident
; when the cookie walk ran and UFW_XPAD_HDR latched 0 forever. Launching
; MDDOOM.TOS from the desktop runs the walk after AUTO instead.
;
; With no provider found, none of it does anything and the RP falls back
; to the IKBD joystick, which is the standard's own fallback ladder.
    ifnd    XPAD_ENABLE
XPAD_ENABLE           equ 0

; How an ETV-hooked Xpad provider gets its tick. 1 = hand TOS's 200 Hz
; Timer-C back when a provider was found, so TOS's own ISR calls
; etv_timer ($400) as it always did. 0 = no tick at all: the pad will
; not update, but everything else runs -- build it that way to prove
; whether the tick is what is bombing.
XPAD_TICK             equ 1
    endc

; FORCE_NO_DMA=1 (build flag) forces detection to report "no DMA" so
; the YM fallback can be exercised on an STE without a plain ST.
    ifnd    FORCE_NO_DMA
FORCE_NO_DMA          equ 0
    endc

; Save area for vectors + MFP regs we'll restore on ESC exit. A fixed
; address in the free part of screen page A's tail, beside the other
; UFW_* state. It used to be physbase - 32, which was fine while the
; only way in was the autostart: on a 1 MB machine that address is the
; top of the GEMDOS pool, and once MD/DOOM can also be launched from
; the desktop it may be inside somebody's allocation -- including our
; own. A5 holds the pointer throughout the userfw run; the exit path
; reloads it in case anything clobbered A5.
;   offset  0: $68  HBL vector save (long)
;   offset  4: $110 Timer-D vector save (long)
;   offset  8: $114 Timer-C vector save (long)
;   offset 12: $118 ACIA vector save (long)
;   offset 16: $120 Timer-B vector save (long)
;   offset 20: $134 Timer-A vector save (long)
;   offset 24: MFP IERA save (byte)
;   offset 25: MFP IERB save (byte)
;   offset 26: MFP IMRA save (byte)
;   offset 27: MFP IMRB save (byte)
;   offset 28: MFP VR save (byte) -- S-bit + vector base, switched to auto-EOI under userfw
;   offset 29-31: reserved / padding (longword align)
UFW_SAVE_SIZE         equ 32
UFW_SAVE              equ $00077FA0          ; 32 B, inside the free $77F86..$77FCF gap
    ifgt  UFW_SAVE+UFW_SAVE_SIZE-$00077FD0
    fail  "userfw save area runs past the free gap at $77FD0"
    endc

    section text

; --- Relocatable image header -------------------------------------
;
; main.s copies this module to UFW_RAM_DEST and jumps to offset 0, so
; offset 0 has to be a branch to the entry point rather than the entry
; point itself. The magic and length let the copier check it is looking
; at what it thinks it is, at both ends of the copy, and let the length
; come from the image rather than from a constant the two files would
; have to keep in step.
;
; Relocating costs nothing in this module: it is linked at $000800, not
; $FA0800, so every self-reference is already PC-relative and there is
; no writable data.
userfw_image_start:
    bra.w   userfw                   ; offset 0: entry
    dc.b    'MDOM'                   ; offset 4: magic
    dc.l    userfw_image_end - userfw_image_start   ; offset 8: image length

userfw:
    ; --- Boot setup (runs once) ---

    ; Our own stack before anything is pushed (UFW_STACK_TOP); the old
    ; one is put back for the return to GEM.
    move.l  sp, UFW_SP_SAVE
    lea     UFW_STACK_TOP, sp

    ; Save the original screen base so we can restore it on ESC exit.
    move.w  #2, -(sp)                ; XBIOS Physbase
    trap    #14
    addq.l  #2, sp
    move.l  d0, UFW_PHYSBASE_SAVE    ; saved screen base lives in RAM now

    ; A 68030's data cache off before the first cartridge read: one the
    ; cache answered would never reach the RP (caches_off).
    move.l  #COOKIE_CPU, d1
    bsr     find_cookie
    move.w  d0, UFW_CPU
    bsr     caches_off

    ; --- Low res or high res? ---------------------------------------
    ; main.s has switched medium to low, so the shifter is now in one of
    ; the two modes with a picture of their own. In mono, colour 0 is
    ; set once to white paper here, and the palette copy in the loop is
    ; skipped: the RP draws ink bits, and nothing else in the palette
    ; means anything to a mono monitor.
    clr.b   UFW_MONO
    clr.b   UFW_HALF
    move.l  #AUDIO_BUFFER_ADDR, UFW_YM_BANK
    clr.b   UFW_YM_MUSIC                 ; the YM starts as the sound effects' DAC
    ; Music: only packets published from now on -- one left from before
    ; an ST reset could hold a note.
    move.w  YM_PKT_ADDR, UFW_YM_SEQ
    move.b  SHIFTER_RES.w, d0
    andi.b  #3, d0
    cmpi.b  #SHIFTER_RES_HIGH, d0
    bne.s   .res_colour
    move.b  #1, UFW_MONO
    move.w  #MONO_PAPER_WHITE, PALETTE_BASE.w
.res_colour:

    ; --- Detect STE DMA sound via the _SND cookie (bit 1) ---------
    ; A null jar or a missing _SND (plain ST / early TOS) leaves
    ; has_dma = 0 -> YM fallback. FORCE_NO_DMA=1 skips the lookup
    ; entirely so the fallback can be tested on an STE.
    moveq   #0, d0                   ; assume no DMA sound
    clr.b   UFW_FALCON_SND
    ifeq    FORCE_NO_DMA
    move.l  #COOKIE_SND, d1
    bsr     find_cookie
    btst    #2, d0                   ; bit 2 = a Falcon's codec (SND_INT_CTRL)
    sne     UFW_FALCON_SND
    lsr.l   #1, d0                   ; bit 1 = DMA / PCM sound
    andi.l  #1, d0
    endc
    move.b  d0, UFW_HAS_DMA

    ; A frame is two VBLs in mono, where the copy is longer than a VBL,
    ; and without DMA sound, where the copy (~17.6 ms) and the YM sample
    ; interrupt (~2 ms) all but fill one on their own, so that anything
    ; more ran past the next VBL. See UFW_TWO_VBL in the VBL loop.
    st      UFW_TWO_VBL
    tst.b   UFW_MONO
    bne.s   .two_vbl_done
    tst.b   d0
    beq.s   .two_vbl_done
    sf      UFW_TWO_VBL
.two_vbl_done:

    ; --- A TT? (_MCH high word 2) Its internal speaker follows keypad (
    ; (ufw_speaker_apply), and the RP learns that it is one from the
    ; sound report, which is why the flag is that report's bit.
    clr.b   UFW_TT
    move.l  #COOKIE_MCH, d1
    bsr     find_cookie
    swap    d0
    cmpi.w  #MCH_TT, d0
    bne.s   .tt_done
    move.b  #SNDCAP_TT, UFW_TT
.tt_done:
    move.b  #1, UFW_SPK_STATE        ; neither off nor on: the first VBL applies the setting

    ; --- 256 colours on a TT or a Falcon? ---------------------------
    ; See "256 colours" in the equates. Needs a colour monitor, DMA
    ; sound (the palette travels in YM bank 1's space) and two pages of
    ; ST RAM from $80000 that nothing owns: main.s passes the top of what
    ; is free in D7. Otherwise this machine gets the ST Low or mono
    ; picture like any other. Done while TOS still runs, which a
    ; Falcon's VsetMode needs; its palette is saved first, for the way
    ; out (eight_restore), and both machines' then go black until the
    ; first frame brings its own. Clobbers D0-D4/A1/A2; XBIOS keeps D7.
    clr.b   UFW_EIGHT
    tst.b   UFW_HAS_DMA
    beq     .eight_done
    move.l  #COOKIE_VDO, d1
    bsr     find_cookie
    swap    d0                       ; the high word: 2 TT, 3 Falcon
    cmpi.w  #VDO_FALCON, d0
    beq.s   .eight_falcon
    cmpi.w  #VDO_TT, d0
    bne     .eight_done
    btst    #MFP_GPIP_MONO, MFP_GPIP.w  ; the monitor, not the mode: a
    beq     .eight_done              ; TT's colour one shows ST High too
    cmpi.l  #UFW8_PAGE_A+2*UFW8_SIZE_TT, d7
    blo     .eight_done
    move.b  #EIGHT_TT, UFW_EIGHT
    lea     UFW8_TOS_PAL, a2
    bsr     pal8_get
    move.w  TT_SHIFT.w, UFW8_OLD_MODE
    move.l  #UFW8_SIZE_TT, d3
    move.l  #UFW8_FIRST_TT, UFW8_FIRST
    move.w  #2*CART_EIGHT_ROW_BYTES, UFW8_STEP
    st      UFW8_TWICE
    bra     .eight_pages
.eight_falcon:
    cmpi.l  #UFW8_PAGE_A+2*UFW8_SIZE_FALCON, d7
    blo     .eight_done
    move.w  #89, -(sp)               ; XBIOS VgetMonitor
    trap    #14
    addq.l  #2, sp
    tst.w   d0
    beq     .eight_done              ; 0: an SM124
    move.w  d0, d3
    move.b  #EIGHT_FALCON, UFW_EIGHT
    lea     UFW8_TOS_PAL, a2
    bsr     pal8_get
    move.w  #-1, -(sp)
    move.w  #88, -(sp)               ; XBIOS VsetMode(-1): the mode now
    trap    #14
    addq.l  #4, sp
    move.w  d0, UFW8_OLD_MODE
    andi.w  #VM_PAL, d0              ; RGB or a TV: 320x200, at the desktop's 50 or 60 Hz
    clr.l   UFW8_FIRST
    cmpi.w  #MON_VGA, d3
    bne.s   .eight_falcon_mode
    move.w  #VM_VGA|VM_VERTFLAG, d0  ; VGA: 320x240, every line twice
    move.l  #UFW8_FIRST_VGA, UFW8_FIRST
.eight_falcon_mode:
    ori.w   #VM_BPS8, d0
    move.w  d0, -(sp)
    move.w  #88, -(sp)               ; XBIOS VsetMode
    trap    #14
    addq.l  #4, sp
    ; TOS 4.04 writes the VIDEL in a VBL after the next (md-xt found):
    ; wait for two before anything else is changed.
    move.w  #37, -(sp)               ; XBIOS Vsync
    trap    #14
    addq.l  #2, sp
    move.w  #37, -(sp)
    trap    #14
    addq.l  #2, sp
    move.l  #UFW8_SIZE_FALCON, d3
    move.w  #CART_EIGHT_ROW_BYTES, UFW8_STEP
    sf      UFW8_TWICE
.eight_pages:
    ; Both pages black, borders and all: only the picture's lines are
    ; written after this. D3 is a page; 16 bytes an iteration.
    lea     UFW8_PAGE_A, a1
    move.l  d3, d0
    lsr.l   #3, d0
    subq.w  #1, d0
.eight_clear:
    clr.l   (a1)+
    clr.l   (a1)+
    clr.l   (a1)+
    clr.l   (a1)+
    dbf     d0, .eight_clear
    ; Page A on screen, black, so page B is drawn first.
    lea     UFW8_PAGE_A, a1          ; zeroes: a black palette
    bsr     pal8_put
    move.b  #((UFW8_PAGE_A>>16)&$FF), VIDEO_BASE_ADDR_HIGH.w
    move.b  #((UFW8_PAGE_A>>8)&$FF), VIDEO_BASE_ADDR_MID.w
    clr.b   VIDEO_BASE_ADDR_LOW.w
    cmpi.b  #EIGHT_TT, UFW_EIGHT
    bne.s   .eight_mode_set
    move.w  #TT_LOW, TT_SHIFT.w
.eight_mode_set:
    move.l  #UFW8_PAGE_A, d0
    add.l   d3, d0
    move.l  d0, UFW_SCREEN_PAGE      ; page B
    eori.l  #UFW8_PAGE_A, d0
    move.l  d0, UFW8_XOR
    clr.b   UFW_MONO                 ; a TT's colour monitor may have been in ST High
    sf      UFW_TWO_VBL
.eight_done:

    ; --- Locate an Xpad provider (optional) -----------------------
    ; Walk the jar for 'XPAD', then validate the block the way
    ; xpad_valid() does: right magic, a major version we understand,
    ; and layout fields describing a pad area that fits inside the
    ; block. Everything the per-VBL read trusts is checked here once,
    ; so that path is a straight load. On any failure UFW_XPAD_HDR
    ; stays 0 and the RP never hears from us.
    ;
    ; The two pad-0 addresses are precomputed now (the header's layout
    ; fields never change) so the VBL path needs no multiply:
    ;   buffer b, pad i  =  base + pads_offset
    ;                       + (b * pad_count + i) * pad_size
    ; Clobbers D0/D1/D2/A0/A1 -- all reloaded by the boot code below.
    ifne    XPAD_ENABLE
    clr.l   UFW_XPAD_HDR
    move.l  COOKIE_JAR_PTR.w, d1
    beq     .xpad_done
    movea.l d1, a0
.xpad_cookie_loop:
    move.l  (a0), d1                 ; cookie tag
    beq     .xpad_done               ; tag 0 = end of jar, no Xpad
    cmp.l   #XPAD_COOKIE, d1         ; the jar tag, not XPAD_MAGIC
    beq.s   .xpad_found
    addq.l  #8, a0                   ; next 8-byte entry (tag, value)
    bra.s   .xpad_cookie_loop
.xpad_found:
    move.l  4(a0), d1                ; cookie value = block address
    beq     .xpad_done
    btst    #0, d1                   ; odd pointer would bus-error below
    bne     .xpad_done
    movea.l d1, a1
    cmp.l   #XPAD_MAGIC, (a1)        ; XPAD.magic -- the block sentinel,
                                     ; deliberately a different constant
                                     ; from the jar tag matched above
    bne     .xpad_done
    cmpi.b  #XPAD_VER_MAJOR, XPAD_VERSION_OFF(a1)   ; high byte = major
    bne     .xpad_done

    moveq   #0, d0
    move.b  XPAD_PADCOUNT_OFF(a1), d0
    beq     .xpad_done               ; no slots at all
    move.w  XPAD_PADSIZE_OFF(a1), d1
    cmp.w   #XPAD_PAD_SIZE_V1, d1
    blo     .xpad_done               ; smaller than v1 = broken provider

    ; pads_offset + 2 * pad_count * pad_size must fit in hdr_size.
    ; pad_count <= 255 and pad_size is a word, so the mulu result is
    ; taken as a long and compared unsigned -- an absurd pad_size then
    ; fails the test rather than wrapping into it.
    move.w  d1, d2
    mulu    d0, d2                   ; d2 = pad_count * pad_size (long)
    add.l   d2, d2                   ; both buffers
    moveq   #0, d1
    move.w  XPAD_PADSOFF_OFF(a1), d1
    add.l   d1, d2
    moveq   #0, d1
    move.w  XPAD_HDRSIZE_OFF(a1), d1
    cmp.l   d1, d2
    bhi     .xpad_done               ; pad area runs past the block

    ; Precompute pad 0 in each buffer and publish the block pointer
    ; last, so the VBL path never sees half-built state.
    moveq   #0, d1
    move.w  XPAD_PADSOFF_OFF(a1), d1
    move.l  a1, d2
    add.l   d1, d2
    move.l  d2, UFW_XPAD_BUF0
    moveq   #0, d1
    move.w  XPAD_PADSIZE_OFF(a1), d1
    mulu    d0, d1                   ; pad_count * pad_size = buffer stride
    add.l   d1, d2
    move.l  d2, UFW_XPAD_BUF1
    move.l  a1, UFW_XPAD_HDR
.xpad_done:
    endc

    ; Save TOS's VBL vector and install ours. We're in supervisor mode
    ; (entered via CA_INIT) so writing $70.w is legal.
    move.l  VBL_VECTOR.w, UFW_VBL_VEC_SAVE   ; TOS VBL vector saved in RAM
    lea     userfw_vbl(pc), a0
    move.l  a0, VBL_VECTOR.w

    ; --- IKBD ownership setup --------------------------------------
    ;
    ; A5 = save area pointer. Used at boot to save the 6 IRQ vectors
    ; + MFP IER/IMR; the exit path reloads it the same way, so A5 does
    ; not need to survive the per-VBL FBDRV_INLINE expansion.
    lea     UFW_SAVE, a5

    ; The command sentinel at CMD_MAGIC_SENTINEL is RP-owned (m68k
    ; can't write to the cart shared region) and is zeroed by the
    ; RP's ERASE_FIRMWARE_IN_RAM at boot, so we don't need to clear
    ; it from here. It's already CMD_NOP=0 on first userfw entry.

    ; Mask all maskable IRQs while we rewrite vectors + MFP state.
    move.w  sr, -(sp)
    ori.w   #$0700, sr

    ; Save the 6 vectors we're about to overwrite ($70 already saved
    ; to UFW_VBL_VEC_SAVE above).
    move.l  VEC_HBL.w, 0(a5)
    move.l  VEC_TIMERD.w, 4(a5)
    move.l  VEC_TIMERC.w, 8(a5)
    move.l  VEC_ACIA.w, 12(a5)
    move.l  VEC_TIMERB.w, 16(a5)
    move.l  VEC_TIMERA.w, 20(a5)

    ; Save MFP IER / IMR for A and B (4 bytes), plus VR (1 byte).
    move.b  MFP_IERA.w, 24(a5)
    move.b  MFP_IERB.w, 25(a5)
    move.b  MFP_IMRA.w, 26(a5)
    move.b  MFP_IMRB.w, 27(a5)
    move.b  MFP_VR.w, 28(a5)             ; TOS uses S=1 (software EOI); we override below

    ; A TT: save port A as TOS left it, so the exit can put the internal
    ; speaker back the way it found it (ufw_speaker_apply switches it).
    tst.b   UFW_TT
    beq.s   .spk_saved
    move.b  #YM_REG_PORT_A, YM_SELECT.w
    move.b  YM_SELECT.w, UFW_SPK_PORTA   ; a read of the select register reads the latched one
.spk_saved:

    ; Install dummies at HBL / Timer-A / Timer-B / Timer-C / Timer-D
    ; / ACIA. userfw_dummy_irq is a single rte; PC-relative for the
    ; same runtime-vs-link-address reason userfw_vbl uses lea(pc).
    ; With IERA/IERB cleared below no MFP source actually fires, but
    ; the dummies cover the boot window between vector install and
    ; the IERA/IERB clears.
    lea     userfw_dummy_irq(pc), a0
    move.l  a0, VEC_HBL.w
    move.l  a0, VEC_TIMERD.w
    move.l  a0, VEC_TIMERC.w
    move.l  a0, VEC_ACIA.w
    move.l  a0, VEC_TIMERB.w
    move.l  a0, VEC_TIMERA.w

    ; Interrupt-driven IKBD -- install a real ACIA handler in
    ; place of the dummy rte so keyboard + joystick bytes are read the
    ; instant they arrive (the ACIA has a 1-byte buffer; polling loses
    ; the multi-byte joystick packets). Enabled at the MFP below.
    lea     userfw_acia_irq(pc), a0
    move.l  a0, VEC_ACIA.w

    ; Stop both timers (kills any prior TOS event).
    clr.b   MFP_TBCR.w
    clr.b   MFP_TACR.w

    ; Disable + mask everything in MFP A/B. Timer-A, Timer-B and the
    ; ACIA are re-enabled explicitly below; everything else stays off.
    clr.b   MFP_IERA.w
    clr.b   MFP_IERB.w
    clr.b   MFP_IMRA.w
    clr.b   MFP_IMRB.w

    ; Flip the MFP Vector Register to AUTO-EOI mode (clear the S bit,
    ; VR bit 3) for BOTH audio paths: the interrupt-driven ACIA/IKBD
    ; handler relies on auto-EOI (it never writes an in-service ack),
    ; so unlike md-mjpeg -- which polls the ACIA and only needs
    ; auto-EOI for its audio timer -- this must happen before the
    ; audio-mode branch, or the first key event on a DMA-sound machine
    ; would wedge the MFP. (For the YM sample clock it additionally
    ; saves ~12 cyc per fire.)
    move.b  28(a5), d0                    ; copy TOS's VR (saved above)
    andi.b  #$F7, d0                      ; clear bit 3 (S) -> auto-EOI
    move.b  d0, MFP_VR.w

    ; Audio back-end chosen at runtime from UFW_HAS_DMA (detected
    ; above). Both the YM and STE DMA paths are compiled in.
    tst.b   UFW_HAS_DMA
    bne     .boot_audio_dma          ; DMA sound present -> STE path

    ; --- YM2149 init: ch A + ch B as Ghostbusters dual-channel DAC
    ; Enable tones on BOTH ch A and ch B (mixer bits 0,1 = 0). Tone
    ; periods all 0 so the counters run at max -- effectively DC
    ; clamp above the audio band, so the volume registers alone
    ; shape each channel's output. The two channels sum
    ; acoustically; the sample handler writes a (vA, vB) pair per
    ; fire, and the Ghostbusters 64-entry hand-tuned LUT picks the
    ; pair that best approximates the desired linear amplitude on
    ; the YM's logarithmic volume curve.
    ;
    ; Reg 8 (ch A volume) is latched LAST so the first sample fire
    ; can write ch A immediately; the handler toggles to reg 9
    ; (ch B) mid-fire and back to reg 8 at the end. A subroutine, as
    ; the loop redoes it when the music hands the YM back (keypad )).
    bsr     ym_dac_init

    ; --- Timer-A setup: the YM sample clock (~5,585 Hz) ----------
    ; Install our handler at $134 (overrides the dummy installed
    ; above). Load count -> TADR, then prescaler -> TACR starts
    ; the countdown. Enable + unmask Timer-A at the MFP. SR is
    ; still IPL=7 at this point (set by `ori.w #$0700, sr` at the
    ; very top of userfw), so no IRQ fires until SR is dropped to
    ; $2300 below. (It was Timer-B until the status bar's palette
    ; split needed that one: see the timer list above YM_TIMER_COUNT.)
    ;
    ; (The MFP is already in auto-EOI mode -- set before the audio
    ; branch above -- so the handler needs no in-service ack.)
    lea     userfw_ym_irq(pc), a0
    move.l  a0, VEC_TIMERA.w
    move.b  #YM_TIMER_COUNT, MFP_TADR.w
    move.b  #YM_TIMER_PRESCALER, MFP_TACR.w

    ; Initialise A0 to the audio buffer base for the sample handler.
    ; A0 is NOT in the FBDRV_INLINE MOVEM list and no other code in
    ; userfw touches it after this point, so the handler can rely on
    ; A0 holding a valid cart-buffer pointer at all times -- saves
    ; the push/pop around it in the hot IRQ path (-24 cyc/fire).
    movea.l #AUDIO_BUFFER_ADDR, a0

    bset    #MFP_TIMERA_BIT, MFP_IERA.w   ; Timer-A IRQ enable
    bset    #MFP_TIMERA_BIT, MFP_IMRA.w   ; Timer-A IRQ unmask
    bra     .boot_audio_done

.boot_audio_dma:
    ; --- STE DMA sound init: mono 25033 Hz, loop mode, double-buffered.
    ; Silence both buffers, measure the length a VBL plays, program
    ; start/end to buffer A, start the loop. The DMA free-runs;
    ; .after_copy refills whichever buffer the DMA isn't reading and
    ; points the loop at it. No YM.
    lea     STE_SND_BUF_A, a0
    move.w  #(STE_SND_COPY/4)-1, d0
.ste_snd_clr_a:
    clr.l   (a0)+
    dbf     d0, .ste_snd_clr_a
    lea     STE_SND_BUF_B, a0
    move.w  #(STE_SND_COPY/4)-1, d0
.ste_snd_clr_b:
    clr.l   (a0)+
    dbf     d0, .ste_snd_clr_b
    move.b  #STE_DMA_MODE_VAL, STE_DMA_MODE.w
    tst.b   UFW_FALCON_SND
    beq.s   .not_falcon_snd
    move.b  SND_INT_CTRL.w, UFW_OLD_8900
    bset    #SND_INT_PLAY_TIMERA, SND_INT_CTRL.w
.not_falcon_snd:
    bsr     snd_measure                   ; UFW_SND_LEN: what a VBL plays here
    clr.w   UFW_SND_PREVPOS
    clr.b   UFW_SND_TICK
    move.b  #((STE_SND_BUF_A>>16)&$FF), STE_DMA_START_HI.w
    move.b  #((STE_SND_BUF_A>>8)&$FF), STE_DMA_START_MID.w
    move.b  #(STE_SND_BUF_A&$FF), STE_DMA_START_LO.w
    ; END = buffer A + the starting length, as userfw_snd_irq computes it.
    move.l  #STE_SND_BUF_A, d0
    add.w   UFW_SND_LEN, d0
    move.b  #((STE_SND_BUF_A>>16)&$FF), STE_DMA_END_HI.w
    move.w  d0, d1
    lsr.w   #8, d1
    move.b  d1, STE_DMA_END_MID.w
    move.b  d0, STE_DMA_END_LO.w
    ; Timer-A in event-count mode, one event per DMA frame end, so
    ; userfw_snd_irq re-points START/END right after every buffer switch
    ; (see the handler). Vector, data, control, then enable + unmask;
    ; interrupts are still masked here, so nothing fires until the
    ; caller's level is restored below.
    lea     userfw_snd_irq(pc), a0
    move.l  a0, VEC_TIMERA.w
    move.b  #1, MFP_TADR.w                ; interrupt on every event
    move.b  #$08, MFP_TACR.w              ; event-count mode
    bset    #MFP_TIMERA_BIT, MFP_IERA.w
    bset    #MFP_TIMERA_BIT, MFP_IMRA.w
    move.b  #$03, STE_DMA_CTRL.w          ; play + loop

.boot_audio_done:

    ; Timer-B on either path: the status bar's palette (userfw_hud_irq).
    ; Left stopped; the VBL loop arms it for each frame that has one.
    ; A1, not A0: on the YM path A0 is the sample cursor from here on.
    clr.w   UFW_SPLIT_NEXT
    clr.w   UFW_SPLIT_SHOW
    lea     userfw_hud_irq(pc), a1
    move.l  a1, VEC_TIMERB.w
    bset    #0, MFP_IERA.w                ; Timer-B IRQ enable (IERA bit 0)
    bset    #0, MFP_IMRA.w                ; Timer-B IRQ unmask (IMRA bit 0)

    bset    #MFP_ACIA_BIT, MFP_IERB.w     ; enable keyboard/MIDI ACIA IRQ
    bset    #MFP_ACIA_BIT, MFP_IMRB.w     ;         and unmask it

    ; --- Give TOS's Timer-C back to an Xpad provider ---------------
    ; A provider hooks etv_timer ($400), TOS's 200 Hz timer vector, and
    ; tail-chains to whatever it displaced. That chain bottoms out in
    ; TOS's own handler, which returns into TOS's Timer-C ISR and rte's
    ; off its interrupt frame -- md-sidepad's resident block says as
    ; much where it explains why "push orig; rts" is valid for $400.
    ;
    ; So the vector cannot be called by hand. Doing it from the VBL
    ; loop, as this used to, put no interrupt frame on the stack and
    ; bombed with 3 (address error) the first time it met a real
    ; provider. Let the real ISR make the call instead: TCDCR is left
    ; alone above, so Timer-C is still counting at TOS's rate and only
    ; its enable was cleared. That also gets the rate right for free
    ; (200 Hz, which providers divide by four) and makes A0 safe
    ; without masking anything, because TOS's ISR saves registers.
    ;
    ; The cost is TOS's 200 Hz handler running during the blit, which
    ; is the trade named when this was planned. Nothing happens at all
    ; unless a provider was found at boot.
    ifne    XPAD_ENABLE
    ifne    XPAD_TICK
    tst.l   UFW_XPAD_HDR
    beq.s   .no_timerc
    move.l  UFW_SAVE+8, VEC_TIMERC.w      ; TOS's handler back in place
    bset    #MFP_TIMERC_BIT, MFP_IERB.w
    bset    #MFP_TIMERC_BIT, MFP_IMRB.w
.no_timerc:
    endc
    endc

    ; Interrupts back on (caller's level, typically $2300).
    move.w  (sp)+, sr

    ; --- The IKBD is left exactly as it is. Do not add mode commands
    ; here. Its default state, which TOS leaves alone, already reports
    ; relative mouse packets ($F8..$FB + signed dx, dy) AND joystick 1
    ; events ($FE/$FF) alongside the keyboard scancodes, which is
    ; everything the game needs. Selecting joystick event reporting
    ; ($14) would switch the mouse back off -- the 6301 treats the two
    ; auto-report modes as one setting -- and $12 (what this used to
    ; send, before the RP demux could frame mouse packets) switches the
    ; mouse off outright. Y is at its "Y=0 at top" default, so positive
    ; dy means the mouse moved towards the user; the RP negates it for
    ; Doom's forward axis. See md-sidepad's sidepong example, which
    ; drives two paddles from a mouse and a joystick the same way.

    ; 256 colours: the pages and the mode were set up with the rest
    ; above (.eight_pages).
    tst.b   UFW_EIGHT
    bne.s   .first_page_set

    ; The first VBL shows whatever palette the RP has published so far.
    lea     PALETTE_ADDR, a6
    movem.l (a6), d0-d7
    movem.l d0-d7, UFW_PAL_LATCH

    ; Initialise the hidden-page pointer. UFW_SCREEN_PAGE holds the
    ; page currently being drawn into; .after_copy toggles it between
    ; UFW_SCREEN_A and UFW_SCREEN_B via XOR with UFW_SCREEN_XOR.
    move.l  #UFW_SCREEN_A, UFW_SCREEN_PAGE

    ; Shifter base HIGH byte ($07) is the same for both screen pages
    ; ($70000 and $78000), so we write it ONCE here and only update
    ; the MID byte per VBL in .after_copy below (saves ~20 cyc/VBL).
    move.b  #(UFW_SCREEN_A >> 16), VIDEO_BASE_ADDR_HIGH.w
.first_page_set:

    ; Pin IRQ state for the duration of .vbl_loop:
    ;   SR = $2300: supervisor mode, IPL=3. Blocks levels 1-3 (HBL
    ;   at IPL 2), allows VBL at IPL 4 and MFP at IPL 6: Timer-A,
    ;   Timer-B and the ACIA (see the timer list above
    ;   YM_TIMER_COUNT).
    move.w  #$2300, sr

    ; 256 colours: how many rows a VBL can copy here, now that the
    ; interrupts are running.
    tst.b   UFW_EIGHT
    beq.s   .calibrated
    bsr     eight_calibrate
.calibrated:

    ; --- Per-VBL loop ---
.vbl_loop:
    ; CPU-halt wait for the next vsync. The m68k `stop #$2300` halts
    ; until an IRQ at level > 3 fires (VBL at IPL=4, MFP at IPL=6).
    ; MFP interrupts (audio, the split, the IKBD) wake it too, so
    ; we can't assume the next wake is the VBL -- the userfw_vbl
    ; handler clears UFW_VBL_FLAG, but the dummy MFP handlers do
    ; not. After each wake we check the flag; if it's still set the
    ; wake came from a non-VBL IRQ and we `stop` again.
    move.w  #-1, UFW_VBL_FLAG.w
.wait_vbl:
    stop    #$2300
    tst.w   UFW_VBL_FLAG.w
    bne.s   .wait_vbl

    ; Report the display to the RP, first thing every VBL: one cart-bus
    ; read at VIDEO_WINDOW_BASE + mono. Being first, it also tells the
    ; RP the cart FB is about to be read again, which closes the window
    ; the frame ack opened (fb_wait_blit_ack). A1/D1 are scratch.
    moveq   #0, d1
    move.b  UFW_MONO, d1
    or.b    UFW_EIGHT, d1
    lea     VIDEO_WINDOW_BASE, a1
    tst.b   (a1, d1.w)

    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_VSYNC, PALETTE_IDX0.w   ; border = vsync mark
    endc

    ; 256 colours: a loop of its own up to the sound (.vbl8_palette).
    tst.b   UFW_EIGHT
    bne     .vbl8_palette

    ; The palette travels with its frame. The page going on screen now
    ; is the one copied last VBL, so it gets the palette latched then;
    ; the RP's slot, written in the same window as the pixels about to
    ; be copied, is latched for the next. Applying the slot directly
    ; would show every palette a frame early, which the adaptive pens
    ; (a new 16 every frame) would turn into a flash on every change.
    ; Cost: two MOVEM pairs, ~330 cyc / VBL. Not in mono, where colour
    ; 0 was set at boot and the rest mean nothing.
    ;
    ; In a two-VBL frame the second VBL shows the same page, so nothing
    ; is latched then; the page's colours are written again all the
    ; same (UFW_PAL_SHOW), as the split has left the status bar's in
    ; the registers.
    tst.b   UFW_MONO
    bne     .palette_done
    tst.b   UFW_HALF
    bne.s   .palette_again
    lea     UFW_PAL_LATCH, a5
    movem.l (a5), d0-d7
    movem.l d0-d7, PALETTE_BASE.w
    movem.l d0-d7, UFW_PAL_SHOW
    lea     PALETTE_ADDR, a6
    movem.l (a6), d0-d7
    movem.l d0-d7, (a5)

    ; The status bar's palette, the same way: the page going on screen
    ; takes the one latched with it, the slot is latched for the next.
    ; Timer-B then counts display lines and fires at the end of the
    ; line before the split; userfw_hud_irq does the rest.
    lea     UFW_HUD_NEXT, a5
    lea     UFW_HUD_SHOW, a6
    movem.l (a5), d0-d7
    movem.l d0-d7, (a6)
    move.w  UFW_SPLIT_NEXT, UFW_SPLIT_SHOW
    lea     HUD_PALETTE_ADDR, a6
    movem.l (a6), d0-d7
    movem.l d0-d7, (a5)
    move.w  SPLIT_ADDR, UFW_SPLIT_NEXT
    bra.s   .palette_split
.palette_again:
    movem.l UFW_PAL_SHOW, d0-d7
    movem.l d0-d7, PALETTE_BASE.w
.palette_split:
    clr.b   MFP_TBCR.w                    ; stopped; armed afresh each VBL
    tst.w   UFW_SPLIT_SHOW
    beq.s   .palette_done
    ; Where the video counter will be at the split: the page on screen
    ; (the base register still holds it; this VBL's flip comes after the
    ; copy) plus SPLIT_LINE rows, which is a whole number of 256 bytes.
    move.b  VIDEO_BASE_ADDR_MID.w, d0
    addi.b  #((SPLIT_LINE*FB_ROW_BYTES)>>8), d0
    move.b  d0, UFW_SPLIT_MID
    move.b  #(SPLIT_LINE-1), MFP_TBDR.w
    move.b  #$08, MFP_TBCR.w              ; event-count mode: display lines
.palette_done:

    ; Pure 68000 CPU copy via the FBDRV_INLINE macro (defined in
    ; the constants block). Same code path on plain ST / STE /
    ; MegaSTE / TT / Falcon -- no _MCH cookie dispatch, no STE
    ; blitter.
    ;
    ; FBDRV_INLINE clobbers D0-D7, A1-A4. A0 and A7 (SP) are
    ; PRESERVED (not in the MOVEM list): A0 holds the YM sample
    ; handler's dedicated audio buffer pointer (initialised at boot,
    ; advances + wraps inside the handler), A7 keeps the supervisor
    ; SP valid so IRQs can fire safely. A6 is the source pointer.
    ; D0-D7 / A1-A4 are scratch and not consumed after.
    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_RUNNING, PALETTE_IDX0.w  ; border = white (blit in flight)
    endc

    ; The second half of a two-VBL frame is due: carry on from where the
    ; last VBL stopped.
    tst.b   UFW_HALF
    bne     .blit_resume

    ; A5 = END of the screen page chunk-covered region. FBDRV_INLINE
    ; uses predec MOVEM (`movem.l list, -(a5)`) and walks A5 backwards
    ; from page_end down to page_start as it stores chunks in reverse
    ; order. After FBDRV_MAIN_ITERS iters A5 ends at the page START,
    ; which is the value .after_copy below expects in A5.
    movea.l UFW_SCREEN_PAGE, a5
    lea     (FBDRV_MAIN_ITERS * FBDRV_ITER_BYTES)(a5), a5
    movea.l #UFW_FB_SRC, a6
    FBDRV_INLINE FBDRV_ITERS_A        ; inline cart->ST screen copy, part 1

    ; With DMA sound in colour the rest follows straight on. Otherwise
    ; this VBL has done its share (UFW_TWO_VBL): in mono the whole copy
    ; takes ~17.6 ms against a 14 ms VBL, and without DMA sound the YM
    ; samples leave too little of a 20 ms one. The rest waits for the
    ; next VBL, and the page is not flipped until then. The resume needs
    ; nothing saved -- UFW_SCREEN_PAGE does not change until the frame
    ; is done, and the source and destination of part 2 follow from it.
    tst.b   UFW_TWO_VBL
    beq.s   .blit_part_b
    st      UFW_HALF
    bra     .after_copy

.blit_resume:
    sf      UFW_HALF
    movea.l UFW_SCREEN_PAGE, a5
    lea     (FBDRV_ITERS_B * FBDRV_ITER_BYTES)(a5), a5
    movea.l #(UFW_FB_SRC + FBDRV_ITERS_A * FBDRV_ITER_BYTES), a6
.blit_part_b:
    FBDRV_INLINE FBDRV_ITERS_B        ; part 2
    FBDRV_TAIL
    ifne    FBDRV_DEBUG_MARKS
    move.w  #BLIT_MARK_DONE, PALETTE_IDX0.w     ; border = green (copy done)
    endc

.after_copy:

    ; Report sound capability to the RP every VBL: one cart-bus read at
    ; SNDCAP_WINDOW_BASE + has_dma + UFW_TT. The RP's commemul ring
    ; decodes the low bit and produces the matching audio format (signed
    ; PCM for DMA, (vA,vB) YM pairs otherwise); bit 1 says a TT. A1/D1
    ; are scratch.
    moveq   #0, d1
    move.b  UFW_HAS_DMA, d1
    or.b    UFW_TT, d1
    lea     SNDCAP_WINDOW_BASE, a1
    tst.b   (a1, d1.w)

    ; The TT's internal speaker, as keypad ( has it (a no-op unless this
    ; is a TT and the setting changed). Clobbers D0/D1/A2, all scratch
    ; here.
    bsr     ufw_speaker_apply

    ; Relocation heartbeat, one read per VBL. Reached only from the
    ; relocated copy, which is the whole point: it is the RP's proof
    ; that nothing is executing from the cartridge code area any more.
    tst.b   RELOC_WINDOW_BASE+RELOC_PROTOCOL


    ; Report pad 0's Xpad buttons to the RP, when a provider was found
    ; at boot. Read tear-free the way the standard prescribes: sample
    ; seq and active, copy, then recheck both -- the provider bumps seq
    ; before flipping active, so a mismatch means the copy straddled a
    ; refresh and this frame is simply skipped. A disconnected slot
    ; (type 0) is skipped too, which is what tells the RP to fall back
    ; to the IKBD joystick. D0-D3/A1/A2 are scratch here (the page flip
    ; below reloads D0 and uses A5); A0 is the audio cursor and is not
    ; touched.
    ifne    XPAD_ENABLE
    move.l  UFW_XPAD_HDR, d1
    beq.s   .no_xpad
    movea.l d1, a1
    move.w  XPAD_SEQ_OFF(a1), d3         ; seq before
    moveq   #0, d2
    move.b  XPAD_ACTIVE_OFF(a1), d2      ; front buffer index
    beq.s   .xpad_buf0
    movea.l UFW_XPAD_BUF1, a2
    bra.s   .xpad_have_buf
.xpad_buf0:
    movea.l UFW_XPAD_BUF0, a2
.xpad_have_buf:
    move.l  (a2), d0                     ; buttons
    tst.b   XPAD_PAD_TYPE(a2)        ; 0 = nothing in this slot
    beq.s   .no_xpad
    cmp.b   XPAD_ACTIVE_OFF(a1), d2      ; buffer flipped under us?
    bne.s   .no_xpad
    cmp.w   XPAD_SEQ_OFF(a1), d3         ; refreshed under us?
    bne.s   .no_xpad
    ; High byte first, low byte second -- the RP commits the pair on the
    ; low byte, so the two reads are atomic from its side.
    ; Only D1's low word indexes the window, so the high byte needs a
    ; word shift (which leaves the top 8 bits clear -- no mask) and the
    ; low byte a zero-extended byte move.
    move.w  d0, d1
    lsr.w   #8, d1
    lea     XPAD_HI_WINDOW_BASE, a2
    tst.b   (a2, d1.w)
    moveq   #0, d1
    move.b  d0, d1
    lea     XPAD_LO_WINDOW_BASE, a2
    tst.b   (a2, d1.w)
.no_xpad:
    endc

    ; When DMA sound is active, refill the buffer the DMA is NOT reading
    ; (double-buffer), so the write never crosses the DMA read pointer.
    ; userfw_snd_irq has already pointed the loop at it. Bit 7 of the
    ; frame-counter mid byte tells us the current buffer: 0 = A ->
    ; refill B; 1 = B -> refill A.
    ; D1/D2/D3/A1/A2 are scratch (clobbered by FBDRV_INLINE; the
    ; page-flip below uses A5/D0). 125 longwords from the cart via
    ; move.l (a1)+,(a2)+ / dbf, which is 30 cycles an iteration =
    ; ~3,750 cycles, ~0.47 ms of the ST's ~3 ms post-blit slack. A MOVEM
    ; burst the way FBDRV_INLINE does it would cut that to ~0.3 ms, and
    ; A0 is free here on the DMA path (no YM samples) -- worth
    ; doing if the slack ever gets tight.
    tst.b   UFW_HAS_DMA
    beq     .no_dma_refill

    ; Where is the chip, and which buffer is it in? Both come out of one
    ; pair of counter reads, so the length decision and the buffer
    ; choice below can never disagree about what it is playing. Bit 15
    ; of the address separates the two buffers ($x7D00 vs $xFD00), so
    ; masking it off leaves the offset within whichever one it is in.
    moveq   #0, d1
    move.b  STE_DMA_CNT_MID.w, d1
    move.w  d1, d3                       ; keep the raw mid byte
    andi.w  #$7F, d1
    lsl.w   #8, d1
    move.b  STE_DMA_CNT_LO.w, d1
    subi.w  #STE_SND_BUF_MASK, d1        ; d1 = offset into the current buffer
    lsr.w   #7, d3
    andi.w  #1, d3                       ; d3 = 0 in buffer A, 1 in buffer B

    ; Steer the length from how the offset drifts. Sampled at the same
    ; point in every frame, it sits still when the chip is eating
    ; exactly one buffer per VBL; it creeps forward when the chip is
    ; running fast (buffers too short) and backwards when it is slow.
    ; Which way it moved is the whole signal -- the true ratio never has
    ; to be known, only its sign.
    ;
    ; Correcting on every frame overshoots and hunts, so the offset is
    ; sampled every frame but the length only moves on every fourth,
    ; which leaves time for the previous nudge to show up. A handover
    ; resets the offset and produces a large jump; those frames are
    ; ignored rather than acted on.
    ;
    ; In mono only the VBL that ends a frame is sampled. The two halves
    ; reach this point a couple of hundred cycles apart, and sampling
    ; both would put that difference into every comparison with the same
    ; sign -- steering the length a fraction of a sample off, so the
    ; chip's phase walks round the VBL and a buffer is replayed each
    ; time it laps. Two VBLs at a time, it is the same point every time.
    tst.b   UFW_HALF
    bne.s   .snd_len_report
    addq.b  #1, UFW_SND_TICK
    move.b  UFW_SND_TICK, d2
    andi.b  #3, d2
    bne.s   .snd_len_keep
    move.w  d1, d2
    sub.w   UFW_SND_PREVPOS, d2          ; d2 = drift since last frame
    cmpi.w  #64, d2
    bgt.s   .snd_len_keep                ; handover, not drift
    cmpi.w  #-64, d2
    blt.s   .snd_len_keep
    tst.w   d2
    beq.s   .snd_len_keep
    bgt.s   .snd_len_inc
    subq.w  #1, UFW_SND_LEN              ; chip falling behind: shorten
    cmpi.w  #STE_SND_LEN_MIN, UFW_SND_LEN
    bcc.s   .snd_len_keep
    move.w  #STE_SND_LEN_MIN, UFW_SND_LEN
    bra.s   .snd_len_keep
.snd_len_inc:
    addq.w  #1, UFW_SND_LEN              ; chip gaining: lengthen
    cmpi.w  #STE_SND_LEN_MAX, UFW_SND_LEN
    bls.s   .snd_len_keep
    move.w  #STE_SND_LEN_MAX, UFW_SND_LEN
.snd_len_keep:
    move.w  d1, UFW_SND_PREVPOS

    ; Tell the RP how many samples to produce next frame. Biased by the
    ; minimum so the whole range fits a byte; it is no longer a multiple
    ; of four, so it cannot be sent as a quarter.
.snd_len_report:
    move.w  UFW_SND_LEN, d1
    subi.w  #STE_SND_LEN_MIN, d1
    lea     SNDLEN_WINDOW_BASE, a1
    tst.b   (a1, d1.w)

    ; Refill the buffer the chip is NOT in, so the write never crosses
    ; the read pointer. The loop already points at it (userfw_snd_irq).
    tst.b   d3
    bne.s   .snd_use_a                   ; DMA in B -> refill A
    move.l  #STE_SND_BUF_B, d2           ; DMA in A -> refill B
    bra.s   .snd_have_buf
.snd_use_a:
    move.l  #STE_SND_BUF_A, d2
.snd_have_buf:
    ; Fixed-size copy from the cart buffer; END (set by userfw_snd_irq
    ; from UFW_SND_LEN) decides how much of it is actually played.
    lea     AUDIO_BUFFER_ADDR, a1
    movea.l d2, a2
    move.w  #(STE_SND_COPY/4)-1, d1
.ste_snd_copy:
    move.l  (a1)+, (a2)+
    dbf     d1, .ste_snd_copy

.no_dma_refill:

    ; Both acks carry the YM bank playing this VBL (VBLSYNC_BANK), so the
    ; refill that follows them goes to the other one. D1/A1 are scratch;
    ; the flip below uses A5/D0.
    moveq   #0, d1
    move.b  UFW_YM_BANK+2, d1
    andi.b  #VBLSYNC_BANK, d1
    lea     VBLSYNC_ADDR, a1
    tst.b   UFW_EIGHT
    bne     .vbl8_copy

    ; Half a two-VBL frame: no flip, and an ack that only says "a VBL
    ; went by" -- the RP keys its audio refill off it, but must not take
    ; the cart FB as free, because the rest is copied out of it next VBL.
    tst.b   UFW_HALF
    beq.s   .frame_done
    tst.b   (VBLSYNC_HALF_ADDR-VBLSYNC_ADDR)(a1, d1.w)
    bra.s   .input_check
.frame_done:

    ; Flip the video base to the just-written page. A5 still holds
    ; UFW_SCREEN_PAGE (preserved by FBDRV_INLINE). Only the MID byte
    ; of the screen base differs between the two pages -- HIGH was
    ; written once at boot (constant $07 for both $70000 / $78000).
    ;
    ; UFW_SCREEN_PAGE is a 32-bit address stored big-endian, so byte
    ; +2 of the longword is exactly the MID byte (bits 8..15) we need
    ; to write to VIDEO_BASE_ADDR_MID. Read it straight from memory
    ; instead of recomputing via lsr/move chain from A5.
    move.b  UFW_SCREEN_PAGE+2, VIDEO_BASE_ADDR_MID.w

    ; Toggle UFW_SCREEN_PAGE between SCREEN_A and SCREEN_B for the
    ; next frame.
    move.l  a5, d0
    eor.l   #UFW_SCREEN_XOR, d0
    move.l  d0, UFW_SCREEN_PAGE

    ; Frame-sync ack: one cart-bus read tells the RP the blit
    ; is finished and the cart FB is free to overwrite. Emitted every
    ; VBL with DMA sound in colour, every other one otherwise (the FB is
    ; free here -- blit done, page flipped). The RP's
    ; commemul ring captures the read; fb_publish() on the RP blocks
    ; until it sees this before running the next chunky-to-planar.
    tst.b   (a1, d1.w)

.input_check:
    ; Music on the YM: write the registers the RP published after the
    ; previous ack, if it published anything new (YM_PKT_ADDR). After
    ; this VBL's ack rather than before it, so it costs the RP none of
    ; its window for the next frame; a packet caught mid-write mixes two
    ; frames for one VBL, and each carries R0..R10 in full, so the next
    ; one puts that right. At most 14 writes, the mask cut to R0..R13
    ; here too, so nothing can reach the floppy and printer ports in
    ; R14/R15.
    ;
    ; With DMA sound the YM is always free. Without it the YM is the
    ; sound effects' DAC unless the RP has given it to the music
    ; (YM_MUSIC_ADDR, keypad )): switch over when that changes, with
    ; interrupts masked so no sample (Timer-A) lands mid-switch.
    tst.b   UFW_HAS_DMA
    bne.s   .ym_write
    tst.w   YM_MUSIC_ADDR
    sne     d0
    cmp.b   UFW_YM_MUSIC, d0
    beq.s   .ym_owner_same
    move.b  d0, UFW_YM_MUSIC
    move.w  sr, -(sp)
    ori.w   #$0700, sr
    tst.b   d0
    beq.s   .ym_to_sfx
    bclr    #MFP_TIMERA_BIT, MFP_IERA.w  ; to the music: the samples stop
    move.b  #8, YM_SELECT.w
    move.b  #0, YM_DATA.w                ; and A, B and C fall silent until
    move.b  #9, YM_SELECT.w              ; the first music packet
    move.b  #0, YM_DATA.w
    move.b  #10, YM_SELECT.w
    move.b  #0, YM_DATA.w
    bra.s   .ym_switched
.ym_to_sfx:
    bsr     ym_dac_init                  ; to the sound effects: the DAC again
    bset    #MFP_TIMERA_BIT, MFP_IERA.w  ; and the sample clock back on
.ym_switched:
    move.w  (sp)+, sr
.ym_owner_same:
    tst.b   UFW_YM_MUSIC
    beq.s   .ym_done
.ym_write:
    lea     YM_PKT_ADDR, a1
    move.w  (a1)+, d0                    ; sequence number
    cmp.w   UFW_YM_SEQ, d0
    beq.s   .ym_done
    move.w  d0, UFW_YM_SEQ
    move.w  (a1)+, d1                    ; registers to write
    andi.w  #$3FFF, d1
    moveq   #0, d0
.ym_reg:
    lsr.w   #1, d1
    bcc.s   .ym_next
    move.b  d0, YM_SELECT.w
    move.b  1(a1), YM_DATA.w             ; the value is the word's low byte
.ym_next:
    addq.l  #2, a1
    addq.b  #1, d0
    tst.w   d1
    bne.s   .ym_reg
.ym_done:

    ; The RP writes CMD_BOOT_GEM into CMD_MAGIC_SENTINEL to be handed
    ; the machine back (ESC, or Quit from the game's menu), and
    ; CMD_RESET to have the ST restarted -- which is how it leaves for
    ; the Booster: the RP jumps to the Booster app while the ST is busy
    ; rebooting, so the Booster's cartridge code is what the ST finds
    ; when it looks. Any other value (NOP, future commands) leaves the
    ; loop running. D0 carries the command through the restore below,
    ; which leaves it alone.
    move.l  CMD_MAGIC_SENTINEL, d0
    cmp.l   #CMD_BOOT_GEM, d0
    beq.s   .exit_requested
    cmp.l   #CMD_RESET, d0
    bne     .vbl_loop

.exit_requested:
    ; --- Restore IRQ state and hand the machine back ---------------
    ;
    ; Mask interrupts before touching MFP / vectors.
    ori.w   #$0700, sr

    ; Stop DMA sound (if it was running) before GEM returns, and any
    ; note the music left on the YM.
    tst.b   UFW_HAS_DMA
    beq.s   .no_dma_stop
    move.b  #$00, STE_DMA_CTRL.w
    tst.b   UFW_FALCON_SND
    beq.s   .no_dma_stop
    move.b  UFW_OLD_8900, SND_INT_CTRL.w
.no_dma_stop:
    move.b  #8, YM_SELECT.w
    move.b  #0, YM_DATA.w                ; ch A volume
    move.b  #9, YM_SELECT.w
    move.b  #0, YM_DATA.w                ; ch B volume
    move.b  #10, YM_SELECT.w
    move.b  #0, YM_DATA.w                ; ch C volume

    ; A TT: its internal speaker back as TOS had it. Only bit 6: the rest
    ; of port A (the floppy selects among them) is left as it now stands.
    tst.b   UFW_TT
    beq.s   .spk_restored
    move.b  #YM_REG_PORT_A, YM_SELECT.w
    move.b  YM_SELECT.w, d1
    bclr    #YM_PORTA_TT_SPEAKER, d1
    btst    #YM_PORTA_TT_SPEAKER, UFW_SPK_PORTA
    beq.s   .spk_restore
    bset    #YM_PORTA_TT_SPEAKER, d1
.spk_restore:
    move.b  d1, YM_DATA.w
.spk_restored:

    ; Reload the save-area pointer in case anything clobbered A5
    ; during the run.
    lea     UFW_SAVE, a5

    ; Stop both timers so no IRQ can fire mid-restore.
    clr.b   MFP_TBCR.w
    clr.b   MFP_TACR.w

    ; Restore MFP IER / IMR.
    move.b  24(a5), MFP_IERA.w
    move.b  25(a5), MFP_IERB.w
    move.b  26(a5), MFP_IMRA.w
    move.b  27(a5), MFP_IMRB.w
    move.b  28(a5), MFP_VR.w             ; restore TOS's S=1 / vector base

    ; Restore the 6 vectors we overwrote.
    move.l  0(a5), VEC_HBL.w
    move.l  4(a5), VEC_TIMERD.w
    move.l  8(a5), VEC_TIMERC.w
    move.l  12(a5), VEC_ACIA.w
    move.l  16(a5), VEC_TIMERB.w
    move.l  20(a5), VEC_TIMERA.w

    ; Restore TOS's VBL vector ($70 save from UFW_VBL_VEC_SAVE).
    move.l  UFW_VBL_VEC_SAVE, VBL_VECTOR.w

    ; And the caches as TOS had them. D0 carries the command: kept.
    move.l  d0, d1
    bsr     caches_back
    move.l  d1, d0

    ; Restore SR to TOS's usual IPL=3 (matches md-oric main.s:230).
    ; From here on TOS handles HBL / Timer / ACIA again -- IKBD will
    ; re-pump GEMDOS's keyboard buffer, GEM mouse cursor revives, etc.
    move.w  #$2300, sr

    ; CMD_RESET: restart the machine instead of returning to GEM. No
    ; delay here, unlike main.s's .reset -- that one runs from the RAM
    ; copy of the boot code, but this runs from the cartridge itself,
    ; which the RP is about to replace with the Booster's. Get out of
    ; it now and let the ST's memory test (the cleared $420 forces a
    ; cold boot, which is the slow one) cover the RP's jump.
    cmp.l   #CMD_RESET, d0
    bne.s   .back_to_gem
    clr.l   $420.w                    ; invalidate memvalid
    clr.l   $43A.w                    ; and memval2
    clr.l   $51A.w                    ; and memval3
    move.l  $4.w, a0                  ; the reset vector
    jmp     (a0)

.back_to_gem:

    ; 256 colours: TOS's colours back, and on a TT its mode.
    tst.b   UFW_EIGHT
    beq.s   .gem_screen
    lea     UFW8_TOS_PAL, a1
    bsr     pal8_put
    cmpi.b  #EIGHT_TT, UFW_EIGHT
    bne.s   .gem_screen
    move.w  UFW8_OLD_MODE, TT_SHIFT.w
.gem_screen:

    ; Restore screen base via XBIOS Setscreen.
    move.w  #-1, -(sp)                ; no rez change
    move.l  UFW_PHYSBASE_SAVE, -(sp)  ; physical screen
    move.l  UFW_PHYSBASE_SAVE, -(sp)  ; logical screen
    move.w  #5, -(sp)                 ; XBIOS Setscreen
    trap    #14
    lea     12(sp), sp

    ; A Falcon's mode back through TOS, which has its VBL again.
    cmpi.b  #EIGHT_FALCON, UFW_EIGHT
    bne.s   .gem_return
    move.w  UFW8_OLD_MODE, -(sp)
    move.w  #88, -(sp)                ; XBIOS VsetMode
    trap    #14
    addq.l  #4, sp
.gem_return:
    movea.l UFW_SP_SAVE, sp           ; the caller's stack, for its return address
    rts

; --- 256 colours: the VBL loop's own parts ----------------------------
; Reached from .vbl_loop and back into it, in place of the palette
; latches, the 16-colour copy and its acks and flip; everything else in
; the loop -- the reports, the sound, the command -- is shared. See
; "256 colours" in the equates.

    ; Top of the VBL: the page that has just gone on screen takes the
    ; palette latched with it.
.vbl8_palette:
    tst.b   UFW8_FLIPPED
    beq     .after_copy
    sf      UFW8_FLIPPED
    lea     UFW8_PAL_LATCH, a1
    bsr     pal8_put
    bra     .after_copy

    ; After the sound: the VBL's ack (A1/D1 from the loop), which the
    ; audio refill keys on, then as much of a part as the VBL has room
    ; for. Nothing here touches the YM: the music packet's space is the
    ; palette's, so the loop goes on from after it.
.vbl8_copy:
    tst.b   (a1, d1.w)
    moveq   #0, d0
    move.w  UFW8_BUDGET, d0
    move.w  d0, UFW8_LEFT
    lea     ROWS_WINDOW_BASE, a1
    tst.b   (a1, d0.w)                    ; for the debug line: <= 100, a byte
    tst.w   UFW8_ROW
    bne.s   .rows8                        ; a part under way
    move.w  PART_SEQ_ADDR, d0
    cmp.w   UFW8_SEQ, d0
    beq     .reack8                       ; nothing new
    move.w  d0, UFW8_SEQ
    moveq   #1, d0
    and.w   PART_IDX_ADDR, d0
    move.w  d0, UFW8_PART
    bne.s   .rows8
    move.w  PAL8_SEQ_ADDR, d0             ; part 0 brings the frame's palette
    cmp.w   UFW8_PALSEQ, d0
    beq.s   .rows8
    move.w  d0, UFW8_PALSEQ
    bsr     latch_pal8
.rows8:
    ; The part's next row: from its place in the FB, to row
    ; part * 100 + row of the picture in the page being drawn.
    move.w  UFW8_ROW, d0
    mulu    #CART_EIGHT_ROW_BYTES, d0
    movea.l #UFW_FB_SRC, a6
    adda.l  d0, a6
    move.w  UFW8_PART, d0
    mulu    #CART_EIGHT_ROWS, d0
    add.w   UFW8_ROW, d0
    mulu    UFW8_STEP, d0
    movea.l UFW_SCREEN_PAGE, a5
    adda.l  UFW8_FIRST, a5
    adda.l  d0, a5
.row8:
    bsr     copy_row8                     ; A6 on to the next row, A5 kept
    adda.w  UFW8_STEP, a5
    addq.w  #1, UFW8_ROW
    cmpi.w  #CART_EIGHT_ROWS, UFW8_ROW
    beq.s   .part8_done
    subq.w  #1, UFW8_LEFT
    bne.s   .row8
    bra     .ym_done                      ; the rest next VBL
.part8_done:
    clr.w   UFW8_ROW
    moveq   #0, d0
    move.b  UFW8_SEQ+1, d0
    lea     PARTACK_WINDOW_BASE, a1
    tst.b   (a1, d0.w)                    ; copied: the RP may write the FB again
    tst.w   UFW8_PART
    beq     .ym_done                      ; part 1 no sooner than next VBL
    ; Part 1: the frame is whole. Its page goes on screen at the next
    ; VBL and its palette with it (.vbl8_palette). Nothing more is
    ; copied in this VBL, so the next frame's palette cannot be latched
    ; before this one is applied.
    move.b  UFW_SCREEN_PAGE+1, VIDEO_BASE_ADDR_HIGH.w
    move.b  UFW_SCREEN_PAGE+2, VIDEO_BASE_ADDR_MID.w
    clr.b   VIDEO_BASE_ADDR_LOW.w
    move.l  UFW_SCREEN_PAGE, d0
    move.l  UFW8_XOR, d1
    eor.l   d1, d0
    move.l  d0, UFW_SCREEN_PAGE
    st      UFW8_FLIPPED
    bra     .ym_done
.reack8:
    ; Nothing new: the last part's ack again. The RP drains its capture
    ; ring only when it gets the chance, and a pack's flash erase holds it
    ; off for about a second, long enough for the ring to lap and lose
    ; the first one; it would then wait out its timeout for nothing.
    moveq   #0, d0
    move.b  UFW8_SEQ+1, d0
    lea     PARTACK_WINDOW_BASE, a1
    tst.b   (a1, d0.w)
    bra     .ym_done

; -------------------------------------------------------------------
; ym_dac_init -- the YM as the sound effects' DAC (no DMA sound): tones
; A and B on at period 0, so the counters run far above the audio band
; and the volume registers alone shape the output; tone C and all the
; noise off; all three volumes 0; and R8 (ch A volume) latched last, as
; the sample handler expects. At boot, and when the music hands the YM
; back (keypad ), YM_MUSIC_ADDR), with interrupts masked. Touches no
; registers.
ym_dac_init:
    move.b  #YM_REG_MIXER, YM_SELECT.w
    move.b  #YM_MIXER_DAC_AB, YM_DATA.w   ; $FC: tones A+B on, tone C off, all noise off, ports out
    move.b  #0, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R0 = ch A fine period
    move.b  #1, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R1 = ch A coarse period
    move.b  #2, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R2 = ch B fine period
    move.b  #3, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; R3 = ch B coarse period
    move.b  #10, YM_SELECT.w
    move.b  #0, YM_DATA.w                 ; ch C vol = 0 (the music may have left it up)
    move.b  #YM_REG_CHB_VOL, YM_SELECT.w  ; latch reg 9 to zero ch B
    move.b  #0, YM_DATA.w                 ; ch B vol = 0 (silence)
    move.b  #YM_REG_CHA_VOL, YM_SELECT.w  ; latch reg 8 (next YM_DATA writes hit ch A volume)
    move.b  #0, YM_DATA.w                 ; ch A vol = 0 (silence)
    rts

; -------------------------------------------------------------------
; find_cookie -- the value of the cookie named D1 in D0, or 0 if there is
; no jar or no such cookie. Clobbers D2/A2. (A bsr is safe: userfw runs
; from ST RAM, never from the cartridge.)
find_cookie:
    moveq   #0, d0
    move.l  COOKIE_JAR_PTR.w, d2
    beq.s   .done
    movea.l d2, a2
.next:
    move.l  (a2), d2
    beq.s   .done                         ; tag 0 = end of the jar
    cmp.l   d1, d2
    beq.s   .found
    addq.l  #8, a2
    bra.s   .next
.found:
    move.l  4(a2), d0
.done:
    rts

; -------------------------------------------------------------------
; ufw_speaker_apply -- once per VBL, on a TT only: switch its internal
; speaker to follow keypad ( (SPEAKER_ADDR), as MD/Speccy, MD/Lynx and
; MD/MJPEG do. The speaker's enable is bit 6 of the YM's port A, active
; low: the TT030 hardware reference lists it as "*Speaker Disable (0
; disables internal speaker)". The other port A bits are the floppy
; selects, the printer strobe, RTS/DTR and the LAN select, so this is a
; read-modify-write, and only when the setting has changed
; (UFW_SPK_STATE starts at neither, so the first VBL applies the
; default, off).
;
; The YM is only touched at IPL 7, and reg 8 is latched again last, for
; the YM sample handler, which shares the register latch and relies on
; reg 8 being latched between fires (it only runs on a TT in a
; FORCE_NO_DMA build).
;
; Clobbers D0/D1/A2. A0 (the YM sample cursor) is left alone.
ufw_speaker_apply:
    tst.b   UFW_TT
    beq.s   .spk_ret                      ; not a TT
    tst.w   SPEAKER_ADDR
    sne     d1                            ; d1: $FF on, $00 off
    cmp.b   UFW_SPK_STATE, d1
    beq.s   .spk_ret                      ; as it is already
    move.b  d1, UFW_SPK_STATE
    lea     YM_SELECT.w, a2
    move.w  sr, -(sp)
    ori.w   #$0700, sr
    move.b  #YM_REG_PORT_A, (a2)
    move.b  (a2), d0                      ; port A as it stands
    bclr    #YM_PORTA_TT_SPEAKER, d0      ; off...
    tst.b   d1
    beq.s   .spk_write
    bset    #YM_PORTA_TT_SPEAKER, d0      ; ...or on
.spk_write:
    move.b  d0, 2(a2)
    move.b  #YM_REG_CHA_VOL, (a2)         ; reg 8 latched again for the sample handler
    move.w  (sp)+, sr
.spk_ret:
    rts

; -------------------------------------------------------------------
; caches_off / caches_back -- as md-xt (MD/SuperCharger) does it. A
; 68020 or 68030 (a TT, a Falcon): its data cache off, so that every
; cartridge read reaches the bus -- one the cache answered would never
; reach the RP, and a stale one shows an old picture -- and its
; instruction cache on, both emptied. TOS 3.06's PMMU makes the
; cartridge cache-inhibited on a TT, but nothing promises that of
; every TOS. A 68040 or 68060: both off, written back first. And on the
; way out, the caches as TOS had them, emptied first. Clobber D0.
caches_off:
    cmpi.w  #20, UFW_CPU
    blo.s   .done
    dc.w    $4E7A, $0002                  ; movec cacr, d0
    move.l  d0, UFW_OLD_CACR
    cmpi.w  #40, UFW_CPU
    bhs.s   .later
    move.l  #$0809, d0                    ; EI, CI, CD
    dc.w    $4E7B, $0002                  ; movec d0, cacr
    rts
.later:
    dc.w    $F4F8                         ; cpusha bc
    moveq   #0, d0
    dc.w    $4E7B, $0002                  ; movec d0, cacr
.done:
    rts

caches_back:
    cmpi.w  #20, UFW_CPU
    blo.s   .done
    move.l  UFW_OLD_CACR, d0
    cmpi.w  #40, UFW_CPU
    bhs.s   .later
    ori.w   #$0808, d0                    ; CI, CD
    dc.w    $4E7B, $0002                  ; movec d0, cacr
    rts
.later:
    dc.w    $F4D8                         ; cinva bc
    dc.w    $4E7B, $0002                  ; movec d0, cacr
.done:
    rts

; -------------------------------------------------------------------
; pal8_put / pal8_get -- 256 colours into the palette registers from A1,
; or out of them into A2: a TT's words at $FF8400, a Falcon's longs at
; $FF9800, by UFW_EIGHT. Clobber D0/A1/A2.
pal8_put:
    move.w  #CART_PAL8_ENTRIES-1, d0
    cmpi.b  #EIGHT_FALCON, UFW_EIGHT
    beq.s   .falcon
    lea     TT_PALETTE.w, a2
.tt:
    move.w  (a1)+, (a2)+
    dbf     d0, .tt
    rts
.falcon:
    lea     FALCON_PALETTE.w, a2
.falcon_loop:
    move.l  (a1)+, (a2)+
    dbf     d0, .falcon_loop
    rts

pal8_get:
    move.w  #CART_PAL8_ENTRIES-1, d0
    cmpi.b  #EIGHT_FALCON, UFW_EIGHT
    beq.s   .falcon
    lea     TT_PALETTE.w, a1
.tt:
    move.w  (a1)+, (a2)+
    dbf     d0, .tt
    rts
.falcon:
    lea     FALCON_PALETTE.w, a1
.falcon_loop:
    move.l  (a1)+, (a2)+
    dbf     d0, .falcon_loop
    rts

; -------------------------------------------------------------------
; latch_pal8 -- the palette the RP sent with part 0, kept in
; UFW8_PAL_LATCH until its frame goes on screen. A TT's words as they
; are. A Falcon's RRRRRRbb GGGGGGbb words (cart_shared.h) made into the
; longs $FF9800 takes: red and green move up a word as they are, and
; blue's two pairs of bits go to 7..6 and 5..4 of the low word.
; Clobbers D0-D3/A1/A2.
latch_pal8:
    lea     PAL8_ADDR, a1
    lea     UFW8_PAL_LATCH, a2
    move.w  #CART_PAL8_ENTRIES-1, d3
    cmpi.b  #EIGHT_FALCON, UFW_EIGHT
    beq.s   .falcon
    lsr.w   #1, d3                        ; 256 words = 128 longs
.tt:
    move.l  (a1)+, (a2)+
    dbf     d3, .tt
    rts
.falcon:
    move.w  (a1)+, d0                     ; RRRRRRbb GGGGGGbb
    move.w  d0, d1
    andi.w  #$FCFC, d1
    swap    d1
    clr.w   d1                            ; RRRRRR00 GGGGGG00 00000000 00000000
    move.w  d0, d2
    lsr.w   #2, d2
    andi.w  #$00C0, d2                    ; blue 5..4 from 9..8
    or.w    d2, d1
    andi.w  #$0003, d0
    lsl.w   #4, d0                        ; blue 3..2 from 1..0
    or.w    d0, d1
    move.l  d1, (a2)+
    dbf     d3, .falcon
    rts

; -------------------------------------------------------------------
; copy_row8 -- one 320-byte row of 8 planes from the cartridge (A6, left
; on the next row) to the screen (A5, kept), and on a TT to the line
; below it as well, from the same registers rather than a second read
; of the cartridge. A loop of 40-byte MOVEMs, small enough for a
; 68030's 256-byte instruction cache, where FBDRV_INLINE's unrolled 5 KB
; would be fetched from RAM all over again. Clobbers D0-D7/A1-A3.
copy_row8:
    moveq   #(CART_EIGHT_ROW_BYTES/40)-1, d7
    tst.b   UFW8_TWICE
    bne.s   .twice
.once:
    movem.l (a6)+, d0-d6/a1-a3
    movem.l d0-d6/a1-a3, (a5)
    lea     40(a5), a5
    dbf     d7, .once
    lea     -CART_EIGHT_ROW_BYTES(a5), a5
    rts
.twice:
    movem.l (a6)+, d0-d6/a1-a3
    movem.l d0-d6/a1-a3, (a5)
    movem.l d0-d6/a1-a3, CART_EIGHT_ROW_BYTES(a5)
    lea     40(a5), a5
    dbf     d7, .twice
    lea     -CART_EIGHT_ROW_BYTES(a5), a5
    rts

; -------------------------------------------------------------------
; eight_calibrate -- 256 colours: how many rows of a part a VBL has room
; to copy on this machine (UFW8_BUDGET), and the copy state from
; scratch. Counts the rows copied between two VBLs -- the same row, into
; the page not on screen, which the first frame overwrites -- and keeps
; three quarters for the rest of the VBL's work: the palette, the sound,
; the reports, the interrupts. Never more than a part, as a VBL copies
; one at most. Needs interrupts at $2300. Clobbers D0-D7/A1-A6.
eight_calibrate:
    move.w  PART_SEQ_ADDR, UFW8_SEQ       ; only parts posted from now on
    move.w  PAL8_SEQ_ADDR, d0
    not.w   d0
    move.w  d0, UFW8_PALSEQ               ; so the first part 0's palette is new
    clr.w   UFW8_ROW
    sf      UFW8_FLIPPED
    move.w  #-1, UFW_VBL_FLAG.w
.sync:
    stop    #$2300
    tst.w   UFW_VBL_FLAG.w
    bne.s   .sync
    move.w  #-1, UFW_VBL_FLAG.w
    suba.l  a4, a4
.row:
    movea.l #UFW_FB_SRC, a6
    movea.l UFW_SCREEN_PAGE, a5
    adda.l  UFW8_FIRST, a5
    bsr     copy_row8
    addq.l  #1, a4
    tst.w   UFW_VBL_FLAG.w
    bne.s   .row
    move.l  a4, d0
    move.w  d0, d1
    lsr.w   #2, d1
    sub.w   d1, d0                        ; three quarters
    bne.s   .some
    moveq   #1, d0
.some:
    cmpi.w  #CART_EIGHT_ROWS, d0
    bls.s   .most
    move.w  #CART_EIGHT_ROWS, d0
.most:
    move.w  d0, UFW8_BUDGET
    rts

; -------------------------------------------------------------------
; snd_measure -- UFW_SND_LEN's starting point: how many samples the DMA
; plays in a VBL on this machine (see STE_SND_LEN_MIN). Plays
; SND_MEASURE_BYTES of silence once from screen page A, reads the frame
; counter at a VBL and SND_MEASURE_VBLS VBLs later, and takes the
; average, clamped to the steer's range. The counter's three bytes are
; read one at a time, so the mid byte is read again and the whole read
; repeated if a carry got in between. The waits need the VBL: interrupts
; are let in for them (no MFP source is enabled yet) and the caller's SR
; is put back. STE_DMA_MODE must be set. Clobbers D0-D3/A0/A1 (A0 by
; userfw_vbl).
snd_measure:
    move.w  sr, -(sp)
    lea     UFW_SCREEN_A, a1
    move.w  #(SND_MEASURE_BYTES/4)-1, d0
.clear:
    clr.l   (a1)+
    dbf     d0, .clear
    move.b  #((UFW_SCREEN_A>>16)&$FF), STE_DMA_START_HI.w
    move.b  #((UFW_SCREEN_A>>8)&$FF), STE_DMA_START_MID.w
    move.b  #(UFW_SCREEN_A&$FF), STE_DMA_START_LO.w
    move.b  #(((UFW_SCREEN_A+SND_MEASURE_BYTES)>>16)&$FF), STE_DMA_END_HI.w
    move.b  #(((UFW_SCREEN_A+SND_MEASURE_BYTES)>>8)&$FF), STE_DMA_END_MID.w
    move.b  #((UFW_SCREEN_A+SND_MEASURE_BYTES)&$FF), STE_DMA_END_LO.w
    move.b  #$01, STE_DMA_CTRL.w          ; play once, no loop
    moveq   #SND_MEASURE_VBLS, d2         ; the first VBL lines up, then N timed
.vbl:
    move.w  #-1, UFW_VBL_FLAG.w
.wait:
    stop    #$2300
    tst.w   UFW_VBL_FLAG.w
    bne.s   .wait
.read:
    move.b  STE_DMA_CNT_MID.w, d0
    lsl.w   #8, d0
    move.b  STE_DMA_CNT_LO.w, d0
    move.w  d0, d1
    lsr.w   #8, d1
    cmp.b   STE_DMA_CNT_MID.w, d1
    bne.s   .read                         ; the low byte carried: again
    cmpi.w  #SND_MEASURE_VBLS, d2
    bne.s   .timed
    move.w  d0, d3                        ; where it was at the first VBL
.timed:
    dbf     d2, .vbl
    clr.b   STE_DMA_CTRL.w                ; stop
    move.w  (sp)+, sr
    sub.w   d3, d0
    lsr.w   #SND_MEASURE_SHIFT, d0
    cmpi.w  #STE_SND_LEN_MIN, d0
    bhs.s   .not_short
    move.w  #STE_SND_LEN_MIN, d0
.not_short:
    cmpi.w  #STE_SND_LEN_MAX, d0
    bls.s   .not_long
    move.w  #STE_SND_LEN_MAX, d0
.not_long:
    move.w  d0, UFW_SND_LEN
    rts

; -------------------------------------------------------------------
; userfw_vbl -- VBL interrupt handler. Two jobs:
;   1. Point A0 at the start of the other YM sample bank
;      (YM_BANK_BYTES), and the sample handler will start from there
;      on the next IRQ. Pinning A0 = base once per VBL eliminates the
;      explicit `cmpa.l + bcs.s` wrap in the sample hot path, so
;      that handler shrinks to a single `move.b (a0)+, YM_DATA.w`
;      + rte. A0 is dedicated to audio (excluded from the
;      FBDRV_INLINE MOVEM list and from the IKBD poll), so it's
;      safe to overwrite here from IRQ context. The banks alternate
;      on every machine; only the YM path reads them.
;   2. Clear UFW_VBL_FLAG so .vbl_loop's `stop`-then-check wait can
;      distinguish a VBL wake from an MFP wake.
;
; This replaces TOS's VBL handler entirely while userfw is running,
; so mouse / cursor-blink / keyboard-repeat / _vblqueue all stop
; firing. The ACIA IRQ ($118) is stubbed too, so GEMDOS's keyboard
; buffer is no longer filled; ESC detection runs through the inline
; IKBD poll in FBDRV_INLINE.
userfw_vbl:
    eori.w  #YM_BANK_BYTES, UFW_YM_BANK+2
    movea.l UFW_YM_BANK, a0
    clr.w   UFW_VBL_FLAG.w
    rte

; -------------------------------------------------------------------
; userfw_ym_irq -- the YM sample clock, Timer-A in /4 delay mode
; (YM_TIMER_COUNT), ~5,585 Hz, on machines without DMA sound.
;
; Dual-channel Ghostbusters-LUT mode: each sample in the cart buffer
; is 2 bytes = (vA, vB), pre-resolved at build time by running the
; raw G1.SAM bytes through the demo's 64-entry SAMPLE1 LUT (top 6
; bits of each PCM byte index a (chA, chB) pair). Per fire:
;   1. Write vA to YM ch A vol (reg 8 latched on entry).
;   2. Latch reg 9 (ch B vol).
;   3. Write vB to YM ch B vol.
;   4. Re-latch reg 8 so the next fire writes ch A immediately.
;
; A0 is a DEDICATED cart audio-buffer cursor (userfw_vbl resets it
; to AUDIO_BUFFER_ADDR each VBL; postinc walks 2 bytes/fire).
;
; MFP is in auto-EOI mode (VR S=0) so the in-service bit clears
; automatically on each IACK cycle.
;
; Cycle budget per fire:
;   move.b  (a0)+, YM_DATA.w               ; 12 cyc -- vA -> ch A vol
;   move.b  #YM_REG_CHB_VOL, YM_SELECT.w   ; 12 cyc -- latch reg 9
;   move.b  (a0)+, YM_DATA.w               ; 12 cyc -- vB -> ch B vol
;   move.b  #YM_REG_CHA_VOL, YM_SELECT.w   ; 12 cyc -- re-latch reg 8
;   rte                                    ; 20 cyc
;   ---                                    ; 68 cyc/IRQ
; At 12,539 Hz: 68 * 251 = ~17.1 k cyc/VBL = 2.1 ms = 10.7% CPU.
; Combined with FB_COPY_LINES=100 macro (~8.8 ms / VBL = 44%),
; total VBL load ~55%, leaving ~9.1 ms slack.
userfw_ym_irq:
    move.b  (a0)+, YM_DATA.w               ; vA -> ch A vol
    move.b  #YM_REG_CHB_VOL, YM_SELECT.w   ; latch ch B vol reg
    move.b  (a0)+, YM_DATA.w               ; vB -> ch B vol
    move.b  #YM_REG_CHA_VOL, YM_SELECT.w   ; back to ch A vol reg for next fire
    rte

; -------------------------------------------------------------------
; userfw_acia_irq -- keyboard/MIDI ACIA IRQ handler.
;
; Reads each IKBD byte the instant it arrives -- interrupt-driven, so no
; byte is lost -- and forwards it to the RP over the same cart-bus
; window the inline poll used. This is what makes multi-byte joystick
; packets survive (polling drops them). Keyboard scancodes ride the same
; path, so keyboard input gets more robust too.
;
; The keyboard and MIDI ACIAs share this one MFP interrupt (IERB bit 6),
; so the MIDI ACIA is drained as well -- otherwise its level-asserted
; IRQ line could re-trigger us forever. MFP is auto-EOI (VR S=0), so no
; in-service ack. Only D0/A1 are touched, and both are saved, so the
; handler is safe to fire mid-blit (which holds D0-D7/A1-A4 live).
userfw_acia_irq:
    move.l  d0, -(sp)
    move.l  a1, -(sp)
    lea     IKBD_WINDOW_BASE, a1
    btst    #0, ACIA_KBD_STATUS.w         ; keyboard RX-data-ready?
    beq.s   .acia_midi
    moveq   #0, d0                        ; clean 0..255 word
    move.b  ACIA_KBD_DATA.w, d0           ; read byte (clears kbd RX-full)
    tst.b   (a1, d0.w)                    ; forward via cart-bus read
.acia_midi:
    btst    #0, ACIA_MIDI_STATUS.w        ; drain MIDI too (shared IRQ line)
    beq.s   .acia_done
    move.b  ACIA_MIDI_DATA.w, d0          ; read + discard
.acia_done:
    movea.l (sp)+, a1
    move.l  (sp)+, d0
    rte

; -------------------------------------------------------------------
; userfw_snd_irq -- MFP Timer-A in event-count mode, one event per STE
; DMA sound frame end. The chip has just latched START/END and begun the
; other buffer, so point START/END at the buffer it left; the VBL loop
; fills that one during the frame (.after_copy). Done here, the update
; is a whole frame away from the next latch and can never be caught
; half-written (see the STE_SND_BUF comment for what that did). Only the
; MID byte of START and the MID/LO bytes of END differ between the two
; buffers; the HI bytes ($07) were written at boot. UFW_SND_LEN is
; steered by the VBL loop with single-instruction adds, so the read here
; is atomic. Saves D0 only; auto-EOI, so no in-service ack; level 6, so
; it can fire inside FBDRV_INLINE like the ACIA handler.
userfw_snd_irq:
    move.l  d0, -(sp)
    btst    #7, STE_DMA_CNT_MID.w         ; 1 = the chip is now in B
    beq.s   .snd_irq_in_a
    move.b  #((STE_SND_BUF_A>>8)&$FF), STE_DMA_START_MID.w
    move.l  #STE_SND_BUF_A, d0
    bra.s   .snd_irq_end
.snd_irq_in_a:
    move.b  #((STE_SND_BUF_B>>8)&$FF), STE_DMA_START_MID.w
    move.l  #STE_SND_BUF_B, d0
.snd_irq_end:
    add.w   UFW_SND_LEN, d0               ; <= 512: no carry out of the low word
    move.b  d0, STE_DMA_END_LO.w
    lsr.w   #8, d0
    move.b  d0, STE_DMA_END_MID.w
    move.l  (sp)+, d0
    rte

; -------------------------------------------------------------------
; userfw_dummy_irq -- single-rte IRQ handler for vectors we want to
; silence (HBL $68, Timer-A $134 until the STE DMA path claims it,
; Timer-C $114, Timer-D $110, ACIA $118). Stopping TOS's handlers cuts the per-frame jitter they
; impose on the blit; we don't need their behaviour because the
; framebuffer template owns the screen + IKBD until ESC exit.
; userfw_hud_irq -- Timer-B in event-count mode: the status bar's 16
; colours, from line SPLIT_LINE. Fires at the end of the line before;
; the colours are loaded into registers, the video address counter
; tells when the shifter has fetched the view's last line, and one
; MOVEM writes them in the blank that follows, so no visible pixel of
; either side changes colour. Starting up to a few hundred cycles late
; (an audio interrupt first, a MOVEM in the copy) still makes the
; blank. The wait is about a line (~64 us) a frame, at IPL 6: the
; 6850s hold a byte each and IKBD bytes are 1.28 ms apart (MIDI's
; 320 us), the DMA frame-end handler has a frame to spare, and without
; DMA sound one YM sample a frame may play up to that much late. The
; bound only matters if the counter never gets there (it always
; should).
userfw_hud_irq:
    movem.l d0-d7/a1-a3, -(sp)
    lea     UFW_HUD_SHOW, a1
    movem.l (a1), d0-d5/a2-a3             ; colours 0..11 in d0-d5, 12..15 in a2/a3
    move.b  UFW_SPLIT_MID, d6
    move.w  #HUD_SPIN_MAX, d7
    lea     VIDEO_COUNT_MID.w, a1
.hud_spin:
    cmp.b   (a1), d6
    dbls    d7, .hud_spin                 ; until counter bits 8..15 >= the split's
    rept    HUD_SPLIT_NOPS
    nop
    endr
    movem.l d0-d5/a2-a3, PALETTE_BASE.w
    clr.b   MFP_TBCR.w                    ; once a frame
    movem.l (sp)+, d0-d7/a1-a3
    rte

userfw_dummy_irq:
    rte

; The copier rounds the length up to a longword, so leave it something
; harmless to read; the same convention the sibling projects use for
; firmware.py's trailing-zero trim.
    nop
    nop
userfw_image_end:

; Build-time guard: the destination is a fixed 8 KB window immediately
; below screen page A, so an image that outgrows it would silently
; overrun the screen -- and before that, the stack at its top.
    ifgt (userfw_image_end - userfw_image_start) - (UFW_RAM_SIZE - UFW_STACK_SIZE)
    fail "userfw image leaves less than UFW_STACK_SIZE of UFW_RAM_SIZE for the stack"
    endc
