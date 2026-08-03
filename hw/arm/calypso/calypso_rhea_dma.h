/*
 * calypso_rhea_dma.h — contrôleur DMA RHEA du Calypso, côté MCU (FFFF:FC00).
 * Source : CAL207 §11 (ti-calypso2.pdf).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef CALYPSO_RHEA_DMA_H
#define CALYPSO_RHEA_DMA_H

#include "exec/hwaddr.h"
#include <stdint.h>
#include <stdbool.h>

#define CALYPSO_RHEA_DMA_BASE 0xFFFFFC00

uint64_t calypso_rhea_dma_read(void *opaque, hwaddr off, unsigned size);
void     calypso_rhea_dma_write(void *opaque, hwaddr off, uint64_t val, unsigned size);

/* Meme banc de registres vu du bus Rhea du DSP (XIO:FC00..FCFF, §11.1).
 * Rend true si PA appartient a la fenetre DMA. */
bool     calypso_rhea_dma_xio(bool write, uint16_t pa, uint16_t *val, uint16_t pc);

#endif /* CALYPSO_RHEA_DMA_H */
