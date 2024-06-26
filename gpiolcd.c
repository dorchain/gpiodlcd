/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) Andriy Gapon <avg@FreeBSD.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions, and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE FOR
 * ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

/*
 * Control HD44780 LCD module hung off 8-pin GPIO.
 *
 * $FreeBSD$
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <stdbool.h>
#include <fcntl.h>
#include <unistd.h>
#include <err.h>
#include <errno.h>
#include <assert.h>
#include <sysexits.h>
#include <stdint.h>
#include <gpiod.h>


/******************************************************************************
 * Driver for the Hitachi HD44780.  This is probably *the* most common driver
 * to be found on 1, 2 and 4-line alphanumeric LCDs.
 *
 * This driver assumes the following connections by default:
 *
 * GPIO         	LCD Module
 * --------------------------------
 * P0            	RS
 * P1            	R/W
 * P2        		E
 * P3        		Backlight control circuit
 * P4-P7      		Data, DB4-DB7
 *
 * FIXME: this driver never reads from the device and never checks busy flag.
 * Instead it uses fixed delays to wait for instruction completions.
 */
#define debug(lev, fmt, args...)	if (debuglevel >= lev) fprintf(stderr, fmt "\n" , ## args);

static void	usage(void);
static char	*progname;

#define	DEFAULT_DEVICE	"/dev/gpiochip1"

enum command {
	CMD_RESET,
	CMD_BKSP,
	CMD_CLR,
	CMD_NL,
	CMD_CR,
	CMD_HOME,
	CMD_TAB,
	CMD_FLASH,
};

enum reg_type {
	HD_COMMAND,
	HD_DATA
};

enum hd_pin_id {
	HD_PIN_DAT0 = 0,
	HD_PIN_DAT1,
	HD_PIN_DAT2,
	HD_PIN_DAT3,
	HD_PIN_DAT4,
	HD_PIN_DAT5,
	HD_PIN_DAT6,
	HD_PIN_DAT7,
	HD_PIN_RS,
	HD_PIN_RW,
	HD_PIN_E,
	HD_PIN_BL,
	HD_PIN_COUNT,
};

typedef struct {
	struct gpiod_chip *chip;
	struct gpiod_line *lines[HD_PIN_COUNT];
} gpio_pins;

static struct hd44780_state {
	gpio_pins	hd_gpio;
	int	hd_ifwidth;
	int	hd_lines;
	int	hd_cols;
	int	hd_blink;
	int 	hd_cursor;
	int	hd_font;
	int	hd_bl_on;
	int	hd_col;
	int	hd_row;
	int	pins[HD_PIN_COUNT];
} hd44780_state;

/* Driver functions */
static void	hd44780_prepare(char *devname, struct hd44780_state * state);
static void	hd44780_finish(void);
static void	hd44780_command(struct hd44780_state *state, enum command cmd);
static void	hd44780_putc(struct hd44780_state *state, int c);

static void	do_char(struct hd44780_state *state, char ch);

static int	debuglevel = 0;

int
main(int argc, char *argv[])
{
	struct hd44780_state *state = &hd44780_state;
	extern char	*optarg;
	extern int	optind;
	char		*cp, *endp;
	char		*devname = DEFAULT_DEVICE;
	int		ch, i;

	if ((progname = strrchr(argv[0], '/'))) {
		progname++;
	} else {
		progname = argv[0];
	}

	state->hd_bl_on = 1;
	state->hd_lines = 2;
	state->hd_cols = 16;
	state->hd_ifwidth = 4;
	for (i = 0; i < HD_PIN_COUNT; i++)
		state->pins[i] = -1;
	state->pins[HD_PIN_RS] = 0;
	state->pins[HD_PIN_RW] = 1;
	state->pins[HD_PIN_E] = 2;
	state->pins[HD_PIN_BL] = 3;
	state->pins[HD_PIN_DAT0] = 4;

	while ((ch = getopt(argc, argv, "BCdD:E:f:Fh:I:L:OR:w:W:")) != -1) {
		switch(ch) {
		case 'd':
			debuglevel++;
			break;
		case 'f':
			devname = optarg;
			break;
		case 'h':
			state->hd_lines = strtol(optarg, &endp, 10);
			if (*endp != '\0') {
				fprintf(stderr, "invalid number of lines %s\n", optarg);
				usage();
			}
			break;
		case 'w':
			state->hd_cols = strtol(optarg, &endp, 10);
			if (*endp != '\0') {
				fprintf(stderr, "invalid number of columsn %s\n", optarg);
				usage();
			}
			break;
		case 'B':
			state->hd_blink = 1;
			break;
		case 'C':
			state->hd_cursor = 1;
			break;
		case 'F':
			state->hd_font = 1;
			break;
		case 'O':
			state->hd_bl_on = 0;
			break;
		case 'I':
			state->hd_ifwidth = strtol(optarg, &endp, 10);
			if (*endp != '\0') {
				fprintf(stderr, "invalid interface width %s\n", optarg);
				usage();
			}
			break;
		case 'R':
			state->pins[HD_PIN_RS] = strtol(optarg, &endp, 10);
			if (*endp != '\0') {
				fprintf(stderr, "invalid pin specification %s\n", optarg);
				usage();
			}
			break;
		case 'W':
			state->pins[HD_PIN_RW] = strtol(optarg, &endp, 10);
			if (*endp != '\0') {
				fprintf(stderr, "invalid pin specification %s\n", optarg);
				usage();
			}
			break;
		case 'E':
			state->pins[HD_PIN_E] = strtol(optarg, &endp, 10);
			if (*endp != '\0') {
				fprintf(stderr, "invalid pin specification %s\n", optarg);
				usage();
			}
			break;
		case 'L':
			state->pins[HD_PIN_BL] = strtol(optarg, &endp, 10);
			if (*endp != '\0') {
				fprintf(stderr, "invalid pin specification %s\n", optarg);
				usage();
			}
			break;
		case 'D':
			state->pins[HD_PIN_DAT0] = strtol(optarg, &endp, 10);
			if (*endp != '\0') {
				fprintf(stderr, "invalid pin specification %s\n", optarg);
				usage();
			}
			break;
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;

	if (state->hd_ifwidth != 4) {
		fprintf(stderr, "Unsupported data interface width %d\n", state->hd_ifwidth);
		usage();
	}
	for (i = 1; i < state->hd_ifwidth; i++) {
		state->pins[HD_PIN_DAT0 + i] = state->pins[HD_PIN_DAT0] + i;
	}

	if (state->hd_lines != 1 && state->hd_lines != 2 && state->hd_lines != 4) {
		fprintf(stderr, "Unsupported number of lines %d\n", state->hd_lines);
		usage();
	}
	if (state->hd_cols <= 0 || state->hd_lines * state->hd_cols > 80) {
		fprintf(stderr, "Unsupported number of columns %d\n", state->hd_cols);
		usage();
	}

	if (state->hd_bl_on && state->pins[HD_PIN_BL] == -1) {
		fprintf(stderr, "Backlight pin is not specified\n");
		usage();
	}

	hd44780_prepare(devname, state);
	atexit(hd44780_finish);

	if (argc > 0) {
		debug(2, "reading input from %d argument%s", argc, (argc > 1) ? "s" : "");
		for (i = 0; i < argc; i++)
			for (cp = argv[i]; *cp; cp++)
				do_char(state, *cp);
	} else {
		debug(2, "reading input from stdin");
		setvbuf(stdin, NULL, _IONBF, 0);
		while ((ch = fgetc(stdin)) != EOF)
			do_char(state, (char)ch);
	}
	exit(EX_OK);
}

static void
usage(void)
{

	fprintf(stderr, "usage: %s [-f device] [-d] [-B] [-C] [-F] [-O] "
	    "[-h <n>] [-w <n>] [-R <n>]\n"
	    "\t[-W <n>] [-E <n>] [-L <n>] [-D <n>] [-I <n>] [args...]\n",
	    progname);
	fprintf(stderr, "Supported hardware: Hitachi HD44780 and compatibles\n");
	fprintf(stderr, "   -d      Increase debugging\n");
	fprintf(stderr, "   -f      Specify device, default is '%s'\n", DEFAULT_DEVICE);
	fprintf(stderr, "   -h <n>  n-line display (default 2)\n"
			"   -w <n>  n-column display (default 16)\n"
			"   -B      Cursor blink enable\n"
			"   -C      Cursor enable\n"
			"   -F      Large font select\n"
			"   -R <n>  R/S pin number (default 0)\n"
			"   -W <n>  R/W pin number (default 1)\n"
			"   -E <n>  E pin number (default 2)\n"
			"   -L <n>  Backlight pin number (default 3)\n"
			"   -O      Turn backlight off (default on)\n"
			"   -D <n>  First data pin number (default 4)\n"
			"   -I <n>  Data interface width (only 4 is supported)\n");
	fprintf(stderr, "  args     Message strings.\n");
	fprintf(stderr, "           Some ASCII control characters and escapes sequences are supported:\n");
	fprintf(stderr, "                  <BS> (\\b)	Backspace\n");
	fprintf(stderr, "                  <LF> (\\f)	Clear display, home cursor\n");
	fprintf(stderr, "                  <NL> (\\n)	Newline\n");
	fprintf(stderr, "                  <CR> (\\r)	Carriage return\n");
	fprintf(stderr, "                  <HT> (\\t)	Tab\n");
	fprintf(stderr, "                  <BEL> (\\a)	Flash screen\n");
	fprintf(stderr, "                  <ESC>R	Reset display\n");
	fprintf(stderr, "                  <ESC>H	Home cursor\n");
	fprintf(stderr, "           If args not supplied, strings are read from standard input\n");
	exit(EX_USAGE);
}

static void
do_char(struct hd44780_state *state, char ch)
{
	static int	esc = 0;

	if (esc) {
		switch(ch) {
		case 'R':
			hd44780_command(state, CMD_RESET);
			break;
		case 'H':
			hd44780_command(state, CMD_HOME);
			break;
		}
		esc = 0;
		return;
	}

	if (ch == 27) {
		esc = 1;
		return;
	}

	switch(ch) {
	case '\n':
		hd44780_command(state, CMD_NL);
		break;
	case '\r':
		hd44780_command(state, CMD_CR);
		break;
	case '\t':
		hd44780_command(state, CMD_TAB);
		break;
	case '\a':
		hd44780_command(state, CMD_FLASH);
		break;
	case '\b':
		hd44780_command(state, CMD_BKSP);
		break;
	case '\f':
		hd44780_command(state, CMD_CLR);
		break;
	default:
		if (isascii(ch) && isprint(ch))
			hd44780_putc(state, ch);
		break;
	}
}

static void
hd44780_set_pin(struct hd44780_state *state, enum hd_pin_id pin, bool on)
{
	int err;

	assert(state->pins[pin] != -1);
	err = gpiod_line_set_value(state->hd_gpio.lines[state->pins[pin]], on);
	if (err != 0)
		debug(1, "%s: error %d", __func__, errno);
}

static void
hd44780_strobe(struct hd44780_state *state)
{
	usleep(20);
	hd44780_set_pin(state, HD_PIN_E, true);
	usleep(40);
	hd44780_set_pin(state, HD_PIN_E, false);
	usleep(20);
}

/* FIXME: hardcoded to 4-bit data interface. */
static void
hd44780_output(struct hd44780_state *state, enum reg_type type, uint8_t data)
{
	int i;

	debug(3, "%s -> 0x%02x", (type == HD_COMMAND) ? "cmd " : "data", data);

	hd44780_set_pin(state, HD_PIN_RW, false);

	if (type == HD_COMMAND)
		hd44780_set_pin(state, HD_PIN_RS, false);
	else
		hd44780_set_pin(state, HD_PIN_RS, true);

	/* Set upper nibble of data. */
	for (i = 0; i < 4; i++) {
		hd44780_set_pin(state, HD_PIN_DAT0 + i,
		    ((1 << (i + 4)) & data) != 0);
	}

	hd44780_strobe(state);

	/* Set lower nibble of data. */
	for (i = 0; i < 4; i++) {
		hd44780_set_pin(state, HD_PIN_DAT0 + i,
		    ((1 << i) & data) != 0);
	}

	hd44780_strobe(state);
}

static void
hd44780_output4(struct hd44780_state *state, enum reg_type type, uint8_t data)
{
	int i;

	debug(3, "%s -> 0x%02x", (type == HD_COMMAND) ? "cmd " : "data", data);

	hd44780_set_pin(state, HD_PIN_RW, false);

	if (type == HD_COMMAND)
		hd44780_set_pin(state, HD_PIN_RS, false);
	else
		hd44780_set_pin(state, HD_PIN_RS, true);

	/* Set upper nibble of data. */
	for (i = 0; i < 4; i++) {
		hd44780_set_pin(state, HD_PIN_DAT0 + i,
		    ((1 << (i + 4)) & data) != 0);
	}

	hd44780_strobe(state);
}

static void
hd44780_prepare(char *devname, struct hd44780_state *state)
{
	int error, i;

	if ((state->hd_gpio.chip = gpiod_chip_open_lookup(devname)) == NULL)
		err(EX_OSFILE, "can't open '%s'", devname);

	/* Get all the lines */
	for (i = 0; i < HD_PIN_COUNT; i++) {
		if (state->pins[i] == -1)
			continue;
		if ((state->hd_gpio.lines[state->pins[i]] = gpiod_chip_get_line(state->hd_gpio.chip, state->pins[i])) == NULL)
			err(EX_OSFILE, "can't open line '%d'", state->pins[i]);
	}

	for (i = 0; i < HD_PIN_COUNT; i++) {
		if (state->pins[i] == -1)
			continue;
		error = gpiod_line_request_output(state->hd_gpio.lines[state->pins[i]], progname, 0);
		if (error != 0)
			err(1, "configuring pin %d as output failed",
			    state->pins[i]);
	}

	for (i = 0; i < HD_PIN_COUNT; i++) {
		if (state->pins[i] == -1)
			continue;

		hd44780_set_pin(state, i, false);
	}

	usleep(20000);
	hd44780_command(state, CMD_RESET);

	if (state->hd_bl_on)
		hd44780_set_pin(state, HD_PIN_BL, true);
}

static void
hd44780_finish(void)
{
	gpiod_chip_close(hd44780_state.hd_gpio.chip);
}

#define	HD_CMD_CLEAR			0x01

#define	HD_CMD_HOME			0x02

#define	HD_CMD_ENTRYMODE		0x04
#define		HD_ENTRY_INCR		0x02
#define		HD_DISP_SHIFT		0x01

#define	HD_CMD_DISPCTRL			0x08
#define		HD_DISP_ON		0x04
#define		HD_CURSOR_ON		0x02
#define		HD_BLINK_ON		0x01

#define	HD_CMD_MOVE			0x10
#define		HD_MOVE_DISP		0x08
#define		HD_MOVE_CURSOR		0x00
#define		HD_MOVE_RIGHT		0x04
#define		HD_MOVE_LEFT		0x00

#define	HD_CMD_SETMODE			0x20
#define		HD_MODE_8BIT_IF		0x10
#define		HD_MODE_2LINES		0x08
#define		HD_MODE_LARGE_FONT	0x04

#define	HD_CMD_SET_CGADDR		0x40

#define	HD_CMD_SET_ADDR			0x80

#define	HD_LINE_DRAM_SIZE		40
#define	HD_LINE1_DRAM_OFFSET		0x40

static uint8_t
hd44780_calc_addr(struct hd44780_state *state)
{
	uint8_t addr;

	addr = state->hd_col;
	if (state->hd_row == 1 || state->hd_row == 3)
		addr += HD_LINE1_DRAM_OFFSET;
	if (state->hd_row == 2 || state->hd_row == 3)
		addr += state->hd_cols;
	return (addr);
}

static void
hd44780_command(struct hd44780_state *state, enum command cmd)
{
	int i;
	uint8_t	val;

	switch (cmd) {
	case CMD_RESET:	/* full manual reset and reconfigure as per datasheet */
		debug(1, "hd44780: reset to %d-bit interface, %d lines, "
		    "%s font,%s%s cursor",
		    state->hd_ifwidth, state->hd_lines,
		    state->hd_font ? "5x10" : "5x8",
		    state->hd_cursor ? "" : " no",
		    state->hd_blink ? " blinking" : "");

		/*
		 * This needs to be repeated three times to guarantee a state
		 * where the desired mode can be configured.
		 */
		val = HD_CMD_SETMODE;
		val |= HD_MODE_8BIT_IF;
		hd44780_output4(state, HD_COMMAND, val);
		usleep(10000);
		hd44780_output4(state, HD_COMMAND, val);
		usleep(1000);
		hd44780_output4(state, HD_COMMAND, val);
		usleep(1000);

		val = HD_CMD_SETMODE;
		if (state->hd_ifwidth == 8)
			val |= HD_MODE_8BIT_IF;
		if (state->hd_lines != 1)
			val |= HD_MODE_2LINES;
		if (state->hd_font)
			val |= HD_MODE_LARGE_FONT;

		/*
		 * At this point the display is in 8-bit mode, so execute
		 * a command to enter the 4-bit mode if needed.
		 */
		if (state->hd_ifwidth == 4)
			hd44780_output4(state, HD_COMMAND, val);

		hd44780_output(state, HD_COMMAND, val);
		usleep(10000);

		val = HD_CMD_DISPCTRL;
		hd44780_output(state, HD_COMMAND, val);
		usleep(1000);
		val |= HD_DISP_ON;
		if (state->hd_cursor)
			val |= HD_CURSOR_ON;
		if (state->hd_blink)
			val |= HD_BLINK_ON;
		hd44780_output(state, HD_COMMAND, val);
		usleep(1000);

		val = HD_CMD_ENTRYMODE;
		val |= HD_ENTRY_INCR;
		hd44780_output(state, HD_COMMAND, val);
		usleep(1000);
		/* FALLTHROUGH */

	case CMD_CLR:
		hd44780_output(state, HD_COMMAND, HD_CMD_CLEAR);
		usleep(2000);
		state->hd_col = 0;
		state->hd_row = 0;
		break;

	case CMD_BKSP:
		if (state->hd_col > 0) {
			/*
			 * Move the cursor back, overwrite with a space
			 * and move back again.
			 */
			hd44780_output(state, HD_COMMAND, HD_CMD_MOVE |
			    HD_MOVE_CURSOR | HD_MOVE_LEFT);
			state->hd_col--;	/* NB: putc increments hd_col */
			hd44780_putc(state, ' ');
			hd44780_output(state, HD_COMMAND, HD_CMD_MOVE |
			    HD_MOVE_CURSOR | HD_MOVE_LEFT);
			state->hd_col--;
		} else {
			/* XXX */
			hd44780_command(state, CMD_FLASH);
		}
		usleep(1000);
		break;

	case CMD_NL:
		/*
		 * Fill the remainder of the current line with spaces.  If there
		 * is no space for another line, then the cursor is held at the
		 * end position.  This way no characters will be output until
		 * the screen is cleared or the cursor is moved otherwise.
		 */
		while (state->hd_col < state->hd_cols)	/* NB: putc increments hd_col */
			hd44780_putc(state, ' ');
		if (state->hd_row < state->hd_lines - 1) {
			state->hd_row++;
			state->hd_col = 0;
			val = hd44780_calc_addr(state);
			hd44780_output(state, HD_COMMAND, HD_CMD_SET_ADDR | val);
			usleep(1000);
		}
		break;

	case CMD_CR:
		state->hd_col = 0;
		val = hd44780_calc_addr(state);
		hd44780_output(state, HD_COMMAND, HD_CMD_SET_ADDR | val);
		usleep(1000);
		break;

	case CMD_HOME:
		/* just move to address 0, also resets display shift */
		hd44780_output(state, HD_COMMAND, HD_CMD_HOME);
		usleep(2000);
		state->hd_col = 0;
		state->hd_row = 0;
		break;

	case CMD_TAB:
		i = 8 - state->hd_col % 8;
		if (state->hd_col + i > state->hd_cols)
			i = state->hd_cols - state->hd_col;
		while (i-- > 0)
			hd44780_putc(state, ' ');
		break;

	case CMD_FLASH:
		/* Turn the display off and on a couple of times. */
		for (i = 0; i < 2; i++) {
			val = HD_CMD_DISPCTRL;
			hd44780_output(state, HD_COMMAND, val);
			usleep(200000);
			val |= HD_DISP_ON;
			if (state->hd_cursor)
				val |= HD_CURSOR_ON;
			if (state->hd_blink)
				val |= HD_BLINK_ON;
			hd44780_output(state, HD_COMMAND, val);
			if (i < 3)
				usleep(200000);
			else
				usleep(1000);
		}
		break;

	default:
		if (isprint(cmd)) {
			warnx("unknown command %c", cmd);
		} else {
			warnx("unknown command 0x%x", cmd);
		}
	}
}

static void
hd44780_putc(struct hd44780_state *state, int c)
{
	/*
	 * Won't print beyond the screen even if there is off-screen DDRAM
	 * available.
	 * Screen shift commands are not supported yet.
	 */
	if (state->hd_col == state->hd_cols)
		return;
	hd44780_output(state, HD_DATA, c);
	usleep(40);
	state->hd_col++;
}
