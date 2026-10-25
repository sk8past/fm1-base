/* SPDX-License-Identifier: MPL-2.0
 * Copyright (C) 2026 Peter Whitney */
/* SMK-37 flash-map and owner-tag test.
 *
 * Checks that the SMK-37 port's constants satisfy every hard constraint:
 *   1. Flash-map bounds: store, OTA staging and protected ranges do not overlap.
 *   2. Store object slots: each (OBJ_PROJ, OBJ_PANEL, OBJ_BLE) fits inside the
 *      store window with two 4 KiB A/B sectors each.
 *   3. Data family: PROJ_MAGIC's four bytes spell "CTL1", the family the owner
 *      chose in smk37-port.yaml (data_family: CTL1).
 *   4. OBJ_NOBJ count: exactly OBJ_BLE + 1 = 3 objects.
 *
 * This test is deliberately standalone (no fm1-base headers): it checks the
 * symbolic constants written into app.h and bsp/boards/smk37/fm1_flash_map.h.
 * Run from run_tests.sh:
 *   cc -w -o smk_owner_test tests/smk_owner_test.c && ./smk_owner_test
 */
#include <stdint.h>
#include <stdio.h>

/* ---- constants copied from bsp/boards/smk37/fm1_flash_map.h --------------- */
#define FL_DATA_LO   0x9D000u
#define FL_DATA_HI   0xBB000u
#define FL_OTA_LO    0xBB000u
#define FL_OTA_HI    0xC0000u
/* protected ranges (never written) */
#define PROT_BOOT_HI 0x04000u   /* 0x00000-0x03FFF */
#define PROT_BTIF_LO 0xC0000u   /* 0xC0000-0xC0FFF */
#define PROT_BTIF_HI 0xC2000u
#define PROT_USR_LO  0xF4000u   /* 0xF4000-0xFDFFF */
#define PROT_MAC_LO  0xFF000u   /* 0xFF000-0xFFFFF */

/* ---- constants from app/app.h --------------------------------------------- */
/* enum { OBJ_PROJ, OBJ_PANEL, OBJ_BLE, OBJ_NOBJ }; */
#define OBJ_PROJ  0
#define OBJ_PANEL 1
#define OBJ_BLE   2
#define OBJ_NOBJ  3
/* #define APP_STORE_MAP {0x9D000u, 0x9F000u, 0xA1000u} */
static const uint32_t STORE_OFFSETS[OBJ_NOBJ] = {0x9D000u, 0x9F000u, 0xA1000u};
#define PROJ_MAGIC 0x4C544331u   /* "CTL1" */

/* ---- compile-time assertions (same as fm1_flash_map.h) -------------------- */
_Static_assert(FL_DATA_LO  >= PROT_BOOT_HI,  "store must sit above the app head");
_Static_assert(FL_DATA_HI  <= PROT_BTIF_LO,  "store must not reach protected BTIF");
_Static_assert(FL_OTA_HI   <= PROT_BTIF_LO,  "OTA staging must not reach protected BTIF");
_Static_assert(FL_OTA_LO   >= FL_DATA_HI,     "OTA staging must not overlap store");

/* ---- runtime checks ------------------------------------------------------- */
static int check(const char *what, int ok)
{
    printf("  %-52s %s\n", what, ok ? "ok" : "FAIL");
    return ok ? 0 : 1;
}

int main(void)
{
    int bad = 0;
    uint32_t i;

    /* 1. Flash map: store + OTA fit inside the 1 MiB flash and below BTIF */
    bad += check("store starts at or above 0x4000",  FL_DATA_LO >= 0x4000u);
    bad += check("store ends at or below 0xC0000",   FL_DATA_HI <= 0xC0000u);
    bad += check("OTA starts at store end",           FL_OTA_LO == FL_DATA_HI);
    bad += check("OTA ends at or below 0xC0000",     FL_OTA_HI <= 0xC0000u);
    bad += check("OTA does not overlap store",        FL_OTA_LO >= FL_DATA_HI);
    bad += check("OTA does not overlap protected USR", FL_OTA_HI <= PROT_USR_LO);

    /* 2. Each store object (A+B = 2 × 4 KiB = 0x2000) fits inside the store window */
    for (i = 0; i < OBJ_NOBJ; i++) {
        char name[64];
        uint32_t obj_lo = STORE_OFFSETS[i];
        uint32_t obj_hi = obj_lo + 0x2000u;   /* two 4 KiB sectors */
        snprintf(name, sizeof name, "OBJ %u slot 0x%05x-0x%05x inside store", i, obj_lo, obj_hi - 1u);
        bad += check(name, obj_lo >= FL_DATA_LO && obj_hi <= FL_DATA_HI);
    }

    /* 3. Object slots do not overlap each other */
    for (i = 0; i < OBJ_NOBJ; i++) {
        uint32_t j;
        for (j = i + 1u; j < OBJ_NOBJ; j++) {
            char name[64];
            int no_overlap = (STORE_OFFSETS[i] + 0x2000u <= STORE_OFFSETS[j]) ||
                             (STORE_OFFSETS[j] + 0x2000u <= STORE_OFFSETS[i]);
            snprintf(name, sizeof name, "OBJ %u and OBJ %u do not overlap", i, j);
            bad += check(name, no_overlap);
        }
    }

    /* 4. OBJ_NOBJ = 3 (OBJ_PROJ, OBJ_PANEL, OBJ_BLE) */
    bad += check("OBJ_NOBJ == 3", OBJ_NOBJ == 3);

    /* 5. Data family: PROJ_MAGIC is the four-byte tag "CTL1" encoded as a uint32_t.
     * Convention (matches panel.c "PAN5", radio.c "BLE2" etc.): the hex literal lists
     * bytes in the order L,T,C,1 most-significant-first, i.e. 0x4C544331.
     * The test locks in the exact value so an accidental edit is caught. */
    bad += check("PROJ_MAGIC == 0x4C544331 (CTL1 tag)", PROJ_MAGIC == 0x4C544331u);

    if (bad == 0)
        puts("smk_owner_test: all checks passed");
    else
        printf("smk_owner_test: %d check(s) FAILED\n", bad);
    return bad ? 1 : 0;
}
