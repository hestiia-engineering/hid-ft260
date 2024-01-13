// SPDX-License-Identifier: GPL-2.0-only
/*
 * hid-ft260.c - FTDI FT260 USB HID to I2C host bridge
 *
 * Copyright (c) 2021, Michael Zaidman <michaelz@xsightlabs.com>
 *
 * Data Sheet:
 *   https://www.ftdichip.com/Support/Documents/DataSheets/ICs/DS_FT260.pdf
 */

#include "hid-ids.h"
#include <linux/hidraw.h>
#include <linux/i2c.h>
#include <linux/module.h>
#include <linux/usb.h>
#include <linux/serial.h>
#include <linux/serial_core.h>
#include <linux/kfifo.h>
#include <linux/tty_flip.h>
#include <linux/minmax.h>
#include <asm/unaligned.h> /* Needed for cpu_to_le16, le16_to_cpu */

#define UART_COUNT_MAX		4	/* Number of UARTs this driver can handle */
#define FIFO_SIZE	256
#define TTY_WAKEUP_WATERMARK	(FIFO_SIZE / 2)

#ifdef DEBUG
static int ft260_debug = 1;
#else
static int ft260_debug;
#endif
module_param_named(debug, ft260_debug, int, 0600);
MODULE_PARM_DESC(debug, "Toggle FT260 debugging messages");

#define ft260_dbg(format, arg...)					  \
	do {								  \
		if (ft260_debug)					  \
			pr_info("%s: " format, __func__, ##arg);	  \
	} while (0)

#define FT260_REPORT_MAX_LENGTH (64)
#define FT260_I2C_DATA_REPORT_ID(len) (FT260_I2C_REPORT_MIN + (len - 1) / 4)
#define FT260_UART_DATA_REPORT_ID(len) (FT260_UART_REPORT_MIN + (len - 1) / 4)

#define FT260_WAKEUP_NEEDED_AFTER_MS (4800) /* 5s minus 200ms margin */

/*
 * The ft260 input report format defines 62 bytes for the data payload, but
 * when requested 62 bytes, the controller returns 60 and 2 in separate input
 * reports. To achieve better performance with the multi-report read data
 * transfers, we set the maximum read payload length to a multiple of 60.
 * With a 100 kHz I2C clock, one 240 bytes read takes about 1/27 second,
 * which is excessive; On the other hand, some higher layer drivers like at24
 * or optoe limit the i2c reads to 128 bytes. To not block other drivers out
 * of I2C for potentially troublesome amounts of time, we select the maximum
 * read payload length to be 180 bytes.
 */
#define FT260_RD_DATA_MAX (180)
#define FT260_WR_DATA_MAX (60)

/*
 * Device interface configuration.
 * The FT260 has 2 interfaces that are controlled by DCNF0 and DCNF1 pins.
 * First implementes USB HID to I2C bridge function and
 * second - USB HID to UART bridge function.
 */
enum {
	FT260_MODE_ALL			= 0x00,
	FT260_MODE_I2C			= 0x01,
	FT260_MODE_UART			= 0x02,
	FT260_MODE_BOTH			= 0x03,
};

/* Control pipe */
enum {
	FT260_GET_RQST_TYPE		= 0xA1,
	FT260_GET_REPORT		= 0x01,
	FT260_SET_RQST_TYPE		= 0x21,
	FT260_SET_REPORT		= 0x09,
	FT260_FEATURE			= 0x03,
};

/* Report IDs / Feature In */
enum {
	FT260_CHIP_VERSION		= 0xA0,
	FT260_SYSTEM_SETTINGS		= 0xA1,
	FT260_I2C_STATUS		= 0xC0,
	FT260_I2C_READ_REQ		= 0xC2,
	FT260_I2C_REPORT_MIN		= 0xD0,
	FT260_I2C_REPORT_MAX		= 0xDE,
	FT260_GPIO			= 0xB0,
	FT260_UART_INTERRUPT_STATUS	= 0xB1,
	FT260_UART_STATUS		= 0xE0,
	FT260_UART_RI_DCD_STATUS	= 0xE1,
	FT260_UART_REPORT_MIN		= 0xF0,
	FT260_UART_REPORT_MAX		= 0xFE,
};

/* Feature Out */
enum {
	FT260_SET_CLOCK			= 0x01,
	FT260_SET_I2C_MODE		= 0x02,
	FT260_SET_UART_MODE		= 0x03,
	FT260_ENABLE_INTERRUPT		= 0x05,
	FT260_SELECT_GPIO2_FUNC		= 0x06,
	FT260_ENABLE_UART_DCD_RI	= 0x07,
	FT260_SELECT_GPIOA_FUNC		= 0x08,
	FT260_SELECT_GPIOG_FUNC		= 0x09,
	FT260_SET_INTERRUPT_TRIGGER	= 0x0A,
	FT260_SET_SUSPEND_OUT_POLAR	= 0x0B,
	FT260_ENABLE_UART_RI_WAKEUP	= 0x0C,
	FT260_SET_UART_RI_WAKEUP_CFG	= 0x0D,
	FT260_SET_I2C_RESET		= 0x20,
	FT260_SET_I2C_CLOCK_SPEED	= 0x22,
	FT260_SET_UART_RESET		= 0x40,
	FT260_SET_UART_CONFIG		= 0x41,
	FT260_SET_UART_BAUD_RATE	= 0x42,
	FT260_SET_UART_DATA_BIT		= 0x43,
	FT260_SET_UART_PARITY		= 0x44,
	FT260_SET_UART_STOP_BIT		= 0x45,
	FT260_SET_UART_BREAKING		= 0x46,
	FT260_SET_UART_XON_XOFF		= 0x49,
};

/* Response codes in I2C status report */
enum {
	FT260_I2C_STATUS_SUCCESS	= 0x00,
	FT260_I2C_STATUS_CTRL_BUSY	= 0x01,
	FT260_I2C_STATUS_ERROR		= 0x02,
	FT260_I2C_STATUS_ADDR_NO_ACK	= 0x04,
	FT260_I2C_STATUS_DATA_NO_ACK	= 0x08,
	FT260_I2C_STATUS_ARBITR_LOST	= 0x10,
	FT260_I2C_STATUS_CTRL_IDLE	= 0x20,
	FT260_I2C_STATUS_BUS_BUSY	= 0x40,
};

/* I2C Conditions flags */
enum {
	FT260_FLAG_NONE			= 0x00,
	FT260_FLAG_START		= 0x02,
	FT260_FLAG_START_REPEATED	= 0x03,
	FT260_FLAG_STOP			= 0x04,
	FT260_FLAG_START_STOP		= 0x06,
	FT260_FLAG_START_STOP_REPEATED	= 0x07,
};

/* Return values for ft260_get_interface_type func */
enum {
	FT260_IFACE_NONE,
	FT260_IFACE_I2C,
	FT260_IFACE_UART
};

#define FT260_SET_REQUEST_VALUE(report_id) ((FT260_FEATURE << 8) | report_id)

/* Feature In reports */

struct ft260_get_chip_version_report {
	u8 report;		/* FT260_CHIP_VERSION */
	u8 chip_code[4];	/* FTDI chip identification code */
	u8 reserved[8];
} __packed;

struct ft260_get_system_status_report {
	u8 report;		/* FT260_SYSTEM_SETTINGS */
	u8 chip_mode;		/* DCNF0 and DCNF1 status, bits 0-1 */
	u8 clock_ctl;		/* 0 - 12MHz, 1 - 24MHz, 2 - 48MHz */
	u8 suspend_status;	/* 0 - not suspended, 1 - suspended */
	u8 pwren_status;	/* 0 - FT260 is not ready, 1 - ready */
	u8 i2c_enable;		/* 0 - disabled, 1 - enabled */
	u8 uart_mode;		/* 0 - OFF; 1 - RTS_CTS, 2 - DTR_DSR, */
				/* 3 - XON_XOFF, 4 - No flow control */
	u8 hid_over_i2c_en;	/* 0 - disabled, 1 - enabled */
	u8 gpio2_function;	/* 0 - GPIO,  1 - SUSPOUT, */
				/* 2 - PWREN, 4 - TX_LED */
	u8 gpioA_function;	/* 0 - GPIO, 3 - TX_ACTIVE, 4 - TX_LED */
	u8 gpioG_function;	/* 0 - GPIO, 2 - PWREN, */
				/* 5 - RX_LED, 6 - BCD_DET */
	u8 suspend_out_pol;	/* 0 - active-high, 1 - active-low */
	u8 enable_wakeup_int;	/* 0 - disabled, 1 - enabled */
	u8 intr_cond;		/* Interrupt trigger conditions */
	u8 power_saving_en;	/* 0 - disabled, 1 - enabled */
	u8 reserved[10];
} __packed;

struct ft260_get_i2c_status_report {
	u8 report;		/* FT260_I2C_STATUS */
	u8 bus_status;		/* I2C bus status */
	__le16 clock;		/* I2C bus clock in range 60-3400 KHz */
	u8 reserved;
} __packed;

/* Feature Out reports */

struct ft260_set_system_clock_report {
	u8 report;		/* FT260_SYSTEM_SETTINGS */
	u8 request;		/* FT260_SET_CLOCK */
	u8 clock_ctl;		/* 0 - 12MHz, 1 - 24MHz, 2 - 48MHz */
} __packed;

struct ft260_set_i2c_mode_report {
	u8 report;		/* FT260_SYSTEM_SETTINGS */
	u8 request;		/* FT260_SET_I2C_MODE */
	u8 i2c_enable;		/* 0 - disabled, 1 - enabled */
} __packed;

struct ft260_set_uart_mode_report {
	u8 report;		/* FT260_SYSTEM_SETTINGS */
	u8 request;		/* FT260_SET_UART_MODE */
	u8 uart_mode;		/* 0 - OFF; 1 - RTS_CTS, 2 - DTR_DSR, */
				/* 3 - XON_XOFF, 4 - No flow control */
} __packed;

struct ft260_set_i2c_reset_report {
	u8 report;		/* FT260_SYSTEM_SETTINGS */
	u8 request;		/* FT260_SET_I2C_RESET */
} __packed;

struct ft260_set_i2c_speed_report {
	u8 report;		/* FT260_SYSTEM_SETTINGS */
	u8 request;		/* FT260_SET_I2C_CLOCK_SPEED */
	__le16 clock;		/* I2C bus clock in range 60-3400 KHz */
} __packed;

/* Data transfer reports */

struct ft260_i2c_write_request_report {
	u8 report;		/* FT260_I2C_REPORT */
	u8 address;		/* 7-bit I2C address */
	u8 flag;		/* I2C transaction condition */
	u8 length;		/* data payload length */
	u8 data[FT260_WR_DATA_MAX]; /* data payload */
} __packed;

struct ft260_i2c_read_request_report {
	u8 report;		/* FT260_I2C_READ_REQ */
	u8 address;		/* 7-bit I2C address */
	u8 flag;		/* I2C transaction condition */
	__le16 length;		/* data payload length */
} __packed;

struct ft260_input_report {
	u8 report;		/* FT260_I2C_REPORT or FT260_UART_REPORT */
	u8 length;		/* data payload length */
	u8 data[2];		/* data payload */
} __packed;

/* UART reports */
struct ft260_uart_write_request_report {
	u8 report;		/* FT260_UART_REPORT */
	u8 length;		/* data payload length */
	u8 data[] __counted_by(length);	/* variable data payload */
} __packed;

struct ft260_configure_uart_request {
	u8 report;		/* FT260_SYSTEM_SETTINGS */
	u8 request;		/* FT260_SET_UART_CONFIG */
	u8 flow_ctrl;		/* 0: OFF, 1: RTS_CTS, 2: DTR_DSR */
				/* 3: XON_XOFF, 4: No flow ctrl */
	/* The baudrate field is unaligned: */
	__le32 baudrate;	/* little endian, 9600 = 0x2580, 19200 = 0x4B00 */
	u8 data_bit;		/* 7 or 8 */
	u8 parity;		/* 0: no parity, 1: odd, 2: even, 3: high, 4: low */
	u8 stop_bit;		/* 0: one stop bit, 2: 2 stop bits */
	u8 breaking;		/* 0: no break */
} __packed;

/* UART interface configuration */
enum {
	FT260_CFG_FLOW_CTRL_OFF		= 0x00,
	FT260_CFG_FLOW_CTRL_RTS_CTS	= 0x01,
	FT260_CFG_FLOW_CTRL_DTR_DSR	= 0x02,
	FT260_CFG_FLOW_CTRL_XON_XOFF	= 0x03,
	FT260_CFG_FLOW_CTRL_NONE	= 0x04,

	FT260_CFG_DATA_BITS_7		= 0x07,
	FT260_CFG_DATA_BITS_8		= 0x08,

	FT260_CFG_PAR_NO		= 0x00,
	FT260_CFG_PAR_ODD		= 0x01,
	FT260_CFG_PAR_EVEN		= 0x02,
	FT260_CFG_PAR_HIGH		= 0x03,
	FT260_CFG_PAR_LOW		= 0x04,

	FT260_CFG_STOP_ONE_BIT		= 0x00,
	FT260_CFG_STOP_TWO_BIT		= 0x02,

	FT260_CFG_BREAKING_NO		= 0x00,
	FT260_CFG_BEAKING_YES		= 0x01,

	FT260_CFG_BAUD_MIN		= 1200,
	FT260_CFG_BAUD_MAX		= 12000000,
};

static const struct hid_device_id ft260_devices[] = {
	{ HID_USB_DEVICE(USB_VENDOR_ID_FUTURE_TECHNOLOGY,
			 USB_DEVICE_ID_FT260) },
	{ /* END OF LIST */ }
};
MODULE_DEVICE_TABLE(hid, ft260_devices);

struct ft260_device {
	struct i2c_adapter adap;
	struct hid_device *hdev;

	bool ft260_is_serial;
	struct list_head device_list;

	/* tty_port lifetime is equal to device lifetime */
	struct tty_port port;
	unsigned int index;
	struct kfifo xmit_fifo;
	/* write_lock: lock to serialize access to xmit fifo */
	spinlock_t write_lock;
	struct uart_icount icount;

	struct timer_list wakeup_timer;
	struct work_struct wakeup_work;
	bool reschedule_work;


	struct completion wait;
	struct mutex lock;
	u8 write_buf[FT260_REPORT_MAX_LENGTH];
	unsigned long need_wakeup_at;
	u8 *read_buf;
	u16 read_idx;
	u16 read_len;
	u16 clock;
};

static int ft260_hid_feature_report_get(struct hid_device *hdev,
					unsigned char report_id, u8 *data,
					size_t len)
{
	u8 *buf;
	int ret;

	buf = kmalloc(len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = hid_hw_raw_request(hdev, report_id, buf, len, HID_FEATURE_REPORT,
				 HID_REQ_GET_REPORT);
	if (likely(ret == len))
		memcpy(data, buf, len);
	else if (ret >= 0)
		ret = -EIO;
	kfree(buf);
	return ret;
}

static int ft260_hid_feature_report_set(struct hid_device *hdev, u8 *data,
					size_t len)
{
	u8 *buf;
	int ret;

	buf = kmemdup(data, len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	buf[0] = FT260_SYSTEM_SETTINGS;

	ret = hid_hw_raw_request(hdev, buf[0], buf, len, HID_FEATURE_REPORT,
				 HID_REQ_SET_REPORT);

	kfree(buf);
	return ret;
}

static int ft260_i2c_reset(struct hid_device *hdev)
{
	struct ft260_set_i2c_reset_report report;
	int ret;

	report.request = FT260_SET_I2C_RESET;

	ret = ft260_hid_feature_report_set(hdev, (u8 *)&report, sizeof(report));
	if (ret < 0) {
		hid_err(hdev, "failed to reset I2C controller: %d\n", ret);
		return ret;
	}

	ft260_dbg("done\n");
	return ret;
}

static int ft260_xfer_status(struct ft260_device *dev, u8 bus_busy)
{
	struct hid_device *hdev = dev->hdev;
	struct ft260_get_i2c_status_report report;
	int ret;

	if (time_is_before_jiffies(dev->need_wakeup_at)) {
		ret = ft260_hid_feature_report_get(hdev, FT260_I2C_STATUS,
						(u8 *)&report, sizeof(report));
		if (unlikely(ret < 0)) {
			hid_err(hdev, "failed to retrieve status: %d, no wakeup\n",
				ret);
		} else {
			dev->need_wakeup_at = jiffies +
				msecs_to_jiffies(FT260_WAKEUP_NEEDED_AFTER_MS);
			ft260_dbg("bus_status %#02x, wakeup\n",
				  report.bus_status);
		}
	}

	ret = ft260_hid_feature_report_get(hdev, FT260_I2C_STATUS,
					   (u8 *)&report, sizeof(report));
	if (unlikely(ret < 0)) {
		hid_err(hdev, "failed to retrieve status: %d\n", ret);
		return ret;
	}

	dev->clock = le16_to_cpu(report.clock);
	ft260_dbg("bus_status %#02x, clock %u\n", report.bus_status,
		  dev->clock);

	if (report.bus_status & (FT260_I2C_STATUS_CTRL_BUSY | bus_busy))
		return -EAGAIN;

	/*
	 * The error condition (bit 1) is a status bit reflecting any
	 * error conditions. When any of the bits 2, 3, or 4 are raised
	 * to 1, bit 1 is also set to 1.
	 */
	if (report.bus_status & FT260_I2C_STATUS_ERROR) {
		hid_err(hdev, "i2c bus error: %#02x\n", report.bus_status);
		return -EIO;
	}

	return 0;
}

static int ft260_hid_output_report(struct hid_device *hdev, u8 *data,
				   size_t len)
{
	u8 *buf;
	int ret;

	buf = kmemdup(data, len, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	ret = hid_hw_output_report(hdev, buf, len);

	kfree(buf);
	return ret;
}

static int ft260_hid_output_report_check_status(struct ft260_device *dev,
						u8 *data, int len)
{
	u8 bus_busy;
	int ret, usec, try = 100;
	struct hid_device *hdev = dev->hdev;
	struct ft260_i2c_write_request_report *rep =
		(struct ft260_i2c_write_request_report *)data;

	ret = ft260_hid_output_report(hdev, data, len);
	if (ret < 0) {
		hid_dbg(hdev, "%s: failed to start transfer, ret %d\n",
			__func__, ret);
		ft260_i2c_reset(hdev);
		return ret;
	}

	/* transfer time = 1 / clock(KHz) * 9 bits * bytes */
	usec = len * 9000 / dev->clock;
	if (usec > 2000) {
		usec -= 1500;
		usleep_range(usec, usec + 100);
		ft260_dbg("wait %d usec, len %d\n", usec, len);
	}

	/*
	 * Do not check the busy bit for combined transactions
	 * since the controller keeps the bus busy between writing
	 * and reading IOs to ensure an atomic operation.
	 */
	if (rep->flag == FT260_FLAG_START)
		bus_busy = 0;
	else
		bus_busy = FT260_I2C_STATUS_BUS_BUSY;

	do {
		ret = ft260_xfer_status(dev, bus_busy);
		if (ret != -EAGAIN)
			break;
	} while (--try);

	if (ret == 0)
		return 0;

	ft260_i2c_reset(hdev);
	return -EIO;
}

static int ft260_i2c_write(struct ft260_device *dev, u8 addr, u8 *data,
			   int len, u8 flag)
{
	int ret, wr_len, idx = 0;
	struct hid_device *hdev = dev->hdev;
	struct ft260_i2c_write_request_report *rep =
		(struct ft260_i2c_write_request_report *)dev->write_buf;

	if (len < 1)
		return -EINVAL;

	rep->flag = FT260_FLAG_START;

	do {
		if (len <= FT260_WR_DATA_MAX) {
			wr_len = len;
			if (flag == FT260_FLAG_START_STOP)
				rep->flag |= FT260_FLAG_STOP;
		} else {
			wr_len = FT260_WR_DATA_MAX;
		}

		rep->report = FT260_I2C_DATA_REPORT_ID(wr_len);
		rep->address = addr;
		rep->length = wr_len;

		memcpy(rep->data, &data[idx], wr_len);

		ft260_dbg("rep %#02x addr %#02x off %d len %d wlen %d flag %#x d[0] %#02x\n",
			  rep->report, addr, idx, len, wr_len,
			  rep->flag, data[0]);

		ret = ft260_hid_output_report_check_status(dev, (u8 *)rep,
							   wr_len + 4);
		if (ret < 0) {
			hid_err(hdev, "%s: failed with %d\n", __func__, ret);
			return ret;
		}

		len -= wr_len;
		idx += wr_len;
		rep->flag = 0;

	} while (len > 0);

	return 0;
}

static int ft260_smbus_write(struct ft260_device *dev, u8 addr, u8 cmd,
			     u8 *data, u8 data_len, u8 flag)
{
	int ret = 0;
	int len = 4;

	struct ft260_i2c_write_request_report *rep =
		(struct ft260_i2c_write_request_report *)dev->write_buf;

	if (data_len >= sizeof(rep->data))
		return -EINVAL;

	rep->address = addr;
	rep->data[0] = cmd;
	rep->length = data_len + 1;
	rep->flag = flag;
	len += rep->length;

	rep->report = FT260_I2C_DATA_REPORT_ID(len);

	if (data_len > 0)
		memcpy(&rep->data[1], data, data_len);

	ft260_dbg("rep %#02x addr %#02x cmd %#02x datlen %d replen %d\n",
		  rep->report, addr, cmd, rep->length, len);

	ret = ft260_hid_output_report_check_status(dev, (u8 *)rep, len);

	return ret;
}

static int ft260_i2c_read(struct ft260_device *dev, u8 addr, u8 *data,
			  u16 len, u8 flag)
{
	u16 rd_len;
	u16 rd_data_max = 60;
	int timeout, ret = 0;
	struct ft260_i2c_read_request_report rep;
	struct hid_device *hdev = dev->hdev;
	u8 bus_busy = 0;

	if ((flag & FT260_FLAG_START_REPEATED) == FT260_FLAG_START_REPEATED)
		flag = FT260_FLAG_START_REPEATED;
	else
		flag = FT260_FLAG_START;
	do {
		if (len <= rd_data_max) {
			rd_len = len;
			flag |= FT260_FLAG_STOP;
		} else {
			rd_len = rd_data_max;
		}
		rd_data_max = FT260_RD_DATA_MAX;

		rep.report = FT260_I2C_READ_REQ;
		rep.length = cpu_to_le16(rd_len);
		rep.address = addr;
		rep.flag = flag;

		ft260_dbg("rep %#02x addr %#02x len %d rlen %d flag %#x\n",
			  rep.report, rep.address, len, rd_len, flag);

		reinit_completion(&dev->wait);

		dev->read_idx = 0;
		dev->read_buf = data;
		dev->read_len = rd_len;

		ret = ft260_hid_output_report(hdev, (u8 *)&rep, sizeof(rep));
		if (ret < 0) {
			hid_err(hdev, "%s: failed with %d\n", __func__, ret);
			goto ft260_i2c_read_exit;
		}

		timeout = msecs_to_jiffies(5000);
		if (!wait_for_completion_timeout(&dev->wait, timeout)) {
			ret = -ETIMEDOUT;
			ft260_i2c_reset(hdev);
			goto ft260_i2c_read_exit;
		}

		dev->read_buf = NULL;

		if (flag & FT260_FLAG_STOP)
			bus_busy = FT260_I2C_STATUS_BUS_BUSY;

		ret = ft260_xfer_status(dev, bus_busy);
		if (ret < 0) {
			ret = -EIO;
			ft260_i2c_reset(hdev);
			goto ft260_i2c_read_exit;
		}

		len -= rd_len;
		data += rd_len;
		flag = 0;

	} while (len > 0);

ft260_i2c_read_exit:
	dev->read_buf = NULL;
	return ret;
}

/*
 * A random read operation is implemented as a dummy write operation, followed
 * by a current address read operation. The dummy write operation is used to
 * load the target byte address into the current byte address counter, from
 * which the subsequent current address read operation then reads.
 */
static int ft260_i2c_write_read(struct ft260_device *dev, struct i2c_msg *msgs)
{
	int ret;
	int wr_len = msgs[0].len;
	int rd_len = msgs[1].len;
	struct hid_device *hdev = dev->hdev;
	u8 addr = msgs[0].addr;
	u16 read_off = 0;

	if (wr_len > 2) {
		hid_err(hdev, "%s: invalid wr_len: %d\n", __func__, wr_len);
		return -EOPNOTSUPP;
	}

	if (ft260_debug) {
		if (wr_len == 2)
			read_off = be16_to_cpu(*(__be16 *)msgs[0].buf);
		else
			read_off = *msgs[0].buf;

		ft260_dbg("%s: off %#x rlen %d wlen %d\n", __func__,
			read_off, rd_len, wr_len);
	}

	ret = ft260_i2c_write(dev, addr, msgs[0].buf, wr_len,
			      FT260_FLAG_START);
	if (ret < 0)
		return ret;

	ret = ft260_i2c_read(dev, addr, msgs[1].buf, rd_len,
			     FT260_FLAG_START_STOP_REPEATED);
	if (ret < 0)
		return ret;

	return 0;
}

static int ft260_i2c_xfer(struct i2c_adapter *adapter, struct i2c_msg *msgs,
			  int num)
{
	int ret;
	struct ft260_device *dev = i2c_get_adapdata(adapter);
	struct hid_device *hdev = dev->hdev;

	mutex_lock(&dev->lock);

	ret = hid_hw_power(hdev, PM_HINT_FULLON);
	if (ret < 0) {
		hid_err(hdev, "failed to enter FULLON power mode: %d\n", ret);
		mutex_unlock(&dev->lock);
		return ret;
	}

	if (num == 1) {
		if (msgs->flags & I2C_M_RD)
			ret = ft260_i2c_read(dev, msgs->addr, msgs->buf,
					     msgs->len, FT260_FLAG_START_STOP);
		else
			ret = ft260_i2c_write(dev, msgs->addr, msgs->buf,
					      msgs->len, FT260_FLAG_START_STOP);
		if (ret < 0)
			goto i2c_exit;

	} else {
		/* Combined write then read message */
		ret = ft260_i2c_write_read(dev, msgs);
		if (ret < 0)
			goto i2c_exit;
	}

	ret = num;
i2c_exit:
	hid_hw_power(hdev, PM_HINT_NORMAL);
	mutex_unlock(&dev->lock);
	return ret;
}

static int ft260_smbus_xfer(struct i2c_adapter *adapter, u16 addr, u16 flags,
			    char read_write, u8 cmd, int size,
			    union i2c_smbus_data *data)
{
	int ret;
	struct ft260_device *dev = i2c_get_adapdata(adapter);
	struct hid_device *hdev = dev->hdev;

	ft260_dbg("smbus size %d\n", size);

	mutex_lock(&dev->lock);

	ret = hid_hw_power(hdev, PM_HINT_FULLON);
	if (ret < 0) {
		hid_err(hdev, "power management error: %d\n", ret);
		mutex_unlock(&dev->lock);
		return ret;
	}

	switch (size) {
	case I2C_SMBUS_BYTE:
		if (read_write == I2C_SMBUS_READ)
			ret = ft260_i2c_read(dev, addr, &data->byte, 1,
					     FT260_FLAG_START_STOP);
		else
			ret = ft260_smbus_write(dev, addr, cmd, NULL, 0,
						FT260_FLAG_START_STOP);
		break;
	case I2C_SMBUS_BYTE_DATA:
		if (read_write == I2C_SMBUS_READ) {
			ret = ft260_smbus_write(dev, addr, cmd, NULL, 0,
						FT260_FLAG_START);
			if (ret)
				goto smbus_exit;

			ret = ft260_i2c_read(dev, addr, &data->byte, 1,
					     FT260_FLAG_START_STOP_REPEATED);
		} else {
			ret = ft260_smbus_write(dev, addr, cmd, &data->byte, 1,
						FT260_FLAG_START_STOP);
		}
		break;
	case I2C_SMBUS_WORD_DATA:
		if (read_write == I2C_SMBUS_READ) {
			ret = ft260_smbus_write(dev, addr, cmd, NULL, 0,
						FT260_FLAG_START);
			if (ret)
				goto smbus_exit;

			ret = ft260_i2c_read(dev, addr, (u8 *)&data->word, 2,
					     FT260_FLAG_START_STOP_REPEATED);
		} else {
			ret = ft260_smbus_write(dev, addr, cmd,
						(u8 *)&data->word, 2,
						FT260_FLAG_START_STOP);
		}
		break;
	case I2C_SMBUS_BLOCK_DATA:
		if (read_write == I2C_SMBUS_READ) {
			ret = ft260_smbus_write(dev, addr, cmd, NULL, 0,
						FT260_FLAG_START);
			if (ret)
				goto smbus_exit;

			ret = ft260_i2c_read(dev, addr, data->block,
					     data->block[0] + 1,
					     FT260_FLAG_START_STOP_REPEATED);
		} else {
			ret = ft260_smbus_write(dev, addr, cmd, data->block,
						data->block[0] + 1,
						FT260_FLAG_START_STOP);
		}
		break;
	case I2C_SMBUS_I2C_BLOCK_DATA:
		if (read_write == I2C_SMBUS_READ) {
			ret = ft260_smbus_write(dev, addr, cmd, NULL, 0,
						FT260_FLAG_START);
			if (ret)
				goto smbus_exit;

			ret = ft260_i2c_read(dev, addr, data->block + 1,
					     data->block[0],
					     FT260_FLAG_START_STOP_REPEATED);
		} else {
			ret = ft260_smbus_write(dev, addr, cmd, data->block + 1,
						data->block[0],
						FT260_FLAG_START_STOP);
		}
		break;
	default:
		hid_err(hdev, "unsupported smbus transaction size %d\n", size);
		ret = -EOPNOTSUPP;
	}

smbus_exit:
	hid_hw_power(hdev, PM_HINT_NORMAL);
	mutex_unlock(&dev->lock);
	return ret;
}

static u32 ft260_functionality(struct i2c_adapter *adap)
{
	return I2C_FUNC_I2C | I2C_FUNC_SMBUS_BYTE |
	       I2C_FUNC_SMBUS_BYTE_DATA | I2C_FUNC_SMBUS_WORD_DATA |
	       I2C_FUNC_SMBUS_BLOCK_DATA | I2C_FUNC_SMBUS_I2C_BLOCK;
}

static const struct i2c_adapter_quirks ft260_i2c_quirks = {
	.flags = I2C_AQ_COMB_WRITE_THEN_READ,
	.max_comb_1st_msg_len = 2,
};

static const struct i2c_algorithm ft260_i2c_algo = {
	.master_xfer = ft260_i2c_xfer,
	.smbus_xfer = ft260_smbus_xfer,
	.functionality = ft260_functionality,
};

static int ft260_get_system_config(struct hid_device *hdev,
				   struct ft260_get_system_status_report *cfg)
{
	int ret;
	int len = sizeof(struct ft260_get_system_status_report);

	ret = ft260_hid_feature_report_get(hdev, FT260_SYSTEM_SETTINGS,
					   (u8 *)cfg, len);
	if (ret < 0) {
		hid_err(hdev, "failed to retrieve system status\n");
		return ret;
	}
	return 0;
}

static int ft260_get_interface_type(struct hid_device *hdev, struct ft260_device *dev)
{
	struct ft260_get_system_status_report cfg;
	struct usb_interface *usbif = to_usb_interface(hdev->dev.parent);
	int interface = usbif->cur_altsetting->desc.bInterfaceNumber;
	int ret;

	ret = ft260_get_system_config(hdev, &cfg);
	if (ret < 0)
		return ret;

	ft260_dbg("interface:  0x%02x\n", interface);
	ft260_dbg("chip mode:  0x%02x\n", cfg.chip_mode);
	ft260_dbg("clock_ctl:  0x%02x\n", cfg.clock_ctl);
	ft260_dbg("i2c_enable: 0x%02x\n", cfg.i2c_enable);
	ft260_dbg("uart_mode:  0x%02x\n", cfg.uart_mode);

	dev->ft260_is_serial = false;

	switch (cfg.chip_mode) {
	case FT260_MODE_ALL:
	case FT260_MODE_BOTH:
		if (interface == 1) {
			ret = FT260_IFACE_UART;
			dev->ft260_is_serial = true;
		} else {
			ret = FT260_IFACE_I2C;
		}
		break;
	case FT260_MODE_UART:
		ret = FT260_IFACE_UART;
		dev->ft260_is_serial = true;
		break;
	case FT260_MODE_I2C:
		ret = FT260_IFACE_I2C;
		break;
	}

	return ret;
}

static int ft260_byte_show(struct hid_device *hdev, int id, u8 *cfg, int len,
			   u8 *field, u8 *buf)
{
	int ret;

	ret = ft260_hid_feature_report_get(hdev, id, cfg, len);
	if (ret < 0)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%d\n", *field);
}

static int ft260_word_show(struct hid_device *hdev, int id, u8 *cfg, int len,
			   __le16 *field, u8 *buf)
{
	int ret;

	ret = ft260_hid_feature_report_get(hdev, id, cfg, len);
	if (ret < 0)
		return ret;

	return scnprintf(buf, PAGE_SIZE, "%d\n", le16_to_cpu(*field));
}

#define FT260_ATTR_SHOW(name, reptype, id, type, func)			       \
	static ssize_t name##_show(struct device *kdev,			       \
				   struct device_attribute *attr, char *buf)   \
	{								       \
		struct reptype rep;					       \
		struct hid_device *hdev = to_hid_device(kdev);		       \
		type *field = &rep.name;				       \
		int len = sizeof(rep);					       \
									       \
		return func(hdev, id, (u8 *)&rep, len, field, buf);	       \
	}

#define FT260_SSTAT_ATTR_SHOW(name)					       \
		FT260_ATTR_SHOW(name, ft260_get_system_status_report,	       \
				FT260_SYSTEM_SETTINGS, u8, ft260_byte_show)

#define FT260_I2CST_ATTR_SHOW(name)					       \
		FT260_ATTR_SHOW(name, ft260_get_i2c_status_report,	       \
				FT260_I2C_STATUS, __le16, ft260_word_show)

#define FT260_ATTR_STORE(name, reptype, id, req, type, ctype, func)	       \
	static ssize_t name##_store(struct device *kdev,		       \
				    struct device_attribute *attr,	       \
				    const char *buf, size_t count)	       \
	{								       \
		struct reptype rep;					       \
		struct hid_device *hdev = to_hid_device(kdev);		       \
		type name;						       \
		int ret;						       \
									       \
		if (!func(buf, 10, (ctype *)&name)) {			       \
			rep.name = name;				       \
			rep.report = id;				       \
			rep.request = req;				       \
			ret = ft260_hid_feature_report_set(hdev, (u8 *)&rep,   \
							   sizeof(rep));       \
			if (!ret)					       \
				ret = count;				       \
		} else {						       \
			ret = -EINVAL;					       \
		}							       \
		return ret;						       \
	}

#define FT260_BYTE_ATTR_STORE(name, reptype, req)			       \
		FT260_ATTR_STORE(name, reptype, FT260_SYSTEM_SETTINGS, req,    \
				 u8, u8, kstrtou8)

#define FT260_WORD_ATTR_STORE(name, reptype, req)			       \
		FT260_ATTR_STORE(name, reptype, FT260_SYSTEM_SETTINGS, req,    \
				 __le16, u16, kstrtou16)

FT260_SSTAT_ATTR_SHOW(chip_mode);
static DEVICE_ATTR_RO(chip_mode);

FT260_SSTAT_ATTR_SHOW(pwren_status);
static DEVICE_ATTR_RO(pwren_status);

FT260_SSTAT_ATTR_SHOW(suspend_status);
static DEVICE_ATTR_RO(suspend_status);

FT260_SSTAT_ATTR_SHOW(hid_over_i2c_en);
static DEVICE_ATTR_RO(hid_over_i2c_en);

FT260_SSTAT_ATTR_SHOW(power_saving_en);
static DEVICE_ATTR_RO(power_saving_en);

FT260_SSTAT_ATTR_SHOW(i2c_enable);
FT260_BYTE_ATTR_STORE(i2c_enable, ft260_set_i2c_mode_report,
		      FT260_SET_I2C_MODE);
static DEVICE_ATTR_RW(i2c_enable);

FT260_SSTAT_ATTR_SHOW(uart_mode);
FT260_BYTE_ATTR_STORE(uart_mode, ft260_set_uart_mode_report,
		      FT260_SET_UART_MODE);
static DEVICE_ATTR_RW(uart_mode);

FT260_SSTAT_ATTR_SHOW(clock_ctl);
FT260_BYTE_ATTR_STORE(clock_ctl, ft260_set_system_clock_report,
		      FT260_SET_CLOCK);
static DEVICE_ATTR_RW(clock_ctl);

FT260_I2CST_ATTR_SHOW(clock);
FT260_WORD_ATTR_STORE(clock, ft260_set_i2c_speed_report,
		      FT260_SET_I2C_CLOCK_SPEED);
static DEVICE_ATTR_RW(clock);

static ssize_t i2c_reset_store(struct device *kdev,
			       struct device_attribute *attr, const char *buf,
			       size_t count)
{
	struct hid_device *hdev = to_hid_device(kdev);
	int ret = ft260_i2c_reset(hdev);

	if (ret)
		return ret;
	return count;
}
static DEVICE_ATTR_WO(i2c_reset);

static const struct attribute_group ft260_attr_group = {
	.attrs = (struct attribute *[]) {
		  &dev_attr_chip_mode.attr,
		  &dev_attr_pwren_status.attr,
		  &dev_attr_suspend_status.attr,
		  &dev_attr_hid_over_i2c_en.attr,
		  &dev_attr_power_saving_en.attr,
		  &dev_attr_i2c_enable.attr,
		  &dev_attr_uart_mode.attr,
		  &dev_attr_clock_ctl.attr,
		  &dev_attr_i2c_reset.attr,
		  &dev_attr_clock.attr,
		  NULL
	}
};

/***
 * START Serial dev part
 */
static DEFINE_MUTEX(ft260_uart_list_lock);
static LIST_HEAD(ft260_uart_device_list);

static struct ft260_device *ft260_dev_by_index(int index)
{
	struct ft260_device *port;

	list_for_each_entry(port, &ft260_uart_device_list, device_list) {
		if (index == port->index)
			return port;
	}
	return NULL;
}

static int ft260_uart_add_port(struct ft260_device *port)
{
	int index = 0, ret = 0;
	struct ft260_device *dev;

	spin_lock_init(&port->write_lock);
	if (kfifo_alloc(&port->xmit_fifo, FIFO_SIZE, GFP_KERNEL))
		return -ENOMEM;

	mutex_lock(&ft260_uart_list_lock);
	list_for_each_entry(dev, &ft260_uart_device_list, device_list) {
		if (dev->index != index)
			break;
		index++;
	}

	port->index = index;
	list_add(&port->device_list, &ft260_uart_device_list);
	mutex_unlock(&ft260_uart_list_lock);

	return ret;
}

static void ft260_uart_port_put(struct ft260_device *port)
{
	tty_port_put(&port->port);
}

static void ft260_uart_port_remove(struct ft260_device *port)
{
	timer_delete_sync(&port->wakeup_timer);

	mutex_lock(&ft260_uart_list_lock);
	list_del(&port->device_list);
	mutex_unlock(&ft260_uart_list_lock);

	spin_lock(&port->write_lock);
	kfifo_free(&port->xmit_fifo);
	spin_unlock(&port->write_lock);

	mutex_lock(&port->port.mutex);
	port->reschedule_work = false;
	tty_port_tty_hangup(&port->port, false);
	mutex_unlock(&port->port.mutex);

	ft260_uart_port_put(port);
}

static struct ft260_device *ft260_uart_port_get(unsigned int index)
{
	struct ft260_device *port;

	if (index >= UART_COUNT_MAX)
		return NULL;

	mutex_lock(&ft260_uart_list_lock);
	port = ft260_dev_by_index(index);
	if (port)
		tty_port_get(&port->port);
	mutex_unlock(&ft260_uart_list_lock);

	return port;
}

static int ft260_uart_open(struct tty_struct *tty, struct file *filp)
{
	int ret;
	struct ft260_device *port = tty->driver_data;

	ret = tty_port_open(&port->port, tty, filp);

	return ret;
}

static void ft260_uart_close(struct tty_struct *tty, struct file *filp)
{
	struct ft260_device *port = tty->driver_data;

	tty_port_close(&port->port, tty, filp);
}

static void ft260_uart_hangup(struct tty_struct *tty)
{
	struct ft260_device *port = tty->driver_data;

	tty_port_hangup(&port->port);
}

static int ft260_uart_transmit_chars(struct ft260_device *port)
{
	struct hid_device *hdev = port->hdev;
	struct kfifo *xmit = &port->xmit_fifo;
	struct tty_struct *tty;
	struct ft260_uart_write_request_report *rep;
	int len, data_len, ret = 0;

	tty = tty_port_tty_get(&port->port);

	data_len = kfifo_len(xmit);
	if (!tty || !data_len) {
		ret = -EINVAL;
		goto tty_out;
	}

	rep = (struct ft260_uart_write_request_report *)port->write_buf;

	do {
		len = min(data_len, FT260_WR_DATA_MAX);

		rep->report = FT260_UART_DATA_REPORT_ID(len);
		rep->length = len;

		len = kfifo_out_locked(xmit, rep->data, len, &port->write_lock);

		ret = ft260_hid_output_report(hdev, (u8 *)rep, len + sizeof(*rep));
		if (ret < 0) {
			hid_err(hdev, "Failed to start transfer, ret %d\n", ret);
			goto tty_out;
		}

		data_len -= len;
		port->icount.tx += len;
	} while (data_len > 0);

	len = kfifo_len(xmit);
	if ((FIFO_SIZE - len) > TTY_WAKEUP_WATERMARK)
		tty_wakeup(tty);

	ret = 0;

tty_out:
	tty_kref_put(tty);
	return ret;
}

static int ft260_uart_receive_chars(struct ft260_device *port,
				    u8 *data, u8 length)
{
	struct hid_device *hdev = port->hdev;
	int ret = 0;

	if (length > FT260_RD_DATA_MAX) {
		hid_err(hdev, "Received too much data (%d)\n", length);
		return -EBADR;
	}

	ret = tty_insert_flip_string(&port->port, data, length);
	if (ret != length)
		hid_err(hdev, "%d char not inserted to flip buffer\n", length - ret);
	port->icount.rx += ret;

	if (ret)
		tty_flip_buffer_push(&port->port);

	return ret;
}

static int ft260_uart_write(struct tty_struct *tty, const unsigned char *buf,
			    int count)
{
	struct ft260_device *port = tty->driver_data;
	struct hid_device *hdev = port->hdev;
	int len, ret;

	len = kfifo_in_locked(&port->xmit_fifo, buf, count, &port->write_lock);
	ft260_dbg("count: %d, len: %d", count, len);

	ret = ft260_uart_transmit_chars(port);
	if (ret < 0) {
		hid_dbg(hdev, "Failed to transmit chars: %d\n", ret);
		return 0;
	}

	ret = kfifo_len(&port->xmit_fifo);
	if (ret > 0) {
		hid_dbg(hdev, "Failed to  all kfifo data bytes\n");
		ft260_dbg("return: %d", len - ret);
		return len - ret;
	}

	return len;
}

static unsigned int ft260_uart_write_room(struct tty_struct *tty)
{
	struct ft260_device *port = tty->driver_data;

	return FIFO_SIZE - kfifo_len(&port->xmit_fifo);
}

static unsigned int ft260_uart_chars_in_buffer(struct tty_struct *tty)
{
	struct ft260_device *port = tty->driver_data;

	return kfifo_len(&port->xmit_fifo);
}

static int ft260_uart_change_speed(struct ft260_device *port,
				   struct ktermios *termios,
				    struct ktermios *old)
{
	struct hid_device *hdev = port->hdev;
	unsigned int baud;
	struct ft260_configure_uart_request req;
	int ret;

	memset(&req, 0, sizeof(req));

	req.report = FT260_SYSTEM_SETTINGS;
	req.request = FT260_SET_UART_CONFIG;

	switch (termios->c_cflag & CSIZE) {
	case CS7:
		req.data_bit = FT260_CFG_DATA_BITS_7;
		break;
	case CS5:
	case CS6:
		hid_err(hdev, "Invalid data bit size, setting to default (8 bit)\n");
		req.data_bit = FT260_CFG_DATA_BITS_8;
		termios->c_cflag &= ~CSIZE;
		termios->c_cflag |= CS8;
		break;
	default:
	case CS8:
		req.data_bit = FT260_CFG_DATA_BITS_8;
		break;
	}

	req.stop_bit = (termios->c_cflag & CSTOPB) ?
		FT260_CFG_STOP_TWO_BIT : FT260_CFG_STOP_ONE_BIT;

	if (termios->c_cflag & PARENB) {
		req.parity = (termios->c_cflag & PARODD) ?
			FT260_CFG_PAR_ODD : FT260_CFG_PAR_EVEN;
	} else {
		req.parity = FT260_CFG_PAR_NO;
	}

	baud = tty_termios_baud_rate(termios);
	if (baud == 0 || baud < FT260_CFG_BAUD_MIN || baud > FT260_CFG_BAUD_MAX) {
		struct tty_struct *tty = tty_port_tty_get(&port->port);

		hid_err(hdev, "Invalid baud rate %d\n", baud);
		baud = 9600;
		tty_encode_baud_rate(tty, baud, baud);
		tty_kref_put(tty);
	}
	put_unaligned_le32(cpu_to_le32(baud), &req.baudrate);

	if (termios->c_cflag & CRTSCTS)
		req.flow_ctrl = FT260_CFG_FLOW_CTRL_RTS_CTS;
	else
		req.flow_ctrl = FT260_CFG_FLOW_CTRL_OFF;

	ft260_dbg("Configured termios: flow control: %d, baudrate: %d, ",
		  req.flow_ctrl, baud);
	ft260_dbg("data_bit: %d, parity: %d, stop_bit: %d, breaking: %d\n",
		  req.data_bit, req.parity,
		  req.stop_bit, req.breaking);

	req.flow_ctrl = FT260_CFG_FLOW_CTRL_NONE;
	req.breaking = FT260_CFG_BREAKING_NO;

	ret = ft260_hid_feature_report_set(hdev, (u8 *)&req, sizeof(req));
	if (ret < 0)
		hid_err(hdev, "ft260_hid_feature_report_set failed: %d\n", ret);

	return ret;
}

static int ft260_uart_get_icount(struct tty_struct *tty,
		struct serial_icounter_struct *icount)
{
	struct ft260_device *port = tty->driver_data;

	memcpy(icount, &port->icount, sizeof(struct uart_icount));

	return 0;
}

static void ft260_uart_set_termios(struct tty_struct *tty,
		const struct ktermios *old_termios)
{
	struct ft260_device *port = tty->driver_data;

	ft260_uart_change_speed(port, &tty->termios, NULL);
}

static int ft260_uart_install(struct tty_driver *driver, struct tty_struct *tty)
{
	int idx = tty->index;
	struct ft260_device *port = ft260_uart_port_get(idx);
	int ret = tty_standard_install(driver, tty);

	if (ret == 0)
		/* This is the ref ft260_uart_port get provided */
		tty->driver_data = port;
	else
		ft260_uart_port_put(port);

	return ret;
}

static void ft260_uart_cleanup(struct tty_struct *tty)
{
	struct ft260_device *port = tty->driver_data;

	tty->driver_data = NULL;	/* Bug trap */
	ft260_uart_port_put(port);
}

static int ft260_uart_proc_show(struct seq_file *m, void *v)
{
	int i;

	seq_printf(m, "ft260 info:1.0 driver%s%s revision:%s\n",
			"", "", "");
	for (i = 0; i < UART_COUNT_MAX; i++) {
		struct ft260_device *port = ft260_uart_port_get(i);

		if (port) {
			seq_printf(m, "%d: uart:FT260", i);
			if (capable(CAP_SYS_ADMIN)) {
				seq_printf(m, " tx:%d rx:%d",
						port->icount.tx, port->icount.rx);
				if (port->icount.frame)
					seq_printf(m, " fe:%d",
							port->icount.frame);
				if (port->icount.parity)
					seq_printf(m, " pe:%d",
							port->icount.parity);
				if (port->icount.brk)
					seq_printf(m, " brk:%d",
							port->icount.brk);
				if (port->icount.overrun)
					seq_printf(m, " oe:%d",
							port->icount.overrun);
				if (port->icount.cts)
					seq_printf(m, " cts:%d",
							port->icount.cts);
				if (port->icount.dsr)
					seq_printf(m, " dsr:%d",
							port->icount.dsr);
				if (port->icount.rng)
					seq_printf(m, " rng:%d",
							port->icount.rng);
				if (port->icount.dcd)
					seq_printf(m, " dcd:%d",
							port->icount.dcd);
			}
			ft260_uart_port_put(port);
			seq_putc(m, '\n');
		}
	}
	return 0;
}

static const struct tty_operations ft260_uart_ops = {
	.open			= ft260_uart_open,
	.close			= ft260_uart_close,
	.write			= ft260_uart_write,
	.write_room		= ft260_uart_write_room,
	.chars_in_buffer	= ft260_uart_chars_in_buffer,
	.set_termios		= ft260_uart_set_termios,
	.hangup			= ft260_uart_hangup,
	.install		= ft260_uart_install,
	.cleanup		= ft260_uart_cleanup,
	.proc_show		= ft260_uart_proc_show,
	.get_icount		= ft260_uart_get_icount,
};

/* The FT260 has a "power saving mode" that causes the device to switch
 * to a 30 kHz oscillator if there's no activity for 5 seconds.
 * Unfortunately this mode can only be disabled by reprogramming
 * internal fuses, which requires an additional programming voltage.
 *
 * One effect of this mode is to cause data loss on a fast UART that
 * transmits after being idle for longer than 5 seconds. We work around
 * this by sending a dummy report at least once per 4 seconds if the
 * UART is in use.
 */
static void ft260_uart_start_wakeup(struct timer_list *t)
{
	struct ft260_device *dev =
		container_of(t, struct ft260_device, wakeup_timer);

	if (dev->reschedule_work) {
		schedule_work(&dev->wakeup_work);
		mod_timer(&dev->wakeup_timer, jiffies +
			msecs_to_jiffies(FT260_WAKEUP_NEEDED_AFTER_MS));
	}
}

static void ft260_uart_do_wakeup(struct work_struct *work)
{
	struct ft260_device *dev =
		container_of(work, struct ft260_device, wakeup_work);
	struct ft260_get_chip_version_report version;
	int ret;

	if (dev->reschedule_work) {
		ret = ft260_hid_feature_report_get(dev->hdev, FT260_CHIP_VERSION,
					  (u8 *)&version, sizeof(version));
		if (ret < 0)
			hid_err(dev->hdev,
				"%s: failed to start transfer, ret %d\n",
				__func__, ret);
	}
}

static void ft260_uart_shutdown(struct tty_port *tport)
{
	struct ft260_device *port =
		container_of(tport, struct ft260_device, port);

	port->reschedule_work = false;
}

static int ft260_uart_activate(struct tty_port *tport, struct tty_struct *tty)
{
	struct ft260_device *port =
		container_of(tport, struct ft260_device, port);

	/*
	 * Set the TTY IO error marker - we will only clear this
	 * once we have successfully opened the port.
	 */
	set_bit(TTY_IO_ERROR, &tty->flags);

	spin_lock(&port->write_lock);
	kfifo_reset(&port->xmit_fifo);
	spin_unlock(&port->write_lock);

	ft260_uart_change_speed(port, &tty->termios, NULL);
	clear_bit(TTY_IO_ERROR, &tty->flags);

	if (port->reschedule_work) {
		mod_timer(&port->wakeup_timer, jiffies +
			  msecs_to_jiffies(FT260_WAKEUP_NEEDED_AFTER_MS));
	}

	return 0;
}

static void ft260_uart_port_destroy(struct tty_port *tport)
{
	struct ft260_device *port =
		container_of(tport, struct ft260_device, port);

	kfree(port);
}

static const struct tty_port_operations ft260_uart_port_ops = {
	.shutdown = ft260_uart_shutdown,
	.activate = ft260_uart_activate,
	.destruct = ft260_uart_port_destroy,
};

static struct tty_driver *ft260_tty_driver;

static int ft260_i2c_probe(struct hid_device *hdev, struct ft260_device *dev)
{
	int ret;

	hid_info(hdev, "USB HID v%x.%02x Device [%s] on %s\n",
		hdev->version >> 8, hdev->version & 0xff, hdev->name,
		hdev->phys);

	hid_set_drvdata(hdev, dev);
	dev->hdev = hdev;
	dev->adap.owner = THIS_MODULE;
	dev->adap.class = I2C_CLASS_HWMON;
	dev->adap.algo = &ft260_i2c_algo;
	dev->adap.quirks = &ft260_i2c_quirks;
	dev->adap.dev.parent = &hdev->dev;
	snprintf(dev->adap.name, sizeof(dev->adap.name),
		 "FT260 usb-i2c bridge");

	mutex_init(&dev->lock);
	init_completion(&dev->wait);

	ret = ft260_xfer_status(dev, FT260_I2C_STATUS_BUS_BUSY);
	if (ret)
		ft260_i2c_reset(hdev);

	i2c_set_adapdata(&dev->adap, dev);
	ret = i2c_add_adapter(&dev->adap);
	if (ret) {
		hid_err(hdev, "failed to add i2c adapter\n");
		return ret;
	}

	ret = sysfs_create_group(&hdev->dev.kobj, &ft260_attr_group);
	if (ret < 0) {
		hid_err(hdev, "failed to create sysfs attrs\n");
		goto err_i2c_free;
	}
	return 0;

err_i2c_free:
	i2c_del_adapter(&dev->adap);
	return ret;
}

static int ft260_uart_probe(struct hid_device *hdev, struct ft260_device *dev)
{
	struct ft260_configure_uart_request req;
	int ret;
	struct device *devt;

	INIT_WORK(&dev->wakeup_work, ft260_uart_do_wakeup);
	// FIXME: Do I need that if I have cancel_work_sync?
	// FIXME: are all kfifo access secured by lock? with irq or not?
	dev->reschedule_work = false;
	/* Work not started at this point */
	timer_setup(&dev->wakeup_timer, ft260_uart_start_wakeup, 0);

	tty_port_init(&dev->port);
	dev->port.ops = &ft260_uart_port_ops;

	ret = ft260_uart_add_port(dev);
	if (ret) {
		hid_err(hdev, "failed to add port\n");
		return ret;
	}
	devt = tty_port_register_device_attr(&dev->port,
					     ft260_tty_driver,
					     dev->index, &hdev->dev,
					     dev, NULL);
	if (IS_ERR(devt)) {
		hid_err(hdev, "failed to register tty port\n");
		ret = PTR_ERR(devt);
		goto err_register_tty;
	}
	hid_info(hdev, "Registering device /dev/%s%d\n",
		ft260_tty_driver->name, dev->index);

	/* Send Feature Report to Configure FT260 as UART 9600-8-N-1 */
	req.report	= FT260_SYSTEM_SETTINGS;
	req.request	= FT260_SET_UART_CONFIG;
	req.flow_ctrl	= FT260_CFG_FLOW_CTRL_NONE;
	put_unaligned_le32(cpu_to_le32(9600), &req.baudrate);
	req.data_bit	= FT260_CFG_DATA_BITS_8;
	req.parity	= FT260_CFG_PAR_NO;
	req.stop_bit	= FT260_CFG_STOP_ONE_BIT;
	req.breaking	= FT260_CFG_BREAKING_NO;

	ret = ft260_hid_feature_report_set(hdev, (u8 *)&req, sizeof(req));
	if (ret < 0) {
		hid_err(hdev, "ft260_hid_feature_report_set failed: %d\n",
			ret);
		goto err_hid_report;
	}

	return 0;

err_hid_report:
	tty_port_unregister_device(&dev->port, ft260_tty_driver, dev->index);
err_register_tty:
	ft260_uart_port_remove(dev);
	return ret;
}

static int ft260_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct ft260_device *dev;
	struct ft260_get_chip_version_report version;
	int ret;

	if (!hid_is_usb(hdev))
		return -EINVAL;

	/* We cannot used devm_kzalloc here, because port has to survive until
	 * destroy function call
	 */
	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto alloc_fail;
	}
	hid_set_drvdata(hdev, dev);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "failed to parse HID\n");
		goto hid_fail;
	}

	ret = hid_hw_start(hdev, 0);
	if (ret) {
		hid_err(hdev, "failed to start HID HW\n");
		goto hid_fail;
	}

	ret = hid_hw_open(hdev);
	if (ret) {
		hid_err(hdev, "failed to open HID HW\n");
		goto err_hid_stop;
	}

	ret = ft260_hid_feature_report_get(hdev, FT260_CHIP_VERSION,
					   (u8 *)&version, sizeof(version));
	if (ret < 0) {
		hid_err(hdev, "failed to retrieve chip version\n");
		goto err_hid_close;
	}

	hid_info(hdev, "chip code: %02x%02x %02x%02x\n",
		 version.chip_code[0], version.chip_code[1],
		 version.chip_code[2], version.chip_code[3]);

	ret = ft260_get_interface_type(hdev, dev);
	if (ret <= FT260_IFACE_NONE)
		goto err_hid_close;

	hid_set_drvdata(hdev, dev);
	dev->hdev = hdev;

	mutex_init(&dev->lock);
	init_completion(&dev->wait);

	if (!dev->ft260_is_serial) {
		ret = ft260_i2c_probe(hdev, dev);
		if (ret)
			goto err_hid_close;
	} else {
		ret = ft260_uart_probe(hdev, dev);
		if (ret)
			goto err_hid_close;
	}

	return 0;

err_hid_close:
	hid_hw_close(hdev);
err_hid_stop:
	hid_hw_stop(hdev);
hid_fail:
	kfree(dev);
alloc_fail:
	return ret;
}

static void ft260_remove(struct hid_device *hdev)
{
	struct ft260_device *dev = hid_get_drvdata(hdev);

	if (!dev)
		return;

	if (dev->ft260_is_serial) {
		// FIXME:
		cancel_work_sync(&dev->wakeup_work);
		tty_port_unregister_device(&dev->port, ft260_tty_driver,
					   dev->index);
		ft260_uart_port_remove(dev);
		/* dev still needed, so we will free it in _destroy func */
	} else {
		sysfs_remove_group(&hdev->dev.kobj, &ft260_attr_group);
		i2c_del_adapter(&dev->adap);
		kfree(dev);
	}

	hid_hw_close(hdev);
	hid_hw_stop(hdev);
}

static int ft260_raw_event(struct hid_device *hdev, struct hid_report *report,
			   u8 *data, int size)
{
	struct ft260_device *dev = hid_get_drvdata(hdev);
	struct ft260_input_report *xfer = (void *)data;

	if (xfer->report >= FT260_I2C_REPORT_MIN &&
	    xfer->report <= FT260_I2C_REPORT_MAX) {
		ft260_dbg("i2c resp: rep %#02x len %d\n", xfer->report,
			  xfer->length);

		if ((dev->read_buf == NULL) ||
		    (xfer->length > dev->read_len - dev->read_idx)) {
			hid_err(hdev, "unexpected report %#02x, length %d\n",
				xfer->report, xfer->length);
			return -1;
		}

		memcpy(&dev->read_buf[dev->read_idx], &xfer->data,
		       xfer->length);
		dev->read_idx += xfer->length;

		if (dev->read_idx == dev->read_len)
			complete(&dev->wait);

		return 0;

	} else if (xfer->length > FT260_RD_DATA_MAX) {
		hid_err(hdev, "Received data too long (%d)\n", xfer->length);
		return -EBADR;
	} else if (xfer->report >= FT260_UART_REPORT_MIN &&
		   xfer->report <= FT260_UART_REPORT_MAX) {
		return ft260_uart_receive_chars(dev, xfer->data, xfer->length);
	}
	hid_err(hdev, "unhandled report %#02x\n", xfer->report);

	return 0;
}

static struct hid_driver ft260_driver = {
	.name		= "ft260",
	.id_table	= ft260_devices,
	.probe		= ft260_probe,
	.remove		= ft260_remove,
	.raw_event	= ft260_raw_event,
};

static int __init ft260_driver_init(void)
{
	int ret;

	ft260_tty_driver = tty_alloc_driver(UART_COUNT_MAX,
		TTY_DRIVER_REAL_RAW | TTY_DRIVER_DYNAMIC_DEV);
	if (IS_ERR(ft260_tty_driver)) {
		pr_err("tty_alloc_driver failed: %d\n",
			(int)PTR_ERR(ft260_tty_driver));
		return PTR_ERR(ft260_tty_driver);
	}

	ft260_tty_driver->driver_name = "ft260_ser";
	ft260_tty_driver->name = "ttyFT";
	ft260_tty_driver->major = 0;
	ft260_tty_driver->minor_start = 0;
	ft260_tty_driver->type = TTY_DRIVER_TYPE_SERIAL;
	ft260_tty_driver->subtype = SERIAL_TYPE_NORMAL;
	ft260_tty_driver->init_termios = tty_std_termios;
	ft260_tty_driver->init_termios.c_cflag = B9600 | CS8 | CREAD | HUPCL | CLOCAL;
	ft260_tty_driver->init_termios.c_ispeed = 9600;
	ft260_tty_driver->init_termios.c_ospeed = 9600;
	tty_set_operations(ft260_tty_driver, &ft260_uart_ops);

	ret = tty_register_driver(ft260_tty_driver);
	if (ret) {
		pr_err("tty_register_driver failed: %d\n", ret);
		goto err_reg_driver;
	}

	ret = hid_register_driver(&(ft260_driver));
	if (ret) {
		pr_err("hid_register_driver failed: %d\n", ret);
		goto err_reg_hid;
	}

	return 0;

err_reg_hid:
	tty_unregister_driver(ft260_tty_driver);
err_reg_driver:
	tty_driver_kref_put(ft260_tty_driver);

	return ret;
}

static void __exit ft260_driver_exit(void)
{
	hid_unregister_driver(&(ft260_driver));
	tty_unregister_driver(ft260_tty_driver);
	tty_driver_kref_put(ft260_tty_driver);
}

module_init(ft260_driver_init);
module_exit(ft260_driver_exit);

MODULE_DESCRIPTION("FTDI FT260 USB HID to I2C host bridge and TTY driver");
MODULE_AUTHOR("Michael Zaidman <michael.zaidman@gmail.com>");
MODULE_LICENSE("GPL v2");
