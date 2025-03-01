// SPDX-License-Identifier: GPL-2.0-only
/*
 * Designware SPI core controller driver (refer pxa2xx_spi.c)
 *
 * Copyright (c) 2009, Intel Corporation.
 */

#include <linux/dma-mapping.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/highmem.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/spi/spi.h>

#include "spi-dw.h"

#ifdef CONFIG_DEBUG_FS
#include <linux/debugfs.h>
#endif

/* Slave spi_dev related */
struct chip_data {
	u8 tmode;		/* TR/TO/RO/EEPROM */
	u8 type;		/* SPI/SSP/MicroWire */

	u16 clk_div;		/* baud rate divider */
	u32 speed_hz;		/* baud rate */
};

#ifdef CONFIG_DEBUG_FS
#define SPI_REGS_BUFSIZE	1024
static ssize_t dw_spi_show_regs(struct file *file, char __user *user_buf,
		size_t count, loff_t *ppos)
{
	struct dw_spi *dws = file->private_data;
	char *buf;
	u32 len = 0;
	ssize_t ret;

	buf = kzalloc(SPI_REGS_BUFSIZE, GFP_KERNEL);
	if (!buf)
		return 0;

	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"%s registers:\n", dev_name(&dws->master->dev));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"=================================\n");
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"CTRLR0: \t0x%08x\n", dw_readl(dws, DW_SPI_CTRLR0));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"CTRLR1: \t0x%08x\n", dw_readl(dws, DW_SPI_CTRLR1));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"SSIENR: \t0x%08x\n", dw_readl(dws, DW_SPI_SSIENR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"SER: \t\t0x%08x\n", dw_readl(dws, DW_SPI_SER));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"BAUDR: \t\t0x%08x\n", dw_readl(dws, DW_SPI_BAUDR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"TXFTLR: \t0x%08x\n", dw_readl(dws, DW_SPI_TXFTLR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"RXFTLR: \t0x%08x\n", dw_readl(dws, DW_SPI_RXFTLR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"TXFLR: \t\t0x%08x\n", dw_readl(dws, DW_SPI_TXFLR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"RXFLR: \t\t0x%08x\n", dw_readl(dws, DW_SPI_RXFLR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"SR: \t\t0x%08x\n", dw_readl(dws, DW_SPI_SR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"IMR: \t\t0x%08x\n", dw_readl(dws, DW_SPI_IMR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"ISR: \t\t0x%08x\n", dw_readl(dws, DW_SPI_ISR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"DMACR: \t\t0x%08x\n", dw_readl(dws, DW_SPI_DMACR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"DMATDLR: \t0x%08x\n", dw_readl(dws, DW_SPI_DMATDLR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"DMARDLR: \t0x%08x\n", dw_readl(dws, DW_SPI_DMARDLR));
	len += scnprintf(buf + len, SPI_REGS_BUFSIZE - len,
			"=================================\n");

	ret = simple_read_from_buffer(user_buf, count, ppos, buf, len);
	kfree(buf);
	return ret;
}

static const struct file_operations dw_spi_regs_ops = {
	.owner		= THIS_MODULE,
	.open		= simple_open,
	.read		= dw_spi_show_regs,
	.llseek		= default_llseek,
};

static int dw_spi_debugfs_init(struct dw_spi *dws)
{
	char name[32];

	snprintf(name, 32, "dw_spi%d", dws->master->bus_num);
	dws->debugfs = debugfs_create_dir(name, NULL);
	if (!dws->debugfs)
		return -ENOMEM;

	debugfs_create_file("registers", S_IFREG | S_IRUGO,
		dws->debugfs, (void *)dws, &dw_spi_regs_ops);
	return 0;
}

static void dw_spi_debugfs_remove(struct dw_spi *dws)
{
	debugfs_remove_recursive(dws->debugfs);
}

#else
static inline int dw_spi_debugfs_init(struct dw_spi *dws)
{
	return 0;
}

static inline void dw_spi_debugfs_remove(struct dw_spi *dws)
{
}
#endif /* CONFIG_DEBUG_FS */

static void dw_spi_microwire_reg_update(struct spi_controller *master,
				struct spi_transfer *transfer)
{
	struct dw_spi *dws = spi_controller_get_devdata(master);
	u32 ctrlr1	= 0;
	u32 mwcr	= 0;
	u16 txlevel	= 0;
	u8 imask	= 0;

	/* MWCR [0] Microwire Transfer Mode */
	if (dws->mwmod == SSI_SEQUENTIAL_TRANSFER)
		mwcr = 1 << DW_SPI_MWCR_MWMOD_OFFSET;

	/* MWCR [1] Microwire Control */
	mwcr |= (dws->mdd) << DW_SPI_MWCR_MDD_OFFSET;

	dw_writel(dws, DW_SPI_MWCR, mwcr);

	if (dws->mdd == SSI_TRANSMIT_DATA) {
		/* TXFTLR [5:0] Transmit FIFO Threshold */
		txlevel = min_t(u16, dws->fifo_len / 2,
			dws->len / dws->n_bytes);
		dw_writel(dws, DW_SPI_TXFTLR, txlevel);

		/* IMR Interrupt Mask Register */
		imask |= SPI_INT_TXEI | SPI_INT_TXOI |
			 SPI_INT_RXUI | SPI_INT_RXOI;

	} else {
		/* IMR Interrupt Mask Register */
		imask |= SPI_INT_TXEI | SPI_INT_TXOI | SPI_INT_RXFI |
			 SPI_INT_RXUI | SPI_INT_RXOI;

		/* CTRL1 NDF [15:0] Number of Data Frames */
		if (dws->mwmod == SSI_SEQUENTIAL_TRANSFER) {
			ctrlr1 |= (dws->len - 1) << DW_SPI_CTRLR1_NDF_OFFSET;
			dw_writel(dws, DW_SPI_CTRLR1, ctrlr1);
		}
	}

	spi_umask_intr(dws, imask);
}

void dw_spi_set_cs(struct spi_device *spi, bool enable)
{
	struct dw_spi *dws = spi_controller_get_devdata(spi->controller);
	bool cs_high = !!(spi->mode & SPI_CS_HIGH);

	/*
	 * DW SPI controller demands any native CS being set in order to
	 * proceed with data transfer. So in order to activate the SPI
	 * communications we must set a corresponding bit in the Slave
	 * Enable register no matter whether the SPI core is configured to
	 * support active-high or active-low CS level.
	 */
	if (cs_high == enable)
		dw_writel(dws, DW_SPI_SER, BIT(spi->chip_select));
	else if (dws->cs_override)
		dw_writel(dws, DW_SPI_SER, 0);
}
EXPORT_SYMBOL_GPL(dw_spi_set_cs);

/* Return the max entries we can fill into tx fifo */
static inline u32 tx_max(struct dw_spi *dws)
{
	u32 tx_left, tx_room, rxtx_gap;

	tx_left = (dws->tx_end - dws->tx) / dws->n_bytes;
	tx_room = dws->fifo_len - dw_readl(dws, DW_SPI_TXFLR);

	/*
	 * Another concern is about the tx/rx mismatch, we
	 * though to use (dws->fifo_len - rxflr - txflr) as
	 * one maximum value for tx, but it doesn't cover the
	 * data which is out of tx/rx fifo and inside the
	 * shift registers. So a control from sw point of
	 * view is taken.
	 */
	rxtx_gap =  ((dws->rx_end - dws->rx) - (dws->tx_end - dws->tx))
			/ dws->n_bytes;

	return min3(tx_left, tx_room, (u32) (dws->fifo_len - rxtx_gap));
}

/* Return the max entries we should read out of rx fifo */
static inline u32 rx_max(struct dw_spi *dws)
{
	u32 rx_left = (dws->rx_end - dws->rx) / dws->n_bytes;

	return min_t(u32, rx_left, dw_readl(dws, DW_SPI_RXFLR));
}

static void dw_writer(struct dw_spi *dws)
{
	u32 max;
	u16 txw = 0;

	spin_lock(&dws->buf_lock);
	max = tx_max(dws);
	while (max--) {
		/* Set the tx word if the transfer's original "tx" is not null */
		if (dws->tx_end - dws->len) {
			if (dws->n_bytes == 1)
				txw = *(u8 *)(dws->tx);
			else
				txw = *(u16 *)(dws->tx);
		}
		dw_write_io_reg(dws, DW_SPI_DR, txw);
		dws->tx += dws->n_bytes;
	}
	spin_unlock(&dws->buf_lock);
}

static void dw_reader(struct dw_spi *dws)
{
	u32 max;
	u16 rxw;

	spin_lock(&dws->buf_lock);
	max = rx_max(dws);
	while (max--) {
		rxw = dw_read_io_reg(dws, DW_SPI_DR);
		/* Care rx only if the transfer's original "rx" is not null */
		if (dws->rx_end - dws->len) {
			if (dws->n_bytes == 1)
				*(u8 *)(dws->rx) = rxw;
			else
				*(u16 *)(dws->rx) = rxw;
		}
		dws->rx += dws->n_bytes;
	}
	spin_unlock(&dws->buf_lock);
}

static void int_error_stop(struct dw_spi *dws, const char *msg)
{
	spi_reset_chip(dws);

	dev_err(&dws->master->dev, "%s\n", msg);
	dws->master->cur_msg->status = -EIO;
	spi_finalize_current_transfer(dws->master);
}

static void microwire_ssi_write_control_word(struct dw_spi *dws)
{
	u16 txw = 0;

	if (dws->dw_ssi_cfs >= 1 && dws->dw_ssi_cfs <= 8)
		txw = (u8)(dws->rcv_cword);
	else
		txw = (u16)(dws->rcv_cword);

	if (dws->cont_non_sequential) {
		/* For cont-non-sequential will write more control words */
		dw_write_io_reg(dws, DW_SPI_DR, txw);
		dw_write_io_reg(dws, DW_SPI_DR, txw);
	} else {
		dw_write_io_reg(dws, DW_SPI_DR, txw);
	}
}

static irqreturn_t interrupt_transfer(struct dw_spi *dws)
{
	u16 irq_status = dw_readl(dws, DW_SPI_ISR);

	/* Error handling */
	if (irq_status & (SPI_INT_TXOI | SPI_INT_RXOI | SPI_INT_RXUI)) {
		dw_readl(dws, DW_SPI_ICR);
		int_error_stop(dws, "interrupt_transfer: fifo overrun/underrun");
		return IRQ_HANDLED;
	}

	if (dws->type == SSI_NS_MICROWIRE) {
		if (dws->mdd == SSI_TRANSMIT_DATA) {
			if (irq_status & SPI_INT_TXEI) {
				spi_mask_intr(dws, SPI_INT_TXEI);
				dw_writer(dws);
				spi_finalize_current_transfer(dws->master);
				return IRQ_HANDLED;
			}
		} else {
			if (irq_status & SPI_INT_TXEI) {
				spi_mask_intr(dws, SPI_INT_TXEI);
				microwire_ssi_write_control_word(dws);
			} else if (irq_status & SPI_INT_RXFI) {
				spi_mask_intr(dws, SPI_INT_RXFI);
				dw_reader(dws);
				spi_umask_intr(dws, SPI_INT_RXFI);

				if (dws->rx_end == dws->rx) {
					spi_mask_intr(dws, SPI_INT_RXFI);
					spi_finalize_current_transfer(
						dws->master);
					return IRQ_HANDLED;
				}
			}
		}
		return IRQ_HANDLED;
	}

	dw_reader(dws);
	if (dws->rx_end == dws->rx) {
		spi_mask_intr(dws, SPI_INT_TXEI);
		spi_finalize_current_transfer(dws->master);
		return IRQ_HANDLED;
	}
	if (irq_status & SPI_INT_TXEI) {
		spi_mask_intr(dws, SPI_INT_TXEI);
		dw_writer(dws);
		/* Enable TX irq always, it will be disabled when RX finished */
		spi_umask_intr(dws, SPI_INT_TXEI);
	}

	return IRQ_HANDLED;
}

static irqreturn_t dw_spi_irq(int irq, void *dev_id)
{
	struct spi_controller *master = dev_id;
	struct dw_spi *dws = spi_controller_get_devdata(master);
	u16 irq_status = dw_readl(dws, DW_SPI_ISR) & 0x3f;

	if (!irq_status)
		return IRQ_NONE;

	if (!master->cur_msg) {
		spi_mask_intr(dws, SPI_INT_TXEI);
		return IRQ_HANDLED;
	}

	return dws->transfer_handler(dws);
}

/* Configure CTRLR0 for DW_apb_ssi */
u32 dw_spi_update_cr0(struct spi_controller *master, struct spi_device *spi,
		      struct spi_transfer *transfer)
{
	struct chip_data *chip = spi_get_ctldata(spi);
	u32 cr0;

	/* Default SPI mode is SCPOL = 0, SCPH = 0 */
	cr0 = (transfer->bits_per_word - 1)
		| (chip->type << SPI_FRF_OFFSET)
		| ((((spi->mode & SPI_CPOL) ? 1 : 0) << SPI_SCOL_OFFSET) |
		   (((spi->mode & SPI_CPHA) ? 1 : 0) << SPI_SCPH_OFFSET) |
		   (((spi->mode & SPI_LOOP) ? 1 : 0) << SPI_SRL_OFFSET))
		| (chip->tmode << SPI_TMOD_OFFSET);

	return cr0;
}
EXPORT_SYMBOL_GPL(dw_spi_update_cr0);

/* Configure CTRLR0 for DWC_ssi */
u32 dw_spi_update_cr0_v1_01a(struct spi_controller *master,
			     struct spi_device *spi,
			     struct spi_transfer *transfer)
{
	struct dw_spi *dws = spi_controller_get_devdata(master);
	struct chip_data *chip = spi_get_ctldata(spi);
	u32 cr0;

	/* CTRLR0[ 4: 0] Data Frame Size */
	cr0 = (transfer->bits_per_word - 1);

	/* CTRLR0[ 7: 6] Frame Format */
	cr0 |= chip->type << DWC_SSI_CTRLR0_FRF_OFFSET;

	/*
	 * SPI mode (SCPOL|SCPH)
	 * CTRLR0[ 8] Serial Clock Phase
	 * CTRLR0[ 9] Serial Clock Polarity
	 */
	cr0 |= ((spi->mode & SPI_CPOL) ? 1 : 0) << DWC_SSI_CTRLR0_SCPOL_OFFSET;
	cr0 |= ((spi->mode & SPI_CPHA) ? 1 : 0) << DWC_SSI_CTRLR0_SCPH_OFFSET;

	/* CTRLR0[11:10] Transfer Mode */
	cr0 |= chip->tmode << DWC_SSI_CTRLR0_TMOD_OFFSET;

	/* CTRLR0[13] Shift Register Loop */
	cr0 |= ((spi->mode & SPI_LOOP) ? 1 : 0) << DWC_SSI_CTRLR0_SRL_OFFSET;

	/* CTRLR0[19:16] Control Frame size */
	if (dws->type == SSI_NS_MICROWIRE)
		cr0 |= (dws->dw_ssi_cfs - 1) << DWC_SPI_CTRLR0_CFS_OFFSET;

	return cr0;
}
EXPORT_SYMBOL_GPL(dw_spi_update_cr0_v1_01a);

static int dw_spi_transfer_one(struct spi_controller *master,
		struct spi_device *spi, struct spi_transfer *transfer)
{
	struct dw_spi *dws = spi_controller_get_devdata(master);
	struct chip_data *chip = spi_get_ctldata(spi);
	unsigned long flags;
	u8 imask = 0;
	u16 txlevel = 0;
	u32 cr0;
	int ret;

	dws->dma_mapped = 0;
	spin_lock_irqsave(&dws->buf_lock, flags);
	dws->tx = (void *)transfer->tx_buf;
	dws->tx_end = dws->tx + transfer->len;
	dws->rx = transfer->rx_buf;
	dws->rx_end = dws->rx + transfer->len;
	dws->len = transfer->len;
	spin_unlock_irqrestore(&dws->buf_lock, flags);

	/* Ensure dw->rx and dw->rx_end are visible */
	smp_mb();

	spi_enable_chip(dws, 0);

	/* Handle per transfer options for bpw and speed */
	if (transfer->speed_hz != dws->current_freq) {
		if (transfer->speed_hz != chip->speed_hz) {
			/* clk_div doesn't support odd number */
			chip->clk_div = (DIV_ROUND_UP(dws->max_freq, transfer->speed_hz) + 1) & 0xfffe;
			chip->speed_hz = transfer->speed_hz;
		}
		dws->current_freq = transfer->speed_hz;
		spi_set_clk(dws, chip->clk_div);
	}

	dws->n_bytes = DIV_ROUND_UP(transfer->bits_per_word, BITS_PER_BYTE);
	dws->dma_width = DIV_ROUND_UP(transfer->bits_per_word, BITS_PER_BYTE);

	cr0 = dws->update_cr0(master, spi, transfer);
	dw_writel(dws, DW_SPI_CTRLR0, cr0);

	/* Check if current transfer is a DMA transaction */
	if (master->can_dma && master->can_dma(master, spi, transfer))
		dws->dma_mapped = master->cur_msg_mapped;

	/* For poll mode just disable all interrupts */
	spi_mask_intr(dws, 0xff);

	/*
	 * Interrupt mode
	 * we only need set the TXEI IRQ, as TX/RX always happen syncronizely
	 */
	if (dws->dma_mapped) {
		ret = dws->dma_ops->dma_setup(dws, transfer);
		if (ret < 0) {
			spi_enable_chip(dws, 1);
			return ret;
		}
	} else {
		/* update microwire required register */
		if (dws->type == SSI_NS_MICROWIRE) {
			dw_spi_microwire_reg_update(master, transfer);
		} else {
			txlevel = min_t(u16, dws->fifo_len / 2,
				dws->len / dws->n_bytes);
			dw_writel(dws, DW_SPI_TXFTLR, txlevel);

			/* Set the interrupt mask */
			imask |= SPI_INT_TXEI | SPI_INT_TXOI |
				 SPI_INT_RXUI | SPI_INT_RXOI;
			spi_umask_intr(dws, imask);
		}
		dws->transfer_handler = interrupt_transfer;
	}

	spi_enable_chip(dws, 1);

	if (dws->dma_mapped)
		return dws->dma_ops->dma_transfer(dws, transfer);

	return 1;
}

static void dw_spi_handle_err(struct spi_controller *master,
		struct spi_message *msg)
{
	struct dw_spi *dws = spi_controller_get_devdata(master);

	if (dws->dma_mapped)
		dws->dma_ops->dma_stop(dws);

	spi_reset_chip(dws);
}

/* This may be called twice for each spi dev */
static int dw_spi_setup(struct spi_device *spi)
{
	struct dw_spi *dws = spi_controller_get_devdata(spi->controller);
	struct chip_data *chip;

	/* Only alloc on first setup */
	chip = spi_get_ctldata(spi);
	if (!chip) {
		chip = kzalloc(sizeof(struct chip_data), GFP_KERNEL);
		if (!chip)
			return -ENOMEM;
		spi_set_ctldata(spi, chip);
	}

	chip->type = dws->type;
	chip->tmode = SPI_TMOD_TR;

	return 0;
}

static void dw_spi_cleanup(struct spi_device *spi)
{
	struct chip_data *chip = spi_get_ctldata(spi);

	kfree(chip);
	spi_set_ctldata(spi, NULL);
}

/* Restart the controller, disable all interrupts, clean rx fifo */
static void spi_hw_init(struct device *dev, struct dw_spi *dws)
{
	spi_reset_chip(dws);

	/*
	 * Try to detect the FIFO depth if not set by interface driver,
	 * the depth could be from 2 to 256 from HW spec
	 */
	if (!dws->fifo_len) {
		u32 fifo;

		for (fifo = 1; fifo < 256; fifo++) {
			dw_writel(dws, DW_SPI_TXFTLR, fifo);
			if (fifo != dw_readl(dws, DW_SPI_TXFTLR))
				break;
		}
		dw_writel(dws, DW_SPI_TXFTLR, 0);

		dws->fifo_len = (fifo == 1) ? 0 : fifo;
		dev_dbg(dev, "Detected FIFO size: %u bytes\n", dws->fifo_len);
	}

	/* enable HW fixup for explicit CS deselect for Amazon's alpine chip */
	if (dws->cs_override)
		dw_writel(dws, DW_SPI_CS_OVERRIDE, 0xF);
}

int dw_spi_add_host(struct device *dev, struct dw_spi *dws)
{
	struct spi_controller *master;
	int ret;

	BUG_ON(dws == NULL);

	master = spi_alloc_master(dev, 0);
	if (!master)
		return -ENOMEM;

	dws->master = master;
	dws->dma_inited = 0;
	dws->dma_addr = (dma_addr_t)(dws->paddr + DW_SPI_DR);
	spin_lock_init(&dws->buf_lock);

	spi_controller_set_devdata(master, dws);

	ret = request_irq(dws->irq, dw_spi_irq, IRQF_SHARED, dev_name(dev),
			  master);
	if (ret < 0) {
		dev_err(dev, "can not get IRQ\n");
		goto err_free_master;
	}

	master->use_gpio_descriptors = true;
	master->mode_bits = SPI_CPOL | SPI_CPHA | SPI_LOOP;
	master->bits_per_word_mask =  SPI_BPW_RANGE_MASK(4, 16);
	master->bus_num = dws->bus_num;
	master->num_chipselect = dws->num_cs;
	master->setup = dw_spi_setup;
	master->cleanup = dw_spi_cleanup;
	master->set_cs = dw_spi_set_cs;
	master->transfer_one = dw_spi_transfer_one;
	master->handle_err = dw_spi_handle_err;
	master->max_speed_hz = dws->max_freq;
	master->dev.of_node = dev->of_node;
	master->dev.fwnode = dev->fwnode;
	if (dws->type == SSI_NS_MICROWIRE)
		master->flags =  SPI_CONTROLLER_HALF_DUPLEX |
				 SPI_MASTER_GPIO_SS;
	else
		master->flags = SPI_MASTER_GPIO_SS;
	master->auto_runtime_pm = true;

	if (dws->set_cs)
		master->set_cs = dws->set_cs;

	/* Basic HW init */
	spi_hw_init(dev, dws);

	if (dws->dma_ops && dws->dma_ops->dma_init) {
		ret = dws->dma_ops->dma_init(dws);
		if (ret) {
			dev_warn(dev, "DMA init failed\n");
			dws->dma_inited = 0;
		} else {
			master->can_dma = dws->dma_ops->can_dma;
			master->flags |= SPI_CONTROLLER_MUST_TX;
		}
	}

	ret = spi_register_controller(master);
	if (ret) {
		dev_err(&master->dev, "problem registering spi master\n");
		goto err_dma_exit;
	}

	dw_spi_debugfs_init(dws);
	return 0;

err_dma_exit:
	if (dws->dma_ops && dws->dma_ops->dma_exit)
		dws->dma_ops->dma_exit(dws);
	spi_enable_chip(dws, 0);
	free_irq(dws->irq, master);
err_free_master:
	spi_controller_put(master);
	return ret;
}
EXPORT_SYMBOL_GPL(dw_spi_add_host);

void dw_spi_remove_host(struct dw_spi *dws)
{
	dw_spi_debugfs_remove(dws);

	spi_unregister_controller(dws->master);

	if (dws->dma_ops && dws->dma_ops->dma_exit)
		dws->dma_ops->dma_exit(dws);

	spi_shutdown_chip(dws);

	free_irq(dws->irq, dws->master);
}
EXPORT_SYMBOL_GPL(dw_spi_remove_host);

int dw_spi_suspend_host(struct dw_spi *dws)
{
	int ret;

	ret = spi_controller_suspend(dws->master);
	if (ret)
		return ret;

	spi_shutdown_chip(dws);
	return 0;
}
EXPORT_SYMBOL_GPL(dw_spi_suspend_host);

int dw_spi_resume_host(struct dw_spi *dws)
{
	spi_hw_init(&dws->master->dev, dws);
	return spi_controller_resume(dws->master);
}
EXPORT_SYMBOL_GPL(dw_spi_resume_host);

MODULE_AUTHOR("Feng Tang <feng.tang@intel.com>");
MODULE_DESCRIPTION("Driver for DesignWare SPI controller core");
MODULE_LICENSE("GPL v2");
