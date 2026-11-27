// SPDX-License-Identifier: GPL-2.0-only
#ifndef NT36536_FW_H
#define NT36536_FW_H
struct spi_device;
/* Receiver bit offset measured on the trim-ID read; capture reads reuse it. */
extern u8 nt36536_read_shift;
int nt36536_load_turner_firmware(struct spi_device *spi);
#endif
