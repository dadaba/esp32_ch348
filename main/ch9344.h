#ifndef CH9344_H
#define CH9344_H


#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "usb/usb_host.h"
#include "ringbuffer.h"


/*
 * Baud rate and default timeout
 */
#define DEFAULT_BAUD_RATE 9600
#define DEFAULT_TIMEOUT 2000
#define DEFAULT_CLOSETIMEOUT 100

/*
 * CMSPAR, some architectures can't have space and mark parity.
 */
#ifndef CMSPAR
#define CMSPAR 0
#endif

#define USB_MINOR_BASE 192

#define CH9344_CTO_D 0x01
#define CH9344_CTO_R 0x02
#define CH9344_CTI_C 0x01
#define CH9344_CTI_DS 0x02
#define CH9344_CTI_R 0x04
#define CH9344_CTI_DC 0x08

#define CH9344_LO 0x10
#define CH9344_LP 0x20
#define CH9344_LF 0x40
#define CH9344_LB 0x80

#define CMD_W_R 0xC0
#define CMD_W_BR 0x80
#define CMD_S_T 0x20

#define CMD_WB_E 0x90
#define CMD_RB_E 0xC0

#define M_NOR 0x00
#define M_RS 0x01
#define M_IO 0x02
#define M_HF 0x03

#define G_DO 0x01
#define G_DI 0x00

#define R_MOD 0x97
#define R_IO_D 0x98
#define R_IO_O 0x99
#define R_IO_I 0x9B
#define R_TM_O 0x9C
#define R_INIT 0xA1

#define R_IO_CE 0xA3
#define R_IO_CD 0xA4
#define R_IO_CO 0xA5
#define R_IO_CI 0xA7
#define R_IO_RE 0xAA
#define R_IO_RD 0xAB

#define R_C1 0x01
#define R_C2 0x02
#define R_C3 0x03
#define R_C4 0x04
#define R_C5 0x06

#define R_II_B1 0x06
#define R_II_B2 0x02
#define R_II_B3 0x00

#define CMD_VER 0x96

#define CH9344_NW 4
#define CH9344_NR 16

#define NUMSTEP 8
#define MAXGPIO 48
#define MAXPORT 8
#define RINGBUF_SIZE 512
#define MAX_RINGBUF_SIZE (512)

#define IO_H 1
#define IO_L 0


typedef unsigned char u8;
typedef unsigned short u16;
typedef unsigned int u32;



#define GOTO_ON_ERROR(exp) ESP_GOTO_ON_ERROR(exp, fail, TAG, "")
#define GOTO_ON_FALSE(exp, err) ESP_GOTO_ON_FALSE( (exp), err, fail, TAG, "" )
#define RETURN_ON_ERROR(exp) ESP_RETURN_ON_ERROR((exp), TAG, "")
#define RETURN_ON_FALSE(exp, err) ESP_RETURN_ON_FALSE( (exp), (err), TAG, "")
#define RETURN_ON_INVALID_ARG(exp) ESP_RETURN_ON_FALSE((exp) != NULL, ESP_ERR_INVALID_ARG, TAG, "")

#define ESP_ERR_CH9344_HOST_BASE        0x1800                      /*!< MSC host error code base */
#define ESP_ERR_CH9344_INTERNAL        (ESP_ERR_CH9344_HOST_BASE + 3)  /*!< MSC host internal error */
#define ESP_ERR_CH9344_STALL           (ESP_ERR_CH9344_HOST_BASE + 4)  /*!< USB transfer stalled */

#define CLIENT_NUM_EVENT_MSG        5

/*
 * Internal driver structures.
 */
struct ch9344_ringbuf {
    struct s_ringbuffer *buf;
};


struct usb_ch9344_line_coding {
	u32 dwDTERate;
	u8 bCharFormat;
#define SB1 0
#define SB1_5 1
#define SB2 2

	u8 bParityType;
#define PAN 0
#define PAO 1
#define PAE 2
#define PAM 3
#define PAS 4

	u8 bDataBits;
} __attribute__((packed));


#define   CS5      (0u << 0)        /** 5 bits. */
#define   CS6      (1u << 0)        /** 6 bits. */
#define   CS7      (2u << 0)        /** 7 bits. */
#define   CS8      (3u << 0)        /** 8 bits. */

typedef enum {
    ACTION_OPEN_DEV = 0x01,
    ACTION_GET_DEV_INFO = 0x02,
    ACTION_GET_DEV_DESC = 0x04,
    ACTION_GET_CONFIG_DESC = 0x08,
    ACTION_GET_STR_DESC = 0x10,
    ACTION_CLOSE_DEV = 0x20,
    ACTION_EXIT = 0x40,
    ACTION_RECONNECT = 0x80,
    ACTION_TRANSFER = 0x100,
} action_t;

typedef enum {
    CMD_EP_OUT,
    CMD_EP_IN,
    DATA_EP_OUT,
    DATA_EP_IN
} ch9344_endpoint_t;

typedef enum {
	CHIP_CH9344 = 0,
	CHIP_CH348L = 1,
	CHIP_CH348Q = 2,
    CHIP_UNKNOWN = -1
} CHIPTYPE;

typedef struct ch9344 {
    CHIPTYPE chiptype;
    int port_offset;
    int port_num;
    u8 uartmode[MAXPORT]; /* uart mode */
    usb_host_client_handle_t client_hdl;
    uint8_t dev_addr;
    usb_device_handle_t dev_hdl;
    uint32_t actions;
    SemaphoreHandle_t transfer_done;
    usb_transfer_t *cmd_txfer;
    usb_transfer_t *cmd_rxfer;
    usb_transfer_t *tx_xfer;
    usb_transfer_t *rx_xfer;
	int rx_endpoint;
	int tx_endpoint;
	int cmdrx_endpoint;
	int cmdtx_endpoint;
    int data_rx_mps;
    int data_tx_mps;
    int cmd_rx_mps;
    int cmd_tx_mps;
    //struct ch9344_ringbuf wb[MAXPORT];
    struct ch9344_ringbuf rb[MAXPORT];
    SemaphoreHandle_t rx_mux;
    SemaphoreHandle_t tx_mux;
    SemaphoreHandle_t rx_available;
    bool initialized;
} ch9344_t;

typedef struct {
	int baudrate;
	int databits;
	int stopbits;
	int parity;
	int flowctrl;
} ch9344_uart_param_t;

int ch9344_configure_uart(ch9344_t *ch9344, u8 index, ch9344_uart_param_t *uart_param);
int ch9344_port_activate(ch9344_t *ch9344, int portnum);
esp_err_t ch9344_uart_send(int port, char *buf, int data_len);
int ch9344_uart_get_available_data_len(int port);
esp_err_t ch9344_uart_clear_recv_data(int port);
int ch9344_uart_recv(int port, char *buf, int data_len);
int ch9344_uart_copy(int port, char *buf, int data_len);
bool ch9344_is_initialized(void);
esp_err_t ch9344_enable_uart(int port);
void ch9344_rx_wait(int wait_ms);


#endif
