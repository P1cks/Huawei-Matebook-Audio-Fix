/* SPDX-License-Identifier: GPL-2.0+ */
/*
 * Copyright 2023 Marian Postevca <posteuca@mutex.one>
 */

#ifndef __ACP3X_ES83XX_H
#define __ACP3X_ES83XX_H

struct acp_mach_ops;
struct snd_soc_card;

void acp3x_es83xx_init_ops(struct acp_mach_ops *ops);
void acp3x_es83xx_shutdown(struct snd_soc_card *card);

#endif

