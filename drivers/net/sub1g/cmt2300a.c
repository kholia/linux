// SPDX-License-Identifier: GPL-2.0-only
/*
 * CMT2300A Sub-GHz radio transceiver driver with character device interface
 *
 * Copyright (C) CMOSTEK SZ
 * Copyright (C) Tribo Consulting
 * Copyright (C) Dhiru Kholia
 *
 * Datasheet: https://www.hoperf.com/uploads/CMT2300ADatasheetEN-V1.7-202307_1695350200.pdf
 *
 * This driver provides both SPI device driver and character device interface
 * for userspace access via /dev/sub1g_dev00 device.
 *
 * This driver is based on https://github.com/Tribo-Consulting/hoperf-rfm300p work.
 */

#include <linux/module.h>
#include <linux/spi/spi.h>
#include <linux/gpio/consumer.h>
#include <linux/of.h>
#include <linux/delay.h>
#include <linux/miscdevice.h>
#include <linux/interrupt.h>
#include <linux/wait.h>
#include <linux/uaccess.h>
#include <linux/mutex.h>
#include "cmt2300a.h"

#define SUBG_DEV_NAME "sub1g_dev"

#define SPI3_SPEED 1

// Bank sizes
#define CMT2300A_CMT_BANK_SIZE 12
#define CMT2300A_SYSTEM_BANK_SIZE 12
#define CMT2300A_FREQ_BANK_SIZE 8
#define CMT2300A_DATARATE_BANK_SIZE 24
#define CMT2300A_BASEBAND_BANK_SIZE 29
#define CMT2300A_TX_BANK_SIZE 11

struct cmt2300a_data {
	struct spi_device *spi;
	struct miscdevice misc_dev;

	/* GPIO descriptors for bit-banging SPI */
	struct gpio_desc *csb; /* Chip Select for registers */
	struct gpio_desc *fcsb; /* Chip Select for FIFO */
	struct gpio_desc *sclk; /* SPI Clock */
	struct gpio_desc *sdio; /* SPI Data I/O (bidirectional) */

	/* GPIO descriptors for chip status monitoring */
	struct gpio_desc *gpio1;
	struct gpio_desc *gpio2;
	struct gpio_desc *gpio3;

	/* IRQ */
	int irq;

	/* Wait queue for data reception and transmission */
	wait_queue_head_t rx_wait_queue;
	wait_queue_head_t tx_wait_queue;
	int rx_wait_event;
	int tx_wait_event;

	/* Data buffers */
	u8 rx_packet[256]; /* Temporary buffer for received packets */
	size_t rx_packet_len;
	u8 tx_data[256];
	size_t tx_len;

	/* Packet statistics */
	unsigned long rx_packets;
	unsigned long rx_packets_crc_good;
	unsigned long rx_packets_crc_bad;
	unsigned long rx_packets_dropped;
	unsigned long tx_packets;
	s8 last_rssi;

	/* Mutex for TX operations */
	struct mutex tx_lock;

	/* Runtime configurable register banks */
	u8 cmt_bank[CMT2300A_CMT_BANK_SIZE];
	u8 system_bank[CMT2300A_SYSTEM_BANK_SIZE];
	u8 freq_bank[CMT2300A_FREQ_BANK_SIZE];
	u8 datarate_bank[CMT2300A_DATARATE_BANK_SIZE];
	u8 baseband_bank[CMT2300A_BASEBAND_BANK_SIZE];
	u8 tx_bank[CMT2300A_TX_BANK_SIZE];
};

// Register bank base addresses
#define CMT2300A_CMT_BANK_BASE 0x00
#define CMT2300A_SYSTEM_BANK_BASE 0x0C
#define CMT2300A_FREQ_BANK_BASE 0x18
#define CMT2300A_DATARATE_BANK_BASE 0x20
#define CMT2300A_BASEBAND_BANK_BASE 0x38
#define CMT2300A_TX_BANK_BASE 0x55

// IDA Pro register configuration values
static const u8 cmt2300a_cmt_bank[CMT2300A_CMT_BANK_SIZE] = {
	0x00, 0x66, 0xEC, 0x1D, 0x70, 0x80, 0x14, 0x08, 0x11, 0x02, 0x02, 0x00
};

static const u8 cmt2300a_system_bank[CMT2300A_SYSTEM_BANK_SIZE] = {
	0xAE, 0xE0, 0x55, 0x00, 0x00, 0xF4, 0x10, 0xE2, 0x42, 0x20, 0x00, 0x81
};

static const u8 cmt2300a_freq_bank[CMT2300A_FREQ_BANK_SIZE] = {
	0x42, 0x16, 0x16, 0x8D, 0x42, 0x0B, 0xBD, 0x1C
};

static const u8 cmt2300a_datarate_bank[CMT2300A_DATARATE_BANK_SIZE] = {
	0x1F, 0xF8, 0x61, 0x10, 0xE3, 0x24, 0x12, 0x0A, 0x9F, 0x4B, 0x29, 0x29,
	0xC0, 0x08, 0x02, 0x53, 0x18, 0x00, 0xB4, 0x00, 0x00, 0x01, 0x00, 0x00
};

static const u8 cmt2300a_baseband_bank[CMT2300A_BASEBAND_BANK_SIZE] = {
	0x12, 0x04, 0x00, 0xAA, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0xD4, 0xAD, 0x01, 0x1F, 0x00, 0x00, 0x00, 0x00, 0x00,
	0x00, 0x00, 0x00, 0x61, 0xFF, 0x00, 0x00, 0x1F, 0x10
};

static const u8 cmt2300a_tx_bank[CMT2300A_TX_BANK_SIZE] = {
	0x70, 0xE7, 0x12, 0x00, 0x02, 0x30, 0x00, 0x8A, 0x18, 0x3F, 0x7F
};

/*
 * Bit-banging SPI functions (based on working RP2040 implementation)
 */

/* GPIO pin control macros */
#define set_csb(d) gpiod_set_value((d)->csb, 1)
#define clr_csb(d) gpiod_set_value((d)->csb, 0)
#define set_fcsb(d) gpiod_set_value((d)->fcsb, 1)
#define clr_fcsb(d) gpiod_set_value((d)->fcsb, 0)
#define set_sclk(d) gpiod_set_value((d)->sclk, 1)
#define clr_sclk(d) gpiod_set_value((d)->sclk, 0)
#define set_sdio(d) gpiod_set_value((d)->sdio, 1)
#define clr_sdio(d) gpiod_set_value((d)->sdio, 0)
#define sdio_high(d) (gpiod_get_value((d)->sdio) == 1)

static void spi3_init(struct cmt2300a_data *data)
{
	gpiod_direction_output(data->csb, 1);
	gpiod_direction_output(data->fcsb, 1);
	gpiod_direction_output(data->sclk, 1);
	gpiod_direction_output(data->sdio, 1);

	set_csb(data);
	set_fcsb(data);
	set_sdio(data);
	clr_sclk(data);
}

/* Write a single byte to register (bit-banging) - matches RP2040 spi3_write_byte */
static void spi3_write_byte(struct cmt2300a_data *data, u8 dat)
{
	u8 i;

	set_fcsb(data);
	gpiod_direction_output(data->sdio, 1);
	set_sdio(data);
	clr_sclk(data);
	clr_csb(data);

	for (i = 8; i != 0; i--) {
		clr_sclk(data);
		if (dat & 0x80)
			set_sdio(data);
		else
			clr_sdio(data);
		set_sclk(data);
		dat <<= 1;
	}
	clr_sclk(data);
	set_sdio(data);
}

/* Read a single byte from register (bit-banging) - matches RP2040 spi3_read_byte */
static u8 spi3_read_byte(struct cmt2300a_data *data)
{
	u8 rd_para = 0;
	u8 i;

	clr_csb(data);
	gpiod_direction_input(data->sdio);

	for (i = 8; i != 0; i--) {
		clr_sclk(data);
		rd_para <<= 1;
		set_sclk(data);
		if (sdio_high(data))
			rd_para |= 0x01;
	}
	clr_sclk(data);
	gpiod_direction_output(data->sdio, 1);
	set_sdio(data);
	set_csb(data);

	return rd_para;
}

/* Write 16-bit value to register - matches RP2040 spi3_write */
static void spi3_write(struct cmt2300a_data *data, u16 dat)
{
	spi3_write_byte(data, (u8)(dat >> 8) & 0x7F);
	spi3_write_byte(data, (u8)dat);
	set_csb(data);
}

/* Read from register - matches RP2040 spi3_read */
static u8 spi3_read(struct cmt2300a_data *data, u8 addr)
{
	spi3_write_byte(data, addr | 0x80);
	return spi3_read_byte(data);
}

/* Write single byte to FIFO - matches RP2040 spi3_write_fifo with critical timing */
static void spi3_write_fifo(struct cmt2300a_data *data, u8 dat)
{
	u8 i;

	set_csb(data);
	gpiod_direction_output(data->sdio, 1);
	clr_sclk(data);

	/* Critical timing: delay before FCSB low */
	udelay(3);
	clr_fcsb(data);

	for (i = 8; i != 0; i--) {
		clr_sclk(data);
		if (dat & 0x80)
			set_sdio(data);
		else
			clr_sdio(data);
		set_sclk(data);
		dat <<= 1;
	}
	clr_sclk(data);

	/* Critical timing: longer delay before FCSB high for reliable FIFO write */
	udelay(4);
	set_fcsb(data);
	set_sdio(data);

	/* Critical timing: delay after FCSB high to ensure byte is latched */
	udelay(6);
}

/* Read single byte from FIFO - matches RP2040 spi3_read_fifo with critical timing */
static u8 spi3_read_fifo(struct cmt2300a_data *data)
{
	u8 rd_para = 0;
	u8 i;

	set_csb(data);
	gpiod_direction_input(data->sdio);
	clr_sclk(data);
	clr_fcsb(data);

	for (i = 8; i != 0; i--) {
		clr_sclk(data);
		rd_para <<= 1;
		set_sclk(data);
		if (sdio_high(data))
			rd_para |= 0x01;
	}

	clr_sclk(data);
	udelay(SPI3_SPEED);
	udelay(SPI3_SPEED);
	set_fcsb(data);
	gpiod_direction_output(data->sdio, 1);
	set_sdio(data);
	udelay(SPI3_SPEED);
	udelay(SPI3_SPEED);

	return rd_para;
}

/* Burst read from FIFO - matches RP2040 spi3_burst_read_fifo */
static void spi3_burst_read_fifo(struct cmt2300a_data *data, u8 ptr[],
				 u8 length)
{
	u8 i;

	if (length != 0) {
		for (i = 0; i < length; i++)
			ptr[i] = spi3_read_fifo(data);
	}
}

/* Burst write to FIFO - matches RP2040 spi3_burst_write_fifo */
static void spi3_burst_write_fifo(struct cmt2300a_data *data, u8 ptr[],
				  u8 length)
{
	u8 i;

	if (length != 0) {
		for (i = 0; i < length; i++)
			spi3_write_fifo(data, ptr[i]);
	}
}

/* State control functions - matches RP2040 implementation */

static bool go_tx(struct cmt2300a_data *data)
{
	u8 i, status;

	spi3_write(data, ((u16)CMT23_MODE_CTL << 8) + MODE_GO_TX);
	for (i = 0; i < 50; i++) {
		udelay(100);
		status = (MODE_MASK_STA & spi3_read(data, CMT23_MODE_STA));
		if (status == MODE_STA_TX)
			return true;
	}
	return false;
}

static bool go_rx(struct cmt2300a_data *data)
{
	u8 i, tmp;

	spi3_write(data, ((u16)CMT23_MODE_CTL << 8) + MODE_GO_RX);
	for (i = 0; i < 50; i++) {
		udelay(100);
		tmp = (MODE_MASK_STA & spi3_read(data, CMT23_MODE_STA));
		if (tmp == MODE_STA_RX)
			return true;
	}
	return false;
}

static bool __maybe_unused go_sleep(struct cmt2300a_data *data)
{
	u8 tmp;

	spi3_write(data, ((u16)CMT23_MODE_CTL << 8) + MODE_GO_SLEEP);
	udelay(100);
	tmp = (MODE_MASK_STA & spi3_read(data, CMT23_MODE_STA));
	return (tmp == MODE_STA_SLEEP);
}

static bool go_standby(struct cmt2300a_data *data)
{
	u8 i, tmp;

	spi3_write(data, ((u16)CMT23_MODE_CTL << 8) + MODE_GO_STBY);
	for (i = 0; i < 50; i++) {
		udelay(100);
		tmp = (MODE_MASK_STA & spi3_read(data, CMT23_MODE_STA));
		if (tmp == MODE_STA_STBY)
			return true;
	}
	return false;
}

static void soft_reset(struct cmt2300a_data *data)
{
	spi3_write(data, ((u16)CMT23_SOFTRST << 8) + 0xFF);
	usleep_range(1000, 2000);
}

static u8 __maybe_unused read_status(struct cmt2300a_data *data)
{
	return (MODE_MASK_STA & spi3_read(data, CMT23_MODE_STA));
}

static u8 read_rssi(struct cmt2300a_data *data, u8 unit_dbm)
{
	if (unit_dbm)
		return spi3_read(data, CMT23_RSSI_DBM);
	else
		return spi3_read(data, CMT23_RSSI_CODE);
}

/* GPIO & Interrupt functions - matches RP2040 implementation */

static void gpio_func_cfg(struct cmt2300a_data *data, u8 io_cfg)
{
	spi3_write(data, ((u16)CMT23_IO_SEL << 8) + io_cfg);
}

static void __maybe_unused int_src_cfg(struct cmt2300a_data *data, u8 int_1,
				       u8 int_2)
{
	u8 tmp;

	tmp = INT_MASK & spi3_read(data, CMT23_INT1_CTL);
	spi3_write(data, ((u16)CMT23_INT1_CTL << 8) + (tmp | int_1));

	tmp = INT_MASK & spi3_read(data, CMT23_INT2_CTL);
	spi3_write(data, ((u16)CMT23_INT2_CTL << 8) + (tmp | int_2));
}

static void __maybe_unused int1_src_cfg(struct cmt2300a_data *data, u8 int_1)
{
	u8 tmp = INT_MASK & spi3_read(data, CMT23_INT1_CTL);

	spi3_write(data, ((u16)CMT23_INT1_CTL << 8) + (tmp | int_1));
}

static void int2_src_cfg(struct cmt2300a_data *data, u8 int_2)
{
	u8 tmp = INT_MASK & spi3_read(data, CMT23_INT2_CTL);

	spi3_write(data, ((u16)CMT23_INT2_CTL << 8) + (tmp | int_2));
}

static void enable_ant_switch(struct cmt2300a_data *data, u8 mode)
{
	u8 tmp = spi3_read(data, CMT23_INT1_CTL);

	tmp &= 0x3F;
	switch (mode) {
	case 1:
		tmp |= RF_SWT1_EN;
		break;
	case 2:
		tmp |= RF_SWT2_EN;
		break;
	default:
		break;
	}
	spi3_write(data, ((u16)CMT23_INT1_CTL << 8) + tmp);
}

static void int_src_enable(struct cmt2300a_data *data, u8 en_int)
{
	spi3_write(data, ((u16)CMT23_INT_EN << 8) + en_int);
}

static u8 int_src_flag_clr(struct cmt2300a_data *data)
{
	spi3_write(data, ((u16)CMT23_INT_CLR1 << 8) + 0x07);
	spi3_write(data, ((u16)CMT23_INT_CLR2 << 8) + 0xFF);
	return 0;
}

static u8 __maybe_unused clear_fifo(struct cmt2300a_data *data)
{
	u8 tmp = spi3_read(data, CMT23_FIFO_FLG);

	spi3_write(data,
		   ((u16)CMT23_FIFO_CLR << 8) + FIFO_CLR_RX + FIFO_CLR_TX);
	return tmp;
}

static void enable_wr_fifo(struct cmt2300a_data *data)
{
	u8 tmp = spi3_read(data, CMT23_FIFO_CTL);

	tmp |= (SPI_FIFO_RD_WR_SEL | FIFO_RX_TX_SEL);
	spi3_write(data, ((u16)CMT23_FIFO_CTL << 8) + tmp);
}

static void enable_rd_fifo(struct cmt2300a_data *data)
{
	u8 tmp = spi3_read(data, CMT23_FIFO_CTL);

	tmp &= (~(SPI_FIFO_RD_WR_SEL | FIFO_RX_TX_SEL));
	spi3_write(data, ((u16)CMT23_FIFO_CTL << 8) + tmp);
}

static void __maybe_unused enable_din(struct cmt2300a_data *data, u8 pin)
{
	u8 tmp = 0x1F & spi3_read(data, CMT23_FIFO_CTL);

	spi3_write(data, ((u16)CMT23_FIFO_CTL << 8) + (tmp | DIN_EN | pin));
}

static void __maybe_unused disable_din(struct cmt2300a_data *data)
{
	u8 tmp = 0x1F & spi3_read(data, CMT23_FIFO_CTL);

	spi3_write(data, ((u16)CMT23_FIFO_CTL << 8) + (tmp | DIN_DIS));
}

static void __maybe_unused set_clock_div(struct cmt2300a_data *data, u8 div,
					 u8 enable)
{
	u8 tmp = spi3_read(data, CMT23_CLK_CTL);

	spi3_write(data, ((u16)CMT23_CLK_CTL << 8) +
				 ((tmp & 0xC0) | (enable << 5) | div));
}

/* Configuration functions - matches RP2040 implementation */

static void cmt_init(struct cmt2300a_data *data)
{
	u8 tmp;

	spi3_init(data);
	gpiod_direction_input(data->gpio1);
	gpiod_direction_input(data->gpio2);
	gpiod_direction_input(data->gpio3);

	soft_reset(data);
	go_standby(data);

	tmp = spi3_read(data, CMT23_MODE_STA);
	tmp |= EEP_CPY_DIS;
	tmp &= (~RSTN_IN_EN);
	spi3_write(data, ((u16)CMT23_MODE_STA << 8) + tmp);
	int_src_flag_clr(data);
}

static void cfg_bank(struct cmt2300a_data *data, u8 base_addr, const u8 cfg[],
		     u8 length)
{
	u8 i;

	if (length != 0) {
		for (i = 0; i < length; i++) {
			u16 reg_addr = base_addr + i;

			spi3_write(data, (u16)(reg_addr << 8) | cfg[i]);
		}
	}
}

/* Getter/setter functions for all register banks */

/* CMT bank */
static void get_cmt_bank(struct cmt2300a_data *data, u8 buf[])
{
	u8 i;

	for (i = 0; i < CMT2300A_CMT_BANK_SIZE; i++)
		buf[i] = spi3_read(data, CMT2300A_CMT_BANK_BASE + i);
}

static void set_cmt_bank(struct cmt2300a_data *data, const u8 buf[])
{
	cfg_bank(data, CMT2300A_CMT_BANK_BASE, buf, CMT2300A_CMT_BANK_SIZE);
}

/* System bank */
static void get_system_bank(struct cmt2300a_data *data, u8 buf[])
{
	u8 i;

	for (i = 0; i < CMT2300A_SYSTEM_BANK_SIZE; i++)
		buf[i] = spi3_read(data, CMT2300A_SYSTEM_BANK_BASE + i);
}

static void set_system_bank(struct cmt2300a_data *data, const u8 buf[])
{
	cfg_bank(data, CMT2300A_SYSTEM_BANK_BASE, buf,
		 CMT2300A_SYSTEM_BANK_SIZE);
}

/* Frequency bank */
static void get_freq_bank(struct cmt2300a_data *data, u8 buf[])
{
	u8 i;

	for (i = 0; i < CMT2300A_FREQ_BANK_SIZE; i++)
		buf[i] = spi3_read(data, CMT2300A_FREQ_BANK_BASE + i);
}

static void set_freq_bank(struct cmt2300a_data *data, const u8 buf[])
{
	cfg_bank(data, CMT2300A_FREQ_BANK_BASE, buf, CMT2300A_FREQ_BANK_SIZE);
}

/* Data rate bank */
static void get_datarate_bank(struct cmt2300a_data *data, u8 buf[])
{
	u8 i;

	for (i = 0; i < CMT2300A_DATARATE_BANK_SIZE; i++)
		buf[i] = spi3_read(data, CMT2300A_DATARATE_BANK_BASE + i);
}

static void set_datarate_bank(struct cmt2300a_data *data, const u8 buf[])
{
	cfg_bank(data, CMT2300A_DATARATE_BANK_BASE, buf,
		 CMT2300A_DATARATE_BANK_SIZE);
}

/* Baseband bank */
static void get_baseband_bank(struct cmt2300a_data *data, u8 buf[])
{
	u8 i;

	for (i = 0; i < CMT2300A_BASEBAND_BANK_SIZE; i++)
		buf[i] = spi3_read(data, CMT2300A_BASEBAND_BANK_BASE + i);
}

static void set_baseband_bank(struct cmt2300a_data *data, const u8 buf[])
{
	cfg_bank(data, CMT2300A_BASEBAND_BANK_BASE, buf,
		 CMT2300A_BASEBAND_BANK_SIZE);
}

/* TX bank */
static void get_tx_bank(struct cmt2300a_data *data, u8 buf[])
{
	u8 i;

	for (i = 0; i < CMT2300A_TX_BANK_SIZE; i++)
		buf[i] = spi3_read(data, CMT2300A_TX_BANK_BASE + i);
}

static void set_tx_bank(struct cmt2300a_data *data, const u8 buf[])
{
	cfg_bank(data, CMT2300A_TX_BANK_BASE, buf, CMT2300A_TX_BANK_SIZE);
}

static void init_config(struct cmt2300a_data *data)
{
	cfg_bank(data, CMT2300A_CMT_BANK_BASE, cmt2300a_cmt_bank,
		 CMT2300A_CMT_BANK_SIZE);
	cfg_bank(data, CMT2300A_SYSTEM_BANK_BASE, cmt2300a_system_bank,
		 CMT2300A_SYSTEM_BANK_SIZE);
	cfg_bank(data, CMT2300A_FREQ_BANK_BASE, cmt2300a_freq_bank,
		 CMT2300A_FREQ_BANK_SIZE);
	cfg_bank(data, CMT2300A_DATARATE_BANK_BASE, cmt2300a_datarate_bank,
		 CMT2300A_DATARATE_BANK_SIZE);
	cfg_bank(data, CMT2300A_BASEBAND_BANK_BASE, cmt2300a_baseband_bank,
		 CMT2300A_BASEBAND_BANK_SIZE);
	cfg_bank(data, CMT2300A_TX_BANK_BASE, cmt2300a_tx_bank,
		 CMT2300A_TX_BANK_SIZE);
}

/* Message functions - matches RP2040 implementation */

static void set_tx_payload_length(struct cmt2300a_data *data, u16 length)
{
	u8 tmp, len;

	tmp = spi3_read(data, CMT23_PKT_CTRL1);
	len = spi3_read(data, CMT23_PKT_LEN);
	tmp &= 0x8E;

	if (length != 0) {
		/* Assume variable packet length (fixed_pkt_length = false) */
		tmp |= (1 << 0);
		len = length;
	} else {
		len = 0;
	}

	tmp |= (((u8)(len >> 8) & 0x07) << 4);
	spi3_write(data, ((u16)CMT23_PKT_CTRL1 << 8) + tmp);
	spi3_write(data, ((u16)CMT23_PKT_LEN << 8) + (u8)len);
}

static u8 get_message(struct cmt2300a_data *data, u8 msg[])
{
	u8 i;

	enable_rd_fifo(data);

	/* For now, assume variable packet length (fixed_pkt_length = false) */
	i = spi3_read_fifo(data);
	spi3_burst_read_fifo(data, msg, i);

	return i;
}

/* Send message - matches RP2040 send_message() */
static bool send_message(struct cmt2300a_data *data, u8 msg[], u8 length)
{
	int_src_flag_clr(data);
	go_standby(data);
	set_tx_payload_length(data, length);
	enable_wr_fifo(data);
	spi3_burst_write_fifo(data, msg, length);
	go_tx(data);
	return true;
}

/* Initialize RX mode - matches RP2040 start_rx() exactly */
static void start_rx(struct cmt2300a_data *data)
{
	u8 tmp;

	/* First disable low power mode */
	tmp = spi3_read(data, CMT23_DUTY_CTL) & DUTY_MASK;
	spi3_write(data, ((u16)CMT23_DUTY_CTL << 8) | tmp);

	/* IRQ Mapping - INT2 for PKT_DONE */
	int2_src_cfg(data, INT_PKT_DONE);

	/* Enable interrupts: CRC_PASS, PKT_DONE, PREAMBLE_PASS, SYNC_PASS */
	int_src_enable(data, CRC_PASS_EN | PKT_DONE_EN | PREAMBLE_PASS_EN |
				     SYNC_PASS_EN);

	/* Clear interrupt flags */
	int_src_flag_clr(data);

	/* Enter RX mode */
	if (go_rx(data)) {
#ifdef DEBUG_CMT2300A
		dev_info(&data->spi->dev, "RX mode ready\n");
#endif
	} else {
		dev_err(&data->spi->dev, "Failed to enter RX mode\n");
	}
}

/* IRQ handler - matches RP2040 polling logic, supports both RX and TX */
static irqreturn_t cmt2300a_irq_handler(int irq, void *dev_id)
{
	struct cmt2300a_data *data = dev_id;
	u8 flg, flg_clr1, tmp;
	s8 rssi;

	/* Check if GPIO3 is high (matches RP2040's GPO3_H() check) */
	if (!data->gpio3 || !gpiod_get_value(data->gpio3))
		return IRQ_HANDLED;

	/* Read interrupt flags */
	flg = spi3_read(data, CMT23_INT_FLG);
	flg_clr1 = spi3_read(data, CMT23_INT_CLR1);

	/* Check for TX_DONE */
	if (flg_clr1 & TX_DONE_FLAG) {
#ifdef DEBUG_CMT2300A
		dev_info(&data->spi->dev, "TX: completed, INT_CLR1: 0x%02X\n",
			 flg_clr1);
#endif
		/* Wake up write() */
		data->tx_wait_event = 1;
		wake_up_interruptible(&data->tx_wait_queue);

		/* Clear interrupt flags */
		int_src_flag_clr(data);
		return IRQ_HANDLED;
	}

	/* Handle RX packet */
	if (flg & RX_DONE_FLAG) {
		/* Read RSSI */
		rssi = read_rssi(data, 1) - 128;

		/* Get message from FIFO */
		tmp = get_message(data, data->rx_packet);

		if (tmp > 0 && tmp <= sizeof(data->rx_packet)) {
			data->rx_packet_len = tmp;
			data->last_rssi = rssi;
			data->rx_packets++;

			/* Wake up read() */
			data->rx_wait_event = 1;
			wake_up_interruptible(&data->rx_wait_queue);

#ifdef DEBUG_CMT2300A
			/* Debug log (matches RP2040's Serial.printf) */
			dev_info(
				&data->spi->dev,
				"RX: %d bytes, RSSI: %d dBm, INT_FLG: 0x%02X\n",
				tmp, rssi, flg);
#endif
		}
	}

	/* Clear interrupt flags */
	int_src_flag_clr(data);

	return IRQ_HANDLED;
}

/*
 * Character device file operations
 */
static int cmt2300a_cdev_open(struct inode *inode, struct file *filp)
{
	struct cmt2300a_data *data;

	data = container_of(filp->private_data, struct cmt2300a_data, misc_dev);
	filp->private_data = data;

	return 0;
}

static int cmt2300a_cdev_release(struct inode *inode, struct file *filp)
{
	return 0;
}

static ssize_t cmt2300a_cdev_read(struct file *filp, char __user *buf,
				  size_t count, loff_t *ppos)
{
	struct cmt2300a_data *data = filp->private_data;
	size_t u8s_to_copy;
	int ret;

	ret = wait_event_interruptible(data->rx_wait_queue,
				       data->rx_wait_event);
	if (ret)
		return ret;

	u8s_to_copy = min(count, data->rx_packet_len);
	if (u8s_to_copy > sizeof(data->rx_packet))
		u8s_to_copy = sizeof(data->rx_packet);

	ret = copy_to_user(buf, data->rx_packet, u8s_to_copy);
	if (ret)
		return -EFAULT;

	data->rx_wait_event = 0;

	return u8s_to_copy;
}

/* Write handler for TX - matches RP2040 TX flow */
static ssize_t cmt2300a_cdev_write(struct file *filp, const char __user *buf,
				   size_t count, loff_t *ppos)
{
	struct cmt2300a_data *data = filp->private_data;
	unsigned long timeout;
	int ret;

	if (count == 0)
		return 0;

	if (count > sizeof(data->tx_data))
		return -EINVAL;

	ret = mutex_lock_interruptible(&data->tx_lock);
	if (ret)
		return ret;

	ret = copy_from_user(data->tx_data, buf, count);
	if (ret) {
		mutex_unlock(&data->tx_lock);
		return -EFAULT;
	}

	/* Reset TX wait event */
	data->tx_wait_event = 0;

	/* Configure for TX mode - matches RP2040 TX setup */
	int2_src_cfg(data, INT_TX_DONE);
	int_src_enable(data, TX_DONE_EN);

	/* Send message - matches RP2040 TX sequence */
	send_message(data, data->tx_data, count);

	/* Wait for TX_DONE with 1 second timeout (matches RP2040) */
	timeout = msecs_to_jiffies(1000);
	ret = wait_event_interruptible_timeout(data->tx_wait_queue,
					       data->tx_wait_event, timeout);

	if (ret == 0) {
		dev_err(&data->spi->dev, "TX: timeout\n");
		/* Restore RX configuration */
		int2_src_cfg(data, INT_PKT_DONE);
		int_src_enable(data, CRC_PASS_EN | PKT_DONE_EN |
					     PREAMBLE_PASS_EN | SYNC_PASS_EN);
		mutex_unlock(&data->tx_lock);
		return -ETIMEDOUT;
	} else if (ret < 0) {
		/* Restore RX configuration */
		int2_src_cfg(data, INT_PKT_DONE);
		int_src_enable(data, CRC_PASS_EN | PKT_DONE_EN |
					     PREAMBLE_PASS_EN | SYNC_PASS_EN);
		mutex_unlock(&data->tx_lock);
		return ret;
	}

	/* Go back to standby after TX (matches RP2040) */
	go_standby(data);

	/* Restore RX configuration - matches RP2040 start_rx() */
	int2_src_cfg(data, INT_PKT_DONE);
	int_src_enable(data, CRC_PASS_EN | PKT_DONE_EN | PREAMBLE_PASS_EN |
				     SYNC_PASS_EN);
	int_src_flag_clr(data);

	/* Go back to RX mode */
	if (!go_rx(data))
		dev_err(&data->spi->dev,
			"Failed to return to RX mode after TX\n");

	/* Increment TX counter */
	data->tx_packets++;

	mutex_unlock(&data->tx_lock);
	return count;
}

static const struct file_operations cmt2300a_fops = {
	.owner = THIS_MODULE,
	.open = cmt2300a_cdev_open,
	.release = cmt2300a_cdev_release,
	.read = cmt2300a_cdev_read,
	.write = cmt2300a_cdev_write,
};

/*
 * Sysfs attributes for runtime configuration
 */
static ssize_t rssi_show(struct device *dev, struct device_attribute *attr,
			 char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);

	return sysfs_emit(buf, "%d\n", data->last_rssi);
}
static DEVICE_ATTR_RO(rssi);

static ssize_t rx_packets_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);

	return sysfs_emit(buf, "%lu\n", data->rx_packets);
}
static DEVICE_ATTR_RO(rx_packets);

static ssize_t tx_packets_show(struct device *dev,
			       struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);

	return sysfs_emit(buf, "%lu\n", data->tx_packets);
}
static DEVICE_ATTR_RO(tx_packets);

/* Helper function to parse space-separated hex values */
static int parse_hex_array(const char *buf, u8 *array, size_t array_size,
			   struct device *dev)
{
	int values_read = 0;
	const char *p = buf;
	char token[16];
	int token_len;
	unsigned long val;
	int ret;

	while (*p && values_read < array_size) {
		/* Skip whitespace */
		while (*p && (*p == ' ' || *p == '\t' || *p == '\n'))
			p++;

		if (!*p)
			break;

		/* Extract token */
		token_len = 0;
		while (*p && *p != ' ' && *p != '\t' && *p != '\n' &&
		       token_len < sizeof(token) - 1)
			token[token_len++] = *p++;
		token[token_len] = '\0';

		if (token_len == 0)
			break;

		/* Parse token */
		ret = kstrtoul(token, 0, &val);
		if (ret) {
			dev_err(dev, "Invalid value '%s' at position %d\n",
				token, values_read);
			return ret;
		}

		if (val > 0xFF) {
			dev_err(dev, "Value 0x%lx out of range at position %d\n",
				val, values_read);
			return -EINVAL;
		}

		array[values_read++] = (u8)val;
	}

	return values_read;
}

static ssize_t cmt_bank_show(struct device *dev, struct device_attribute *attr,
			     char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	ssize_t len = 0;
	u8 i;

	/* Read current values from hardware */
	get_cmt_bank(data, data->cmt_bank);

	/* Format as space-separated hex values */
	for (i = 0; i < CMT2300A_CMT_BANK_SIZE; i++) {
		len += sysfs_emit_at(buf, len, "0x%02x", data->cmt_bank[i]);
		if (i < CMT2300A_CMT_BANK_SIZE - 1)
			len += sysfs_emit_at(buf, len, " ");
	}
	len += sysfs_emit_at(buf, len, "\n");

	return len;
}

static ssize_t cmt_bank_store(struct device *dev, struct device_attribute *attr,
			      const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	u8 new_bank[CMT2300A_CMT_BANK_SIZE];
	int values_read;

	values_read = parse_hex_array(buf, new_bank, CMT2300A_CMT_BANK_SIZE, dev);
	if (values_read < 0)
		return values_read;

	if (values_read != CMT2300A_CMT_BANK_SIZE) {
		dev_err(dev, "Expected %d values, got %d\n",
			CMT2300A_CMT_BANK_SIZE, values_read);
		return -EINVAL;
	}

	/* Write to hardware and update cached copy */
	set_cmt_bank(data, new_bank);
	memcpy(data->cmt_bank, new_bank, CMT2300A_CMT_BANK_SIZE);

#ifdef DEBUG_CMT2300A
	dev_info(dev, "CMT bank updated successfully\n");
#endif
	return count;
}
static DEVICE_ATTR_RW(cmt_bank);

/* System bank sysfs attribute */
static ssize_t system_bank_show(struct device *dev,
				struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	ssize_t len = 0;
	u8 i;

	get_system_bank(data, data->system_bank);
	for (i = 0; i < CMT2300A_SYSTEM_BANK_SIZE; i++) {
		len += sysfs_emit_at(buf, len, "0x%02x", data->system_bank[i]);
		if (i < CMT2300A_SYSTEM_BANK_SIZE - 1)
			len += sysfs_emit_at(buf, len, " ");
	}
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t system_bank_store(struct device *dev,
				 struct device_attribute *attr, const char *buf,
				 size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	u8 new_bank[CMT2300A_SYSTEM_BANK_SIZE];
	int values_read;

	values_read = parse_hex_array(buf, new_bank, CMT2300A_SYSTEM_BANK_SIZE, dev);
	if (values_read < 0)
		return values_read;

	if (values_read != CMT2300A_SYSTEM_BANK_SIZE) {
		dev_err(dev, "Expected %d values, got %d\n",
			CMT2300A_SYSTEM_BANK_SIZE, values_read);
		return -EINVAL;
	}

	set_system_bank(data, new_bank);
	memcpy(data->system_bank, new_bank, CMT2300A_SYSTEM_BANK_SIZE);
#ifdef DEBUG_CMT2300A
	dev_info(dev, "System bank updated successfully\n");
#endif
	return count;
}
static DEVICE_ATTR_RW(system_bank);

/* Frequency bank sysfs attribute */
static ssize_t freq_bank_show(struct device *dev, struct device_attribute *attr,
			      char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	ssize_t len = 0;
	u8 i;

	get_freq_bank(data, data->freq_bank);
	for (i = 0; i < CMT2300A_FREQ_BANK_SIZE; i++) {
		len += sysfs_emit_at(buf, len, "0x%02x", data->freq_bank[i]);
		if (i < CMT2300A_FREQ_BANK_SIZE - 1)
			len += sysfs_emit_at(buf, len, " ");
	}
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t freq_bank_store(struct device *dev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	u8 new_bank[CMT2300A_FREQ_BANK_SIZE];
	int values_read;

	values_read = parse_hex_array(buf, new_bank, CMT2300A_FREQ_BANK_SIZE, dev);
	if (values_read < 0)
		return values_read;

	if (values_read != CMT2300A_FREQ_BANK_SIZE) {
		dev_err(dev, "Expected %d values, got %d\n",
			CMT2300A_FREQ_BANK_SIZE, values_read);
		return -EINVAL;
	}

	set_freq_bank(data, new_bank);
	memcpy(data->freq_bank, new_bank, CMT2300A_FREQ_BANK_SIZE);
#ifdef DEBUG_CMT2300A
	dev_info(dev, "Frequency bank updated successfully\n");
#endif
	return count;
}
static DEVICE_ATTR_RW(freq_bank);

/* Data rate bank sysfs attribute */
static ssize_t datarate_bank_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	ssize_t len = 0;
	u8 i;

	get_datarate_bank(data, data->datarate_bank);
	for (i = 0; i < CMT2300A_DATARATE_BANK_SIZE; i++) {
		len += sysfs_emit_at(buf, len, "0x%02x",
				     data->datarate_bank[i]);
		if (i < CMT2300A_DATARATE_BANK_SIZE - 1)
			len += sysfs_emit_at(buf, len, " ");
	}
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t datarate_bank_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	u8 new_bank[CMT2300A_DATARATE_BANK_SIZE];
	int values_read;

	values_read = parse_hex_array(buf, new_bank, CMT2300A_DATARATE_BANK_SIZE, dev);
	if (values_read < 0)
		return values_read;

	if (values_read != CMT2300A_DATARATE_BANK_SIZE) {
		dev_err(dev, "Expected %d values, got %d\n",
			CMT2300A_DATARATE_BANK_SIZE, values_read);
		return -EINVAL;
	}

	set_datarate_bank(data, new_bank);
	memcpy(data->datarate_bank, new_bank, CMT2300A_DATARATE_BANK_SIZE);
#ifdef DEBUG_CMT2300A
	dev_info(dev, "Data rate bank updated successfully\n");
#endif
	return count;
}
static DEVICE_ATTR_RW(datarate_bank);

/* Baseband bank sysfs attribute */
static ssize_t baseband_bank_show(struct device *dev,
				  struct device_attribute *attr, char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	ssize_t len = 0;
	u8 i;

	get_baseband_bank(data, data->baseband_bank);
	for (i = 0; i < CMT2300A_BASEBAND_BANK_SIZE; i++) {
		len += sysfs_emit_at(buf, len, "0x%02x",
				     data->baseband_bank[i]);
		if (i < CMT2300A_BASEBAND_BANK_SIZE - 1)
			len += sysfs_emit_at(buf, len, " ");
	}
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t baseband_bank_store(struct device *dev,
				   struct device_attribute *attr,
				   const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	u8 new_bank[CMT2300A_BASEBAND_BANK_SIZE];
	int values_read;

	values_read = parse_hex_array(buf, new_bank, CMT2300A_BASEBAND_BANK_SIZE, dev);
	if (values_read < 0)
		return values_read;

	if (values_read != CMT2300A_BASEBAND_BANK_SIZE) {
		dev_err(dev, "Expected %d values, got %d\n",
			CMT2300A_BASEBAND_BANK_SIZE, values_read);
		return -EINVAL;
	}

	set_baseband_bank(data, new_bank);
	memcpy(data->baseband_bank, new_bank, CMT2300A_BASEBAND_BANK_SIZE);
#ifdef DEBUG_CMT2300A
	dev_info(dev, "Baseband bank updated successfully\n");
#endif
	return count;
}
static DEVICE_ATTR_RW(baseband_bank);

/* TX bank sysfs attribute */
static ssize_t tx_bank_show(struct device *dev, struct device_attribute *attr,
			    char *buf)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	ssize_t len = 0;
	u8 i;

	get_tx_bank(data, data->tx_bank);
	for (i = 0; i < CMT2300A_TX_BANK_SIZE; i++) {
		len += sysfs_emit_at(buf, len, "0x%02x", data->tx_bank[i]);
		if (i < CMT2300A_TX_BANK_SIZE - 1)
			len += sysfs_emit_at(buf, len, " ");
	}
	len += sysfs_emit_at(buf, len, "\n");
	return len;
}

static ssize_t tx_bank_store(struct device *dev, struct device_attribute *attr,
			     const char *buf, size_t count)
{
	struct spi_device *spi = to_spi_device(dev);
	struct cmt2300a_data *data = spi_get_drvdata(spi);
	u8 new_bank[CMT2300A_TX_BANK_SIZE];
	int values_read;

	values_read = parse_hex_array(buf, new_bank, CMT2300A_TX_BANK_SIZE, dev);
	if (values_read < 0)
		return values_read;

	if (values_read != CMT2300A_TX_BANK_SIZE) {
		dev_err(dev, "Expected %d values, got %d\n",
			CMT2300A_TX_BANK_SIZE, values_read);
		return -EINVAL;
	}

	set_tx_bank(data, new_bank);
	memcpy(data->tx_bank, new_bank, CMT2300A_TX_BANK_SIZE);
#ifdef DEBUG_CMT2300A
	dev_info(dev, "TX bank updated successfully\n");
#endif
	return count;
}
static DEVICE_ATTR_RW(tx_bank);

static struct attribute *cmt2300a_attrs[] = {
	&dev_attr_rssi.attr,	      &dev_attr_rx_packets.attr,
	&dev_attr_tx_packets.attr,    &dev_attr_cmt_bank.attr,
	&dev_attr_system_bank.attr,   &dev_attr_freq_bank.attr,
	&dev_attr_datarate_bank.attr, &dev_attr_baseband_bank.attr,
	&dev_attr_tx_bank.attr,	      NULL,
};
ATTRIBUTE_GROUPS(cmt2300a);

/* Probe function - initializes driver matching RP2040 setup() flow */
static int cmt2300a_probe(struct spi_device *spi)
{
	struct cmt2300a_data *data;
	int ret;

	dev_info(&spi->dev, "CMT2300A probe starting\n");

	data = devm_kzalloc(&spi->dev, sizeof(*data), GFP_KERNEL);
	if (!data)
		return -ENOMEM;

	data->spi = spi;
	spi_set_drvdata(spi, data);

	/* Get GPIO descriptors for bit-banged SPI */
	data->csb = devm_gpiod_get(&spi->dev, "csb", GPIOD_OUT_HIGH);
	if (IS_ERR(data->csb))
		return dev_err_probe(&spi->dev, PTR_ERR(data->csb),
				     "Failed to get CSB GPIO\n");

	data->fcsb = devm_gpiod_get(&spi->dev, "fcsb", GPIOD_OUT_HIGH);
	if (IS_ERR(data->fcsb))
		return dev_err_probe(&spi->dev, PTR_ERR(data->fcsb),
				     "Failed to get FCSB GPIO\n");

	data->sclk = devm_gpiod_get(&spi->dev, "sclk", GPIOD_OUT_HIGH);
	if (IS_ERR(data->sclk))
		return dev_err_probe(&spi->dev, PTR_ERR(data->sclk),
				     "Failed to get SCLK GPIO\n");

	data->sdio = devm_gpiod_get(&spi->dev, "sdio", GPIOD_OUT_HIGH);
	if (IS_ERR(data->sdio))
		return dev_err_probe(&spi->dev, PTR_ERR(data->sdio),
				     "Failed to get SDIO GPIO\n");

	/* Get GPIO descriptors for status monitoring */
	data->gpio1 = devm_gpiod_get_optional(&spi->dev, "gpio1", GPIOD_IN);
	data->gpio2 = devm_gpiod_get_optional(&spi->dev, "gpio2", GPIOD_IN);
	data->gpio3 = devm_gpiod_get(&spi->dev, "gpio3", GPIOD_IN);
	if (IS_ERR(data->gpio3))
		return dev_err_probe(&spi->dev, PTR_ERR(data->gpio3),
				     "Failed to get GPIO3 (IRQ pin)\n");

	/* Initialize wait queues */
	init_waitqueue_head(&data->rx_wait_queue);
	init_waitqueue_head(&data->tx_wait_queue);
	mutex_init(&data->tx_lock);

	/* Initialize chip - matches RP2040 init_2300a() */
	cmt_init(data);
	init_config(data);

	/* Initialize runtime register bank copies from default values */
	memcpy(data->cmt_bank, cmt2300a_cmt_bank, CMT2300A_CMT_BANK_SIZE);
	memcpy(data->system_bank, cmt2300a_system_bank,
	       CMT2300A_SYSTEM_BANK_SIZE);
	memcpy(data->freq_bank, cmt2300a_freq_bank, CMT2300A_FREQ_BANK_SIZE);
	memcpy(data->datarate_bank, cmt2300a_datarate_bank,
	       CMT2300A_DATARATE_BANK_SIZE);
	memcpy(data->baseband_bank, cmt2300a_baseband_bank,
	       CMT2300A_BASEBAND_BANK_SIZE);
	memcpy(data->tx_bank, cmt2300a_tx_bank, CMT2300A_TX_BANK_SIZE);

	enable_ant_switch(data, 1);
	gpio_func_cfg(data, GPIO3_INT2);

	/* Disable low power mode */
	spi3_write(data, ((u16)CMT23_DUTY_CTL << 8) |
				 (spi3_read(data, CMT23_DUTY_CTL) & DUTY_MASK));

	/* Setup IRQ on GPIO3 */
	data->irq = gpiod_to_irq(data->gpio3);
	if (data->irq < 0) {
		dev_err(&spi->dev, "Failed to get IRQ from GPIO3\n");
		return data->irq;
	}

	ret = devm_request_threaded_irq(&spi->dev, data->irq, NULL,
					cmt2300a_irq_handler,
					IRQF_TRIGGER_RISING | IRQF_ONESHOT,
					"cmt2300a", data);
	if (ret) {
		dev_err(&spi->dev, "Failed to request IRQ: %d\n", ret);
		return ret;
	}

	/* Register character device */
	data->misc_dev.minor = MISC_DYNAMIC_MINOR;
	data->misc_dev.name = SUBG_DEV_NAME "00";
	data->misc_dev.fops = &cmt2300a_fops;
	data->misc_dev.parent = &spi->dev;

	ret = misc_register(&data->misc_dev);
	if (ret) {
		dev_err(&spi->dev, "Failed to register misc device: %d\n", ret);
		return ret;
	}

	/* Start RX mode - matches RP2040 start_rx() */
	start_rx(data);

	dev_info(&spi->dev, "CMT2300A initialized successfully\n");
	return 0;
}

static void cmt2300a_remove(struct spi_device *spi)
{
	struct cmt2300a_data *data = spi_get_drvdata(spi);

	/* Unregister character device */
	misc_deregister(&data->misc_dev);
}

static const struct of_device_id cmt2300a_of_match[] = {
	{ .compatible = "cmostek,cmt2300a" },
	{}
};
MODULE_DEVICE_TABLE(of, cmt2300a_of_match);

static const struct spi_device_id cmt2300a_id[] = { { "cmt2300a", 0 }, {} };
MODULE_DEVICE_TABLE(spi, cmt2300a_id);

static struct spi_driver cmt2300a_driver = {
	.driver = {
		.name = "cmt2300a",
		.of_match_table = cmt2300a_of_match,
		.dev_groups = cmt2300a_groups,
	},
	.probe = cmt2300a_probe,
	.remove = cmt2300a_remove,
	.id_table = cmt2300a_id,
};

module_spi_driver(cmt2300a_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("CMOSTEK SZ");
MODULE_AUTHOR("Tribo Consulting");
MODULE_AUTHOR("Dhiru Kholia <dhiru@openwall.com>");
MODULE_DESCRIPTION(
	"CMT2300A Sub-GHz Radio Transceiver Driver with Interrupt-based TX/RX");
MODULE_VERSION("2.0");
