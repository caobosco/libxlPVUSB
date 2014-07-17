/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU Lesser General Public License as published
 * by the Free Software Foundation; version 2.1 only. with the special
 * exception on linking described in file LICENSE.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 */

#include "libxl_osdeps.h" /* must come before any other headers */

#include "libxl_internal.h"
//define the usb sys path
#define SYSFS_USB_DEVS_PATH "/sys/bus/usb/devices"
#define SYSFS_USB_DEV_BDEVICECLASS_PATH "/bDeviceClass"
#define SYSFS_USB_DEV_BDEVICESUBCLASS_PATH "/bDeviceSubClass"
#define SYSFS_USB_DEV_DEVNUM_PATH "/devnum"
#define SYSFS_USB_DEV_IDVENDOR_PATH "/idVendor"
#define SYSFS_USB_DEV_IDPRODUCT_PATH "/idProduct"
#define SYSFS_USB_DEV_MANUFACTURER_PATH "/manufacturer"
#define SYSFS_USB_DEV_PRODUCT_PATH "/product"
#define SYSFS_USB_DEV_SERIAL_PATH "/serial"
#define SYSFS_USB_DEV_DRIVER_PATH "/driver"
#define SYSFS_USB_DRIVER_BIND_PATH "/bind"
#define SYSFS_USB_DRIVER_UNBIND_PATH "/unbind"
#define SYSFS_USBBACK_PATH "/bus/usb/drivers/usbback"
#define SYSFS_PORTIDS_PATH "/port_ids"
#define USBHUB_CLASS_CODE 9

//use system call open and write to manipulate usb device
static int get_usb_bDeviceClass (libxl_device_usb *dev)
{
    
}

static int get_usb_idvendor(libxl_device_usb *dev)
{
    
}

static int get_usb_idProduct(libxl_device_usb *dev)
{

}

static int get_usb_manufacturer(libxl_device_usb *dev, char *manu)
{

}

static int get_usb_product(libxl_device_usb *dev, char *product)
{

}

static int get_all_assigned_devices(libxl__gc *gc, libxl_device_usb **list, int *num)
{

}

int libxl_device_usb_controller_create(libxl__gc *gc, uint32_t domid, libxl_device_usb_controller *dev)
{
     
}

int libxl_device_usb_controller_destroy(libxl__gc *gc, uint32_t domid, libxl_device_usb_controller *dev)
{

}
int libxl_device_usb_add(libxl__gc *gc, uint32_t domid, libxl_device_usb *dev)
{

}

int libxl_device_usb_remove(libxl__gc *gc, uint32_t domid, libxl_device_usb *dev)
{

}
int libxl_device_usb_destroy(libxl__gc *gc, uint32_t domid, libxl_device_usb *dev)
{

}
libxl_device_usb *libxl_device_usb_list(libxl__gc *gc, uint32_t domid, int *num)
{

}

/*Get assignable usb devices*/
libxl_device_usb *libxl_device_usb_assigned_list(libxl__gc *gc, int *num)
{
    
}

