/*
 * This file is part of the flashrom project.
 *
 * Copyright (C) 2019 Miklós Márton martonmiklosqdev@gmail.com
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

/*
 * Driver for programming SPI flash chips using the SPI port
 * of the STMicroelectronics's STLINK-V3 programmer/debugger.
 *
 * The implementation is inspired by the ST's STLINK-V3-BRIDGE C++ API:
 * https://www.st.com/en/development-tools/stlink-v3-bridge.html
 */

#include "flash.h"
#include "programmer.h"
#include "spi.h"

#include <libusb.h>
#include <limits.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
	FW_VERSION_OK,
	FW_VERSION_OLD,
} fw_version_check_result_t;

typedef enum {
	SPI_BAUDRATEPRESCALER_2 = 0,
	SPI_BAUDRATEPRESCALER_4 = 1,
	SPI_BAUDRATEPRESCALER_8 = 2,
	SPI_BAUDRATEPRESCALER_16 = 3,
	SPI_BAUDRATEPRESCALER_32 = 4,
	SPI_BAUDRATEPRESCALER_64 = 5,
	SPI_BAUDRATEPRESCALER_128 = 6,
	SPI_BAUDRATEPRESCALER_256 = 7
} SPI_prescaler_t;

typedef enum {
	SPI_DIRECTION_2LINES_FULLDUPLEX = 0,
	SPI_DIRECTION_2LINES_RXONLY = 1,
	SPI_DIRECTION_1LINE_RX = 2,
	SPI_DIRECTION_1LINE_TX = 3
} SPI_dir_t;

typedef enum {
	SPI_MODE_SLAVE = 0,
	SPI_MODE_MASTER = 1
} SPI_mode_t;

typedef enum {
	SPI_DATASIZE_16B = 0,
	SPI_DATASIZE_8B = 1
} SPI_datasize_t;

typedef enum {
	SPI_CPOL_LOW = 0,
	SPI_CPOL_HIGH = 1
} SPI_CPOL_t;

typedef enum {
	SPI_CPHA_1EDGE = 0,
	SPI_CPHA_2EDGE = 1
} SPI_CPHA_t;

typedef enum {
	SPI_FIRSTBIT_LSB = 0,
	SPI_FIRSTBIT_MSB = 1
} SPI_firstbit_t;

typedef enum {
	SPI_NSS_SOFT = 0,
	SPI_NSS_HARD = 1
} SPI_NSS_t;

typedef enum {
	SPI_NSS_LOW = 0,
	SPI_NSS_HIGH = 1
} SPI_NSS_Level_t;

#define ST_GETVERSION_EXT						0xFB

#define STLINK_BRIDGE_COMMAND					0xFC
#define STLINK_BRIDGE_CLOSE						0x01
#define STLINK_BRIDGE_GET_RWCMD_STATUS			0x02
#define STLINK_BRIDGE_GET_CLOCK					0x03
#define STLINK_BRIDGE_INIT_SPI					0x20
#define STLINK_BRIDGE_WRITE_SPI					0x21
#define STLINK_BRIDGE_READ_SPI					0x22
#define STLINK_BRIDGE_CS_SPI					0x23

#define STLINK_BRIDGE_SPI_ERROR					0x02

#define STLINK_SPI_COM							0x02

#define STLINK_EP_OUT							0x06
#define STLINK_EP_IN							0x86

#define FIRMWARE_BRIDGE_STLINK_V3_LAST_VERSION	3

#define USB_TIMEOUT								5000

const struct dev_entry devs_stlinkv3_spi[] = {
	{0x0483, 0x374F, OK, "STMicroelectronics", "STLINK-V3"},
	{0}
};

static struct libusb_context *usb_ctx;
static libusb_device_handle *stlinkv3_handle;

/**
 * @param[out] bridge_input_clk Current input frequency in kHz of the given com.
 */
static int stlinkv3_get_clk(uint32_t *bridge_input_clk)
{
	uint8_t command[16];
	uint8_t answer[12];
	int actualLength = 0;
	int rc = 0;

	if (bridge_input_clk == NULL)
		return -1;

	memset(command, 0, sizeof(command));
	memset(answer, 0, sizeof(answer));

	command[0] = STLINK_BRIDGE_COMMAND;
	command[1] = STLINK_BRIDGE_GET_CLOCK;
	command[2] = STLINK_SPI_COM;

	rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_OUT,
							  command, sizeof(command),
							  &actualLength, USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(command)) {
		msg_perr("Failed to issue the STLINK_BRIDGE_GET_CLOCK command: '%s'\n",
				 libusb_error_name(rc));
		return -1;
	}

	rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_IN,
							  answer, sizeof(answer),
							  &actualLength, USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(answer)) {
		msg_perr("Failed to get STLINK_BRIDGE_GET_CLOCK answer: '%s'\n",
				 libusb_error_name(rc));
		return -1;
	}

	*bridge_input_clk = (uint32_t)answer[4]
						| (uint32_t)answer[5]<<8
						| (uint32_t)answer[6]<<16
						| (uint32_t)answer[7]<<24;
	return 0;

}

static int stlinkv3_spi_calc_prescaler(uint16_t reqd_freq_in_kHz,
									   SPI_prescaler_t *prescaler,
									   uint16_t *calculated_freq_in_kHz)
{
	uint32_t bridge_clk_in_kHz;
	uint32_t calculated_prescaler = 1;
	uint16_t prescaler_value;

	if (stlinkv3_get_clk(&bridge_clk_in_kHz))
		return -1;

	calculated_prescaler  = bridge_clk_in_kHz/reqd_freq_in_kHz;
	// Apply a smaller frequency if not exact
	if (calculated_prescaler  <= 2) {
		*prescaler = SPI_BAUDRATEPRESCALER_2;
		prescaler_value = 2;
	} else if (calculated_prescaler  <= 4) {
		*prescaler = SPI_BAUDRATEPRESCALER_4;
		prescaler_value = 4;
	} else if (calculated_prescaler  <= 8) {
		*prescaler = SPI_BAUDRATEPRESCALER_8;
		prescaler_value = 8;
	} else if (calculated_prescaler  <= 16) {
		*prescaler = SPI_BAUDRATEPRESCALER_16;
		prescaler_value = 16;
	} else if (calculated_prescaler  <= 32) {
		*prescaler = SPI_BAUDRATEPRESCALER_32;
		prescaler_value = 32;
	} else if (calculated_prescaler  <= 64) {
		*prescaler = SPI_BAUDRATEPRESCALER_64;
		prescaler_value = 64;
	} else if (calculated_prescaler  <= 128) {
		*prescaler = SPI_BAUDRATEPRESCALER_128;
		prescaler_value = 128;
	} else if (calculated_prescaler  <= 256) {
		*prescaler = SPI_BAUDRATEPRESCALER_256;
		prescaler_value = 256;
	} else {
		// smaller frequency not possible
		*prescaler = SPI_BAUDRATEPRESCALER_256;
		prescaler_value = 256;
	}

	*calculated_freq_in_kHz = bridge_clk_in_kHz / prescaler_value;

	return 0;
}

static int stlinkv3_check_version(fw_version_check_result_t *result)
{
	uint8_t answer[12];
	uint8_t command[16];
	int actualLength = 0;
	int rc = 0;

	memset(command, 0, sizeof(command));

	command[0] = ST_GETVERSION_EXT;
	command[1] = 0x80;

	rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_OUT,
							  command, sizeof(command),
							  &actualLength, USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(command)) {
		msg_perr("Failed to issue the ST_GETVERSION_EXT command: '%s'\n",
				 libusb_error_name(rc));
		return -1;
	}

	rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_IN,
							  answer, sizeof(answer),
							  &actualLength, USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(answer)) {
		msg_perr("Failed to retrive the ST_GETVERSION_EXT answer: '%s'\n",
				 libusb_error_name(rc));
		return -1;
	}

	msg_pinfo("Connected to STLink V3 with bridge FW version: %d\n", answer[4]);
	*result = answer[4] >= FIRMWARE_BRIDGE_STLINK_V3_LAST_VERSION
			? FW_VERSION_OK
			: FW_VERSION_OLD;
	return 0;
}

static int stlinkv3_spi_open(uint16_t reqested_freq_in_kHz)
{
	uint8_t command[16];
	uint8_t answer[2];
	uint16_t SCK_freq_in_kHz;
	int actualLength = 0;
	int rc = 0;
	SPI_prescaler_t prescaler;
	fw_version_check_result_t fw_check_result;

	if (stlinkv3_check_version(&fw_check_result)) {
		msg_perr("Failed to query FW version");
		return -1;
	}

	if (fw_check_result != FW_VERSION_OK) {
		msg_pinfo("Your STLink V3 has too old version of the bridge interface\n"
				  "Please update the firmware with the "
				  "STSW-LINK007 which can be downloaded from here:\n"
				  "https://www.st.com/en/development-tools/stsw-link007.html");
		return -1;
	}

	if (stlinkv3_spi_calc_prescaler(reqested_freq_in_kHz,
									&prescaler,
									&SCK_freq_in_kHz)) {
		msg_perr("Failed to calculate SPI clock prescaler");
		return -1;
	}
	msg_pinfo("SCK frequency set to %d kHz\n", SCK_freq_in_kHz);

	memset(command, 0, sizeof(command));
	memset(answer, 0, sizeof(answer));

	command[0] = STLINK_BRIDGE_COMMAND;
	command[1] = STLINK_BRIDGE_INIT_SPI;
	command[2] = SPI_DIRECTION_2LINES_FULLDUPLEX;
	command[3] = (SPI_MODE_MASTER
			| (SPI_CPHA_1EDGE << 1)
			| (SPI_CPOL_LOW << 2)
			| (SPI_FIRSTBIT_MSB << 3));
	command[4] = SPI_DATASIZE_8B;
	command[5] = SPI_NSS_SOFT;
	command[6] = (uint8_t)prescaler;

	rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_OUT,
							  command, sizeof(command),
							  &actualLength, USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED && actualLength != sizeof(command)) {
		msg_perr("Failed to issue the STLINK_BRIDGE_INIT_SPI command: '%s'\n",
				 libusb_error_name(rc));
		return -1;
	}

	rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_IN,
							  answer, sizeof(answer),
							  &actualLength, USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(answer)) {
		msg_perr("Failed to retrive the STLINK_BRIDGE_INIT_SPI answer: '%s'\n",
				 libusb_error_name(rc));
		return -1;
	}
	return 0;
}

static int stlinkv3_get_last_readwrite_status(uint32_t *status)
{
	uint8_t command[16];
	uint16_t answer[4];
	int rc = 0;
	int actualLength = 0;

	memset(command, 0, sizeof(command));
	memset(answer, 0, sizeof(answer));

	command[0] = STLINK_BRIDGE_COMMAND;
	command[1] = STLINK_BRIDGE_GET_RWCMD_STATUS;

	rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_OUT,
							  command, sizeof(command),
							  &actualLength, USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(command)) {
		msg_perr("Failed to issue the STLINK_BRIDGE_GET_RWCMD_STATUS command: '%s'\n",
				 libusb_error_name(rc));
		return -1;
	}

	rc = libusb_bulk_transfer(stlinkv3_handle,
							  STLINK_EP_IN,
							  (uint8_t *)answer,
							  sizeof(answer),
							  &actualLength,
							  USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(answer)) {
		msg_perr("Failed to retrive the STLINK_BRIDGE_GET_RWCMD_STATUS answer: '%s'\n",
				 libusb_error_name(rc));
		return -1;
	}

	*status = (uint32_t)answer[2] | (uint32_t)answer[3]<<16;
	return 0;
}

static int stlinkv3_spi_set_SPI_NSS(SPI_NSS_Level_t NSS_level)
{
	uint8_t command[16];
	uint8_t answer[2];
	int rc = 0;
	int actualLength = 0;

	memset(command, 0, sizeof(command));
	memset(answer, 0, sizeof(answer));

	command[0] = STLINK_BRIDGE_COMMAND;
	command[1] = STLINK_BRIDGE_CS_SPI;
	command[2] = (uint8_t) (NSS_level);

	rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_OUT,
							  command, sizeof(command),
							  &actualLength, USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(command)) {
		msg_perr("Failed to issue the STLINK_BRIDGE_CS_SPI command: '%s'\n",
				 libusb_error_name(rc));
		return -1;
	}

	rc = libusb_bulk_transfer(stlinkv3_handle,
							  STLINK_EP_IN,
							  (uint8_t *)answer,
							  sizeof(answer),
							  &actualLength,
							  USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(answer)) {
		msg_perr("Failed to retrive the STLINK_BRIDGE_CS_SPI answer: '%s'\n",
				 libusb_error_name(rc));
		return -1;
	}
	return 0;
}

static int stlinkv3_spi_transmit(struct flashctx *flash,
								 unsigned int write_cnt,
								 unsigned int read_cnt,
								 const unsigned char *write_arr,
								 unsigned char *read_arr)
{
	uint8_t command[16];
	int rc = 0 ;
	int actualLength = 0;
	uint32_t rw_status = 0;

	if (stlinkv3_spi_set_SPI_NSS(SPI_NSS_LOW)) {
		msg_perr("Failed to set the NSS pin to low\n");
		return -1;
	}

	memset(command, 0, sizeof(command));

	command[0] = STLINK_BRIDGE_COMMAND;
	command[1] = STLINK_BRIDGE_WRITE_SPI;
	command[2] = (uint8_t)write_cnt;
	command[3] = (uint8_t)(write_cnt >> 8);

	for (unsigned int i = 0; (i < 8) && (i < write_cnt); i++)
		command[4+i] = write_arr[i];

	rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_OUT,
							  command, sizeof(command),
							  &actualLength, USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(command)) {
		msg_perr("Failed to issue the STLINK_BRIDGE_WRITE_SPI command: '%s'\n",
				 libusb_error_name(rc));
		goto transmit_err;
	}

	if (write_cnt > 8) {
		rc = libusb_bulk_transfer(stlinkv3_handle,
								  STLINK_EP_OUT,
								  (unsigned char *)&write_arr[8],
								  (unsigned int)(write_cnt - 8),
								  &actualLength,
								  USB_TIMEOUT);
		if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != (write_cnt - 8)) {
			msg_perr("Failed to send the  data after the "
					 "STLINK_BRIDGE_WRITE_SPI command: '%s'\n",
					 libusb_error_name(rc));
			goto transmit_err;
		}
	}

	if (stlinkv3_get_last_readwrite_status(&rw_status))
		return -1;

	if (rw_status != 0) {
		msg_perr("SPI read/write failure: %d\n", rw_status);
		goto transmit_err;
	}

	if (read_cnt) {
		command[1] = STLINK_BRIDGE_READ_SPI;
		command[2] = (uint8_t)read_cnt;
		command[3] = (uint8_t)(read_cnt >> 8);

		rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_OUT,
								  command, sizeof(command),
								  &actualLength, USB_TIMEOUT);
		if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(command)) {
			msg_perr("Failed to issue the STLINK_BRIDGE_READ_SPI command: '%s'\n",
					 libusb_error_name(rc));
			goto transmit_err;
		}

		rc = libusb_bulk_transfer(stlinkv3_handle,
								  STLINK_EP_IN,
								  (unsigned char *)read_arr,
								  (int)read_cnt,
								  &actualLength,
								  USB_TIMEOUT);
		if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != read_cnt) {
			msg_perr("Failed to retrive the STLINK_BRIDGE_READ_SPI answer: '%s'\n",
					 libusb_error_name(rc));
			goto transmit_err;
		}
	}

	if (stlinkv3_get_last_readwrite_status(&rw_status))
		goto transmit_err;

	if (rw_status != 0) {
		msg_perr("SPI read/write failure: %d\n", rw_status);
		goto transmit_err;
	}

	if (stlinkv3_spi_set_SPI_NSS(SPI_NSS_HIGH)) {
		msg_perr("Failed to set the NSS pin to high\n");
		return -1;
	}
	return 0;

transmit_err:
	if (stlinkv3_spi_set_SPI_NSS(SPI_NSS_HIGH))
		msg_perr("Failed to set the NSS pin to high\n");
	return -1;
}

static int stlinkv3_spi_shutdown(void *data)
{
	uint8_t command[16];
	uint8_t answer[2];
	int actualLength = 0;
	int rc = 0;

	memset(command, 0, sizeof(command));
	memset(answer, 0, sizeof(answer));

	command[0] = STLINK_BRIDGE_COMMAND;
	command[1] = STLINK_BRIDGE_CLOSE;
	command[2] = STLINK_SPI_COM;

	rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_OUT,
							  command, sizeof(command), &actualLength, USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(command))
		msg_perr("Failed to issue the STLINK_BRIDGE_CLOSE command: '%s'\n",
				 libusb_error_name(rc));

	rc = libusb_bulk_transfer(stlinkv3_handle, STLINK_EP_IN,
							  answer, sizeof(answer), &actualLength, USB_TIMEOUT);
	if (rc != LIBUSB_TRANSFER_COMPLETED || actualLength != sizeof(answer))
		msg_perr("Failed to retrive the STLINK_BRIDGE_CLOSE answer: '%s'\n",
				 libusb_error_name(rc));

	libusb_close(stlinkv3_handle);
	libusb_exit(usb_ctx);

	return 0;
}

static const struct spi_master spi_programmer_stlinkv3 = {
	.max_data_read = UINT16_MAX,
	.max_data_write = UINT16_MAX,
	.command = stlinkv3_spi_transmit,
	.multicommand = default_spi_send_multicommand,
	.read = default_spi_read,
	.write_256 = default_spi_write_256,
	.write_aai = default_spi_write_aai,
};

int stlinkv3_spi_init(void)
{
	uint16_t sck_freq_kHz = 1000;	// selecting 1 MHz SCK is a good bet
	char *speed_str = NULL;
	char *serialno = NULL;
	char *endptr = NULL;

	libusb_init(&usb_ctx);
	if (!usb_ctx) {
		msg_perr("Could not initialize libusb!\n");
		return 1;
	}

	serialno = extract_programmer_param("serial");
	if (serialno)
		msg_pdbg("Opening STLINK-V3 with serial: %s\n", serialno);
	stlinkv3_handle = usb_dev_get_by_vid_pid_serial(usb_ctx,
			devs_stlinkv3_spi[0].vendor_id, devs_stlinkv3_spi[0].device_id, serialno);

	if (!stlinkv3_handle) {
		if (serialno)
			msg_perr("No STLINK-V3 seems to be connected with serial %s\n", serialno);
		else
			msg_perr("Could not find any connected STLINK-V3\n");
		free(serialno);
		goto err_exit;
	}
	free(serialno);

	speed_str = extract_programmer_param("spispeed");
	if (speed_str) {
		sck_freq_kHz = strtoul(speed_str, &endptr, 0);
		if (*endptr) {
			msg_perr("The spispeed parameter passed with invalid format: %s\n",
									 speed_str);
			msg_perr("Please pass the parameter with a simple number in kHz\n");
			return -1;
		}
		free(speed_str);
	}

	if (stlinkv3_spi_open(sck_freq_kHz))
		goto err_exit;

	if (register_shutdown(stlinkv3_spi_shutdown, NULL))
		goto err_exit;

	if (register_spi_master(&spi_programmer_stlinkv3))
		goto err_exit;

	return 0;

err_exit:
	libusb_exit(usb_ctx);
	return 1;
}