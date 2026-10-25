/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Peter Whitney */
/* CTL-1 on the M-VAVE SMK-37: plat.h over felucca-bsp's chip and smk37 board layers.
 * Part of the unity build (unity.c), after usb.c, storage.c and panel.c. Replaces
 * plat_fm1.c for BOARD_SMK37 builds.
 *
 * Key differences from plat_fm1.c:
 *   - No TRS MIDI (FM1B_TRS = 0; PH8 is a matrix row on the SMK)
 *   - No Bluetooth (USB-only packages first per smk37-port.yaml)
 *   - MASTER is virtual: K1 encoder accumulated in fm1_an.master (fm1_input.h)
 *   - Four faders: CC 16-19, soft takeover, stored as fader_cc[] and fader_val[]
 *   - 16 RGB pad LEDs (WS2812, PC10) — driven by smk_rgb_flush() in fm1_input.h
 *   - 19 single-colour button LEDs (two commons at 0x16A0C)
 *   - Panel default: fixed (SMK-37 has no calibration; the matrix ids are exact)
 *
 * Remote control (USB SysEx) is inherited unchanged from main_fm1.c's remote_frame().
 */

uint32_t plat_ms(void) { return fm1_ms; }

/* ---- remote control (same wire format as the FM-1; main_fm1.c remote_frame handles it) ----------- */
static struct {
    volatile uint32_t btn, keys;
    volatile int32_t enc[NE];
    volatile int32_t master;             /* -1: use the virtual encoder; >= 0: override */
} remote = {0, 0, {0}, -1};

/* Called from main_fm1.c's remote_frame() — same op table as the FM-1.
 * Panel button / key / encoder deltas added on top of the hardware scan. */
void smk_remote_btn(uint32_t mask)   { remote.btn  = mask; }
void smk_remote_keys(uint32_t mask)  { remote.keys = mask; }
void smk_remote_enc(int r, int d)    { if (r >= 0 && r < NE) remote.enc[r] += d; }
void smk_remote_master(int32_t v)    { remote.master = v; }   /* 4097+: revert to encoder */

/* ---- buttons --------------------------------------------------------------------- */

uint32_t plat_buttons(void)
{
    uint32_t raw = fm1_in.buttons, out = 0, i;
    for (i = 0; i < NB; i++)
        if (raw & (1u << panel.btn[i]))
            out |= 1u << i;
    return out | remote.btn;
}

uint32_t plat_keys(void) { return (fm1_in.notes | remote.keys) & ((1u << NKEYS) - 1u); }

/* ---- encoders -------------------------------------------------------------------- */

int32_t plat_enc(int role)
{
    int32_t r;
    if (role < 0 || role >= NE)
        return 0;
    r = remote.enc[role];
    remote.enc[role] = 0;
    return fm1_enc_take(panel.enc[role]) * panel.dir[role] + r;
}

/* ---- MASTER (virtual K1 encoder) ------------------------------------------------- */
/* fm1_input_tick() accumulates K1 encoder turns into fm1_an.master (0..1023, 10-bit).
 * plat_master() scales to 0..4092 (the FM-1 pot's 10-bit × 4 range) and applies any
 * remote override.  master_pos (main_fm1.c) is set from this return value each loop. */

uint32_t plat_master(void)
{
    /* fm1_an.master: accumulated 10-bit virtual pot value (0..1023, saturating).
     * Scale to 0..4092 so APP_GAIN(m) = m * m >> 12 gives the same range as the FM-1. */
    uint32_t v = (uint32_t)fm1_an.master * 4u;
    if (v > 4092u) v = 4092u;
    return remote.master >= 0 ? (uint32_t)remote.master : v;
}

/* ---- faders: CC 16-19, soft takeover --------------------------------------------- */
/* Each fader sends CC only once the physical fader has passed through its last-sent value
 * (soft takeover: no sudden jump on page switch or preset load).  The fader's current
 * ADC reading (0..1023, 10-bit) is in fm1_an.faders[0..3].  The last-CC-sent value is
 * in fader_val[]; fader_cc[] carries the CC numbers (default 16-19, settable via FADER
 * BANK + KNOB 1-4).  fader_taken[] tracks whether the takeover threshold was crossed. */
static uint8_t fader_cc[4]   = {16, 17, 18, 19};
static uint8_t fader_val[4]  = {64, 64, 64, 64};
static uint8_t fader_taken[4]= {0,  0,  0,  0};
static uint16_t fader_adc[4] = {511, 511, 511, 511};  /* last ADC reading (0..1023) */

/* fader_cc_set: FADER BANK held, K5-K8 (KNOB 1-4) set CC numbers.
 * Called from the mapping layer (Task 5, app.c / ui.c). */
void smk_fader_cc_set(int idx, uint8_t cc) { if (idx >= 0 && idx < 4) fader_cc[idx] = cc; }
uint8_t smk_fader_cc_get(int idx) { return (idx >= 0 && idx < 4) ? fader_cc[idx] : 0; }

/* smk_faders_tick: call once per main-loop frame.
 * Reads fm1_an.faders[], applies soft takeover, sends CC if the fader moved past the
 * takeover point and the value changed.  The MIDI channel comes from proj.chan. */
void smk_faders_tick(void)
{
    int i;
    extern project_t proj;           /* app/project.c: proj.chan (0..15) */
    for (i = 0; i < 4; i++) {
        uint16_t adc = fm1_an.faders[i];     /* 0..1023, 10-bit */
        uint8_t v7   = (uint8_t)(adc >> 3);  /* 0..127 */
        if (adc == fader_adc[i])
            continue;                          /* no movement */
        fader_adc[i] = adc;
        if (!fader_taken[i]) {
            /* soft takeover: accept once the fader crosses the last-sent value */
            int prev = fader_val[i];
            int cur  = (int)v7;
            int last = (int)(fader_adc[i] >> 3);   /* prev tick's ADC as 7-bit */
            /* crossed: the fader moved through the stored value from either direction */
            if ((prev >= cur && prev <= last) || (prev <= cur && prev >= last))
                fader_taken[i] = 1;
            if (!fader_taken[i])
                continue;
        }
        if (v7 == fader_val[i])
            continue;
        fader_val[i] = v7;
        /* send CC on proj.chan */
        {
            uint32_t ch  = proj.chan & 0xFu;
            uint32_t pkt = (0x0Bu << 24) | (0xB0u | ch) << 16 | (uint32_t)fader_cc[i] << 8 | v7;
            plat_midi_out(pkt);
        }
        /* brief screen overlay: handled by the mapping layer (Task 5) */
    }
}

/* smk_fader_val / smk_fader_reset: read back and reset takeover on a preset load */
uint8_t smk_fader_val(int idx) { return (idx >= 0 && idx < 4) ? fader_val[idx] : 0; }
void smk_fader_reset_takeover(void)
{
    int i;
    for (i = 0; i < 4; i++)
        fader_taken[i] = 0;
}

/* ---- LEDs ------------------------------------------------------------------------ */

/* Button LEDs: 19 single-colour LEDs (smk37/fm1_input.h smk__btnled_word).
 * The bit assignments follow the SMK_BTN_POS[] order:
 *   bits 0..5   = KNOB BANK, FADER BANK, <, >, OCT-, OCT+
 *   bits 6..11  = ARP, NOTE REPEAT, SCALE, CHORD, GLOBE, BT
 *   bits 12..17 = PATCH, PARA, FX, SEQ, SEQ PLAY, SEQ REC
 * The two LED commons at 0x16A0C are driven active-low by fm1_input.h's
 * smk__btnled_word() and written by fm1_input_tick(). */
static uint32_t smk_btnled;    /* current button LED word sent to the 595 each tick */

void plat_lights(uint32_t buttons, uint32_t keys, uint32_t dim_buttons, uint32_t dim_keys, int glow)
{
    /* The SMK-37 has 19 single-colour button LEDs and 16 RGB pad LEDs (Task 5 drives the
     * pads separately via smk_rgb_set_step()).  Here we only manage the button LEDs.
     * On the SMK, 'keys' LEDs are not present (the keys have no individual LEDs), and the
     * 'dim' (glow) effect is approximated by a half-bright cycle written in fm1_input.h.
     * (glow is stored so the tick can implement the pulse; we just record it.) */
    uint32_t i, word = 0;
    (void)keys; (void)dim_keys;
    for (i = 0; i < NB; i++) {
        int on  = (buttons     >> i) & 1u;
        int dim = glow > 0 && ((dim_buttons >> i) & 1u);
        if (on || dim)
            word |= 1u << panel.btn[i];
    }
    smk_btnled = word;
    fm1_led_btn(word);   /* smk37/fm1_input.h: fm1_led_btn drives the button-LED column */
}

static uint8_t glow_on = 1, play_red;
void plat_glow(int on)     { glow_on = (uint8_t)(on != 0); }
void plat_play_red(int red) { play_red = (uint8_t)(red != 0); }

void plat_leds(uint32_t buttons, uint32_t keys)
{
    uint32_t play = 1u << B_PLAY, b = buttons, dim = glow_on ? ((1u << NB) - 1u) & ~buttons : 0u;
    /* On the SMK, PLAY has one LED (no separate green LED).  play_red has no effect
     * here (the SMK has no green/red PLAY LED pair) but is kept for API completeness. */
    (void)play; (void)play_red;
    plat_lights(b, keys, dim, 0, glow_on ? 2 : 0);
}

/* ---- tick / lock (same as FM-1) -------------------------------------------------- */

static volatile uint8_t in_tick;
static uint32_t lock_depth;
uint32_t lock_max, lock_from;
static uint32_t lock_t0, lock_ra;

uint32_t plat_lock(void)
{
    fm1_irq_off();
    if (lock_depth == 0) {
        lock_t0 = fm1_ticks();
        lock_ra = (uint32_t)(uintptr_t)__builtin_return_address(0);
    }
    return lock_depth++;
}

void plat_unlock(uint32_t f)
{
    (void)f;
    if (lock_depth && --lock_depth == 0) {
        uint32_t us = (fm1_ticks() - lock_t0) / FM1_TICKS_PER_US;
        if (us > lock_max) {
            lock_max = us;
            lock_from = lock_ra;
        }
        fm1_irq_on();
    }
}

void plat_sysex_send(const uint8_t *frame, uint32_t n) { ota_wire_send(frame, n); }

/* ---- data area (same API; SMK window: 0x9D000-0xBAFFF) --------------------------- */

/* On the SMK-37, PLAT_DATA_LO / HI refer to the FM-1's 0x97000/0xE0000 which are
 * irrelevant here.  We expose the SMK's FL_DATA window (0x9D000-0xBAFFF) instead. */
const uint8_t *plat_data(uint32_t off)
{
    return flash_ok && FL_IN(off, 1u, FL_DATA_LO, FL_DATA_HI) ? (const uint8_t *)FL_XIP(off) : 0;
}
int plat_data_erase(uint32_t off)
{
    return flash_ok && !(off & 0xFFFu) && FL_IN(off, 0x1000u, FL_DATA_LO, FL_DATA_HI) ? st_erase(off) : -1;
}
int plat_data_write(uint32_t off, const void *src, uint32_t n)
{
    return flash_ok && FL_IN(off, n, FL_DATA_LO, FL_DATA_HI) ? st_prog(off, src, n) : -1;
}

void plat_yield(void)
{
    /* No Bluetooth or WiFi in USB-only SMK builds; ble_poll() / wifi_poll() are no-ops
     * (radio_none.c) but are still called to satisfy the linker. */
#ifdef APP_YIELD
    app_yield();
#endif
}

/* ---- MIDI (same as FM-1; no TRS jack on the SMK) --------------------------------- */

static uint32_t midi_n[NPORTS][2], midi_t[NPORTS][2];
static void midi_counted(int port, int out, uint32_t pkt)
{
    if ((pkt >> 8 & 0xFFu) >= 0xF8u)
        return;
    midi_n[port][out]++;
    midi_t[port][out] = plat_ms() | 1u;
}

uint32_t plat_midi_count(int port, int out) { return port >= 0 && port < NPORTS ? midi_n[port][!!out] : 0; }
uint32_t plat_midi_ago(int port, int out)
{
    return port < 0 || port >= NPORTS || !midi_t[port][!!out] ? 0xFFFFFFFFu : plat_ms() - midi_t[port][!!out];
}

int plat_midi_in(uint32_t *pkt, int *port)
{
    int from;
    if (mi_r != mi_w) {
        uint32_t p = midi_in_q[mi_r % MQ];
        RING_PUBLISH();
        mi_r++;
        from = PORT_USB;   /* no TRS on SMK */
        *pkt = p & ~0xF0u;
    } else {
        return 0;
    }
    if (port)
        *port = from;
    midi_counted(from, 0, *pkt);
    return 1;
}

void plat_midi_send(uint32_t pkt, uint32_t to)
{
    uint32_t f = 0;
    if (!in_tick)
        f = plat_lock();
    if (to & TO_USB) {
        midi_out_event(pkt);
        midi_counted(PORT_USB, 1, pkt);
    }
    /* TO_BLE: no-op on USB-only SMK builds */
    if (!in_tick)
        plat_unlock(f);
}
void plat_midi_out(uint32_t pkt) { plat_midi_send(pkt, TO_USB); }

/* ---- velocity / pressure (APP_KEY_SENSE) ----------------------------------------- */
/* The SMK-37's keys give velocity (two contact rows C1/C2), not aftertouch.
 * plat_sense(on) enables the velocity scan by setting fm1_an.drive and fm1_an.probe
 * in the same way as the FM-1 — the input tick's ADC cycle uses them already.
 * FM1_ADC_ROW_PA7 and FM1_ADC_ROW_PA8 are the same channel numbers on the SMK. */
void plat_sense(int on)
{
    if (!on) {
        fm1_an.con = 0;
        return;
    }
    fm1_an.drive = 1;
    fm1_an.probe = 1;
    fm1_an.con = (uint16_t)SMK_ADC_CON(FM1_ADC_ROW_PA7);
}

uint32_t plat_analog_mask(void)
{
    /* On the SMK-37 all 37 keys are on the two analog rows (C1 = PA7-driven, C2 = PA8-driven).
     * Keys 0..26 (the FM-1 range) are the middle 27 keys; keys 0..36 are all keys.
     * Return a bitmask over NKEYS (27) keys that are on an analog row. */
    return (1u << NKEYS) - 1u;   /* all 27 FM-1-range keys are analog */
}

/* ---- storage (same wrappers as FM-1) --------------------------------------------- */

int plat_store_load(uint32_t obj, void *dst, uint32_t max) { return flash_ok ? st_load(obj, dst, max) : -1; }
int plat_store_save(uint32_t obj, const void *src, uint32_t len) { return flash_ok ? st_save(obj, src, len) : -9; }

/* ---- CPU / xruns (same as FM-1) -------------------------------------------------- */

static volatile uint32_t audio_cpu_pct, audio_xruns;
uint32_t plat_cpu_pct(void)  { return audio_cpu_pct; }
uint32_t plat_xruns(void)    { return audio_xruns; }

/* ---- SMK-37 panel default ------------------------------------------------------- */
/* The SMK-37's matrix IDs are fixed (no calibration mode; OCT- + OCT+ at power-on
 * starts calibration on the FM-1, which is a no-op here — panel_setup() exits at
 * once because the button IDs from the default match perfectly).
 *
 * Button assignment (B_* enum vs. SMK_BTN_POS[] indices 0..17 in fm1_input.h):
 *   B_FX=0:SEQ_FX(16)  B_SEL=1:GLOBE(11)  B_ENV=2:PARA(14)  B_LFO=3:FX(15)
 *   B_EDIT=4:SEQ(13)   B_GLO=5:CHORD(10)  B_HOME=6:SCALE(9) B_SAVE=7:PATCH(12)
 *   B_ARP=8:ARP(6)     B_SEQ=9:SEQ REC(17) B_PLAY=10:SEQ PLAY(16) B_REC=11:NOTE REPEAT(7)
 *   B_OCTDN=12:OCT-(4) B_OCTUP=13:OCT+(5)
 *
 * Encoder assignment (EN_* roles vs. SMK encoder IDs 0..7 in fm1_input.h):
 *   EN_SELECT=0 → K2=enc 1
 *   EN_ALGO=1   → K3=enc 2
 *   EN_PRESET=2 → K4=enc 3
 *   EN_K1=3     → K5=enc 4
 *   EN_K2=4     → K6=enc 5
 *   EN_K3=5     → K7=enc 6
 *   EN_K4=6     → K8=enc 7
 *   K1=enc 0    → virtual MASTER (not in panel; accumulated by plat_master())
 */
static void smk_panel_init(void)
{
    /* Load saved calibration (panel_init() runs before us, so panel is already set);
     * if the magic is wrong (blank flash), apply the SMK default directly. */
    static const panel_t SMK_DEFAULT = {
        PANEL_MAGIC,
        /* btn[NB]: matrix button id (the SMK_BTN_POS[] index) per B_* label */
        {15, 11, 14, 16, 13, 10, 9, 12, 6, 17, 16, 7, 4, 5},
        /* enc[NE]: encoder id (SMK enc 0 = K1/MASTER; 1..7 = K2..K8) per EN_* role */
        {1, 2, 3, 4, 5, 6, 7},
        /* dir[NE]: +1 = clockwise is positive */
        {1, 1, 1, 1, 1, 1, 1}
    };
    /* If a valid panel was loaded from flash, trust it; otherwise use the SMK default. */
    if (panel.magic != PANEL_MAGIC)
        panel = SMK_DEFAULT;
}
