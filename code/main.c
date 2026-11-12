// GK mini: a Timetop GameKing emulator "on" the Pokemon mini.
//
// Same architecture as GB mini: the emulator runs on the RP2040 of the
// PM2040 flash cart (core 0), while the Pokemon mini runs a small program
// (rpfb.h) that pulls frame and audio data out of the cart's address space
// and writes back the key state. Core 1 converts frames to the PM's packed
// format and feeds audio samples.
//
// Flash layout (offsets from the start of flash):
//   0x000000  firmware (must stay below 0x3F000)
//   0x03F000  info block written by tools/gkpatch.py
//   0x040000  GameKing BIOS (512 KB internal ROM image)
//   0x0C0000  cartridge ROM (up to the end of flash)

#include "pico/stdlib.h"
#include <string.h>

#include "rpfb.h"
#include "hardware/clocks.h"
#include "hardware/vreg.h"
#include "hardware/pio.h"
#include "hardware/dma.h"
#include "hardware/sync.h"
#include "hardware/flash.h"
#include "pico/multicore.h"

#include "oe.pio.h"
#include "pushData.pio.h"
#include "hale.pio.h"
#include "lale.pio.h"
#include "writecheck.pio.h"
#include "writecheck_addr.pio.h"

#include "gk.h"

// System clock. GB mini runs at 240 MHz on the same hardware.
#ifndef SYS_CLOCK_KHZ
#define SYS_CLOCK_KHZ 240000
#endif

// ---------------------------------------------------------------------
// Flash image layout
// ---------------------------------------------------------------------
#define GK_INFO_OFFSET 0x03F000
#define GK_BIOS_OFFSET 0x040000
#define GK_ROM_OFFSET  0x0C0000
#define GK_BIOS_SIZE   0x80000

typedef struct {
	char magic[8];        // "GKMINI\0\1"
	uint32_t biosSize;
	uint32_t romSize;
	uint32_t flags;
} gk_info_t;

#define GK_INFO ((const gk_info_t *)(XIP_BASE + GK_INFO_OFFSET))
#define GK_BIOS ((const uint8_t *)(XIP_BASE + GK_BIOS_OFFSET))
#define GK_ROM  ((const uint8_t *)(XIP_BASE + GK_ROM_OFFSET))

// RAM copies of hot ROM pages: the BIOS resident pages (vectors, services)
// and the start of the cartridge. Everything else runs from XIP flash.
#define SHADOW_BIOS_PAGES 2
#define SHADOW_ROM_PAGES  4
// Not zeroed at boot (.uninitialized_data) to keep startup short.
static uint8_t __uninitialized_ram(shadow)[SHADOW_BIOS_PAGES + SHADOW_ROM_PAGES][16384];

// Emulator core code: linked to RAM, stored in flash, copied in doGK().
extern uint8_t __gk_ram_start__[], __gk_ram_end__[], __gk_ram_source__[];

// ---------------------------------------------------------------------
// PM side interface (rpfb.h program)
// ---------------------------------------------------------------------
#define FBOFFSET 0x4000
#define RUMBLE    *((volatile uint8_t *)(rom + 0x4300))
#define AUDIO     *((volatile uint8_t *)(rom + 0x4301))
#define AUDIO_VOL *((volatile uint8_t *)(rom + 0x4302))

// PM key bits as written by the PM program.
#define PM_A     0x01
#define PM_B     0x02
#define PM_C     0x04
#define PM_UP    0x08
#define PM_DOWN  0x10
#define PM_LEFT  0x20
#define PM_RIGHT 0x40
#define PM_POWER 0x80

// FRAMEBLEND: number of PM frames blended to show the four GameKing grey
// levels (see GB mini). 2 = four levels over three frames, combined with
// a spatial pattern across each 2x2 block of PM pixels.
#define FRAMEBLEND 2
static volatile unsigned frameBlendCnt = 0;

// Triple-buffered GameKing frames (48x32 darkness levels 0..3).
#define FBDEPTH 3
static uint8_t gkFrame[FBDEPTH][GK_LCD_W * GK_LCD_H];
static volatile uint32_t readPtr = 0;
static volatile uint32_t writePtr = 1;

// PIO state for PM writes (key input).
static PIO pioWE_g;
static uint sm_we_g, sm_we_addr_g;
static volatile uint32_t pmKeys = 0;
static volatile uint32_t pmKeyReports = 0;   // key writes received so far

// Mute: hold C while booting, as in Pokemon mini games. The C press that
// selected it is not passed on to the game until C is released.
#define MUTE_WAIT_US 300000
static volatile uint32_t audioMuted = 0;
static uint32_t ignoreC = 0;

// Audio ring buffer: core 0 produces at GK_AUDIO_RATE of emulated time,
// core 1 consumes at the same rate in real time. A full buffer stalls the
// emulator, which locks emulation speed to real time.
#define AUDIO_RING 1024
#define AUDIO_HIGH 512
static uint8_t audioRing[AUDIO_RING];
static volatile uint32_t audioHead = 0, audioTail = 0;

// ---------------------------------------------------------------------
// Emulator callbacks (core 0)
// ---------------------------------------------------------------------
static uint8_t mapKeys(uint32_t k)
{
	if (ignoreC) {
		if (k & PM_C)
			k &= ~PM_C;
		else
			ignoreC = 0;
	}
	uint8_t b = 0;
	if (k & PM_A)     b |= GK_BTN_A;
	if (k & PM_B)     b |= GK_BTN_B;
	if (k & PM_C)     b |= GK_BTN_START;
	if (k & PM_POWER) b |= GK_BTN_SELECT;
	if (k & PM_UP)    b |= GK_BTN_UP;
	if (k & PM_DOWN)  b |= GK_BTN_DOWN;
	if (k & PM_LEFT)  b |= GK_BTN_LEFT;
	if (k & PM_RIGHT) b |= GK_BTN_RIGHT;
	return b;
}

void __not_in_flash_func(gk_plat_frame)(void)
{
	gk_decode_frame(gkFrame[writePtr]);
	__dmb();
	uint32_t next = writePtr + 1;
	if (next == FBDEPTH)
		next = 0;
	if (next != readPtr)
		writePtr = next;
}

void __not_in_flash_func(gk_plat_audio)(uint8_t s)
{
	while ((audioHead - audioTail) >= AUDIO_HIGH)
		tight_loop_contents();
	audioRing[audioHead & (AUDIO_RING - 1)] = s;
	__dmb();
	audioHead++;

	// Poll the PM keys every 64 samples (~4 ms). Done here rather than per
	// LCD frame so input still arrives while the game has the LCD off.
	if ((audioHead & 63) == 0)
		gk_set_buttons(mapKeys(pmKeys));
}

// ---------------------------------------------------------------------
// Core 1: audio output and frame conversion
// ---------------------------------------------------------------------
static bool __not_in_flash_func(doAudio)(repeating_timer_t *t)
{
	(void)t;
	if (audioTail != audioHead) {
		if (!audioMuted)
			AUDIO = audioRing[audioTail & (AUDIO_RING - 1)];
		__dmb();
		audioTail++;
	}
	return true;
}

static void __not_in_flash_func(showStatus)(uint8_t *fb, int ok)
{
	(void)ok;
	memset(fb, 0, 96 * 8);
}

// ---------------------------------------------------------------------
// Diagnostic screen, shown when no valid image is found in flash: eight
// lines of 24 hex digits (3x5 font, one PM byte row per line).
//   1  info block +0..11   (cached XIP)    "474B4D494E490001" = magic
//   2  info block +12..19  (BIOS size, ROM size; little endian)
//   3  info block +0..11   (uncached XIP)
//   4  BIOS +0x0000..      5  BIOS +0x7FF4.. (vectors)
//   6  ROM  +0x0000..
//   7  flash JEDEC ID (3 bytes), then the result code
//   8  PICO_FLASH_SIZE_BYTES the firmware was built for
// ---------------------------------------------------------------------
static const uint8_t hexFont[16][3] = {
	{ 0x3E, 0x22, 0x3E }, { 0x24, 0x3E, 0x20 }, { 0x3A, 0x2A, 0x2E },
	{ 0x2A, 0x2A, 0x3E }, { 0x0E, 0x08, 0x3E }, { 0x2E, 0x2A, 0x3A },
	{ 0x3E, 0x2A, 0x3A }, { 0x02, 0x02, 0x3E }, { 0x3E, 0x2A, 0x3E },
	{ 0x2E, 0x2A, 0x3E }, { 0x3E, 0x0A, 0x3E }, { 0x3E, 0x2A, 0x14 },
	{ 0x3E, 0x22, 0x22 }, { 0x3E, 0x22, 0x1C }, { 0x3E, 0x2A, 0x2A },
	{ 0x3E, 0x0A, 0x0A },
};

static void diagHex(uint8_t *fb, unsigned line, unsigned col, unsigned v)
{
	uint8_t *p = fb + line * 96 + col * 4;
	for (unsigned c = 0; c < 3; c++)
		p[c] = hexFont[v & 0xF][c];
	p[3] = 0;
}

static void diagBytes(uint8_t *fb, unsigned line, const volatile uint8_t *src, unsigned n)
{
	for (unsigned i = 0; i < n && i < 12; i++) {
		diagHex(fb, line, i * 2, src[i] >> 4);
		diagHex(fb, line, i * 2 + 1, src[i]);
	}
}

static void diagWord(uint8_t *fb, unsigned line, unsigned col, uint32_t v, unsigned digits)
{
	for (unsigned i = 0; i < digits; i++)
		diagHex(fb, line, col + i, v >> (4 * (digits - 1 - i)));
}

static void showDiag(uint8_t *fb, unsigned code)
{
	memset(fb, 0, 96 * 8);
	const volatile uint8_t *info = (const volatile uint8_t *)(XIP_BASE + GK_INFO_OFFSET);
	const volatile uint8_t *infoNc = (const volatile uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + GK_INFO_OFFSET);
	diagBytes(fb, 0, info, 12);
	diagBytes(fb, 1, info + 12, 8);
	diagBytes(fb, 2, infoNc, 12);
	diagBytes(fb, 3, GK_BIOS, 12);
	diagBytes(fb, 4, GK_BIOS + 0x7FF4, 12);
	diagBytes(fb, 5, GK_ROM, 12);

	// JEDEC ID: manufacturer, type, capacity (2^n bytes, e.g. 15 = 2 MB).
	uint8_t tx[4] = { 0x9F, 0, 0, 0 }, rx[4] = { 0 };
	uint32_t ints = save_and_disable_interrupts();
	flash_do_cmd(tx, rx, 4);
	restore_interrupts(ints);
	diagBytes(fb, 6, rx + 1, 3);
	diagWord(fb, 6, 8, code, 2);

	diagWord(fb, 7, 0, PICO_FLASH_SIZE_BYTES, 8);
}

static void __not_in_flash_func(handleBuffer)(void)
{
	uint8_t *fb = rom + FBOFFSET;

	// Audio timer on core 1's own alarm pool, so its IRQ stays off core 0.
	static repeating_timer_t t;
	alarm_pool_t *pool = alarm_pool_create_with_unused_hardware_alarm(4);
	alarm_pool_add_repeating_timer_us(pool, -(int64_t)(1000000 / GK_AUDIO_RATE),
		doAudio, 0, &t);

#if FRAMEBLEND == 2
	static const uint8_t colorPattern[4][3] = {
		{ 0, 0, 0 }, { 0, 0, 1 }, { 0, 1, 1 }, { 1, 1, 1 }
	};
#elif FRAMEBLEND == 1
	static const uint8_t colorPattern[4][2] = {
		{ 0, 0 }, { 0, 1 }, { 1, 1 }, { 1, 1 }
	};
#endif

	while (1) {
		if (frameBlendCnt == 0) {
			uint32_t nextReadPtr = readPtr + 1;
			if (nextReadPtr == FBDEPTH)
				nextReadPtr = 0;
			if (nextReadPtr != writePtr)
				readPtr = nextReadPtr;
		}
		__dmb();
		const uint8_t *src = gkFrame[readPtr];

		// 48x32 -> 96x64, each GameKing pixel becomes a 2x2 block. PM bytes
		// hold 8 vertical pixels, LSB on top.
		for (unsigned y = 0; y < 8; ++y) {
			for (unsigned x = 0; x < 96; ++x) {
				unsigned tmp = 0;
				for (unsigned i = 0; i < 8; ++i) {
					unsigned row = y * 8 + i;
					unsigned lvl = src[(row >> 1) * GK_LCD_W + (x >> 1)];
#if FRAMEBLEND > 0
					unsigned pat = (x + row + frameBlendCnt) % (FRAMEBLEND + 1);
					unsigned px = colorPattern[lvl][pat];
#else
					unsigned px = lvl >= 2;
#endif
					tmp |= px << i;
				}
				fb[x + y * 96] = tmp;
			}
		}

		if (frameBlendCnt == FRAMEBLEND)
			frameBlendCnt = 0;
		else
			++frameBlendCnt;

		// Wait for the PM to finish reading the frame: it writes the key
		// state to cart address 0 afterwards.
		while (1) {
			if (!pio_sm_is_rx_fifo_empty(pioWE_g, sm_we_g)) {
				pmKeys = pio_sm_get(pioWE_g, sm_we_g);
				uint32_t addrData = pio_sm_get(pioWE_g, sm_we_addr_g);
				if (addrData == 0x000) {
					pmKeyReports++;
					break;
				}
			}
		}
	}
}

static void __not_in_flash_func(handleNoBios)(void)
{
	while (1) {
		if (!pio_sm_is_rx_fifo_empty(pioWE_g, sm_we_g)) {
			pio_sm_get(pioWE_g, sm_we_g);
			pio_sm_get(pioWE_g, sm_we_addr_g);
		}
	}
}

// ---------------------------------------------------------------------
// Core 0: the emulator
// ---------------------------------------------------------------------
// 0 = valid; 1 = bad magic, 2 = bad BIOS size, 3 = ROM too large.
static unsigned imageCheck(void)
{
	const gk_info_t *info = GK_INFO;
	if (memcmp(info->magic, "GKMINI\0\1", 8) != 0)
		return 1;
	if (info->biosSize != GK_BIOS_SIZE)
		return 2;
	if (info->romSize > PICO_FLASH_SIZE_BYTES - GK_ROM_OFFSET)
		return 3;
	return 0;
}

static void __not_in_flash_func(doGK)(void)
{
	AUDIO = 16;
	AUDIO_VOL = 3;
	RUMBLE = 0;

	// Bring the emulator core into RAM (see memmap.ld). Only gk.c lives
	// there; nothing in it may run before this copy.
	memcpy(__gk_ram_start__, __gk_ram_source__,
		(size_t)(__gk_ram_end__ - __gk_ram_start__));
	__dsb();
	__isb();

	// Mute check: wait for the PM to report its keys a couple of times
	// (it does so after every frame it pulls), then look at C.
	uint64_t t0 = time_us_64();
	while (pmKeyReports < 3 && time_us_64() - t0 < MUTE_WAIT_US)
		tight_loop_contents();
	if (pmKeys & PM_C) {
		audioMuted = 1;
		ignoreC = 1;
		AUDIO = 0;   // constant level = silence
	}

	const uint32_t romSize = GK_INFO->romSize;
	const uint8_t *romPtr = romSize ? GK_ROM : 0;

	gk_init(GK_BIOS, romPtr, romSize);

	// Shadow the BIOS resident pages and the start of the cartridge.
	unsigned s = 0;
	for (unsigned p = 0; p < SHADOW_BIOS_PAGES; p++, s++) {
		memcpy(shadow[s], GK_BIOS + p * 16384, 16384);
		gk_add_shadow(GK_BIOS + p * 16384, shadow[s]);
	}
	for (unsigned p = 0; p < SHADOW_ROM_PAGES && (p + 1) * 16384 <= romSize; p++, s++) {
		memcpy(shadow[s], romPtr + p * 16384, 16384);
		gk_add_shadow(romPtr + p * 16384, shadow[s]);
	}
	gk_reset();

	while (1)
		gk_run(GK_CPU_HZ);
}

// Pin definitions (PM2040).
#define A0A10 0
#define D0 17
#define HALE 11
#define LALE 12
#define WE 13
#define OE 14

static void __not_in_flash_func(doPIOStuff)(void)
{
	PIO pio = pio0;
	uint sm_oe = pio_claim_unused_sm(pio, false);
	uint offset_oe = pio_add_program(pio, &oe_toggle_program);
	uint sm_pushData = pio_claim_unused_sm(pio, false);
	uint offset_pushData = pio_add_program(pio, &push_databits_program);
	uint sm_hale = pio_claim_unused_sm(pio, false);
	uint offset_hale = pio_add_program(pio, &hale_latch_program);
	uint sm_lale = pio_claim_unused_sm(pio, false);
	uint offset_lale = pio_add_program(pio, &lale_latch_program);

	int hale_dma = dma_claim_unused_channel(true);
	int lale_addr_dma = dma_claim_unused_channel(true);
	int data_dma = dma_claim_unused_channel(true);

	// High address -> LALE SM.
	dma_channel_config c = dma_channel_get_default_config(hale_dma);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, false);
	channel_config_set_dreq(&c, pio_get_dreq(pio, sm_hale, false));
	dma_channel_configure(hale_dma, &c, &pio->txf[sm_lale], &pio->rxf[sm_hale], 1, false);

	// Full address from the LALE SM -> read address of the data channel.
	c = dma_channel_get_default_config(lale_addr_dma);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_32);
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, false);
	channel_config_set_dreq(&c, pio_get_dreq(pio, sm_lale, false));
	channel_config_set_chain_to(&c, hale_dma);
	dma_channel_configure(lale_addr_dma, &c, &dma_hw->ch[data_dma].al3_read_addr_trig,
		&pio->rxf[sm_lale], 1, false);

	// Byte from the cart RAM array -> data push SM.
	c = dma_channel_get_default_config(data_dma);
	channel_config_set_transfer_data_size(&c, DMA_SIZE_8);
	channel_config_set_read_increment(&c, false);
	channel_config_set_write_increment(&c, false);
	channel_config_set_chain_to(&c, lale_addr_dma);
	channel_config_set_high_priority(&c, true);
	dma_channel_configure(data_dma, &c, &pio->txf[sm_pushData], &rom[0], 1, false);

	oe_toggle_program_init(pio, sm_oe, offset_oe, D0, OE);
	push_databits_program_init(pio, sm_pushData, offset_pushData, D0);
	hale_latch_program_init(pio, sm_hale, offset_hale, A0A10, HALE);
	lale_latch_program_init(pio, sm_lale, offset_lale, A0A10, LALE);

	pio_sm_put(pio, sm_lale, ((uint32_t)rom) >> 15);

	dma_start_channel_mask(1u << hale_dma);
	dma_start_channel_mask(1u << lale_addr_dma);

	// Write check (PM -> cart writes carry the key state).
	PIO pioWE = pio1;
	uint sm_we = pio_claim_unused_sm(pioWE, false);
	uint offset_we = pio_add_program(pioWE, &write_check_program);
	write_check_program_init(pioWE, sm_we, offset_we, D0, WE);
	uint sm_we_addr = pio_claim_unused_sm(pioWE, false);
	uint offset_we_addr = pio_add_program(pioWE, &write_check_addr_program);
	write_check_addr_program_init(pioWE, sm_we_addr, offset_we_addr, A0A10, WE);

	pioWE_g = pioWE;
	sm_we_g = sm_we;
	sm_we_addr_g = sm_we_addr;

	unsigned code = imageCheck();
	if (code) {
		showDiag(rom + FBOFFSET, code);
		multicore_launch_core1(handleNoBios);
		while (1)
			tight_loop_contents();
	}

	// Clear the frames before the PM starts showing them.
	memset(gkFrame, 0, sizeof gkFrame);
	showStatus(rom + FBOFFSET, 1);

	multicore_launch_core1(handleBuffer);
	doGK();
}

int main(void)
{
	sleep_ms(2);
	vreg_set_voltage(VREG_VOLTAGE_1_30);
	sleep_ms(2);
	set_sys_clock_khz(SYS_CLOCK_KHZ, true);

	doPIOStuff();
	return 0;
}
