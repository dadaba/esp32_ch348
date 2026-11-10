# ESP32 CH348/CH9344 USB driver

| Supported Targets | ESP32-P4 | ESP32-S2 | ESP32-S3 | ESP32-S31 | 
| ----------------- | -------- | -------- | -------- | --------  |

# CH348/CH9344 ESP32 USB driver porting according to ch348 linux driver

(See the README.md file in the upper level 'examples' directory for more information about examples.)

This driver does not support ch348 connected by usb hub, only support ch348 connected directly to usb root hub.
All ch348 uart data recv will be stored in ringbuffer. ch9344_uart_recv is actually reading data from ringbuffer.
The main task is example to recv all 8 uart ports of ch348, and echo back received data.


## Example Output

```
I (289) main_task: Started on CPU0
I (289) main_task: Calling app_main()
I (299) USB host lib: USB host library initial
I (299) USB host lib: Installing USB Host Library
I (339) main: waiting for ch9344 to be initialized...

I (339) CH9344: Registering Client
I (719) CH9344: Opening device at address 1
I (719) CH9344: Getting device information
I (719) CH9344:         Full speed
I (719) CH9344:         bConfigurationValue 1
I (729) CH9344: Getting device descriptor
*** Device descriptor ***
bLength 18
bDescriptorType 1
bcdUSB 2.00
bDeviceClass 0xff
bDeviceSubClass 0x80
bDeviceProtocol 0x37
bMaxPacketSize0 64
idVendor 0x1a86
idProduct 0x55d9
bcdDevice 1.30
iManufacturer 1
iProduct 2
iSerialNumber 0
bNumConfigurations 1
I (759) CH9344: Getting config descriptor
*** Configuration descriptor ***
bLength 9
bDescriptorType 2
wTotalLength 46
bNumInterfaces 1
bConfigurationValue 1
iConfiguration 0
bmAttributes 0x80
bMaxPower 200mA
        *** Interface descriptor ***
        bLength 9
        bDescriptorType 4
        bInterfaceNumber 0
        bAlternateSetting 0
        bNumEndpoints 4
        bInterfaceClass 0xff
        bInterfaceSubClass 0x80
        bInterfaceProtocol 0x37
        iInterface 0
                *** Endpoint descriptor ***
                bLength 7
                bDescriptorType 5
                bEndpointAddress 0x82   EP 2 IN
                bmAttributes 0x2        BULK
                wMaxPacketSize 64
                bInterval 0
                *** Endpoint descriptor ***
                bLength 7
                bDescriptorType 5
                bEndpointAddress 0x2    EP 2 OUT
                bmAttributes 0x2        BULK
                wMaxPacketSize 64
                bInterval 0
                *** Endpoint descriptor ***
                bLength 7
                bDescriptorType 5
                bEndpointAddress 0x81   EP 1 IN
                bmAttributes 0x2        BULK
                wMaxPacketSize 64
                bInterval 0
                *** Endpoint descriptor ***
                bLength 7
                bDescriptorType 5
                bEndpointAddress 0x1    EP 1 OUT
                bmAttributes 0x2        BULK
                wMaxPacketSize 64
                bInterval 0
                EndpointAddress: 81,1,82,2
                wMaxPacketSize: 64,64,64,64
I (859) CH9344: Getting Manufacturer string descriptor
wch.cn
I (859) CH9344: Getting Product string descriptor
USB2.0 To Multi Serial Ports
chiptype = 2
I (869) ch9344: init_ch9344
I (869) CH9344: activate and configure ch9344 uart port.
I (879) ch9344: init_ch9344 end
I (939) main: ch9344 initialized done.

I (949) CH9344: activate ch9344 uart 0.
I (959) CH9344: configure ch9344 uart 0.
I (969) CH9344: activate ch9344 uart 1.
I (979) CH9344: configure ch9344 uart 1.
I (989) CH9344: activate ch9344 uart 2.
I (999) CH9344: configure ch9344 uart 2.
I (1009) CH9344: activate ch9344 uart 3.
I (1019) CH9344: configure ch9344 uart 3.
I (1029) CH9344: activate ch9344 uart 4.
I (1039) CH9344: configure ch9344 uart 4.
I (1049) CH9344: activate ch9344 uart 5.
I (1059) CH9344: configure ch9344 uart 5.
I (1069) CH9344: activate ch9344 uart 6.
I (1079) CH9344: configure ch9344 uart 6.
I (1089) CH9344: activate ch9344 uart 7.
I (1099) CH9344: configure ch9344 uart 7.
I (1099) main: ch9344 port enable done.

```

## Troubleshooting

In practice, suggest read/write ch348 uart data in unique thread to avoid race condition.
