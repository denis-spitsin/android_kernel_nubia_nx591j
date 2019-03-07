/*
 * Copyright (C) 2013 Cypress.
 *
 * This software is licensed under the terms of the GNU General Public
 * License version 2, as published by the Free Software Foundation, and
 * may be copied, distributed, and modified under those terms.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/init.h>
#include <linux/i2c.h>
#include <linux/input.h>
#include <linux/delay.h>
#include <linux/module.h>
#include <linux/interrupt.h>
#include <linux/debugfs.h>
#include <linux/gpio.h>
#include <linux/of_gpio.h>
#include <linux/slab.h>
#include "cyttsp_button.h"
#include <linux/firmware.h>
#include <linux/regulator/consumer.h>
#ifdef CONFIG_FB
#include <linux/notifier.h>
#include <linux/fb.h>
#endif

#define ZHWW_ADD_SUSPEND_RESUME
#define ZHWW_ADD_FW_UPDATE
#define ZHWW_I2C_MUTEX
//#define CONFIG_BUTTON_GLOVE_MODE

#define CYTTSP_CMD_VERIFY_CHKSUM		0x31
#define CYTTSP_CMD_GET_FLASH_SIZE		0x32
#define CYTTSP_CMD_ERASE_ROW		0x34
#define CYTTSP_CMD_SEND_DATA		0x37
#define CYTTSP_CMD_ENTER_BTLD		0x38
#define CYTTSP_CMD_PROGRAM_ROW		0x39
#define CYTTSP_CMD_VERIFY_ROW		0x3A
#define CYTTSP_CMD_EXIT_BTLD		0x3B
#define CYTTSP_CMD_READ_LOCKDOWN_INFO	0x40

#define CYTTSP_REG_TOUCHMODE		0x00
#define CYTTSP_REG_HWVERSION		0x48
#define CYTTSP_REG_LOCKDOWN_INFO_START	0x5E
#define CYTTSP_REG_INVALID		0xFF

#define CYTTSP_NORMAL_MODE		0x00
#define CYTTSP_STANDBY_MODE		0x01

#define CYTTSP_STS_SUCCESS		0x00

#define CYTTSP_BUTTON_OP_MODE		0x00
#define CYTTSP_BUTTON_FW_VER1		0x46
#define CYTTSP_BUTTON_FW_VER2		0x47

#define CYTTSP_PACKET_START		0x01
#define CYTTSP_PACKET_END		0x17

#define CYTTSP_MAX_PAYLOAD_LEN		0x15

#define CYTTSP_RESP_HEADER_LEN		0x4
#define CYTTSP_RESP_TAIL_LEN		0x3

#define CYTTSP_ENTER_BTLD_RESP_LEN	8
#define CYTTSP_GET_FLASHSZ_RESP_LEN	4
#define CYTTSP_READ_LOCKDOWN_RESP_LEN	8

#define CYTTSP_GLOVE_MODE_SHIFT		2

#define CYTTSP_LOCKDOWN_INFO_SIZE	8

struct cyttsp_button_data {
	struct i2c_client *client;
	struct cyttsp_button_platform_data *pdata;
	struct regulator *regulator_vdd;
	struct regulator *regulator_avdd;
	struct regulator *regulator_vddio;
	struct input_dev *input_dev;
	bool dbgdump;
	unsigned long keystatus;
	bool enable;
	u8 bootloader_addr;
	u8 app_addr;
#ifdef CONFIG_FB
	struct notifier_block fb_notif;
#endif

#ifdef CONFIG_BUTTON_GLOVE_MODE
	struct notifier_block glove_mode_notifier;
	bool glove_mode;
#endif

#if 1
	struct work_struct irq_work;
	struct workqueue_struct *irq_workqueue;
#endif

	u8 lockdown_info[CYTTSP_LOCKDOWN_INFO_SIZE];

#ifdef ZHWW_I2C_MUTEX
	struct mutex button_i2c_lock;
#endif
};

#ifdef ZHWW_I2C_MUTEX
static int cyttsp_i2c_recv(struct cyttsp_button_data *data, struct device *dev,
				u8 len, void *buf)
#else
static int cyttsp_i2c_recv(struct device *dev,
				u8 len, void *buf)
#endif
{
	struct i2c_client *client = to_i2c_client(dev);
	int count = 0;
#ifdef ZHWW_I2C_MUTEX
	mutex_lock(&data->button_i2c_lock);
#endif
	count = i2c_master_recv(client, buf, len);
#ifdef ZHWW_I2C_MUTEX
	mutex_unlock(&data->button_i2c_lock);
#endif
	return count < 0 ? count : 0;
}

#ifdef ZHWW_I2C_MUTEX
static int cyttsp_i2c_send(struct cyttsp_button_data *data, struct device *dev,
				u8 len, const void *buf)
#else
static int cyttsp_i2c_send(struct device *dev,
				u8 len, const void *buf)
#endif
{
	struct i2c_client *client = to_i2c_client(dev);
	int count = 0;
#ifdef ZHWW_I2C_MUTEX
	mutex_lock(&data->button_i2c_lock);
#endif
	count = i2c_master_send(client, buf, len);
#ifdef ZHWW_I2C_MUTEX
	mutex_unlock(&data->button_i2c_lock);
#endif
	return count < 0 ? count : 0;
}

static u8 calculate_checksum8(u8 *src, int len)
{
	u8 total = 0;
	int i;

	for (i = 0; i < len; i++)
		total += src[i];

	total = 256 - total;

	return total;
}

static u16 cyttsp_checksum16(u8 *data, int len)
{
	u16 total = 0;
	int i;

	for (i = 0; i < len; i++)
		total += data[i];

	total = 0xFFFF - total;

	return (u16)(total & 0xFFFF);
}

static int cyttsp_recv_command(struct cyttsp_button_data *data,
				u8 *error_code, u8 *payload, int *data_len)
{
	struct device *dev = &data->client->dev;
	u8 buffer[512];
	int remain_len;
	int len;
	int ret;
	int offset;
	int i = 0, k = 0;
	u8 *curr = payload;
	u16 cal_checksum;

	while (1) {
#ifdef ZHWW_I2C_MUTEX
		ret = cyttsp_i2c_recv(data, dev, 1, buffer);
#else
		ret = cyttsp_i2c_recv(dev, 1, buffer);
#endif
		if (ret != 0) {
			dev_err(dev, "Failed to read buffer!\n");
			return ret;
		}
		if (buffer[0] == CYTTSP_PACKET_START)
			break;
	}

#ifdef ZHWW_I2C_MUTEX
	ret = cyttsp_i2c_recv(data, dev, CYTTSP_RESP_HEADER_LEN - 1, &buffer[1]);
#else
	ret = cyttsp_i2c_recv(dev, CYTTSP_RESP_HEADER_LEN - 1, &buffer[1]);
#endif
	if (ret != 0) {
		dev_err(dev, "Failed to read response header!\n");
		return ret;
	}

	if (buffer[1] != CYTTSP_STS_SUCCESS) {
		*error_code = buffer[1];
		return -EINVAL;
	}

	remain_len = buffer[2] | (buffer[3] << 8);
	*data_len = remain_len;
	offset = 4;

	while (remain_len > 0) {
		if (remain_len > CYTTSP_MAX_PAYLOAD_LEN)
			len = CYTTSP_MAX_PAYLOAD_LEN;
		else
			len = remain_len;

#ifdef ZHWW_I2C_MUTEX
		ret = cyttsp_i2c_recv(data, dev, len, &buffer[offset]);
#else
		ret = cyttsp_i2c_recv(dev, len, &buffer[offset]);
#endif
		if (ret != 0) {
			dev_err(dev, "Failed to receive response at %d!\n", i);
			return ret;
		}
		memcpy(curr, &buffer[offset], len);
		offset += len;
		curr += len;
		remain_len -= len;
		i++;
	}

	cal_checksum = cyttsp_checksum16(&buffer[1], 3 + (*data_len));

#ifdef ZHWW_I2C_MUTEX
	ret = cyttsp_i2c_recv(data, dev, CYTTSP_RESP_TAIL_LEN, &buffer[offset]);
#else
	ret = cyttsp_i2c_recv(dev, CYTTSP_RESP_TAIL_LEN, &buffer[offset]);
#endif
	if (ret != 0) {
		dev_err(dev, "Failed to receive tail!\n");
		return ret;
	}

	if (cal_checksum != ((buffer[offset+1] << 8) | (buffer[offset]))) {
		dev_err(dev, "checksum not equal!\n");
		return ret;
	}

	if (buffer[offset+2] != CYTTSP_PACKET_END) {
		dev_err(dev, "Invalid packet tail!\n");
		return -EINVAL;
	}


	if (data->dbgdump) {
		dev_info(dev, "recv buffer = ");
		for (k = 0; k < *data_len + CYTTSP_RESP_HEADER_LEN + CYTTSP_RESP_TAIL_LEN; k++)
			printk(KERN_CONT "0x%x ", buffer[k]);

		dev_info(dev, "\n");
	}


	return 0;
}

static int cyttsp_handle_response(struct cyttsp_button_data *data,
				u8 cmd, u8 *buffer, int resp_len, u8 other)
{
	struct device *dev = &data->client->dev;
	if (cmd == CYTTSP_CMD_ENTER_BTLD) {
		if (resp_len < CYTTSP_ENTER_BTLD_RESP_LEN)
			dev_warn(dev, "Response length mismatch for enter bootloader\n");
		dev_info(dev, "silicon id = 0x%x 0x%x 0x%x 0x%x, "
			"rev = 0x%x, bootloader ver = 0x%x 0x%x 0x%x\n",
			buffer[0], buffer[1], buffer[2], buffer[3],
			buffer[4], buffer[5], buffer[6], buffer[7]);

	} else if (cmd == CYTTSP_CMD_GET_FLASH_SIZE) {
		if (resp_len < CYTTSP_GET_FLASHSZ_RESP_LEN)
			dev_warn(dev, "Response length mismatch for get flash size!\n");
		dev_info(dev, "first and last row number of flash = 0x%x 0x%x 0x%x 0x%x\n",
				buffer[0], buffer[1], buffer[2], buffer[3]);
	} else if (cmd == CYTTSP_CMD_VERIFY_ROW) {
		if (other != buffer[0]) {
			dev_err(dev, "row_checksum = 0x%x, received = 0x%x\n",
				other, buffer[4]);
			return -EINVAL;
		}
	} else if (cmd == CYTTSP_CMD_READ_LOCKDOWN_INFO) {
		if (resp_len != CYTTSP_READ_LOCKDOWN_RESP_LEN) {
			dev_err(dev, "Response length mismatch for lockdown info\n");
			return -EINVAL;
		} else {
			memcpy(data->lockdown_info, buffer, CYTTSP_READ_LOCKDOWN_RESP_LEN);
		}
	}

	return 0;
}

static int cyttsp_send_command(struct cyttsp_button_data *data,
				u8 cmd, u8 *payload, int data_len, u8 other)
{
	struct device *dev = &data->client->dev;
	int ret;
	int i, k;
	u8 buffer[512];
	u8 resp[50];
	u8 error_code = 0;
	int resp_len = 0;
	u16 checksum;
	int len;
	u8 *curr_buf = payload;

	buffer[0] = CYTTSP_PACKET_START;
	buffer[1] = cmd;

	do {
		i = 0;
		if (data_len > CYTTSP_MAX_PAYLOAD_LEN)
			len = CYTTSP_MAX_PAYLOAD_LEN;
		else
			len = data_len;

		buffer[2] = (u8)(len & 0xFF);
		buffer[3] = (u8)((len & 0xFF00) >> 8);

		data_len -= len;
		if (len != 0)
			memcpy(&buffer[4], curr_buf, len);
		i = 4 + len;

		checksum = cyttsp_checksum16(buffer+1, len+3);
		curr_buf += len;

		buffer[i++] = (u8)(checksum & 0xFF);
		buffer[i++] = (u8)((checksum & 0xFF00) >> 8);
		buffer[i++] = CYTTSP_PACKET_END;

#ifdef ZHWW_I2C_MUTEX
		ret = cyttsp_i2c_send(data, dev, i, buffer);
#else
		ret = cyttsp_i2c_send(dev, i, buffer);
#endif
		if (ret != 0) {
			dev_err(dev, "Failed to send cmd 0x%x\n", cmd);
			return ret;
		}

		if (cmd != CYTTSP_CMD_EXIT_BTLD) {
			error_code = 0;
			resp_len = 0;
			ret = cyttsp_recv_command(data, &error_code, resp, &resp_len);
			if (ret) {
				dev_err(dev, "Response error code = 0x%x\n", error_code);
				return ret;
			}
			ret = cyttsp_handle_response(data, cmd, resp, resp_len, other);
			if (ret) {
				dev_err(dev, "Response error for cmd 0x%x\n", cmd);
				return ret;
			}
		}

		if (data->dbgdump) {
			dev_info(dev, "send buffer = ");
			for (k = 0; k < i; k++)
				printk(KERN_CONT "0x%x ", buffer[k]);

			dev_info(dev, "\n");
		}
	} while (data_len > 0);

	return 0;
}

#ifdef ZHWW_I2C_MUTEX
static int cyttsp_i2c_read_block(struct cyttsp_button_data *btdata, struct device *dev, u8 addr,
				u8 len, void *data)
#else
static int cyttsp_i2c_read_block(struct device *dev, u8 addr,
				u8 len, void *data)
#endif
{
	struct i2c_client *client = to_i2c_client(dev);
	int ret;

#ifdef ZHWW_I2C_MUTEX
	mutex_lock(&btdata->button_i2c_lock);
#endif
	ret = i2c_master_send(client, &addr, 1);
#ifdef ZHWW_I2C_MUTEX
	mutex_unlock(&btdata->button_i2c_lock);
#endif
	if (ret != 1) {
		dev_err(dev, "Failed to read block!\n");
		return -EIO;
	}

#ifdef ZHWW_I2C_MUTEX
	mutex_lock(&btdata->button_i2c_lock);
#endif
	ret = i2c_master_recv(client, data, len);
#ifdef ZHWW_I2C_MUTEX
	mutex_unlock(&btdata->button_i2c_lock);
#endif

	return (ret < 0) ? ret : ret != len ? -EIO : 0;
}

static u8 cyttsp_read_reg(struct cyttsp_button_data *data, u8 reg)
{
	int ret;
	u8 val;

#ifdef ZHWW_I2C_MUTEX
	ret = cyttsp_i2c_read_block(data, &data->client->dev,
				reg, 1, &val);
#else
	ret = cyttsp_i2c_read_block(&data->client->dev,
				reg, 1, &val);
#endif
	if (ret < 0) {
		dev_err(&data->client->dev, "Failed to read reg!\n");
		return ret;
	}

	return val;
}

static int cyttsp_write_reg(struct cyttsp_button_data *data,
				u8 reg, u8 val)
{
	int ret;
	u8 buffer[2];
	struct device *dev = &data->client->dev;
	struct i2c_client *client = to_i2c_client(dev);

	buffer[0] = reg;
	buffer[1] = val;
#ifdef ZHWW_I2C_MUTEX
	mutex_lock(&data->button_i2c_lock);
#endif
	ret = i2c_master_send(client, buffer, sizeof(buffer));
#ifdef ZHWW_I2C_MUTEX
	mutex_unlock(&data->button_i2c_lock);
#endif
	if (ret != sizeof(buffer)) {
		dev_err(&data->client->dev, "Failed to write reg!\n");
		return ret;
	}

	return 0;
}

static int cyttsp_get_one_line(const u8 *src, u8 *dst)
{
	int i = 0;

	while (src[i++] != '\n')
		;

	strncpy(dst, src, i-1);

	return i;
}

static u8 cyttsp_convert(u8 c)
{
	if (c >= '0' && c <= '9')
		return c - '0';
	if (c >= 'a' && c <= 'f')
		return c - 'a' + 10;
	if (c >= 'A' && c <= 'F')
		return c - 'A' + 10;

	return 0;
}

static u8 cyttsp_convert_to_u8(u8 *buf)
{
	u8 msb = cyttsp_convert(buf[0]);
	u8 lsb = cyttsp_convert(buf[1]);
	u8 ret = msb;

	ret = ret * 16 + lsb;
	return ret;
}

static u16 cyttsp_convert_to_u16(u8 *buf)
{
	u8 msb = cyttsp_convert_to_u8(buf);
	u8 lsb = cyttsp_convert_to_u8(buf + 2);
	u16 ret = msb;

	ret = (ret << 16) | lsb;
	return ret;
}

#ifdef CONFIG_OF
static void cyttsp_button_dump_value(struct device *dev,
			struct cyttsp_button_platform_data *pdata)
{
	int i;

	dev_info(dev, "irq gpio = %d\n", pdata->irq_gpio);
	dev_info(dev, "irqflags = %d\n", (int)pdata->irqflags);
	dev_info(dev, "input name = %s\n", pdata->input_name);
	dev_info(dev, "button number = %d\n", pdata->nbuttons);
	dev_info(dev, "button_status_reg = 0x%x\n", pdata->button_status_reg);
	dev_info(dev, "standby_reg = 0x%x\n", pdata->standby_reg);
	dev_info(dev, "cut_off_power = %d\n", (int)pdata->cut_off_power);
	dev_info(dev, "default_config = %d\n", pdata->default_config);

	for (i = 0; i < pdata->nbuttons; i++)
		dev_info(dev, "button [%d] = %d\n", i, pdata->key_code[i]);
	for (i = 0; i < pdata->config_array_size; i++) {
		dev_info(dev, "panel id = 0x%02x\n", pdata->config_array[i].panel_id);
		dev_info(dev, "fw name = %s\n", pdata->config_array[i].fw_name);
	}
}

static int cyttsp_button_parse_dt(struct device *dev,
			struct cyttsp_button_platform_data *pdata)
{
	int ret;
	struct device_node *temp, *np = dev->of_node;
	u32 temp_val;
	struct cyttsp_config_info *info;

	/* irq gpio */
	pdata->irq_gpio = of_get_named_gpio_flags(np, "cyttsp,irq-gpio",
				0, &pdata->irq_gpio_flags);
	ret = of_property_read_u32(np, "cypress,irq-flags", &temp_val);
	if (ret) {
		dev_err(dev, "Unable to read irqflags id\n");
		return ret;
	} else
		pdata->irqflags = temp_val;

	ret = of_property_read_string(np, "cyttsp,input-name",
			&pdata->input_name);
	if (ret && (ret != -EINVAL)) {
		dev_info(dev, "Unable to read input name\n");
	}

	pdata->cut_off_power = of_property_read_bool(np,
			"cyttsp,cut-off-power");

	ret = of_property_read_u32(np, "cyttsp,button-status-reg",
			&temp_val);
	if (ret) {
		dev_err(dev, "Unable to read fw button-status-reg\n");
		return ret;
	} else
		pdata->button_status_reg = (u8)temp_val;

	ret = of_property_read_u32(np, "cyttsp,standby-reg",
			&temp_val);
	if (ret) {
		dev_err(dev, "Unable to read standby reg\n");
		return ret;
	} else
		pdata->standby_reg = (u8)temp_val;

	ret = of_property_read_u32(np, "cyttsp,bootloader-addr",
			&temp_val);
	if (ret)
		dev_err(dev, "Unable to read bootloader address\n");
	else
		pdata->bootloader_addr = (u8)temp_val;

	ret = of_property_read_u32(np, "cyttsp,key-num", &temp_val);
	if (ret) {
		dev_err(dev, "Unable to read key num\n");
		return ret;
	} else
		pdata->nbuttons = temp_val;

	if (pdata->nbuttons != 0) {
		pdata->key_code = devm_kzalloc(dev,
					sizeof(int) * pdata->nbuttons, GFP_KERNEL);
		if (!pdata->key_code)
			return -ENOMEM;
		ret = of_property_read_u32_array(np, "cyttsp,key-codes",
						pdata->key_code, pdata->nbuttons);
		if (ret) {
			dev_err(dev, "Unable to read key codes\n");
			return ret;
		}
	}

	ret = of_property_read_u32(np, "cyttsp,default-config",
			&pdata->default_config);
	if (ret) {
		dev_err(dev, "Unable to get default config\n");
		pdata->default_config = -1;
	}

	ret = of_property_read_u32(np, "cyttsp,config-array-size",
			&pdata->config_array_size);
	if (ret) {
		dev_err(dev, "Unable to get array size\n");
		return ret;
	}

	if (pdata->config_array_size != 0) {
		pdata->config_array = devm_kzalloc(dev, pdata->config_array_size *
						sizeof(struct cyttsp_config_info), GFP_KERNEL);
		if (!pdata->config_array) {
			dev_err(dev, "Unable to allocate memory\n");
			return -ENOMEM;
		}

		info = pdata->config_array;

		for_each_child_of_node(np, temp) {
			ret = of_property_read_u32(temp, "cyttsp,panel-id", &temp_val);
			if (ret) {
				dev_err(dev, "Unable to read panel id\n");
				return ret;
			} else
				info->panel_id = (u8)temp_val;

			ret = of_property_read_string(temp, "cyttsp,fw-name",
							&info->fw_name);
			if (ret && (ret != -EINVAL)) {
				dev_err(dev, "Unable to read fw name\n");
				return ret;
			}

			info++;
		}
	}

	cyttsp_button_dump_value(dev, pdata);

	return 0;
}
#else
static int cyttsp_button_parse_dt(struct device *dev,
			struct cyttsp_button_platform_data *pdata)
{
	return -ENODEV;
}
#endif

static int cyttsp_button_input_enable(struct input_dev *in_dev)
{
	struct cyttsp_button_data *data = input_get_drvdata(in_dev);
	struct cyttsp_button_platform_data *pdata = data->pdata;
	struct device *dev = &data->client->dev;
	int ret;

	if (data->enable == true)
		return 0;

	if (pdata->cut_off_power) {
		if (data->regulator_vdd) {
			ret = regulator_enable(data->regulator_vdd);
			if (ret < 0) {
				dev_err(dev, "%s: Failed to enable regulator vdd\n",
						__func__);
			}
		}

		if (data->regulator_vddio) {
			ret = regulator_enable(data->regulator_vddio);
			if (ret < 0) {
				dev_err(dev, "%s: Failed to enable regulator vddio\n",
						__func__);
			}
		}

		if (data->regulator_avdd) {
			ret = regulator_enable(data->regulator_avdd);
			if (ret < 0) {
				dev_err(dev, "%s: Failed to enable regulator avdd\n",
						__func__);
			}
		}

		enable_irq(data->client->irq);
	} else {
		if (pdata->standby_reg != CYTTSP_REG_INVALID) {
			ret = cyttsp_write_reg(data, pdata->standby_reg, CYTTSP_NORMAL_MODE);
			if (ret) {
				dev_err(dev, "Failed to enter normal mode!\n");
				return ret;
			}
		}
	}

	data->enable = true;

	return 0;
}

static int cyttsp_button_input_disable(struct input_dev *in_dev)
{
	struct cyttsp_button_data *data = input_get_drvdata(in_dev);
	struct cyttsp_button_platform_data *pdata = data->pdata;
	struct device *dev = &data->client->dev;
	int ret;

	if (data->enable == false)
		return 0;

	if (pdata->cut_off_power) {
		disable_irq(data->client->irq);

		if (data->regulator_vdd) {
			ret = regulator_disable(data->regulator_vdd);
			if (ret < 0) {
				dev_err(dev, "%s: Failed to disable regulator vdd\n",
						__func__);
			}
		}

		if (data->regulator_vddio) {
			ret = regulator_disable(data->regulator_vddio);
			if (ret < 0) {
				dev_err(dev, "%s: Failed to disable regulator vddio\n",
						__func__);
			}
		}

		if (data->regulator_avdd) {
			ret = regulator_disable(data->regulator_avdd);
			if (ret < 0) {
				dev_err(dev, "%s: Failed to disable regulator avdd\n",
						__func__);
			}
		}
	} else {
		if (pdata->standby_reg != CYTTSP_REG_INVALID) {
			ret = cyttsp_write_reg(data, pdata->standby_reg, CYTTSP_STANDBY_MODE);
			if (ret) {
				dev_err(dev, "Failed to enter standby mode!\n");
				return ret;
			}
		}

	}

	data->enable = false;

	return 0;
}

#ifdef CONFIG_BUTTON_GLOVE_MODE
static int cyttsp_glove_mode_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{
	struct cyttsp_button_data *cyt_data =
		container_of(self, struct cyttsp_button_data, glove_mode_notifier);
	u8 val;
	int ret = 0;

	mutex_lock(&cyt_data->input_dev->mutex);

	if (cyt_data->glove_mode == !!event) {
		mutex_unlock(&cyt_data->input_dev->mutex);
		return 0;
	}

	dev_dbg(&cyt_data->client->dev, "enter glove mode: %lu\n", event);

	val = (!!event) ? (1 << CYTTSP_GLOVE_MODE_SHIFT) : 1;

	ret = cyttsp_write_reg(cyt_data, CYTTSP_REG_TOUCHMODE, val);
	if (ret)
		dev_err(&cyt_data->client->dev, "Write %d to Reg0 failed\n", val);
	else
		cyt_data->glove_mode = !!event;

	mutex_unlock(&cyt_data->input_dev->mutex);

	return ret;
}
#endif

#ifdef CONFIG_FB
static int fb_notifier_callback(struct notifier_block *self,
		unsigned long event, void *data)
{

	struct fb_event *evdata = data;
	int *blank;
	int ret;
	struct cyttsp_button_data *cyt_data =
		container_of(self, struct cyttsp_button_data, fb_notif);
	struct device *dev = &cyt_data->client->dev;

	if (evdata && evdata->data &&
			event == FB_EVENT_BLANK && cyt_data) {
		blank = evdata->data;
		if (*blank == FB_BLANK_UNBLANK) {
			dev_info(dev, "UNBLANK SCREEN FOR CYTTSP\n");
			ret = cyttsp_button_input_enable(cyt_data->input_dev);
			if (ret)
				dev_err(dev, "Failed to enable button\n");
		} else if (*blank == FB_BLANK_POWERDOWN) {
			dev_info(dev, "BLANK SCREEN FOR CYTTSP\n");
			ret = cyttsp_button_input_disable(cyt_data->input_dev);
			if (ret)
				dev_err(dev, "Failed to disable button\n");
		}
	}

	return 0;
}
#endif

static irqreturn_t cyttsp_button_interrupt(int irq, void *dev_id)
{
	struct cyttsp_button_data *data = dev_id;
	if (data) {
		queue_work(data->irq_workqueue, &data->irq_work);
	}
	return IRQ_HANDLED;
}

#define BUTTON_REPORT_ERROR -1
#define BUTTON_REPORT_OK 0
int cypress_button_report(struct cyttsp_button_data *unuse_data)
{
	struct cyttsp_button_data *data = unuse_data;
	struct cyttsp_button_platform_data *pdata = data->pdata;
	bool curr_state, new_state;
	bool sync = false;
	u8 val;
	u8 key;
	unsigned long keystates;

	if (data->enable) {
#if 0
		val = cyttsp_read_reg(data, CYTTSP_REG_TOUCHMODE);
		if (val < 0) {
			dev_err(&data->client->dev, "Failed to read touch mode reg\n");
			return BUTTON_REPORT_ERROR;
		} else {
#ifdef CONFIG_BUTTON_GLOVE_MODE
			mutex_lock(&data->input_dev->mutex);
			data->glove_mode = !!(val & (1 << CYTTSP_GLOVE_MODE_SHIFT));
			mutex_unlock(&data->input_dev->mutex);
#endif
		}
#endif

		val = cyttsp_read_reg(data, pdata->button_status_reg);
		if (val < 0) {
			dev_err(&data->client->dev, "Failed to read status!\n");
			return BUTTON_REPORT_ERROR;
		}

		keystates = (unsigned long)val;

		for (key = 0; key < pdata->nbuttons; key++) {
			curr_state = test_bit(key, &data->keystatus);
			new_state = test_bit(key, &keystates);

			if (curr_state ^ new_state) {
				input_event(data->input_dev, EV_KEY,
					pdata->key_code[key], !!(keystates & (1 << key)));
				sync = true;
			}

		}

		data->keystatus = keystates;

		if (sync)
			input_sync(data->input_dev);

	}

	return BUTTON_REPORT_OK;
}

static bool cyttsp_need_update(struct cyttsp_button_data *data,
					const struct firmware *fw)
{
	struct device *dev = &data->client->dev;
	const u8 *curr = fw->data;
	int total = fw->size;
	int ret;
	u8 buffer[256];
	u8 id[2], in_chip_id[2];

	while (total != 0) {
		ret = cyttsp_get_one_line(curr, buffer);
		total -= ret;
		curr += ret;
	}

	id[1] = cyttsp_convert_to_u8(&buffer[55]);
	id[0] = cyttsp_convert_to_u8(&buffer[57]);

	ret = cyttsp_read_reg(data, CYTTSP_BUTTON_FW_VER1);
	if (ret < 0) {
		dev_err(dev, "Failed to read fw id0!\n");
		return false;
	}
	in_chip_id[0] = ret;

	ret = cyttsp_read_reg(data, CYTTSP_BUTTON_FW_VER2);
	if (ret < 0) {
		dev_err(dev, "Failed to read fw id1!\n");
		return false;
	}
	in_chip_id[1] = ret;

	dev_info(dev, "fw id = 0x%x 0x%x in chip id = 0x%x 0x%x\n",
			id[0], id[1], in_chip_id[0], in_chip_id[1]);

	if (id[0] != in_chip_id[0]
		|| id[1] != in_chip_id[1])
		return true;

	return false;
}

static const char *cyttsp_get_fw_name(struct cyttsp_button_data *data)
{
	struct device *dev = &data->client->dev;
	struct cyttsp_button_platform_data *pdata = data->pdata;
	int i;
	u8 panel_id;
	const char *fw_name = NULL;

	/* The first byte in lockdown info is panel maker */
	panel_id = data->lockdown_info[0];

	#if 0
	/*
	 * WAD: Check whether the lockdown info is valid, if not, just use Biel
	 * as the default one
	 */
	if (!(data->lockdown_info[1] >= 0x31 && data->lockdown_info[1] <= 0x39 &&
			data->lockdown_info[2] >= 0x31 && data->lockdown_info[2] <= 0x39 &&
			data->lockdown_info[3] >= 0x31 && data->lockdown_info[3] <= 0x39))
		panel_id = 0x31;	/* Biel */
	#endif

	dev_info(dev, "panel_id in chip = 0x%02x\n", panel_id);

	for (i = 0; i < pdata->config_array_size; i++) {
		if (panel_id == pdata->config_array[i].panel_id) {
			fw_name = pdata->config_array[i].fw_name;
			dev_info(dev, "fw name = %s\n", fw_name);
			break;
		}
	}

	if (!fw_name && pdata->default_config >= 0 &&
			pdata->default_config < pdata->config_array_size) {
		fw_name = pdata->config_array[pdata->default_config].fw_name;
		dev_info(dev, "Use default config, fw name = %s\n", fw_name);
	}

	return fw_name;
}

static int cyttsp_get_lockdown_info(struct cyttsp_button_data *data,
					bool in_bootloader)
{
	int ret = 0;
	u8 cmd[4];

	if (in_bootloader) {
		/* TODO: Get lockdown info in bootloader */
		ret = cyttsp_send_command(data, CYTTSP_CMD_ENTER_BTLD,
				NULL, 0, 0);
		if (ret) {
			dev_err(&data->client->dev,
					"Unable to enter bootloader\n");
			return ret;
		}

		cmd[0] = 0;
		ret = cyttsp_send_command(data, CYTTSP_CMD_READ_LOCKDOWN_INFO,
				cmd, 1, 0);
		if (ret) {
			dev_err(&data->client->dev,
					"Unable to get lockdown info\n");
			return ret;
		}
	} else {
#ifdef ZHWW_I2C_MUTEX
		ret = cyttsp_i2c_read_block(data, &data->client->dev,
				CYTTSP_REG_LOCKDOWN_INFO_START,
				CYTTSP_LOCKDOWN_INFO_SIZE,
				data->lockdown_info);
#else
		ret = cyttsp_i2c_read_block(&data->client->dev,
				CYTTSP_REG_LOCKDOWN_INFO_START,
				CYTTSP_LOCKDOWN_INFO_SIZE,
				data->lockdown_info);
#endif
		if (ret) {
			dev_err(&data->client->dev,
					"Unable to get lockdown info\n");
			return ret;
		}
	}

	dev_info(&data->client->dev, "Get lockdown info: 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X 0x%02X",
			data->lockdown_info[0], data->lockdown_info[1],
			data->lockdown_info[2], data->lockdown_info[3],
			data->lockdown_info[4], data->lockdown_info[5],
			data->lockdown_info[6], data->lockdown_info[7]);

	return ret;
}

static int cyttsp_firmware_update(struct cyttsp_button_data *data,
					bool in_bootloader)
{
	struct device *dev = &data->client->dev;
	struct cyttsp_button_platform_data *pdata = data->pdata;
	bool need_update = false;
	int ret = 0;
	const struct firmware *fw = NULL;
	int total, i = 0;
	const u8 *tmp_buf;
	const char *fw_name = NULL;
	u8 buffer[256];
	u8 cmd[256];

	u8 switch_to_btld_cmd[5] = {
		0x06, 0x2B, 0x2B, 0xFE, 0xFA,
	};
	/*
	u8 switch_to_softreset_cmd[2] = {
		0x05, 0x3d
	};

	u8 write_0x05_cmd[2] = {
		0x05, 0xaa
	};
	*/
	ret = cyttsp_get_lockdown_info(data, in_bootloader);

	fw_name = cyttsp_get_fw_name(data);

	if (fw_name == NULL) {
		dev_err(dev, "fw_name is empty, don't update!\n");
		return -EINVAL;
	}

	ret = request_firmware(&fw, fw_name, dev);
	if (ret < 0) {
		dev_err(dev, "Failed to request firmware %s\n", fw_name);
		return ret;
	}

	total = fw->size;
	tmp_buf = fw->data;

	/* check version, if not equal, update fw */
	need_update = cyttsp_need_update(data, fw);
	if (!need_update) {
		dev_err(dev, "version equal, no need update!\n");
		goto free_fw;
	}

	if (!in_bootloader) {
#ifdef ZHWW_I2C_MUTEX
		ret = cyttsp_i2c_send(data, dev, sizeof(switch_to_btld_cmd), switch_to_btld_cmd);
#else
		ret = cyttsp_i2c_send(dev, sizeof(switch_to_btld_cmd), switch_to_btld_cmd);
#endif
		if (ret) {
			dev_err(dev, "Failed to switch to bootloader!\n");
			goto free_fw;
		}
		dev_err(dev, "Success to send switch_to_btld_cmd\n");
	}

	msleep(20);

	data->app_addr = data->client->addr;

	data->bootloader_addr = pdata->bootloader_addr;

	data->client->addr = data->bootloader_addr;

	/* 1. enter into bootloader */
	ret = cyttsp_send_command(data, CYTTSP_CMD_ENTER_BTLD, NULL, 0, 0);
	if (ret) {
		dev_err(dev, "Failed to enter into bootloader!\n");
		goto end;
	}
	dev_err(dev, "enter into bootloader\n");

	/* 2. get flash size */
	cmd[0] = 0;
	ret = cyttsp_send_command(data, CYTTSP_CMD_GET_FLASH_SIZE, cmd, 1, 0);
	if (ret) {
		dev_err(dev, "Failed to get flash size!\n");
		goto end;
	}
	dev_err(dev, "get flash size\n");

	ret = cyttsp_get_one_line(tmp_buf, buffer);
	total -= ret;
	tmp_buf += ret;

	/* 3. send data and program, verify*/
	while (total > 0) {
		int j;
		u8 array_id;
		u16 row_num, length;
		u8 row_checksum;
		u8 data_buf[70];
		u8 tmp[4];
		ret = cyttsp_get_one_line(tmp_buf, buffer);
		total -= ret;
		tmp_buf += ret;

		array_id = cyttsp_convert_to_u8(&buffer[1]);
		row_num = cyttsp_convert_to_u16(&buffer[3]);
		length = cyttsp_convert_to_u16(&buffer[7]);
		dev_err(dev, "array_id = 0x%x, row_num = 0x%x, len = 0x%x\n",
				array_id, row_num, length);

		for (j = 0; j < length * 2; j += 2) {
			data_buf[j/2] =
				cyttsp_convert_to_u8(&buffer[11+j]);
		}
		/* send data here */
		ret = cyttsp_send_command(data, CYTTSP_CMD_SEND_DATA,
					&data_buf[0], length - 1, 0);
		if (ret) {
			dev_err(dev, "Failed to send data at time %d\n", i);
			goto end;
		}

		/* program here */
		tmp[0] = 0x00;
		tmp[1] = row_num & 0xFF;
		tmp[2] = (row_num & 0xFF00) >> 8;
		tmp[3] = data_buf[length - 1];
		ret = cyttsp_send_command(data, CYTTSP_CMD_PROGRAM_ROW, tmp, 4, 0);
		if (ret) {
			dev_err(dev, "Failed to program row at time %d!\n", i);
			goto end;
		}

		/* verify row */
		row_checksum = calculate_checksum8(&data_buf[0], length);
		tmp[0] = 0x00;
		tmp[1] = row_num & 0xFF;
		tmp[2] = (row_num & 0xFF00) >> 8;
		ret = cyttsp_send_command(data, CYTTSP_CMD_VERIFY_ROW, tmp, 3, row_checksum);
		if (ret) {
			dev_err(dev, "Failed to verify row at time %d\n", i);
			goto end;
		}

		i++;
	}
	dev_err(dev, "send data and program, verify\n");

end:
	/* 4. Exit bootloader mode */
	cmd[0] = 0;
	ret = cyttsp_send_command(data, CYTTSP_CMD_EXIT_BTLD, cmd, 1, 0);
	if (ret)
		dev_err(dev, "Failed to exit bootloader mode !\n");

	data->client->addr = data->app_addr;
free_fw:
	release_firmware(fw);
	return ret;
}

static ssize_t cyttsp_firmware_update_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct cyttsp_button_data *data = dev_get_drvdata(dev);
	int i;

	if (sscanf(buf, "%u", &i) == 1)
		if (i == 1)
			cyttsp_firmware_update(data, false);

	return count;
}

static ssize_t cyttsp_debug_enable_show(struct device *dev,
			struct device_attribute *attr, char *buf)
{
	struct cyttsp_button_data *data = dev_get_drvdata(dev);
	int count;
	char c;

	c = data->dbgdump ? '1' : '0';
	count = sprintf(buf, "%c\n", c);

	return count;
}

static ssize_t cyttsp_debug_enable_store(struct device *dev,
			struct device_attribute *attr, const char *buf, size_t count)
{
	struct cyttsp_button_data *data = dev_get_drvdata(dev);
	int i;

	if (sscanf(buf, "%u", &i) == 1 && i < 2) {
		data->dbgdump = (i == 1);

		dev_dbg(dev, "%s\n", i ? "debug enabled" : "debug disabled");
		return count;
	} else {
		dev_dbg(dev, "debug_enabled write error\n");
		return -EINVAL;
	}
}

static DEVICE_ATTR(firmware_update, S_IWUSR, NULL, cyttsp_firmware_update_store);
static DEVICE_ATTR(debug_enable, S_IWUSR | S_IRUSR, cyttsp_debug_enable_show,
			cyttsp_debug_enable_store);

static struct attribute *cyttsp_attrs[] = {
	&dev_attr_firmware_update.attr,
	&dev_attr_debug_enable.attr,
	NULL
};

static const struct attribute_group cyttsp_attr_group = {
	.attrs = cyttsp_attrs,
};

static int cyttsp_initialize_regulator(struct cyttsp_button_data *data)
{
	int ret;
	struct i2c_client *client = data->client;

	data->regulator_vdd = devm_regulator_get(&client->dev, "vdd");
	if (IS_ERR(data->regulator_vdd)) {
		dev_err(&client->dev,
			"%s: regulator_get failed: %ld\n",
			__func__, PTR_ERR(data->regulator_vdd));
		goto err_null_regulator;
	}

	data->regulator_vddio = devm_regulator_get(&client->dev, "vddio");
	if (IS_ERR(data->regulator_vddio)) {
		dev_err(&client->dev,
			"%s: regulator_get failed: %ld\n",
			__func__, PTR_ERR(data->regulator_vddio));
		goto err_put_regulator_vdd;
	}

	data->regulator_avdd = devm_regulator_get(&client->dev, "avdd");
	if (IS_ERR(data->regulator_avdd)) {
		dev_err(&client->dev,
			"%s: regulator_get failed: %ld\n",
			__func__, PTR_ERR(data->regulator_avdd));
		goto err_put_regulator_vddio;
	}

	ret = regulator_enable(data->regulator_vdd);
	if (ret < 0) {
		dev_err(&client->dev,
			"%s: regulator_enable failed: %d\n",
			__func__, ret);
		goto err_put_regulator_avdd;
	}

	ret = regulator_enable(data->regulator_vddio);
	if (ret < 0) {
		dev_err(&client->dev,
			"%s: regulator_enable failed: %d\n",
			__func__, ret);
		goto err_disable_regulator_vdd;
	}

	ret = regulator_enable(data->regulator_avdd);
	if (ret < 0) {
		dev_err(&client->dev,
			"%s: regulator_enable failed: %d\n",
			__func__, ret);
		goto err_disable_regulator_vddio;
	}

	return 0;

err_disable_regulator_vddio:
	regulator_disable(data->regulator_vddio);
err_disable_regulator_vdd:
	regulator_disable(data->regulator_vdd);
err_put_regulator_avdd:
	devm_regulator_put(data->regulator_avdd);
err_put_regulator_vddio:
	devm_regulator_put(data->regulator_vddio);
err_put_regulator_vdd:
	devm_regulator_put(data->regulator_vdd);
err_null_regulator:
	data->regulator_vdd = NULL;

	return ret;
}

static void cypress_irq_work_func(struct work_struct *work)
{
	struct cyttsp_button_data *data =  container_of(work, struct cyttsp_button_data, irq_work);
	if (data) {
		cypress_button_report(data);
	}else{
		printk("%s:Fail to get data.\n",__func__);
	}
}

static int cyttsp_button_probe(struct i2c_client *client,
		const struct i2c_device_id *id)
{
	struct cyttsp_button_platform_data *pdata;
	struct cyttsp_button_data *data;
	int error;
	int i;
	u8 val;

	pdata = kzalloc(sizeof(*pdata), GFP_KERNEL);
	dev_err(&client->dev, "cyttsp_button_probe start\n");

	if (client->dev.of_node) {
		pdata = devm_kzalloc(&client->dev,
				sizeof(struct cyttsp_button_platform_data), GFP_KERNEL);
		if (!pdata) {
			dev_err(&client->dev, "Failed to allocate memroy for pdata\n");
			return -ENOMEM;
		}

		error = cyttsp_button_parse_dt(&client->dev, pdata);
		if (error)
			return error;
	} else
		pdata = client->dev.platform_data;

	if (!pdata)
		return -EINVAL;

	data = kzalloc(sizeof(struct cyttsp_button_data), GFP_KERNEL);
	if (!data) {
		dev_err(&client->dev, "Failed to allocate memory for data!\n");
		return -ENOMEM;
	}

	data->client = client;
	data->pdata = pdata;

	error = cyttsp_initialize_regulator(data);
	if (error < 0) {
		dev_err(&client->dev, "Failed to initialize regulator\n");
		goto err_free_data;
	}

	#ifdef ZHWW_I2C_MUTEX
	mutex_init(&data->button_i2c_lock);
	#endif
#ifdef ZHWW_I2C_MUTEX
	error = cyttsp_i2c_read_block(data, &data->client->dev,
				CYTTSP_REG_TOUCHMODE, 1, &val);
#else
	error = cyttsp_i2c_read_block(&data->client->dev,
				CYTTSP_REG_TOUCHMODE, 1, &val);
#endif
	if (error < 0) {
		dev_err(&client->dev, "Failed to read register!\n");
		error = cyttsp_firmware_update(data, true);
		if (error)
			goto err_free_regulator;
	} else {
		#ifdef ZHWW_ADD_FW_UPDATE
		error = cyttsp_firmware_update(data, false);
		if (error){
			dev_err(&client->dev, "===4=== cyttsp_firmware_update error!\n");
			dev_err(&client->dev, "Failed to update firmware;\n");
		}
		#endif
	}

	error = gpio_request(pdata->irq_gpio, "cyttsp_button_irq_gpio");
	if (error) {
		dev_err(&client->dev, "Unable to request gpio [%d]\n",
				pdata->irq_gpio);
		goto err_free_regulator;
	}

	error = gpio_direction_input(pdata->irq_gpio);
	if (error) {
		dev_err(&client->dev, "unable to set direction for gpio [%d]\n",
				pdata->irq_gpio);
		goto err_irq_gpio_req;
	}

	i2c_set_clientdata(data->client, data);

	/* Initialize input device */
	data->input_dev = input_allocate_device();
	if (!data->input_dev) {
		dev_err(&client->dev, "Failed to allocate input device\n");
		goto err_irq_gpio_req;
	}

	if (data->pdata->input_name)
		data->input_dev->name = data->pdata->input_name;
	else
		data->input_dev->name = "cyttsp_button";

	data->input_dev->id.bustype = BUS_I2C;
	data->input_dev->dev.parent = &data->client->dev;
#ifndef CONFIG_FB
	data->input_dev->enable = cyttsp_button_input_enable;
	data->input_dev->disable = cyttsp_button_input_disable;
	data->input_dev->enabled = 1;
#else
	data->fb_notif.notifier_call = fb_notifier_callback;
	error = fb_register_client(&data->fb_notif);
	if (error)
		dev_err(&client->dev,
			"Unable to register fb_notifier: %d", error);
#endif

	for (i = 0; i < pdata->nbuttons; i++) {
		input_set_capability(data->input_dev, EV_KEY,
					pdata->key_code[i]);
	}

	input_set_drvdata(data->input_dev, data);
	dev_set_drvdata(&data->client->dev, data);

	__set_bit(EV_SYN, data->input_dev->evbit);
	__set_bit(EV_KEY, data->input_dev->evbit);

	error = input_register_device(data->input_dev);
	if (error) {
		dev_err(&client->dev, "Unable to register input device, error: %d\n", error);
		goto err_free_input;
	}

	error = request_threaded_irq(client->irq, NULL, cyttsp_button_interrupt,
					pdata->irqflags | IRQF_ONESHOT, client->dev.driver->name, data);
	if (error) {
		dev_err(&client->dev, "Error %d registering irq\n", error);
		goto err_unreg_input;
	}

	INIT_WORK(&data->irq_work, cypress_irq_work_func);
	data->irq_workqueue = create_singlethread_workqueue("cypress_irq_workqueue");
	if (!data->irq_workqueue) {
		printk(KERN_ERR "%s: Can't create cypress irq workqueue\n", __func__);
		goto err_create_irq_workqueue;
	}

	error = sysfs_create_group(&client->dev.kobj, &cyttsp_attr_group);
	if (error)
		dev_err(&client->dev, "Failure %d creating sysfs group\n",
			error);

#ifdef CONFIG_BUTTON_GLOVE_MODE
	data->glove_mode_notifier.notifier_call = cyttsp_glove_mode_notifier_callback;
	register_glove_mode_notifier(&data->glove_mode_notifier);
#endif

	data->enable = true;
	dev_err(&client->dev, "cyttsp_button_probe probe successfully\n");
	return 0;

err_unreg_input:
	input_unregister_device(data->input_dev);
err_create_irq_workqueue:
	//none
err_free_input:
	input_free_device(data->input_dev);
err_irq_gpio_req:
	if (gpio_is_valid(pdata->irq_gpio))
		gpio_free(pdata->irq_gpio);
err_free_regulator:
	if (data->regulator_vdd)
		regulator_disable(data->regulator_vdd);
	if (data->regulator_avdd)
		regulator_disable(data->regulator_avdd);
	if (data->regulator_vddio)
		regulator_disable(data->regulator_vddio);
err_free_data:
	kfree(data);
	return error;
}

static int cyttsp_button_remove(struct i2c_client *client)
{
	struct cyttsp_button_data *data = i2c_get_clientdata(client);
	const struct cyttsp_button_platform_data *pdata = data->pdata;

#ifdef CONFIG_BUTTON_GLOVE_MODE
	unregister_glove_mode_notifier(&data->glove_mode_notifier);
#endif
	sysfs_remove_group(&client->dev.kobj, &cyttsp_attr_group);
	free_irq(client->irq, data);
	input_unregister_device(data->input_dev);
	if (gpio_is_valid(pdata->irq_gpio))
		gpio_free(pdata->irq_gpio);

	if (data->regulator_vdd)
		regulator_disable(data->regulator_vdd);
	if (data->regulator_avdd)
		regulator_disable(data->regulator_avdd);
	if (data->regulator_vddio)
		regulator_disable(data->regulator_vddio);

	kfree(data);
	data = NULL;

	return 0;
}

static void cyttsp_button_shutdown(struct i2c_client *client)
{
	disable_irq(client->irq);
}

static const struct i2c_device_id cyt_id[] = {
	{"cyttsp_button", 0},
	{ },
};

#if defined(CONFIG_PM_SLEEP)
static int cyttsp_button_suspend(struct device *dev)
{
	struct cyttsp_button_data *data = dev_get_drvdata(dev);
	dev_err(dev, "disable_irq:%d\n", data->client->irq);
	disable_irq(data->client->irq);
	return 0;
}

static int cyttsp_button_resume(struct device *dev)
{
	struct cyttsp_button_data *data = dev_get_drvdata(dev);
	dev_err(dev, "enable_irq:%d\n", data->client->irq);
	enable_irq(data->client->irq);
	return 0;
}
#endif

#ifdef ZHWW_ADD_SUSPEND_RESUME
const struct dev_pm_ops cyttsp_button_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(cyttsp_button_suspend, cyttsp_button_resume)
};
#endif

#ifdef CONFIG_OF
static struct of_device_id cyttsp_match_table[] = {
	{ .compatible = "cypress,s1302s",},
	{ },
};
#else
#define cyttsp_match_table NULL
#endif

static struct i2c_driver cyttsp_button_driver = {
	.driver = {
		.name	= "cyttsp_button",
		.owner	= THIS_MODULE,
		.of_match_table = cyttsp_match_table,
#ifdef ZHWW_ADD_SUSPEND_RESUME
		.pm = &cyttsp_button_pm_ops,
#endif
	},
	.probe		= cyttsp_button_probe,
	.remove		= cyttsp_button_remove,
	.shutdown	= cyttsp_button_shutdown,
	.id_table	= cyt_id,
};

static int __init cyttsp_button_init(void)
{
	return i2c_add_driver(&cyttsp_button_driver);
}

static void __exit cyttsp_button_exit(void)
{
	i2c_del_driver(&cyttsp_button_driver);
}

module_init(cyttsp_button_init);
module_exit(cyttsp_button_exit);

/* Module information */
MODULE_AUTHOR("XXX <XXXX@cypress.com>");
MODULE_DESCRIPTION("Cypress StreetFighter Button Driver");
MODULE_LICENSE("GPL");

