/*
 * This file is part of Espruino, a JavaScript interpreter for Microcontrollers
 *
 * Copyright (C) 2019 Gordon Williams <gw@pur3.co.uk>
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 *
 * ----------------------------------------------------------------------------
 * Graphics Backend for drawing to SPI displays
 * ----------------------------------------------------------------------------
 */

#include "platform_config.h"
#include "jsutils.h"
#include "jshardware.h"
#include "lcd_spilcd.h"
#include "lcd_spilcd_info.h"
#include "lcd_spilcd_palette.h"
#if defined(NRF52_SERIES)
#include "nrf_gpio.h"
#endif

// ======================================================================

#define LCD_STRIDE ((LCD_WIDTH*LCD_BPP+7)>>3)
unsigned char lcdBuffer[LCD_STRIDE*LCD_HEIGHT];
#if LCD_BPP==4
unsigned short lcdPalette[16];
#endif
#if LCD_BPP==8
unsigned short lcdPalette[256];
#endif

#define LCD_SPI EV_SPI1

// ======================================================================


// ======================================================================

void lcdCmd_SPILCD(int cmd, int dataLen, const unsigned char *data) {
  jshPinSetValue(LCD_SPI_DC, 0); // command
  jshPinSetValue(LCD_SPI_CS, 0);
  jshSPISend(LCD_SPI, cmd);
  if (dataLen) {
    jshPinSetValue(LCD_SPI_DC, 1); // data
    while (dataLen) {
      jshSPISend(LCD_SPI, *(data++));
      dataLen--;
    }
  }
  jshPinSetValue(LCD_SPI_CS, 1);
}
void lcdSendInitCmd_SPILCD() {
  // Send initialization commands to ST7735
  const unsigned char *cmd = SPILCD_INIT_CODE;
  while(cmd[CMDINDEX_DATALEN]!=255) {
    lcdCmd_SPILCD(cmd[CMDINDEX_CMD], cmd[CMDINDEX_DATALEN], &cmd[3]);
    if (cmd[CMDINDEX_DELAY])
      jshDelayMicroseconds(1000*cmd[CMDINDEX_DELAY]);
    cmd += 3 + cmd[CMDINDEX_DATALEN];
  }
}
void lcdSetPalette_SPILCD(const char *pal) {
#ifdef LCD_PALETTED
  memcpy(lcdPalette, pal ? pal : SPILCD_PALETTE, sizeof(lcdPalette));
#endif
}

// ======================================================================


unsigned int lcdGetPixel_SPILCD(JsGraphics *gfx, int x, int y) {
#if LCD_BPP==4
  int addr = (x + (y*LCD_WIDTH)) >> 1;
  unsigned char b = lcdBuffer[addr];
  return (x&1) ? (b&15) : (b>>4);
#endif
#if LCD_BPP==8
  int addr = x + (y*LCD_WIDTH);
  return lcdBuffer[addr];
#endif
#if LCD_BPP==12
  int addr = (x>>1)*3 + (y*LCD_STRIDE) - 1;
  uint32_t *p = (uint32_t*)(&lcdBuffer[addr]);
  uint32_t c = __builtin_bswap32(*p);
  if (x&1) return c & 0xFFF;
  else return (c>>12) & 0xFFF;
#endif
#if LCD_BPP==16
  int addr = (x<<1) + (y*LCD_STRIDE);
  uint16_t *p = (uint16_t*)(&lcdBuffer[addr]);
  return __builtin_bswap16(*p);
#endif
}


void lcdSetPixel_SPILCD(JsGraphics *gfx, int x, int y, unsigned int col) {
#if LCD_BPP==4
  int addr = (x + (y*LCD_WIDTH)) >> 1;
  if (x&1) lcdBuffer[addr] = (lcdBuffer[addr] & 0xF0) | (col&0x0F);
  else lcdBuffer[addr] = (lcdBuffer[addr] & 0x0F) | (col << 4);
#endif
#if LCD_BPP==8
  int addr = x + (y*LCD_WIDTH);
  lcdBuffer[addr] = col;
#endif
#if LCD_BPP==12
  // For a 12 bit write we store inside 32 bits. This is frustrated by the LCD
  // working on a different byte order, but at least ARM has instructions for
  // endian swaps
  int addr = (x>>1)*3 + (y*LCD_STRIDE) - 1;
  uint32_t *p = (uint32_t*)(&lcdBuffer[addr]);
  uint32_t c = __builtin_bswap32(*p);
  if (x&1) c = (c & 0xFFFFF000) | (col & 0xFFF);
  else c = (c & 0xFF000FFF) | ((col & 0xFFF)<<12);
  *p = __builtin_bswap32(c);
#endif
#if LCD_BPP==16
  int addr = (x<<1) + (y*LCD_STRIDE);
  uint16_t *p = (uint16_t*)(&lcdBuffer[addr]);
  *p = __builtin_bswap16(col);
#endif
}

void lcdFlip_SPILCD_callback() {
  // just an empty stub for SPIsend - we'll just push data as fast as we can
}

void lcdFlip_SPILCD(JsGraphics *gfx) {
  if (gfx->data.modMinX > gfx->data.modMaxX) return; // nothing to do!

  // use nearest 2 pixels as we're sending 12 bits
  gfx->data.modMinX = (gfx->data.modMinX)&~1;
  gfx->data.modMaxX = (gfx->data.modMaxX+2)&~1;
#if LCD_BPP==12 || LCD_BPP==16
  // Just send full rows as this allows us to issue a single SPI
  // transfer.
  // TODO: could swap to a transfer per row if we're filling less than half a row
  gfx->data.modMinX = 0;
  gfx->data.modMaxX = LCD_WIDTH-1;
#else
  int xlen = gfx->data.modMaxX - gfx->data.modMinX;
  int xstart = gfx->data.modMinX;
#endif
  unsigned char buffer1[LCD_STRIDE];

  jshPinSetValue(LCD_SPI_CS, 0);
  jshPinSetValue(LCD_SPI_DC, 0); // command
  buffer1[0] = SPILCD_CMD_WINDOW_X;
  jshSPISendMany(LCD_SPI, buffer1, NULL, 1, NULL);
  jshPinSetValue(LCD_SPI_DC, 1); // data
  buffer1[0] = 0;
  buffer1[1] = gfx->data.modMinX;
  buffer1[2] = 0;
  buffer1[3] = gfx->data.modMaxX;
  jshSPISendMany(LCD_SPI, buffer1, NULL, 4, NULL);
  jshPinSetValue(LCD_SPI_DC, 0); // command
  buffer1[0] = SPILCD_CMD_WINDOW_Y;
  jshSPISendMany(LCD_SPI, buffer1, NULL, 1, NULL);
  jshPinSetValue(LCD_SPI_DC, 1); // data
  buffer1[0] = 0;
  buffer1[1] = gfx->data.modMinY;
  buffer1[2] = 0;
  buffer1[3] = gfx->data.modMaxY;
  jshSPISendMany(LCD_SPI, buffer1, NULL, 4, NULL);
  jshPinSetValue(LCD_SPI_DC, 0); // command
  buffer1[0] = SPILCD_CMD_DATA;
  jshSPISendMany(LCD_SPI, buffer1, NULL, 1, NULL);
  jshPinSetValue(LCD_SPI_DC, 1); // data

#if LCD_BPP==12 || LCD_BPP==16
  // FIXME: hack because SPI send on NRF52 fails for >65k transfers
  // we should fix this in jshardware.c
  unsigned char *p = &lcdBuffer[LCD_STRIDE*gfx->data.modMinY];
  int c = (gfx->data.modMaxY+1-gfx->data.modMinY)*LCD_STRIDE;
  while (c) {
    int n = c;
    if (n>65535) n=65535;
    jshSPISendMany(
        LCD_SPI,
        p,
        0,
        n,
        NULL);
    p+=n;
    c-=n;
  }
#else
  unsigned char buffer2[LCD_STRIDE];
  for (int y=gfx->data.modMinY;y<=gfx->data.modMaxY;y++) {
    unsigned char *buffer = (y&1)?buffer1:buffer2;
    // skip any lines that don't need updating
#if LCD_BPP==4
    unsigned char *px = &lcdBuffer[y*LCD_STRIDE + (xstart>>1)];
#endif
#if LCD_BPP==8
    unsigned char *px = &lcdBuffer[y*LCD_STRIDE + xstart];
#endif
    unsigned char *bufPtr = (unsigned char*)buffer;
    for (int x=0;x<xlen;x+=2) {
#if LCD_BPP==4
      unsigned char c = *(px++);
      unsigned int a = lcdPalette[c >> 4];
      unsigned int b = lcdPalette[c & 15];
#endif
#if LCD_BPP==8
      unsigned int a = lcdPalette[*(px++)];
      unsigned int b = lcdPalette[*(px++)];
#endif
      *(bufPtr++) = a>>4;
      *(bufPtr++) = (a<<4) | (b>>8);
      *(bufPtr++) = b;
    }
    size_t len = ((unsigned char*)bufPtr)-buffer;
    jshSPISendMany(LCD_SPI, buffer, 0, len, lcdFlip_SPILCD_callback);
  }
  jshSPIWait(LCD_SPI);
#endif
  jshPinSetValue(LCD_SPI_CS,1);
  // Reset modified-ness
  gfx->data.modMaxX = -32768;
  gfx->data.modMaxY = -32768;
  gfx->data.modMinX = 32767;
  gfx->data.modMinY = 32767;
}


void lcdInit_SPILCD(JsGraphics *gfx) {
  gfx->data.width = LCD_WIDTH;
  gfx->data.height = LCD_HEIGHT;
  gfx->data.bpp = LCD_BPP;

  lcdSetPalette_SPILCD(0);

#ifdef LCD_BL
  jshPinOutput(LCD_BL, 1);
#endif
#ifdef LCD_EN
  jshPinOutput(LCD_EN, 1);
#endif
  jshPinOutput(LCD_SPI_CS,1);
  jshPinOutput(LCD_SPI_DC,1);
  jshPinOutput(LCD_SPI_SCK,1);
  jshPinOutput(LCD_SPI_MOSI,1);
  jshPinOutput(LCD_SPI_RST,1);
  jshDelayMicroseconds(10000);
  jshPinOutput(LCD_SPI_RST, 1);
  jshDelayMicroseconds(10000);

  JshSPIInfo inf;
  jshSPIInitInfo(&inf);
#ifndef LCD_SPI_BITRATE
#define LCD_SPI_BITRATE 8000000
#endif
  inf.baudRate = LCD_SPI_BITRATE;
  inf.pinMOSI = LCD_SPI_MOSI;
#ifdef LCD_SPI_MISO
  inf.pinMISO = LCD_SPI_MISO;
#endif
  inf.pinSCK = LCD_SPI_SCK;
  jshSPISetup(LCD_SPI, &inf);
#if defined(NRF52_SERIES) // configure 'high drive' for GPIOs
  nrf_gpio_cfg(
      LCD_SPI_MOSI,
      GPIO_PIN_CNF_DIR_Output,
      GPIO_PIN_CNF_INPUT_Disconnect,
      GPIO_PIN_CNF_PULL_Disabled,
      GPIO_PIN_CNF_DRIVE_H0H1,
      GPIO_PIN_CNF_SENSE_Disabled);
  nrf_gpio_cfg(
      LCD_SPI_SCK,
      GPIO_PIN_CNF_DIR_Output,
      GPIO_PIN_CNF_INPUT_Disconnect,
      GPIO_PIN_CNF_PULL_Disabled,
      GPIO_PIN_CNF_DRIVE_H0H1,
      GPIO_PIN_CNF_SENSE_Disabled);
#endif

  lcdSendInitCmd_SPILCD();
}

void lcdSetCallbacks_SPILCD(JsGraphics *gfx) {
  gfx->setPixel = lcdSetPixel_SPILCD;
  gfx->getPixel = lcdGetPixel_SPILCD;
  //gfx->idle = lcdIdle_PCD8544;
}

