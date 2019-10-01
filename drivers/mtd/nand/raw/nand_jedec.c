// SPDX-License-Identifier: GPL-2.0
/*
 *  Copyright (C) 2000 Steven J. Hill (sjhill@realitydiluted.com)
 *		  2002-2006 Thomas Gleixner (tglx@linutronix.de)
 *
 *  Credits:
 *	David Woodhouse for adding multichip support
 *
 *	Aleph One Ltd. and Toby Churchill Ltd. for supporting the
 *	rework for 2K page size chips
 *
 * This file contains all ONFI helpers.
 */

#include <linux/slab.h>

#include "internals.h"

/*
 * Check if the NAND chip is JEDEC compliant, returns 1 if it is, 0 otherwise.
 */
int nand_jedec_detect(struct nand_chip *chip)
{
	struct mtd_info *mtd = nand_to_mtd(chip);
	struct nand_jedec_params *p;
	struct jedec_ecc_info *ecc;
	int jedec_version = 0;
	char id[5];
	int i, val, ret;

	/* Try JEDEC for unknown chip or LP */
	ret = nand_readid_op(chip, 0x40, id, sizeof(id));
	if (ret || strncmp(id, "JEDEC", sizeof(id)))
		return 0;

	/* JEDEC chip: allocate a buffer to hold its parameter page */
	p = kzalloc(sizeof(*p), GFP_KERNEL);
	if (!p)
		return -ENOMEM;

	ret = nand_read_param_page_op(chip, 0x40, NULL, 0);
	if (ret) {
		ret = 0;
		goto free_jedec_param_page;
	}

	for (i = 0; i < 3; i++) {
		ret = nand_read_data_op(chip, p, sizeof(*p), true);
		if (ret) {
			ret = 0;
			goto free_jedec_param_page;
		}

		if (onfi_crc16(ONFI_CRC_BASE, (uint8_t *)p, 510) ==
				le16_to_cpu(p->crc))
			break;
	}

	if (i == 3) {
		pr_err("Could not find valid JEDEC parameter page; aborting\n");
		goto free_jedec_param_page;
	}

	/* Check version */
	val = le16_to_cpu(p->revision);
	if (val & (1 << 2))
		jedec_version = 10;
	else if (val & (1 << 1))
		jedec_version = 1; /* vendor specific version */

	if (!jedec_version) {
		pr_info("unsupported JEDEC version: %d\n", val);
		goto free_jedec_param_page;
	}

	sanitize_string(p->manufacturer, sizeof(p->manufacturer));
	sanitize_string(p->model, sizeof(p->model));
	chip->parameters.model = kstrdup(p->model, GFP_KERNEL);
	if (!chip->parameters.model) {
		ret = -ENOMEM;
		goto free_jedec_param_page;
	}

	mtd->writesize = le32_to_cpu(p->byte_per_page);

	/* Please reference to the comment for nand_flash_detect_onfi. */
	mtd->erasesize = 1 << (fls(le32_to_cpu(p->pages_per_block)) - 1);
	mtd->erasesize *= mtd->writesize;

	mtd->oobsize = le16_to_cpu(p->spare_bytes_per_page);

	/* Please reference to the comment for nand_flash_detect_onfi. */
	chip->chipsize = 1 << (fls(le32_to_cpu(p->blocks_per_lun)) - 1);
	chip->chipsize *= (uint64_t)mtd->erasesize * p->lun_count;
	chip->bits_per_cell = p->bits_per_cell;

	if (le16_to_cpu(p->features) & JEDEC_FEATURE_16_BIT_BUS)
		chip->options |= NAND_BUSWIDTH_16;

	/* ECC info */
	ecc = &p->ecc_info[0];

	if (ecc->codeword_size >= 9) {
		chip->ecc_strength_ds = ecc->ecc_bits;
		chip->ecc_step_ds = 1 << ecc->codeword_size;
	} else {
		pr_warn("Invalid codeword size\n");
	}

	ret = 1;

free_jedec_param_page:
	kfree(p);
	return ret;
}
