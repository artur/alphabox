/* Alphabox Alpha Emulator
 * Copyright (C) 2026 Artur Goulão
 * Website: https://github.com/artur/alphabox
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301,
 * USA.
 */

/**
 * \file
 * The AudioPCI register file. The names and bit meanings come from the
 * Linux `snd-ens1370`/`snd-ens1371` driver, which is where the family is
 * documented in public; the two parts share this window, so what is common
 * is spelled ES_REG_/ES_ and what only one of them has says which.
 **/
#if !defined(__ES137X_REGS_H__)
#define __ES137X_REGS_H__

/* The register window both parts have in common. The DMA engine, the
   interrupt bits and the serial (sample format) control are the same
   silicon on the two chips; only how a channel's sample rate is chosen,
   and how the mixer is reached, moved between them. */
#define ES_REG_CONTROL 0x00
#define ES_REG_STATUS 0x04
#define ES_REG_UART_DATA 0x08
#define ES_REG_UART_STATUS 0x09
#define ES_REG_UART_CONTROL 0x09
#define ES_REG_UART_TEST 0x0a
#define ES_REG_MEMPAGE 0x0c
#define ES_REG_SERIAL_CONTROL 0x20
#define ES_REG_DAC1_SCOUNT 0x24
#define ES_REG_DAC2_SCOUNT 0x28
#define ES_REG_ADC_SCOUNT 0x2c

/* The ES1370's own mixer (an AK4531) is written through here one register
   at a time; the ES1371 dropped it and put the AC'97 serial interface at
   0x14 instead, with the sample rate converter at 0x10. */
#define ES1370_REG_CODEC 0x10
#define ES1371_REG_SMPRATE 0x10
#define ES1371_REG_CODEC 0x14
#define ES1371_REG_LEGACY 0x18

/* The frame address and count pair of each channel lives in a 16-byte
   window at 0x30, which MEMPAGE selects between: page 0xc holds the two
   playback channels, page 0xd the capture channel (and a fourth, phantom,
   pair that no channel is behind). */
#define ES_REG_DAC1_FRAMEADR 0xc30
#define ES_REG_DAC1_FRAMECNT 0xc34
#define ES_REG_DAC2_FRAMEADR 0xc38
#define ES_REG_DAC2_FRAMECNT 0xc3c
#define ES_REG_ADC_FRAMEADR 0xd30
#define ES_REG_ADC_FRAMECNT 0xd34
#define ES_REG_PHANTOM_FRAMEADR 0xd38
#define ES_REG_PHANTOM_FRAMECNT 0xd3c

static const unsigned dac1_samplerate[] = {5512, 11025, 22050, 44100};

#define DAC2_SRTODIV(x) (((1411200 + (x) / 2) / (x)) - 2)
#define DAC2_DIVTOSR(x) (1411200 / ((x) + 2))

#define CTRL_ADC_STOP 0x80000000 /* 1 = ADC stopped */
#define CTRL_XCTL1 0x40000000    /* electret mic bias */
#define CTRL_OPEN 0x20000000     /* no function, can be read and written */
#define CTRL_PCLKDIV 0x1fff0000  /* ADC/DAC2 clock divider */
#define CTRL_SH_PCLKDIV 16
#define CTRL_MSFMTSEL 0x00008000 /* MPEG serial data fmt: 0 = Sony, 1 = I2S */
#define CTRL_M_SBB 0x00004000    /* DAC2 clock: 0 = PCLKDIV, 1 = MPEG */
#define CTRL_WTSRSEL                                                           \
  0x00003000 /* DAC1 clock freq: 0=5512, 1=11025, 2=22050, 3=44100 */
#define CTRL_SH_WTSRSEL 12
#define CTRL_DAC_SYNC 0x00000800  /* 1 = DAC2 runs off DAC1 clock */
#define CTRL_CCB_INTRM 0x00000400 /* 1 = CCB "voice" ints enabled */
#define CTRL_M_CB 0x00000200      /* recording source: 0 = ADC, 1 = MPEG */
#define CTRL_XCTL0 0x00000100     /* 0 = Line in, 1 = Line out */
#define CTRL_BREQ 0x00000080      /* 1 = test mode (internal mem test) */
#define CTRL_DAC1_EN 0x00000040   /* enable DAC1 */
#define CTRL_DAC2_EN 0x00000020   /* enable DAC2 */
#define CTRL_ADC_EN 0x00000010    /* enable ADC */
#define CTRL_UART_EN 0x00000008   /* enable MIDI uart */
#define CTRL_JYSTK_EN                                                          \
  0x00000004 /* enable Joystick port (presumably at address 0x200) */
#define CTRL_CDC_EN 0x00000002   /* enable serial (CODEC) interface */
#define CTRL_SERR_DIS 0x00000001 /* 1 = disable PCI SERR signal */

/* The ES1371 reuses the top of CONTROL for its own purposes; only the two
   the driver actually drives matter here. */
#define CTRL_1371_SYNC_RES 0x00004000 /* warm AC'97 reset */
#define CTRL_1371_ADC_STOP 0x00002000 /* 1 = ADC transfers stopped */

#define STAT_INTR 0x80000000          /* wired or of all interrupt bits */
#define STAT_1371_AC97_RST 0x20000000 /* cold AC'97 reset, held while 1 */
#define STAT_CSTAT 0x00000400 /* 1 = codec busy or codec write in progress */
#define STAT_CBUSY 0x00000200 /* 1 = codec busy */
#define STAT_CWRIP 0x00000100 /* 1 = codec write in progress */
#define STAT_VC 0x00000060 /* CCB int source, 0=DAC1, 1=DAC2, 2=ADC, 3=undef   \
                            */
#define STAT_SH_VC 5
#define STAT_MCCB 0x00000010 /* CCB int pending */
#define STAT_UART 0x00000008 /* UART int pending */
#define STAT_DAC1 0x00000004 /* DAC1 int pending */
#define STAT_DAC2 0x00000002 /* DAC2 int pending */
#define STAT_ADC 0x00000001  /* ADC int pending */

#define USTAT_RXINT 0x80 /* UART rx int pending */
#define USTAT_TXINT 0x04 /* UART tx int pending */
#define USTAT_TXRDY 0x02 /* UART tx ready */
#define USTAT_RXRDY 0x01 /* UART rx ready */

#define UCTRL_RXINTEN 0x80   /* 1 = enable RX ints */
#define UCTRL_TXINTEN 0x60   /* TX int enable field mask */
#define UCTRL_ENA_TXINT 0x20 /* enable TX int */
#define UCTRL_CNTRL 0x03     /* control field */
#define UCTRL_CNTRL_SWR 0x03 /* software reset command */

#define SCTRL_P2ENDINC 0x00380000 /*  */
#define SCTRL_SH_P2ENDINC 19
#define SCTRL_P2STINC 0x00070000 /*  */
#define SCTRL_SH_P2STINC 16
#define SCTRL_R1LOOPSEL 0x00008000 /* 0 = loop mode */
#define SCTRL_P2LOOPSEL 0x00004000 /* 0 = loop mode */
#define SCTRL_P1LOOPSEL 0x00002000 /* 0 = loop mode */
#define SCTRL_P2PAUSE 0x00001000   /* 1 = pause mode */
#define SCTRL_P1PAUSE 0x00000800   /* 1 = pause mode */
#define SCTRL_R1INTEN 0x00000400   /* enable interrupt */
#define SCTRL_P2INTEN 0x00000200   /* enable interrupt */
#define SCTRL_P1INTEN 0x00000100   /* enable interrupt */
#define SCTRL_P1SCTRLD 0x00000080  /* reload sample count register for DAC1 */
#define SCTRL_P2DACSEN                                                         \
  0x00000040 /* 1 = DAC2 play back last sample when disabled */
#define SCTRL_R1SEB 0x00000020 /* 1 = 16bit */
#define SCTRL_R1SMB 0x00000010 /* 1 = stereo */
#define SCTRL_R1FMT 0x00000030 /* format mask */
#define SCTRL_SH_R1FMT 4
#define SCTRL_P2SEB 0x00000008 /* 1 = 16bit */
#define SCTRL_P2SMB 0x00000004 /* 1 = stereo */
#define SCTRL_P2FMT 0x0000000c /* format mask */
#define SCTRL_SH_P2FMT 2
#define SCTRL_P1SEB 0x00000002 /* 1 = 16bit */
#define SCTRL_P1SMB 0x00000001 /* 1 = stereo */
#define SCTRL_P1FMT 0x00000003 /* format mask */
#define SCTRL_SH_P1FMT 0

/* ES1371_REG_CODEC, the AC'97 serial interface. A driver writes a codec
   register by putting its number and value here, and reads one by setting
   PIRD and then polling until RDY comes up with the value in the low half.
   WIP says a write is still on the AC-link. */
#define CODEC_RDY 0x80000000
#define CODEC_WIP 0x40000000
#define CODEC_PIRD 0x00800000 /* 0 = write, 1 = read */
#define CODEC_ADDR 0x007f0000
#define CODEC_SH_ADDR 16
#define CODEC_DATA 0x0000ffff

/* ES1371_REG_SMPRATE, the sample rate converter interface. The SRC's
   coefficient and state RAM is read and written a word at a time through
   this one register; the three DIS_ bits and SRC_DISABLE gate the
   converter, and BUSY says the RAM access has not finished. */
#define SRC_RAM_ADDR 0xfe000000
#define SRC_SH_RAM_ADDR 25
#define SRC_RAM_WE 0x01000000
#define SRC_RAM_BUSY 0x00800000
#define SRC_DISABLE 0x00400000
#define SRC_DIS_P1 0x00200000
#define SRC_DIS_P2 0x00100000
#define SRC_DIS_R1 0x00080000
#define SRC_RAM_DATA 0x0000ffff
/* Bits 16 to 18 are the interface's own state, which no datasheet we have
   describes but which every driver polls; see ES137xCodec.cpp. */
#define SRC_STATE 0x00070000

/* The SRC RAM words that hold a channel's rate. Each channel has a block
   of 32 words; the two that matter are the integer and fractional parts of
   the step the converter walks its input with. */
#define SRC_RAM_DAC1 0x70
#define SRC_RAM_DAC2 0x74
#define SRC_RAM_ADC 0x78
#define SRC_RAM_TRUNC_N 0x00
#define SRC_RAM_INT_REGS 0x01
#define SRC_RAM_ACCUM_FRAC 0x02
#define SRC_RAM_VFREQ_FRAC 0x03
#define SRC_RAM_WORDS 0x80

#define NB_CHANNELS 3
#define DAC1_CHANNEL 0
#define DAC2_CHANNEL 1
#define ADC_CHANNEL 2

#endif //! defined(__ES137X_REGS_H__)
