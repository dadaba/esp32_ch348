/*
 * SPDX-FileCopyrightText: 2021-2023 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Unlicense OR CC0-1.0
 */

#include <stdlib.h>
#include "freertos/FreeRTOSConfig.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_mac.h"
#include <string.h>
#include "ch9344.h"


static const char *TAG = "CH9344";
static ch9344_t *s_ch9344;

static int data_rx_cb(const uint8_t *data, size_t data_len, void *arg)
{
	ch9344_t *ch9344 = s_ch9344;
	int res = -1;
	u8 port = data[0];
	u8 len = data[1];
	if(port > MAXPORT || len > 30){
		ESP_LOGE(TAG, "data received error");
		return pdFALSE;
	}
	//ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);
	int freesize = getRingbufferFreeLenNoLock(ch9344->rb[port].buf);
	//ESP_LOGI(TAG, "ringbuf free size:%d", freesize);
	if(freesize < len){
		ESP_LOGE(TAG, "port:%d, ringbuf no enough space,free:%d,len:%d\n", port, freesize, len);
	}
	res = writeRingbufferNoLock((unsigned char *)data + 2, len, ch9344->rb[port].buf);
	if(res != 0){
		ESP_LOGE(TAG, "xRingbufferSend failed:%d", res);
	}
	xSemaphoreGive(ch9344->rx_available);
    return res;

}
static bool cmd_data_rx_cb(const uint8_t *data, size_t data_len, void *arg)
{
    //ESP_LOGI(TAG, "cmd data received");
    //ESP_LOG_BUFFER_HEXDUMP(TAG, data, data_len, ESP_LOG_INFO);
    return true;
}

static void reset_rx_transfer(usb_transfer_t *transfer)
{
	ch9344_t *ch9344 = (ch9344_t *)transfer->context;
    transfer->num_bytes = transfer->data_buffer_size;
    // This is a hotfix for IDF changes, where 'transfer->data_buffer_size' does not contain actual buffer length,
    // but *allocated* buffer length, which can be larger if CONFIG_HEAP_POISONING_COMPREHENSIVE is enabled
    transfer->num_bytes -= transfer->data_buffer_size % ch9344->data_rx_mps;
}

static bool is_transfer_completed(usb_transfer_t *transfer)
{
    bool completed = false;

    switch (transfer->status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        completed = true;
        break;
    case USB_TRANSFER_STATUS_NO_DEVICE: // User is notified about device disconnection from usb_event_cb
    case USB_TRANSFER_STATUS_CANCELED:
        break;
    case USB_TRANSFER_STATUS_ERROR:
    case USB_TRANSFER_STATUS_TIMED_OUT:
    case USB_TRANSFER_STATUS_STALL:
    case USB_TRANSFER_STATUS_OVERFLOW:
    case USB_TRANSFER_STATUS_SKIPPED:
    default:
        ESP_LOGE(TAG, "Transfer failed, status %d", transfer->status);
        break;
    }
    return completed;
}
static void in_xfer_cb(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "in xfer cb");
    ch9344_t *ch9344 = (ch9344_t *)transfer->context;

    if (!is_transfer_completed(transfer)) {
        return;
    }

	data_rx_cb(transfer->data_buffer, transfer->actual_num_bytes, 0);
	reset_rx_transfer(ch9344->rx_xfer);
    ESP_LOGD(TAG, "Submitting poll for BULK IN transfer");
    usb_host_transfer_submit(ch9344->rx_xfer);
}

static void cmd_in_xfer_cb(usb_transfer_t *transfer)
{
    ESP_LOGD(TAG, "cmd in xfer cb");
    ch9344_t *ch9344 = (ch9344_t *)transfer->context;

    if (!is_transfer_completed(transfer)) {
        return;
    }

	const bool data_processed = cmd_data_rx_cb(transfer->data_buffer, transfer->actual_num_bytes, 0);

	// Information for developers:
	// In order to save RAM and CPU time, the application can indicate that the received data was not processed and that the application expects more data.
	// In this case, the next received data must be appended to the existing buffer.
	// Since the data_buffer in usb_transfer_t is a constant pointer, we must cast away to const qualifier.
	if (!data_processed) {

	} else {
		reset_rx_transfer(ch9344->cmd_rxfer);
	}

    ESP_LOGD(TAG, "Submitting poll for cmd BULK IN transfer");
    usb_host_transfer_submit(ch9344->cmd_rxfer);
}


static esp_err_t rx_poll_submit(ch9344_t *device) 
{
    if (device->rx_xfer) {
        ESP_LOGD(TAG, "Submitting poll for BULK IN transfer");
        RETURN_ON_ERROR(usb_host_transfer_submit(device->rx_xfer));
    }	
	if (device->cmd_rxfer) {
        ESP_LOGD(TAG, "Submitting poll for BULK IN transfer");
        RETURN_ON_ERROR(usb_host_transfer_submit(device->cmd_rxfer));
    }
	return ESP_OK;
}


static void transfer_callback(usb_transfer_t *transfer)
{
    ch9344_t *device = (ch9344_t *)transfer->context;

    if (transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        ESP_LOGE("Transfer failed", "Status %d", transfer->status);
    }
	//ESP_LOGI("transfer success", "endpoint addr=%x\n", transfer->bEndpointAddress);

    xSemaphoreGive(device->transfer_done);
}

static usb_transfer_status_t wait_for_transfer_done(usb_transfer_t *xfer)
{
    ch9344_t *device = (ch9344_t *)xfer->context;
    BaseType_t received = xSemaphoreTake(device->transfer_done, pdMS_TO_TICKS(xfer->timeout_ms));
    usb_transfer_status_t status = xfer->status;

    if (received != pdTRUE) {
        usb_host_endpoint_halt(xfer->device_handle, xfer->bEndpointAddress);
        usb_host_endpoint_flush(xfer->device_handle, xfer->bEndpointAddress);
        usb_host_endpoint_clear(xfer->device_handle, xfer->bEndpointAddress);
        xSemaphoreTake(device->transfer_done, portMAX_DELAY); // Since we flushed the EP, this should return immediately
        status = USB_TRANSFER_STATUS_TIMED_OUT;
    }

    return status;
}

static esp_err_t usb_bulk_transfer(ch9344_t *device, uint8_t *data, int size, ch9344_endpoint_t ep)
{
    esp_err_t ret = ESP_OK;
    usb_transfer_t *xfer = NULL;
    int transfer_size = 32;

	//xSemaphoreTake(device->tx_endpoint, portMAX_DELAY);

    if (ep == CMD_EP_OUT){
        transfer_size = size;
		xfer = device->cmd_txfer;
        xfer->bEndpointAddress = device->cmdtx_endpoint;
    } else if (ep == DATA_EP_OUT) {
        transfer_size = size;
		xfer = device->tx_xfer;
        xfer->bEndpointAddress = device->tx_endpoint;
    } else {
		ret= ESP_ERR_INVALID_ARG;
		goto end;
	}

	//ESP_LOGI("usb_bulk_transfer", "transfer_size = %d,endpoint addr=%x, data_buffer_size=%d\n", transfer_size, xfer->bEndpointAddress, xfer->data_buffer_size);
     
	if (ep == CMD_EP_OUT || ep == DATA_EP_OUT) {
		//memset(xfer->data_buffer, 0, transfer_size);
        memcpy(xfer->data_buffer, data, size);
    }
	//ESP_LOG_BUFFER_HEXDUMP(TAG, xfer->data_buffer, transfer_size, ESP_LOG_INFO);

    xfer->num_bytes = transfer_size;
    xfer->device_handle = device->dev_hdl;
    xfer->callback = transfer_callback;
    xfer->timeout_ms = 5000;
    xfer->context = device;

    RETURN_ON_ERROR( usb_host_transfer_submit(xfer) );
    const usb_transfer_status_t status = wait_for_transfer_done(xfer);
    switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED:
        if (ep == CMD_EP_IN || ep == DATA_EP_IN) {
            memcpy(data, xfer->data_buffer, xfer->actual_num_bytes);
        }
        ret = ESP_OK;
        break;
    case USB_TRANSFER_STATUS_STALL:
        ret = ESP_ERR_CH9344_STALL; break;
    default:
        ret = ESP_ERR_CH9344_INTERNAL; break;
    }

end:
    return ret;
}

/*
 * Functions for CH9344 cmd messages.
 */
static int ch9344_cmd_out(ch9344_t *ch9344, char *buf, int count)
{
	uint8_t *data = (uint8_t *)buf;
	return ESP_ERROR_CHECK_WITHOUT_ABORT(usb_bulk_transfer(ch9344, data, count, CMD_EP_OUT));
}


static inline int ch9344_set_control(ch9344_t *ch9344, int portnum, int control)
{
	char buffer[3];
	int retval;
	const unsigned size = 3;
	u8 rgadd = 0;

	if (ch9344->chiptype == CHIP_CH9344)
		rgadd = 0x10 * portnum + 0x08;
	else if (ch9344->chiptype == CHIP_CH348L || ch9344->chiptype == CHIP_CH348Q) {
		if (portnum < 4)
			rgadd = 0x10 * portnum;
		else
			rgadd = 0x10 * (portnum - 4) + 0x08;
	}

	buffer[0] = CMD_W_BR;
	buffer[1] = rgadd + R_C4;
	buffer[2] = control;

	retval = ch9344_cmd_out(ch9344, buffer, size);

	return retval < 0 ? retval : 0;
}


static int ch9344_configure(ch9344_t *ch9344, int portnum)
{
	char *buffer;
	int ret;
	u8 request;
	u8 rgadd = 0;

	buffer = malloc(ch9344->cmd_tx_mps);
	if (!buffer)
		return ESP_ERR_NO_MEM;

	request = CMD_W_R;
	if (ch9344->chiptype == CHIP_CH9344)
		rgadd = 0x10 * portnum + 0x08;
	else if (ch9344->chiptype == CHIP_CH348L || ch9344->chiptype == CHIP_CH348Q) {
		if (portnum < 4)
			rgadd = 0x10 * portnum;
		else
			rgadd = 0x10 * (portnum - 4) + 0x08;
	}

	buffer[0] = request;
	buffer[1] = rgadd + R_C2;
	buffer[2] = 0x87;
	ret = ch9344_cmd_out(ch9344, buffer, 0x03);
	if (ret < 0)
		goto out;

	if (ch9344->chiptype == CHIP_CH9344) {
		buffer[1] = rgadd + R_C3;
		buffer[2] = 0x03;
		ret = ch9344_cmd_out(ch9344, buffer, 0x03);
		if (ret < 0)
			goto out;
	}

	buffer[1] = rgadd + R_C4;
	buffer[2] = 0x08;
	ret = ch9344_cmd_out(ch9344, buffer, 0x03);
	if (ret < 0)
		goto out;
out:
	free(buffer);
	return ret < 0 ? ret : 0;
}


static void client_event_cb(const usb_host_client_event_msg_t *event_msg, void *arg)
{
    ch9344_t *ch9344 = (ch9344_t *)arg;
    switch (event_msg->event) {
    case USB_HOST_CLIENT_EVENT_NEW_DEV:
        if (ch9344->dev_addr == 0) {
            ch9344->dev_addr = event_msg->new_dev.address;
            //Open the device next
            ch9344->actions |= ACTION_OPEN_DEV;
        }
        break;
    case USB_HOST_CLIENT_EVENT_DEV_GONE:
        if (ch9344->dev_hdl != NULL) {
            //Cancel any other actions and close the device next
            ch9344->actions = ACTION_CLOSE_DEV;
        }
        break;
    default:
        //Should never occur
        abort();
    }
}


static int ch9344_get(int clockRate, int bval, unsigned char *bd1, unsigned char *bd2)
{
	int dis, x2;

	if (bval < 0 || bval > 12000000) {
		return -1;
	}
	/* caculate dis from bval */
	if (bval == 2000000) {
		*bd1 = 2;
		*bd2 = 0;
	} else {
		dis = 10 * clockRate / 16 / bval;
		x2 = dis % 10;
		dis /= 10;
		if (x2 >= 5)
			dis++;
		*bd1 = dis;
		*bd2 = (unsigned char)(dis >> 8);
	}

	return 0;
}

void cal_outdata(char *buffer, u8 rol, u8 xor)
{
	u8 en_status, i;

	for (i = 0; i < rol; i++) {
		en_status = buffer[0];
		buffer[0] = buffer[0] << 1;
		buffer[0] = buffer[0] | ((buffer[1] & 0x80) ? 1 : 0);
		buffer[1] = buffer[1] << 1;
		buffer[1] = buffer[1] | ((buffer[2] & 0x80) ? 1 : 0);
		buffer[2] = buffer[2] << 1;
		buffer[2] = buffer[2] | ((buffer[3] & 0x80) ? 1 : 0);
		buffer[3] = buffer[3] << 1;
		buffer[3] = buffer[3] | ((buffer[4] & 0x80) ? 1 : 0);
		buffer[4] = buffer[4] << 1;
		buffer[4] = buffer[4] | ((buffer[5] & 0x80) ? 1 : 0);
		buffer[5] = buffer[5] << 1;
		buffer[5] = buffer[5] | ((buffer[6] & 0x80) ? 1 : 0);
		buffer[6] = buffer[6] << 1;
		buffer[6] = buffer[6] | ((buffer[7] & 0x80) ? 1 : 0);
		buffer[7] = buffer[7] << 1;
		buffer[7] = buffer[7] | ((en_status & 0x80) ? 1 : 0);
	}
	for (i = 0; i < 8; i++) {
		buffer[i] = buffer[i] ^ xor;
	}
}

u8 cal_recv_tmt(u32 bd)
{
	int dly = 1000000 * 15 / bd;

	if (bd >= 921600)
		return 5;

	return (dly / 100 + 1);
}

static int ch9344_get_portnum(int index)
{
	return index % NUMSTEP;
}


int ch9344_port_activate(ch9344_t *ch9344, int portnum)
{
	int retval = 0;

	retval = ch9344_configure(ch9344, portnum);
	if (retval)
		return retval;

	return ESP_OK;

}


static int ch9344_set_uartmode(ch9344_t *ch9344, int portnum, u8 index, u8 mode)
{
	char buffer[4];
	int ret = 0;
	u8 request;
	u8 rgadd = 0;
	u8 mode4 = 0;
	int i;

	if (ch9344->chiptype == CHIP_CH9344)
		rgadd = 0x10 * portnum + 0x08;
	else if (ch9344->chiptype == CHIP_CH348L || ch9344->chiptype == CHIP_CH348Q) {
		if (portnum < 4)
			rgadd = 0x10 * portnum;
		else
			rgadd = 0x10 * (portnum - 4) + 0x08;
	}

	if (mode == M_HF) {
		buffer[0] = CMD_W_BR;
		buffer[1] = rgadd + R_C4;
		buffer[2] = 0x51;
		ret = ch9344_cmd_out(ch9344, buffer, 0x03);
		if (ret < 0)
			goto out;
	}

	if (mode == M_NOR) {
		buffer[0] = CMD_W_BR;
		buffer[1] = rgadd + R_C4;
		buffer[2] = 0x50;
		ret = ch9344_cmd_out(ch9344, buffer, 0x03);
		if (ret < 0)
			goto out;
	}

	if (ch9344->chiptype == CHIP_CH348L || ch9344->chiptype == CHIP_CH348Q)
		goto out;

	for (i = 0; i < MAXPORT; i++) {
		if (i != index)
			mode4 |= (ch9344->uartmode[i]) << (i * 2);
	}
	mode4 |= mode << (index * 2);
	request = CMD_WB_E + portnum + ch9344->port_offset;
	rgadd = R_MOD;
	memset(buffer, 0x00, sizeof(buffer));
	buffer[0] = request;
	buffer[1] = rgadd;
	buffer[2] = mode4;
	ret = ch9344_cmd_out(ch9344, buffer, 0x04);

out:
	return ret < 0 ? ret : 0;
}

int ch9344_configure_uart(ch9344_t *ch9344, u8 index, ch9344_uart_param_t *uart_param)
{
	struct usb_ch9344_line_coding newline;
	int portnum = ch9344_get_portnum(index);
	int ret;
	unsigned char bd1 = 0;
	unsigned char bd2 = 0;
	unsigned char bd3 = 0;
	unsigned char dbit = 0, pbit = 0, sbit = 0;
	unsigned char pedt = 0x00;
	int clrt = 1843200;
	u8 rgadd = 0;
	char buffer[12];
	u8 xor, rol;
	u8 rbytes[2];

	newline.dwDTERate = uart_param->baudrate;
	if (newline.dwDTERate == 0)
		newline.dwDTERate = 9600;
	if (newline.dwDTERate > 115200) {
		pedt = 0x01;
		clrt = 44236800;
	}
	ch9344_get(clrt, newline.dwDTERate, &bd1, &bd2);
	//printf("newline.dwDTERate = %d, clrt = %d, bd1 = %d, bd2 = %d\n", newline.dwDTERate, clrt, bd1, bd2);
	switch (newline.dwDTERate) {
	case 250000:
		bd3 = 1;
		break;
	case 500000:
		bd3 = 2;
		break;
	case 1000000:
		bd3 = 3;
		break;
	case 1500000:
		bd3 = 4;
		break;
	case 3000000:
		bd3 = 5;
		break;
	case 12000000:
		bd3 = 6;
		break;
	default:
		bd3 = 0;
		break;
	}
	//printf("bd1 = %d, bd2 = %d, bd3 = %d\n", bd1, bd2, bd3);

	newline.bCharFormat = uart_param->stopbits;
	if (newline.bCharFormat == 2)
		sbit = 0x04;

	newline.bParityType = uart_param->parity ? 1 : 0;

	switch (newline.bParityType) {
	case 0x01:
		pbit = 0x08;
		break;
	case 0x02:
		pbit = (0x01 << 4) + 0x08;
		break;
	case 0x03:
		pbit = (0x02 << 4) + 0x08;
		break;
	case 0x04:
		pbit = (0x03 << 4) + 0x08;
		break;
	default:
		pbit = 0x00;
		break;
	}

	switch (uart_param->databits) {
	case CS5:
		newline.bDataBits = 5;
		dbit = 0x00;
		break;
	case CS6:
		newline.bDataBits = 6;
		dbit = 0x01;
		break;
	case CS7:
		newline.bDataBits = 7;
		dbit = 0x02;
		break;
	case CS8:
	default:
		newline.bDataBits = 8;
		dbit = 0x03;
		break;
	}

	if (ch9344->chiptype == CHIP_CH9344) {
		rgadd = 0x10 * ch9344_get_portnum(index) + 0x08;

		memset(buffer, 0x00, sizeof(buffer));
		buffer[0] = CMD_W_BR;
		buffer[1] = rgadd + 0x01;
		buffer[2] = pedt + 0x50;
		ret = ch9344_cmd_out(ch9344, buffer, 0x03);
		if (ret < 0)
			goto out;

		memset(buffer, 0x00, sizeof(buffer));
		buffer[0] = CMD_S_T;
		buffer[1] = rgadd + 0x03;
		buffer[2] = bd1;
		buffer[3] = bd2;
		buffer[4] = bd3;
		ret = ch9344_cmd_out(ch9344, buffer, 0x06);
		if (ret < 0)
			goto out;

		memset(buffer, 0x00, sizeof(buffer));
		buffer[0] = CMD_W_R;
		buffer[1] = rgadd + 0x03;
		buffer[2] = dbit | pbit | sbit;
		ret = ch9344_cmd_out(ch9344, buffer, 0x03);
		if (ret < 0)
			goto out;

		/* uart receive timeout */
		memset(buffer, 0x00, sizeof(buffer));
		buffer[0] = CMD_WB_E + portnum + ch9344->port_offset;
		buffer[1] = R_TM_O;
		buffer[2] = portnum + ch9344->port_offset;
		buffer[3] = cal_recv_tmt(newline.dwDTERate);
		ret = ch9344_cmd_out(ch9344, buffer, 0x04);
		if (ret < 0)
			goto out;
	} else {
		memset(rbytes, 0, 2);
		rol = (u8)rbytes[0] & 0x0f;
		xor = (u8)rbytes[1];
		buffer[0] = CMD_WB_E | (portnum & 0x0F);
		buffer[1] = R_INIT;
		buffer[2] = (u8)((portnum & 0x0F) | ((rol << 4) & 0xf0));
		buffer[3] = (char)(newline.dwDTERate >> 24);
		buffer[4] = (char)(newline.dwDTERate >> 16);
		buffer[5] = (char)(newline.dwDTERate >> 8);
		buffer[6] = (char)(newline.dwDTERate);
		if (newline.bCharFormat == 2)
			buffer[7] = 0x02;
		else if (newline.bCharFormat == 1)
			buffer[7] = 0x00;
		buffer[8] = newline.bParityType;
		buffer[9] = newline.bDataBits;
		buffer[10] = cal_recv_tmt(newline.dwDTERate);
		buffer[11] = xor;
		cal_outdata(buffer + 3, rol & 0x0f, xor);

		ret = ch9344_cmd_out(ch9344, buffer, 0x0c);
		if (ret < 0)
			goto out;
	}

	if (ch9344->chiptype == CHIP_CH9344)
		rgadd = 0x10 * portnum + 0x08;
	else {
		if (portnum < 4)
			rgadd = 0x10 * portnum;
		else
			rgadd = 0x10 * (portnum - 4) + 0x08;
	}

	buffer[0] = CMD_W_R;
	buffer[1] = rgadd + R_C1;
	if (true)
		buffer[2] = 0x0F;
	else
		buffer[2] = 0x07;
	ret = ch9344_cmd_out(ch9344, buffer, 0x03);
	if (ret < 0)
		goto out;

	if ((ch9344->chiptype == CHIP_CH348Q) && (portnum > 3)) {
	} else {
		ch9344_set_control(ch9344, portnum, 0x00);
	}

	ret = ch9344_set_uartmode(ch9344, portnum, portnum, M_NOR);

out:
	return ret < 0 ? ret : 0;
}


static void action_open_dev(ch9344_t *ch9344)
{
    assert(ch9344->dev_addr != 0);
    ESP_LOGI(TAG, "Opening device at address %d", ch9344->dev_addr);
    ESP_ERROR_CHECK(usb_host_device_open(ch9344->client_hdl, ch9344->dev_addr, &ch9344->dev_hdl));
    //Get the device's information next
    ch9344->actions &= ~ACTION_OPEN_DEV;
    ch9344->actions |= ACTION_GET_DEV_INFO;
}

static void action_get_info(ch9344_t *ch9344)
{
    assert(ch9344->dev_hdl != NULL);
    ESP_LOGI(TAG, "Getting device information");
    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usb_host_device_info(ch9344->dev_hdl, &dev_info));
    ESP_LOGI(TAG, "\t%s speed", (char *[]) {
        "Low", "Full", "High"
    }[dev_info.speed]);
    ESP_LOGI(TAG, "\tbConfigurationValue %d", dev_info.bConfigurationValue);

    //Get the device descriptor next
    ch9344->actions &= ~ACTION_GET_DEV_INFO;
    ch9344->actions |= ACTION_GET_DEV_DESC;
}


static void action_get_dev_desc(ch9344_t *ch9344)
{
    assert(ch9344->dev_hdl != NULL);
    ESP_LOGI(TAG, "Getting device descriptor");
    const usb_device_desc_t *dev_desc;
    ESP_ERROR_CHECK(usb_host_get_device_descriptor(ch9344->dev_hdl, &dev_desc));
    usb_print_device_descriptor(dev_desc);
    if(dev_desc->idProduct == 0x55d9) {
    	ch9344->chiptype = CHIP_CH348Q;
		ch9344->port_offset = 0;
		ch9344->port_num = 8;
    } else if (dev_desc->idProduct == 0xe018) {
		ch9344->port_offset = 4;
		ch9344->port_num = 4;
    	ch9344->chiptype = CHIP_CH9344;
    } else {
        ch9344->chiptype = CHIP_UNKNOWN;
    }
    //Get the device's config descriptor next
    ch9344->actions &= ~ACTION_GET_DEV_DESC;
    ch9344->actions |= ACTION_GET_CONFIG_DESC;
}

static void action_get_config_desc(ch9344_t *ch9344)
{
    assert(ch9344->dev_hdl != NULL);
    ESP_LOGI(TAG, "Getting config descriptor");
    const usb_config_desc_t *cfg_desc;
    ESP_ERROR_CHECK(usb_host_get_active_config_descriptor(ch9344->dev_hdl, &cfg_desc));
    usb_print_config_descriptor(cfg_desc, NULL);
	
    if (cfg_desc == NULL) {
        ESP_LOGI(TAG, "No config descriptor found");
        return;
    }

    int offset = 0;
    int ep_index = 0;
    uint16_t wTotalLength = cfg_desc->wTotalLength;
    const usb_standard_desc_t *next_desc = (const usb_standard_desc_t *)cfg_desc;

    do {
        switch (next_desc->bDescriptorType) {
        case USB_B_DESCRIPTOR_TYPE_ENDPOINT:
            usb_ep_desc_t *ep = (usb_ep_desc_t *)next_desc;
			if (ep_index == 0) {
                ch9344->rx_endpoint = ep->bEndpointAddress;
                ch9344->data_rx_mps = ep->wMaxPacketSize;	
            } else if (ep_index == 1) {
                ch9344->tx_endpoint = ep->bEndpointAddress;
                ch9344->data_tx_mps = ep->wMaxPacketSize;	
            } else if(ep_index == 2) {
                ch9344->cmdrx_endpoint = ep->bEndpointAddress;
                ch9344->cmd_rx_mps = ep->wMaxPacketSize;
            } else if (ep_index == 3) {
                ch9344->cmdtx_endpoint = ep->bEndpointAddress;
                ch9344->cmd_tx_mps = ep->wMaxPacketSize;
            }
            ep_index++;
            break;
        default:
            break;
        }
        next_desc = usb_parse_next_descriptor(next_desc, wTotalLength, &offset);
    } while (next_desc != NULL);
    printf("\t\tEndpointAddress: %x,%x,%x,%x\n", ch9344->cmdrx_endpoint, ch9344->cmdtx_endpoint, ch9344->rx_endpoint, ch9344->tx_endpoint);
    printf("\t\twMaxPacketSize: %d,%d,%d,%d\n", ch9344->cmd_rx_mps, ch9344->cmd_tx_mps, ch9344->data_rx_mps, ch9344->data_tx_mps);
    //Get the device's string descriptors next
    ch9344->actions &= ~ACTION_GET_CONFIG_DESC;
    ch9344->actions |= ACTION_GET_STR_DESC;
}

static void action_get_str_desc(ch9344_t *ch9344)
{
    assert(ch9344->dev_hdl != NULL);
    usb_device_info_t dev_info;
    ESP_ERROR_CHECK(usb_host_device_info(ch9344->dev_hdl, &dev_info));
    if (dev_info.str_desc_manufacturer) {
        ESP_LOGI(TAG, "Getting Manufacturer string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_manufacturer);
    }
    if (dev_info.str_desc_product) {
        ESP_LOGI(TAG, "Getting Product string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_product);
    }
    if (dev_info.str_desc_serial_num) {
        ESP_LOGI(TAG, "Getting Serial Number string descriptor");
        usb_print_string_descriptor(dev_info.str_desc_serial_num);
    }
	printf("chiptype = %d\n", ch9344->chiptype);
    //Nothing to do until the device disconnects
    if(ch9344->chiptype != CHIP_UNKNOWN) {
		ch9344->actions &= ~ACTION_GET_STR_DESC;
        ch9344->actions = 0;
    } else {
        ch9344->actions &= ~ACTION_GET_STR_DESC;
        ch9344->actions |= ACTION_CLOSE_DEV;
    }
}

static void action_close_dev(ch9344_t *ch9344)
{
    ESP_ERROR_CHECK(usb_host_device_close(ch9344->client_hdl, ch9344->dev_hdl));
    ch9344->dev_hdl = NULL;
    ch9344->dev_addr = 0;
    //We need to connect a new device
    ch9344->actions &= ~ACTION_CLOSE_DEV;
    ch9344->actions |= ACTION_RECONNECT;
}
	/*
static void test_data_tx(ch9344_t *device) {

	static u8 i = 0;
	unsigned char buf[32];
	buf[0] = i % 8;
	buf[1] = 1;
	buf[2] = 0;
	buf[3] = '0' + (i++ % 8);
	usb_bulk_transfer(device, buf, 4, DATA_EP_OUT);
}
*/

int ch9344_uart_get_available_data_len(int port) 
{
	ch9344_t *ch9344 = s_ch9344;

	if(port >= MAXPORT) {
		ESP_LOGE("ch9344", "ch9344_uart_recv port=%d error", port);
		return ESP_ERR_INVALID_ARG;
	}
	return getRingbufferValidLenNoLock(ch9344->rb[port].buf);
}

esp_err_t ch9344_uart_clear_recv_data(int port) 
{
	ch9344_t *ch9344 = s_ch9344;
	esp_err_t err = ESP_OK;

	if(port >= MAXPORT) {
		ESP_LOGE("ch9344", "ch9344_uart_recv port=%d error", port);
		return ESP_ERR_INVALID_ARG;
	}
	initRingbuffer(ch9344->rb[port].buf);
	return err;
}

int ch9344_uart_recv(int port, char *buf, int data_len)
{
	ch9344_t *ch9344 = s_ch9344;

	if(port >= MAXPORT) {
		ESP_LOGE("ch9344", "ch9344_uart_recv port=%d error", port);
		return 0;
	}
	//xSemaphoreTake(ch9344->rx_mux, portMAX_DELAY);
	unsigned long len = 0;
	if(data_len == 0) {
		len = getRingbufferValidLen(ch9344->rb[port].buf);
	} else {
		len = data_len;
	}
	//ESP_LOGI(TAG, "ch9344_uart_recv port=%d validLen=%lu, len=%lu", port, getRingbufferValidLen(ch9344->rb[port].buf), len);
	len = readRingbuffer((unsigned char *)buf, len, ch9344->rb[port].buf);

	//xSemaphoreGive(ch9344->rx_mux);
	return len;
}

int ch9344_uart_copy(int port, char *buf, int data_len)
{
	ch9344_t *ch9344 = s_ch9344;

	if(port >= MAXPORT) {
		ESP_LOGE("ch9344", "ch9344_uart_recv port=%d error", port);
		return 0;
	}
	//xSemaphoreTake(ch9344->rx_mux, portMAX_DELAY);
	unsigned long len = 0;
	if(data_len == 0) {
		len = getRingbufferValidLenNoLock(ch9344->rb[port].buf);
	} else {
		len = data_len;
	}
	//ESP_LOGI(TAG, "ch9344_uart_recv port=%d validLen=%lu, len=%lu", port, getRingbufferValidLen(ch9344->rb[port].buf), len);
	len = copyRingbuffer((unsigned char *)buf, len, ch9344->rb[port].buf);

	//xSemaphoreGive(ch9344->rx_mux);
	return len;
}

esp_err_t ch9344_uart_send(int port, char *buf, int data_len)
{
	ch9344_t *ch9344 = s_ch9344;
	esp_err_t err = ESP_OK;
	unsigned char *send_buf = malloc(ch9344->data_tx_mps);
	int max_blk_len = ch9344->data_tx_mps - 3;
	int i = 0;
	int len = data_len;
	int snd_blk_n = (data_len + max_blk_len - 1) / max_blk_len;

	//ESP_LOGI("ch9344", "ch9344_uart_send len = %d snd_blk_n = %d", len, snd_blk_n);
	xSemaphoreTake(ch9344->tx_mux, portMAX_DELAY);
	for(i = 0; i < snd_blk_n; i++)
	{
		unsigned short size;
		if(len > max_blk_len) {
			size = max_blk_len;
			memcpy(send_buf + 3, buf + (i * max_blk_len), size);
		} else {
			size = len;
			memcpy(send_buf + 3, buf + (i * max_blk_len), size);
		}
		send_buf[0] = port % 8;
		send_buf[1] = size & 0xff;
		send_buf[2] = (size >> 8) & 0xff;
		err = usb_bulk_transfer(ch9344, send_buf, size+3, DATA_EP_OUT);
		if(err != ESP_OK) {
			ESP_LOGE("ch9344", "usb_bulk_transfer error");
			break;
		}
		len -= size;
		//ESP_LOGI("ch9344", "left len=%d\n", len);
	}
	xSemaphoreGive(ch9344->tx_mux);
	free(send_buf);
	return err;
}
ch9344_uart_param_t s_ch9344_uart_configs[8] = {
	{.baudrate = 115200, .databits=8, .stopbits=1, .parity=0, .flowctrl=0},
	{.baudrate = 115200, .databits=8, .stopbits=1, .parity=0, .flowctrl=0},
	{.baudrate = 115200, .databits=8, .stopbits=1, .parity=0, .flowctrl=0},
	{.baudrate = 115200, .databits=8, .stopbits=1, .parity=0, .flowctrl=0},
	{.baudrate = 115200, .databits=8, .stopbits=1, .parity=0, .flowctrl=0},
	{.baudrate = 115200, .databits=8, .stopbits=1, .parity=0, .flowctrl=0},
	{.baudrate = 115200, .databits=8, .stopbits=1, .parity=0, .flowctrl=0},
	{.baudrate = 115200, .databits=8, .stopbits=1, .parity=0, .flowctrl=0},
};


static void init_ch9344(ch9344_t *ch9344)
{
	ESP_LOGI("ch9344", "init_ch9344");
    ESP_ERROR_CHECK(usb_host_transfer_alloc(ch9344->cmd_rx_mps, 0, &ch9344->cmd_rxfer));
	ch9344->cmd_rxfer->callback = cmd_in_xfer_cb;
	ch9344->cmd_rxfer->num_bytes = ch9344->cmd_rx_mps;
	ch9344->cmd_rxfer->bEndpointAddress = ch9344->cmdrx_endpoint;
	ch9344->cmd_rxfer->device_handle = ch9344->dev_hdl;
	ch9344->cmd_rxfer->context = ch9344;

    ESP_ERROR_CHECK(usb_host_transfer_alloc(ch9344->data_rx_mps, 0, &ch9344->rx_xfer));
	ch9344->rx_xfer->callback = in_xfer_cb;
	ch9344->rx_xfer->num_bytes = ch9344->data_rx_mps;
	ch9344->rx_xfer->bEndpointAddress = ch9344->rx_endpoint;
	ch9344->rx_xfer->device_handle = ch9344->dev_hdl;
	ch9344->rx_xfer->context = ch9344;

    ESP_ERROR_CHECK(usb_host_transfer_alloc(ch9344->cmd_tx_mps, 0, &ch9344->cmd_txfer));
    ESP_ERROR_CHECK(usb_host_transfer_alloc(ch9344->data_tx_mps, 0, &ch9344->tx_xfer));	

    ch9344->transfer_done = xSemaphoreCreateBinary();
	//xSemaphoreGive(ch9344->transfer_done);
    ch9344->rx_mux = xSemaphoreCreateMutex();
    ch9344->tx_mux = xSemaphoreCreateMutex();
	ch9344->rx_available = xSemaphoreCreateBinary();
    for(int i = 0; i < MAXPORT; i++) {
		ch9344->rb[i].buf = malloc(sizeof(struct s_ringbuffer));
		memset(ch9344->rb[i].buf, 0, sizeof(struct s_ringbuffer));
        initRingbuffer(ch9344->rb[i].buf);
		ch9344->uartmode[i] = M_NOR;
    }
	ESP_ERROR_CHECK_WITHOUT_ABORT(usb_host_interface_claim(ch9344->client_hdl, ch9344->dev_hdl, 0, 0));
	rx_poll_submit(ch9344);
	ESP_LOGI(TAG, "activate and configure ch9344 uart port.");

	ch9344->initialized = true;

	ESP_LOGI("ch9344", "init_ch9344 end");

}

esp_err_t ch9344_enable_uart(int port)
{
	ch9344_t *ch9344 = s_ch9344;
	ESP_ERROR_CHECK_WITHOUT_ABORT(ch9344_port_activate(ch9344, port));
	vTaskDelay(pdMS_TO_TICKS(10));
	ESP_LOGI(TAG, "activate ch9344 uart %d.", port);

	ESP_ERROR_CHECK_WITHOUT_ABORT(ch9344_configure_uart(ch9344, port, &s_ch9344_uart_configs[port]));
	vTaskDelay(pdMS_TO_TICKS(10));
	ESP_LOGI(TAG, "configure ch9344 uart %d.", port);
	return ESP_OK;
}

bool ch9344_is_initialized(void)
{
	return s_ch9344->initialized;
}

void ch9344_rx_wait(int wait_ms)
{
	ch9344_t *ch9344 = s_ch9344;
	xSemaphoreTake(ch9344->rx_available, wait_ms / portTICK_PERIOD_MS);
}
void ch9344_driver_task(void *arg)
{
    ch9344_t _ch9344 = {0};
	ch9344_t *ch9344 = &_ch9344;

    ESP_LOGI(TAG, "Registering Client");
    usb_host_client_config_t client_config = {
        .is_synchronous = false,    //Synchronous clients currently not supported. Set this to false
        .max_num_event_msg = CLIENT_NUM_EVENT_MSG,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = (void *) ch9344,
        },
    };
    ESP_ERROR_CHECK(usb_host_client_register(&client_config, &ch9344->client_hdl));
    s_ch9344 = ch9344;
    //分配一个 USB 传输
    //usb_transfer_t *transfer;
    //ESP_ERROR_CHECK(usb_host_transfer_alloc(64, 0, &transfer));
	ch9344->initialized = false;
    while (1) {
        if (ch9344->actions == 0) {
            usb_host_client_handle_events(ch9344->client_hdl, portMAX_DELAY);
			//test_data_tx(&ch9344);
        } else {
            if (ch9344->actions & ACTION_OPEN_DEV) {
                action_open_dev(ch9344);
            }
            if (ch9344->actions & ACTION_GET_DEV_INFO) {
                action_get_info(ch9344);
            }
            if (ch9344->actions & ACTION_GET_DEV_DESC) {
                action_get_dev_desc(ch9344);
            }
            if (ch9344->actions & ACTION_GET_CONFIG_DESC) {
                action_get_config_desc(ch9344);
            }
            if (ch9344->actions & ACTION_GET_STR_DESC) {
                action_get_str_desc(ch9344);
				if(ch9344->chiptype == CHIP_CH9344 || ch9344->chiptype == CHIP_CH348L || ch9344->chiptype == CHIP_CH348Q) {
					init_ch9344(ch9344);
				}
            }
            if (ch9344->actions & ACTION_CLOSE_DEV) {
                action_close_dev(ch9344);
            }
            if (ch9344->actions & ACTION_EXIT) {
                break;
            }
            if (ch9344->actions & ACTION_RECONNECT) {
                ch9344->actions = 0;
            }
        }
    }

    usb_host_transfer_free(ch9344->cmd_rxfer);
	usb_host_transfer_free(ch9344->cmd_txfer);
    usb_host_transfer_free(ch9344->rx_xfer);
    usb_host_transfer_free(ch9344->tx_xfer);
    ESP_LOGI(TAG, "Deregistering Client");
    ESP_ERROR_CHECK(usb_host_client_deregister(ch9344->client_hdl));
    vTaskSuspend(NULL);
}

void class_driver_client_deregister(void)
{
    if (s_ch9344->dev_hdl != NULL) {
        s_ch9344->actions = ACTION_CLOSE_DEV;
    }
    s_ch9344->actions |= ACTION_EXIT;

    // Unblock, exit the loop and proceed to deregister client
    ESP_ERROR_CHECK(usb_host_client_unblock(s_ch9344->client_hdl));
}
