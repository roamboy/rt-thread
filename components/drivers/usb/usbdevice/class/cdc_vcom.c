/*
 * File      : cdc_vcom.c
 * This file is part of RT-Thread RTOS
 * COPYRIGHT (C) 2012, RT-Thread Development Team
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with this program; if not, write to the Free Software Foundation, Inc.,
 *  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 *
 * Change Logs:
 * Date           Author       Notes
 * 2012-10-02     Yi Qiu       first version
 * 2012-12-12     heyuanjie87  change endpoints and function handler 
 * 2013-06-25     heyuanjie87  remove SOF mechinism
 * 2013-07-20     Yi Qiu       do more test
 * 2016-02-01     Urey         Fix some error
 */

#include <rthw.h>
#include <rtthread.h>
#include <rtservice.h>
#include <rtdevice.h>
#include <drivers/serial.h>
#include "drivers/usb_device.h"
#include "cdc.h"

#ifdef RT_USB_DEVICE_CDC

#define TX_TIMEOUT              1000
#define CDC_RX_BUFSIZE          128
#define CDC_MAX_PACKET_SIZE     64
#define VCOM_DEVICE             "vcom"

#ifdef RT_VCOM_TASK_STK_SIZE
#define VCOM_TASK_STK_SIZE      RT_VCOM_TASK_STK_SIZE
#else /*!RT_VCOM_TASK_STK_SIZE*/
#define VCOM_TASK_STK_SIZE      512
#endif /*RT_VCOM_TASK_STK_SIZE*/

#ifdef RT_VCOM_TX_USE_DMA
#define VCOM_TX_USE_DMA
#endif /*RT_VCOM_TX_USE_DMA*/

#ifdef RT_VCOM_SERNO
#define _SER_NO RT_VCOM_SERNO
#else /*!RT_VCOM_SERNO*/
#define _SER_NO  "32021919830108"
#endif /*RT_VCOM_SERNO*/

#ifdef RT_VCOM_SER_LEN
#define _SER_NO_LEN RT_VCOM_SER_LEN
#else /*!RT_VCOM_SER_LEN*/
#define _SER_NO_LEN 14 /*rt_strlen("32021919830108")*/
#endif /*RT_VCOM_SER_LEN*/

ALIGN(RT_ALIGN_SIZE)
static rt_uint8_t vcom_thread_stack[VCOM_TASK_STK_SIZE];
static struct rt_thread vcom_thread;
static struct ucdc_line_coding line_coding;

#define CDC_TX_BUFSIZE    1024

#define CDC_TX_HAS_DATE   0x01

struct vcom
{
    struct rt_serial_device     serial;
    uep_t ep_out;
    uep_t ep_in;
    uep_t ep_cmd;
    rt_bool_t connected;
    rt_bool_t in_sending;
    struct rt_completion wait;
    rt_uint8_t rx_rbp[CDC_RX_BUFSIZE];    
    struct rt_ringbuffer rx_ringbuffer;
    rt_uint8_t tx_rbp[CDC_TX_BUFSIZE];
    struct rt_ringbuffer tx_ringbuffer;
    struct rt_event  tx_event;
};

struct vcom_tx_msg
{
    struct rt_serial_device * serial;
    const char *buf;
    rt_size_t size;
};

static struct udevice_descriptor dev_desc =
{
    USB_DESC_LENGTH_DEVICE,     //bLength;
    USB_DESC_TYPE_DEVICE,       //type;
    USB_BCD_VERSION,            //bcdUSB;
    USB_CLASS_CDC,              //bDeviceClass;
    0x00,                       //bDeviceSubClass;
    0x00,                       //bDeviceProtocol;
    CDC_MAX_PACKET_SIZE,          //bMaxPacketSize0;
    _VENDOR_ID,                 //idVendor;
    _PRODUCT_ID,                //idProduct;
    USB_BCD_DEVICE,             //bcdDevice;
    USB_STRING_MANU_INDEX,      //iManufacturer;
    USB_STRING_PRODUCT_INDEX,   //iProduct;
    USB_STRING_SERIAL_INDEX,    //iSerialNumber;
    USB_DYNAMIC,                //bNumConfigurations;
};

static struct usb_qualifier_descriptor dev_qualifier =
{
    sizeof(dev_qualifier),
    USB_DESC_TYPE_DEVICEQUALIFIER,
    0x0200,
    USB_CLASS_CDC,
    0x00,
    64,
    0x01,
    0,
};

/* communcation interface descriptor */
const static struct ucdc_comm_descriptor _comm_desc =
{
#ifdef RT_USB_DEVICE_COMPOSITE
    /* Interface Association Descriptor */
    USB_DESC_LENGTH_IAD,
    USB_DESC_TYPE_IAD,
    USB_DYNAMIC,
    0x02,
    USB_CDC_CLASS_COMM,
    USB_CDC_SUBCLASS_ACM,
    USB_CDC_PROTOCOL_V25TER,
    0x00,
#endif
    /* Interface Descriptor */
    USB_DESC_LENGTH_INTERFACE,
    USB_DESC_TYPE_INTERFACE,
    USB_DYNAMIC,
    0x00,   
    0x01,
    USB_CDC_CLASS_COMM,
    USB_CDC_SUBCLASS_ACM,
    USB_CDC_PROTOCOL_V25TER,
    0x00,
    /* Header Functional Descriptor */   
    0x05,                              
    USB_CDC_CS_INTERFACE,
    USB_CDC_SCS_HEADER,
    0x0110,
    /* Call Management Functional Descriptor */   
    0x05,            
    USB_CDC_CS_INTERFACE,
    USB_CDC_SCS_CALL_MGMT,
    0x00,
    USB_DYNAMIC,
    /* Abstract Control Management Functional Descriptor */
    0x04,
    USB_CDC_CS_INTERFACE,
    USB_CDC_SCS_ACM,
    0x02,
    /* Union Functional Descriptor */   
    0x05,
    USB_CDC_CS_INTERFACE,
    USB_CDC_SCS_UNION,
    USB_DYNAMIC,
    USB_DYNAMIC,
    /* Endpoint Descriptor */    
    USB_DESC_LENGTH_ENDPOINT,
    USB_DESC_TYPE_ENDPOINT,
    USB_DYNAMIC | USB_DIR_IN,
    USB_EP_ATTR_INT,
    0x08,
    0xFF,
};

/* data interface descriptor */
const static struct ucdc_data_descriptor _data_desc =
{
    /* interface descriptor */
    USB_DESC_LENGTH_INTERFACE,
    USB_DESC_TYPE_INTERFACE,
    USB_DYNAMIC,
    0x00,
    0x02,         
    USB_CDC_CLASS_DATA,
    0x00,                             
    0x00,                             
    0x00,              
    /* endpoint, bulk out */
    USB_DESC_LENGTH_ENDPOINT,     
    USB_DESC_TYPE_ENDPOINT,
    USB_DYNAMIC | USB_DIR_OUT,
    USB_EP_ATTR_BULK,      
    USB_CDC_BUFSIZE,
    0x00,          
    /* endpoint, bulk in */
    USB_DESC_LENGTH_ENDPOINT,
    USB_DESC_TYPE_ENDPOINT,
    USB_DYNAMIC | USB_DIR_IN,
    USB_EP_ATTR_BULK,      
    USB_CDC_BUFSIZE,
    0x00,
};

static char serno[_SER_NO_LEN + 1] = {'\0'};
RT_WEAK rt_err_t vcom_get_stored_serno(char *serno, int size);

rt_err_t vcom_get_stored_serno(char *serno, int size)
{
    return RT_ERROR;
}

const static char* _ustring[] =
{
    "Language",
    "RT-Thread Team.",
    "RTT Virtual Serial",
    serno,
    "Configuration",
    "Interface",
};
static void rt_usb_vcom_init(struct ufunction *func);

static void _vcom_reset_state(ufunction_t func)
{
    struct vcom* data;
    int lvl;
    
    RT_ASSERT(func != RT_NULL)

    data = (struct vcom*)func->user_data;
    
    lvl = rt_hw_interrupt_disable();
    data->connected = RT_FALSE;
    data->in_sending = RT_FALSE;
    /*rt_kprintf("reset USB serial\n", cnt);*/
    rt_hw_interrupt_enable(lvl);
}

/**
 * This function will handle cdc bulk in endpoint request.
 *
 * @param func the usb function object.
 * @param size request size.
 *
 * @return RT_EOK.
 */
static rt_err_t _ep_in_handler(ufunction_t func, rt_size_t size)
{
    struct vcom *data;

    RT_ASSERT(func != RT_NULL);

    RT_DEBUG_LOG(RT_DEBUG_USB, ("_ep_in_handler %d\n", size));
    data = (struct vcom*)func->user_data;
    if ((size != 0) && (size % CDC_MAX_PACKET_SIZE == 0))
    {
        /* don't have data right now. Send a zero-length-packet to
         * terminate the transaction.
         *
         * FIXME: actually, this might not be the right place to send zlp.
         * Only the rt_device_write could know how much data is sending. */
        data->in_sending = RT_TRUE;

        data->ep_in->request.buffer = RT_NULL;
        data->ep_in->request.size = 0;
        data->ep_in->request.req_type = UIO_REQUEST_WRITE;
        rt_usbd_io_request(func->device, data->ep_in, &data->ep_in->request);

        return RT_EOK;
    }
    
    rt_completion_done(&data->wait);
    
    return RT_EOK;
}

/**
 * This function will handle cdc bulk out endpoint request.
 *
 * @param func the usb function object.
 * @param size request size.
 *
 * @return RT_EOK.
 */
static rt_err_t _ep_out_handler(ufunction_t func, rt_size_t size)
{
    rt_uint32_t level;
    struct vcom *data;

    RT_ASSERT(func != RT_NULL);

    RT_DEBUG_LOG(RT_DEBUG_USB, ("_ep_out_handler %d\n", size));
    
    data = (struct vcom*)func->user_data;
    /* receive data from USB VCOM */
    level = rt_hw_interrupt_disable();

    rt_ringbuffer_put(&data->rx_ringbuffer, data->ep_out->buffer, size);
    rt_hw_interrupt_enable(level);

    /* notify receive data */
    rt_hw_serial_isr(&data->serial,RT_SERIAL_EVENT_RX_IND);

    data->ep_out->request.buffer = data->ep_out->buffer;
    data->ep_out->request.size = EP_MAXPACKET(data->ep_out);
    data->ep_out->request.req_type = UIO_REQUEST_READ_BEST;
    rt_usbd_io_request(func->device, data->ep_out, &data->ep_out->request);

    return RT_EOK;
}

/**
 * This function will handle cdc interrupt in endpoint request.
 *
 * @param device the usb device object.
 * @param size request size.
 *
 * @return RT_EOK.
 */
static rt_err_t _ep_cmd_handler(ufunction_t func, rt_size_t size)
{
    RT_ASSERT(func != RT_NULL);

    RT_DEBUG_LOG(RT_DEBUG_USB, ("_ep_cmd_handler\n"));

    return RT_EOK;
}

/**
 * This function will handle cdc_get_line_coding request.
 *
 * @param device the usb device object.
 * @param setup the setup request.
 *
 * @return RT_EOK on successful.
 */
static rt_err_t _cdc_get_line_coding(udevice_t device, ureq_t setup)
{
    struct ucdc_line_coding data;
    rt_uint16_t size;

    RT_ASSERT(device != RT_NULL);
    RT_ASSERT(setup != RT_NULL);

    RT_DEBUG_LOG(RT_DEBUG_USB, ("_cdc_get_line_coding\n"));

    data.dwDTERate = 115200;
    data.bCharFormat = 0;
    data.bDataBits = 8;
    data.bParityType = 0;
    size = setup->wLength > 7 ? 7 : setup->wLength;

    rt_usbd_ep0_write(device, (void*)&data, size);

    return RT_EOK;
}

static rt_err_t _cdc_set_line_coding_callback(udevice_t device, rt_size_t size)
{
    RT_DEBUG_LOG(RT_DEBUG_USB, ("_cdc_set_line_coding_callback\n"));

    dcd_ep0_send_status(device->dcd);
    
    return RT_EOK;
}

/**
 * This function will handle cdc_set_line_coding request.
 *
 * @param device the usb device object.
 * @param setup the setup request.
 *
 * @return RT_EOK on successful.
 */
static rt_err_t _cdc_set_line_coding(udevice_t device, ureq_t setup)
{
    RT_ASSERT(device != RT_NULL);
    RT_ASSERT(setup != RT_NULL);

    RT_DEBUG_LOG(RT_DEBUG_USB, ("_cdc_set_line_coding\n"));

    rt_usbd_ep0_read(device, (void*)&line_coding, sizeof(struct ucdc_line_coding),
        _cdc_set_line_coding_callback);

    return RT_EOK;
}

/**
 * This function will handle cdc interface request.
 *
 * @param device the usb device object.
 * @param setup the setup request.
 *
 * @return RT_EOK on successful.
 */
static rt_err_t _interface_handler(ufunction_t func, ureq_t setup)
{
    struct vcom *data;

    RT_ASSERT(func != RT_NULL);
    RT_ASSERT(func->device != RT_NULL);
    RT_ASSERT(setup != RT_NULL);

    data = (struct vcom*)func->user_data;
    
    switch(setup->bRequest)
    {
    case CDC_SEND_ENCAPSULATED_COMMAND:
        break;
    case CDC_GET_ENCAPSULATED_RESPONSE:
        break;
    case CDC_SET_COMM_FEATURE:
        break;
    case CDC_GET_COMM_FEATURE:
        break;
    case CDC_CLEAR_COMM_FEATURE:
        break;
    case CDC_SET_LINE_CODING:
        _cdc_set_line_coding(func->device, setup);
        break;
    case CDC_GET_LINE_CODING:
        _cdc_get_line_coding(func->device, setup);
        break;
    case CDC_SET_CONTROL_LINE_STATE:
        data->connected = (setup->wValue & 0x01) > 0?RT_TRUE:RT_FALSE;
        RT_DEBUG_LOG(RT_DEBUG_USB, ("vcom state:%d \n", data->connected));
        dcd_ep0_send_status(func->device->dcd);
        break;
    case CDC_SEND_BREAK:
        break;
    default:
        rt_kprintf("unknown cdc request\n",setup->request_type);
        return -RT_ERROR;
    }

    return RT_EOK;
}

/**
 * This function will run cdc function, it will be called on handle set configuration request.
 *
 * @param func the usb function object.
 *
 * @return RT_EOK on successful.
 */
static rt_err_t _function_enable(ufunction_t func)
{
    struct vcom *data;

    RT_ASSERT(func != RT_NULL);

    RT_DEBUG_LOG(RT_DEBUG_USB, ("cdc function enable\n"));

    _vcom_reset_state(func);
    
    data = (struct vcom*)func->user_data;
    data->ep_out->buffer = rt_malloc(CDC_RX_BUFSIZE);

    data->ep_out->request.buffer = data->ep_out->buffer;
    data->ep_out->request.size = EP_MAXPACKET(data->ep_out);
    
    data->ep_out->request.req_type = UIO_REQUEST_READ_BEST;
    rt_usbd_io_request(func->device, data->ep_out, &data->ep_out->request);
    
    return RT_EOK;
}

/**
 * This function will stop cdc function, it will be called on handle set configuration request.
 *
 * @param func the usb function object.
 *
 * @return RT_EOK on successful.
 */
static rt_err_t _function_disable(ufunction_t func)
{
    struct vcom *data;

    RT_ASSERT(func != RT_NULL);

    RT_DEBUG_LOG(RT_DEBUG_USB, ("cdc function disable\n"));

    _vcom_reset_state(func);

    data = (struct vcom*)func->user_data;
    if(data->ep_out->buffer != RT_NULL)
    {
        rt_free(data->ep_out->buffer);
        data->ep_out->buffer = RT_NULL;        
    }

    return RT_EOK;
}

static struct ufunction_ops ops =
{
    _function_enable,
    _function_disable,
    RT_NULL,
};

/**
 * This function will configure cdc descriptor.
 *
 * @param comm the communication interface number.
 * @param data the data interface number.
 *
 * @return RT_EOK on successful.
 */
static rt_err_t _cdc_descriptor_config(ucdc_comm_desc_t comm, 
    rt_uint8_t cintf_nr, ucdc_data_desc_t data, rt_uint8_t dintf_nr)
{
    comm->call_mgmt_desc.data_interface = dintf_nr;
    comm->union_desc.master_interface = cintf_nr;
    comm->union_desc.slave_interface0 = dintf_nr;
#ifdef RT_USB_DEVICE_COMPOSITE
    comm->iad_desc.bFirstInterface = cintf_nr;
#endif

    return RT_EOK;
}

/**
 * This function will create a cdc function instance.
 *
 * @param device the usb device object.
 *
 * @return RT_EOK on successful.
 */
ufunction_t rt_usbd_function_cdc_create(udevice_t device)
{
    ufunction_t func;
    struct vcom* data;
    uintf_t intf_comm, intf_data;
    ualtsetting_t comm_setting, data_setting;
    ucdc_data_desc_t data_desc;
    ucdc_comm_desc_t comm_desc;

    /* parameter check */
    RT_ASSERT(device != RT_NULL);

    rt_memset(serno, 0, _SER_NO_LEN + 1);
    if(vcom_get_stored_serno(serno, _SER_NO_LEN) != RT_EOK)
    {
        rt_memset(serno, 0, _SER_NO_LEN + 1);
        rt_memcpy(serno, _SER_NO, rt_strlen(_SER_NO));
    }
    /* set usb device string description */
    rt_usbd_device_set_string(device, _ustring);
    
    /* create a cdc function */
    func = rt_usbd_function_new(device, &dev_desc, &ops);
    rt_usbd_device_set_qualifier(device, &dev_qualifier);
    
    /* allocate memory for cdc vcom data */
    data = (struct vcom*)rt_malloc(sizeof(struct vcom));
    rt_memset(data, 0, sizeof(struct vcom));
    func->user_data = (void*)data;
    
    /* initilize vcom */
    rt_usb_vcom_init(func);

    /* create a cdc communication interface and a cdc data interface */
    intf_comm = rt_usbd_interface_new(device, _interface_handler);
    intf_data = rt_usbd_interface_new(device, _interface_handler);

    /* create a communication alternate setting and a data alternate setting */
    comm_setting = rt_usbd_altsetting_new(sizeof(struct ucdc_comm_descriptor));
    data_setting = rt_usbd_altsetting_new(sizeof(struct ucdc_data_descriptor));

    /* config desc in alternate setting */
    rt_usbd_altsetting_config_descriptor(comm_setting, &_comm_desc,
                                         (rt_off_t)&((ucdc_comm_desc_t)0)->intf_desc);
    rt_usbd_altsetting_config_descriptor(data_setting, &_data_desc, 0);
    /* configure the cdc interface descriptor */
    _cdc_descriptor_config(comm_setting->desc, intf_comm->intf_num, data_setting->desc, intf_data->intf_num);

    /* create a command endpoint */
    comm_desc = (ucdc_comm_desc_t)comm_setting->desc;
    data->ep_cmd = rt_usbd_endpoint_new(&comm_desc->ep_desc, _ep_cmd_handler);

    /* add the command endpoint to the cdc communication interface */
    rt_usbd_altsetting_add_endpoint(comm_setting, data->ep_cmd);

    /* add the communication alternate setting to the communication interface,
       then set default setting of the interface */
    rt_usbd_interface_add_altsetting(intf_comm, comm_setting);
    rt_usbd_set_altsetting(intf_comm, 0);

    /* add the communication interface to the cdc function */
    rt_usbd_function_add_interface(func, intf_comm);

    /* create a bulk in and a bulk endpoint */
    data_desc = (ucdc_data_desc_t)data_setting->desc;
    data->ep_out = rt_usbd_endpoint_new(&data_desc->ep_out_desc, _ep_out_handler);
    data->ep_in = rt_usbd_endpoint_new(&data_desc->ep_in_desc, _ep_in_handler);

    /* add the bulk out and bulk in endpoints to the data alternate setting */
    rt_usbd_altsetting_add_endpoint(data_setting, data->ep_in);
    rt_usbd_altsetting_add_endpoint(data_setting, data->ep_out);

    /* add the data alternate setting to the data interface
            then set default setting of the interface */
    rt_usbd_interface_add_altsetting(intf_data, data_setting);
    rt_usbd_set_altsetting(intf_data, 0);

    /* add the cdc data interface to cdc function */
    rt_usbd_function_add_interface(func, intf_data);
    
    return func;
}

/**
* UART device in RT-Thread
*/
static rt_err_t _vcom_configure(struct rt_serial_device *serial,
                                struct serial_configure *cfg)
{
    return RT_EOK;
}

static rt_err_t _vcom_control(struct rt_serial_device *serial,
                              int cmd, void *arg)
{
    switch (cmd)
    {
    case RT_DEVICE_CTRL_CLR_INT:
        /* disable rx irq */
        break;
    case RT_DEVICE_CTRL_SET_INT:
        /* enable rx irq */
        break;
    }

    return RT_EOK;
}

static int _vcom_getc(struct rt_serial_device *serial)
{
    int result;
    rt_uint8_t ch;
    rt_uint32_t level;
    struct ufunction *func;
    struct vcom *data;
    
    func = (struct ufunction*)serial->parent.user_data;
    data = (struct vcom*)func->user_data;

    result = -1;

    level = rt_hw_interrupt_disable();

    if(rt_ringbuffer_getchar(&data->rx_ringbuffer, &ch) != 0)
    {
        result = ch;
    }

    rt_hw_interrupt_enable(level);

    return result;
}
static rt_size_t _vcom_tx(struct rt_serial_device *serial, rt_uint8_t *buf, rt_size_t size,int direction)
{
    rt_uint32_t level;

    struct ufunction *func;
    struct vcom *data;
    rt_uint32_t baksize = size;
    rt_size_t ptr = 0;
    int empty = 0;
    rt_uint8_t crlf[2] = {'\r', '\n',};

    func = (struct ufunction*)serial->parent.user_data;
    data = (struct vcom*)func->user_data;

    RT_ASSERT(serial != RT_NULL);
    RT_ASSERT(buf != RT_NULL);

    RT_DEBUG_LOG(RT_DEBUG_USB, ("%s\n",__func__));

    if (data->connected)
    {
        size = 0;
        if((serial->parent.open_flag & RT_DEVICE_FLAG_STREAM))
        {
            empty = 0;
            while(ptr < baksize)
            {
                while(ptr < baksize && buf[ptr] != '\n')
                {
                    ptr++;
                }
                if(ptr < baksize)
                {
                    level = rt_hw_interrupt_disable();
                    size += rt_ringbuffer_put_force(&data->tx_ringbuffer, (const rt_uint8_t *)&buf[size], ptr - size);
                    rt_hw_interrupt_enable(level);
                    if(size == ptr)
                    {
                        level = rt_hw_interrupt_disable();
                        if(rt_ringbuffer_space_len(&data->tx_ringbuffer) >= 2)
                        {
                            rt_ringbuffer_put_force(&data->tx_ringbuffer, crlf, 2);
                            size++;
                        }
                        rt_hw_interrupt_enable(level);
                    }
                    else
                    {
                        empty = 1;
                        break;
                    }
                    if(size == ptr)
                    {
                        empty = 1;
                        break;
                    }
                    ptr++;
                }
                else
                {
                    break;
                }
            }
        }
        if(size < baksize && !empty)
        {
            level = rt_hw_interrupt_disable();
            size += rt_ringbuffer_put_force(&data->tx_ringbuffer, (rt_uint8_t *)&buf[size], baksize - size);
            rt_hw_interrupt_enable(level);
        }


        if(size)
        {
            rt_event_send(&data->tx_event, CDC_TX_HAS_DATE);
        }
    }
    return size;
}
static int _vcom_putc(struct rt_serial_device *serial, char c)
{
    rt_uint32_t level;
    struct ufunction *func;
    struct vcom *data;

    func = (struct ufunction*)serial->parent.user_data;
    data = (struct vcom*)func->user_data;

    RT_ASSERT(serial != RT_NULL);

    if (data->connected)
    {
        if(c == '\n' && (serial->parent.open_flag & RT_DEVICE_FLAG_STREAM))
        {
            level = rt_hw_interrupt_disable();
            rt_ringbuffer_putchar_force(&data->tx_ringbuffer, '\r');
            rt_hw_interrupt_enable(level);
            rt_event_send(&data->tx_event, CDC_TX_HAS_DATE);
        }
        level = rt_hw_interrupt_disable();
        rt_ringbuffer_putchar_force(&data->tx_ringbuffer, c);
        rt_hw_interrupt_enable(level);
        rt_event_send(&data->tx_event, CDC_TX_HAS_DATE);
    }

    return 1;
}

static const struct rt_uart_ops usb_vcom_ops =
{
    _vcom_configure,
    _vcom_control,
    _vcom_putc,
    _vcom_getc,
    _vcom_tx
};

/* Vcom Tx Thread */
static void vcom_tx_thread_entry(void* parameter)
{
    rt_uint32_t level;
    rt_uint32_t res;
    struct ufunction *func = (struct ufunction *)parameter;
    struct vcom *data = (struct vcom*)func->user_data;
    rt_uint8_t ch[64];

    while (1)
    {
        if
        (
            (rt_event_recv(&data->tx_event, CDC_TX_HAS_DATE,
                    RT_EVENT_FLAG_OR | RT_EVENT_FLAG_CLEAR,
                    RT_WAITING_FOREVER, &res) != RT_EOK) ||
                                 (!(res & CDC_TX_HAS_DATE))
        )
        {
            continue;
        }
        if(!res & CDC_TX_HAS_DATE)
        {
            continue;
        }
        while(rt_ringbuffer_data_len(&data->tx_ringbuffer))
        {
            level = rt_hw_interrupt_disable();
            res = rt_ringbuffer_get(&data->tx_ringbuffer, ch, 64);
            rt_hw_interrupt_enable(level);

            if(!res)
            {
                continue;
            }
            if (!data->connected)
            {
                if(data->serial.parent.open_flag &
#ifndef VCOM_TX_USE_DMA
                         RT_DEVICE_FLAG_INT_TX
#else
                         RT_DEVICE_FLAG_DMA_TX
#endif
                )
                {
                /* drop msg */
#ifndef VCOM_TX_USE_DMA
                    rt_hw_serial_isr(&data->serial,RT_SERIAL_EVENT_TX_DONE);
#else
                    rt_hw_serial_isr(&data->serial,RT_SERIAL_EVENT_TX_DMADONE);
#endif
                }
                continue;
            }
            rt_completion_init(&data->wait);
            data->ep_in->request.buffer     = ch;
            data->ep_in->request.size       = res;

            data->ep_in->request.req_type   = UIO_REQUEST_WRITE;

            rt_usbd_io_request(func->device, data->ep_in, &data->ep_in->request);

            if (rt_completion_wait(&data->wait, TX_TIMEOUT) != RT_EOK)
            {
                RT_DEBUG_LOG(RT_DEBUG_USB, ("vcom tx timeout\n"));
            }
            if(data->serial.parent.open_flag &
#ifndef VCOM_TX_USE_DMA
                         RT_DEVICE_FLAG_INT_TX
#else
                         RT_DEVICE_FLAG_DMA_TX
#endif
            )
            {
#ifndef VCOM_TX_USE_DMA
                rt_hw_serial_isr(&data->serial,RT_SERIAL_EVENT_TX_DONE);
#else
                rt_hw_serial_isr(&data->serial,RT_SERIAL_EVENT_TX_DMADONE);
#endif
            }
        }

    }
}

static void rt_usb_vcom_init(struct ufunction *func)
{
    rt_err_t result = RT_EOK;
    struct serial_configure config;
    struct vcom *data = (struct vcom*)func->user_data;
    
    /* initialize ring buffer */
    rt_ringbuffer_init(&data->rx_ringbuffer, data->rx_rbp, CDC_RX_BUFSIZE);
    rt_ringbuffer_init(&data->tx_ringbuffer, data->tx_rbp, CDC_TX_BUFSIZE);

    rt_event_init(&data->tx_event, "vom", RT_IPC_FLAG_FIFO);

    config.baud_rate    = BAUD_RATE_115200;
    config.data_bits    = DATA_BITS_8;
    config.stop_bits    = STOP_BITS_1;
    config.parity       = PARITY_NONE;
    config.bit_order    = BIT_ORDER_LSB;
    config.invert       = NRZ_NORMAL;
    config.bufsz        = CDC_RX_BUFSIZE;

    data->serial.ops        = &usb_vcom_ops;
    data->serial.serial_rx  = RT_NULL;
    data->serial.config     = config;

    /* register vcom device */
    rt_hw_serial_register(&data->serial, VCOM_DEVICE,
#ifndef VCOM_TX_USE_DMA
                          RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_INT_TX,
#else
                          RT_DEVICE_FLAG_RDWR | RT_DEVICE_FLAG_INT_RX | RT_DEVICE_FLAG_DMA_TX,
#endif
                          func);

    /* init usb device thread */
    rt_thread_init(&vcom_thread, "vcom",
                   vcom_tx_thread_entry, func,
                   vcom_thread_stack, VCOM_TASK_STK_SIZE,
                   16, 20);
    result = rt_thread_startup(&vcom_thread);
    RT_ASSERT(result == RT_EOK);       
}

#endif

