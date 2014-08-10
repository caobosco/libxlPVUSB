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
#define SYSFS_USBBACK_DRIVER "/sys/bus/usb/drivers/usbback"
#define USBHUB_CLASS_CODE 9

int libxl__device_usbctrl_setdefault(libxl__gc *gc, 
                 libxl_device_usbctrl *usbctrl, uint32_t domid)
{
    int rc;
    
    if (!usbctrl->usb_version)
        usbctrl->usb_version = 2;
    if (!usbctrl->num_ports)
        usbctrl->num_ports = 8;

    if(!usbctrl->backend_domid)
        usbctrl->backend_domid = 0;
    
    rc = libxl__resolve_domid(gc, usbctrl->backend_domname, &usbctrl->backend_domid);
    if (rc < 0) return rc;

    switch (libxl__domain_type(gc, domid)) {
    case LIBXL_DOMAIN_TYPE_HVM:
        if (!usbctrl->type)
            usbctrl->type = LIBXL_USBCTRL_TYPE_DEVICEMODEL;
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        if (usbctrl->type == LIBXL_USBCTRL_TYPE_DEVICEMODEL) {
            LOG(ERROR, "trying to create PV guest with an emulated interface");
            return ERROR_INVAL;
        }
        usbctrl->type = LIBXL_USBCTRL_TYPE_PV;
        break;
    case LIBXL_DOMAIN_TYPE_INVALID:
        return ERROR_FAIL;
    default:
        abort();
    }
    return rc;
}

static int libxl__device_from_usbctrl(libxl__gc *gc, uint32_t domid,
                                   libxl_device_usbctrl *usbctrl,
                                   libxl__device *device)
{
    device->backend_devid   = usbctrl->devid;
    device->backend_domid   = usbctrl->backend_domid;
    device->backend_kind    = LIBXL__DEVICE_KIND_VUSB;
    device->devid           = usbctrl->devid;
    device->domid           = domid;
    device->kind            = LIBXL__DEVICE_KIND_VUSB;

   return 0;    
}

static int do_pvusbctrl_add(libxl__gc *gc, uint32_t domid,
                        libxl_device_usbctrl *usbctrl) 
{
    flexarray_t *front;
    flexarray_t *back;
    libxl__device *device;
    unsigned int rc = 0;

    rc = libxl__device_usbctrl_setdefault(gc, usbctrl, domid);
    if(rc) goto out;
    
    front = flexarray_make(gc, 4, 1);
    back = flexarray_make(gc, 12, 1);

    if (usbctrl->devid == -1) {
        if ((usbctrl->devid = libxl__device_nextid(gc, domid, "vusb")) < 0) {
            rc = ERROR_FAIL;
            goto out;
        }
    }
    
    GCNEW(device);
    rc = libxl__device_from_usbctrl(gc, domid, usbctrl, device);
    if ( rc != 0 ) goto out;

    flexarray_append(back, "frontend-id");
    flexarray_append(back, libxl__sprintf(gc, "%d", domid));
    flexarray_append(back, "online");
    flexarray_append(back, "1");
    flexarray_append(back, "state");
    flexarray_append(back, libxl__sprintf(gc, "%d", 1));
    flexarray_append(back, "usb-ver");
    flexarray_append(back, libxl__sprintf(gc, "%d", usbctrl->usb_version));
    flexarray_append(back, "num-ports");
    flexarray_append(back, libxl__sprintf(gc, "%d", usbctrl->num_ports));
    flexarray_append(back, "type");
    switch(usbctrl->type) {
    case LIBXL_USBCTRL_TYPE_PV:{
        flexarray_append(back, "PVUSB");
        break;
    }
    case LIBXL_USBCTRL_TYPE_DEVICEMODEL: {
        flexarray_append(back, "IOEMU");
        break;
    }
    default:
        abort();
    }
    flexarray_append(front, "backend-id");
    flexarray_append(front, libxl__sprintf(gc, "%d", usbctrl->backend_domid));
    flexarray_append(front, "state");
    flexarray_append(front, libxl__sprintf(gc, "%d", 1));
    libxl__device_generic_add(gc, XBT_NULL, device,
                              libxl__xs_kvs_of_flexarray(gc, back, back->count),
                              libxl__xs_kvs_of_flexarray(gc, front, front->count),
                              NULL);
    /*If we add usbctrl from usb-add, enable the funciton below will report errors.
     *This is because the state of usbctrl change too fast for the device_addrm_complete to catch,
     *from 2 to 4. Since we won't need hot-plug script in this usbctrl_add, it's okay to take if off.
    aodev->dev = device;
    aodev->action = LIBXL__DEVICE_ACTION_ADD;
    //libxl__wait_device_connection(egc, aodev);
    //ao->complete = 1;

    rc = 0;
out:
    aodev->rc = rc;
    if (rc) aodev->callback(egc, aodev);
    return 0;
    */
out:
    return rc;
}

static int libxl_port_add_xenstore(libxl__gc *gc, uint32_t domid,
                                    libxl_device_usbctrl *usbctrl) {
    libxl_ctx *ctx = CTX;
    char *path;

    path = libxl__sprintf(gc, "%s/backend/vusb/%d/%d/port", 
                        libxl__xs_get_dompath(gc, 0), domid, usbctrl->devid);
    if (libxl__xs_mkdir(gc, XBT_NULL, path, NULL, 0) ) {
        return 1;
    }
    
    int i, num_ports = usbctrl->num_ports;
    for ( i = 1; i <= num_ports; ++i) {
        if (libxl__xs_write(gc, XBT_NULL, 
                      libxl__sprintf(gc,"%s/%d", path, i),"%s", "") ) {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_WARNING,
                        "Create port %d to %s failed.", i, path);
            return 1;
        }
    }
    return 0;
}
                              
int libxl__device_usbctrl_add(libxl__gc *gc, uint32_t domid,
                           libxl_device_usbctrl *usbctrl) 
{
    switch (libxl__domain_type(gc, domid)) {
    case LIBXL_DOMAIN_TYPE_HVM:
         /* TO_DO */
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        if (do_pvusbctrl_add(gc, domid, usbctrl) ) {
            return ERROR_FAIL;
        }

        /* Add sub-port to Xenstore */
        if (libxl_port_add_xenstore(gc, domid, usbctrl) ) {
            return ERROR_FAIL;
        }
        break;
    case LIBXL_DOMAIN_TYPE_INVALID:
            return ERROR_FAIL;
    }

    return 0;
}

int libxl_device_usbctrl_add(libxl_ctx *ctx, uint32_t domid, 
                                libxl_device_usbctrl *usbctrl, const libxl_asyncop_how *ao_how)
{
    AO_CREATE(ctx, domid, ao_how);
    int rc; 
    
    rc = libxl__device_usbctrl_add(gc, domid, usbctrl);
    libxl__ao_complete(egc, ao, rc);
    return AO_INPROGRESS; 
}

libxl_device_usbctrl *libxl_device_usbctrl_list(libxl_ctx *ctx, uint32_t domid, int *num)
{
    GC_INIT(ctx);

    libxl_device_usbctrl *usbctrls = NULL;
    char *fe_path = NULL, *result = NULL;
    char **dir = NULL;
    unsigned int ndirs = 0;
    
    *num = 0;
    
    fe_path = libxl__sprintf(gc, "%s/device/vusb", libxl__xs_get_dompath(gc, domid));
    dir = libxl__xs_directory(gc, XBT_NULL, fe_path, &ndirs);

    if (dir && ndirs) {
        usbctrls = malloc(sizeof(*usbctrls) * ndirs);
        libxl_device_usbctrl* usbctrl;
        libxl_device_usbctrl* end = usbctrls + ndirs;
        for(usbctrl = usbctrls; usbctrl < end; ++usbctrl, ++dir, (*num)++) {
            const char *be_path = libxl__xs_read(gc, XBT_NULL,
                                    GCSPRINTF("%s/%s/backend", fe_path, *dir));

            libxl_device_usbctrl_init(usbctrl);

            usbctrl->devid = atoi(*dir);

            result = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/%s/backend-id",
                                                   fe_path, *dir));
            if( result == NULL)
                goto outerr;
            usbctrl->backend_domid = atoi(result);

            result = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/usb-ver", be_path));
            usbctrl->usb_version =  atoi(result);

            result = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/num-ports", be_path));
            usbctrl->num_ports = atoi(result);
       }
    }
    *num = ndirs;
    
    return usbctrls;
outerr:
    LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "Unable to list USB Controllers");
    while (*num) {
        (*num)--;
        libxl_device_usbctrl_dispose(&usbctrls[*num]);
    }
    free(usbctrls);
    return NULL;
}

static int libxl__device_usb_remove_common(libxl__gc *gc, uint32_t domid,
                                libxl_device_usb *usb, int force);

static int libxl__device_usbctrl_remove_common(libxl_ctx *ctx, uint32_t domid,
                            libxl_device_usbctrl *usbctrl,
                            const libxl_asyncop_how *ao_how, int force)
{
    AO_CREATE(ctx, domid, ao_how);
    libxl__device *device;
    libxl__ao_device *aodev;
    int hvm = 0, rc;
    libxl_device_usb *usbs;
    int i, numusb = 0;
    
    GCNEW(device); 
    rc = libxl__device_from_usbctrl(gc, domid, usbctrl, device);
    if(rc) goto out;

    switch (libxl__domain_type(gc, domid)) {
    case LIBXL_DOMAIN_TYPE_HVM:
        hvm = 1;
        /* TO_DO */
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        /* Remove usb devives first */
        rc  = libxl__device_usb_list(gc, domid, &usbs, usbctrl->devid, &numusb);
        if (rc) goto out;
        for (i = 0; i < numusb; i++) {
            if (libxl__device_usb_remove_common(gc, domid, &usbs[i], 0)) {
                fprintf(stderr, "libxl_device_usb_remove failed.\n");
                return 1;
            }
        }
        /* remove usbctrl */
        GCNEW(aodev);
        libxl__prepare_ao_device(ao, aodev);
        aodev->action = LIBXL__DEVICE_ACTION_REMOVE; 
        aodev->dev = device;
        aodev->callback = device_addrm_aocomplete;
        aodev->force = force;
        libxl__initiate_device_remove(egc, aodev);
        break;
    default:
        abort();
    }
    
out:
    if(rc) return AO_ABORT(rc);
    return AO_INPROGRESS;
}

int libxl_device_usbctrl_remove(libxl_ctx *ctx, uint32_t domid,
                            libxl_device_usbctrl *usbctrl,
                            const libxl_asyncop_how *ao_how)
{
    return libxl__device_usbctrl_remove_common(ctx, domid, usbctrl, ao_how, 0);
}

int libxl_device_usbctrl_destroy(libxl_ctx *ctx, uint32_t domid,
                            libxl_device_usbctrl *usbctrl,
                            const libxl_asyncop_how *ao_how)
{
    return libxl__device_usbctrl_remove_common(ctx, domid, usbctrl, ao_how, 1);
}


int libxl_device_usbctrl_getinfo(libxl_ctx *ctx, uint32_t domid,
                                libxl_device_usbctrl *usbctrl,
                                libxl_usbctrlinfo *usbctrlinfo)
{
    GC_INIT(ctx);
    char *dompath, *usbctrlpath;
    char *val;

    dompath = libxl__xs_get_dompath(gc, domid);
    usbctrlinfo->devid = usbctrl->devid;
    usbctrlinfo->num_ports = usbctrl->num_ports;
    usbctrlinfo->version = usbctrl->usb_version;

    usbctrlpath = libxl__sprintf(gc, "%s/device/vusb/%d", dompath, usbctrlinfo->devid);
    usbctrlinfo->backend = xs_read(ctx->xsh, XBT_NULL,
                                libxl__sprintf(gc, "%s/backend", usbctrlpath), NULL);
    if (!usbctrlinfo->backend) {
        GC_FREE;
        return ERROR_FAIL;
    }
    
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/backend-id", usbctrlpath));
    usbctrlinfo->backend_id = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/state", usbctrlpath));
    usbctrlinfo->state = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/event-channel", usbctrlpath));
    usbctrlinfo->evtch = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/urb-ring-ref", usbctrlpath));
    usbctrlinfo->ref_urb = val ? strtoul(val, NULL, 10) : -1;
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/conn-ring-ref", usbctrlpath));
    usbctrlinfo->ref_conn= val ? strtoul(val, NULL, 10) : -1;
    usbctrlinfo->type = xs_read(ctx->xsh, XBT_NULL,
                                libxl__sprintf(gc, "%s/type", usbctrlinfo->backend), NULL);
    usbctrlinfo->frontend = xs_read(ctx->xsh, XBT_NULL,
                                 libxl__sprintf(gc, "%s/frontend", usbctrlinfo->backend), NULL);
    val = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/frontend-id", usbctrlinfo->backend));
    usbctrlinfo->frontend_id = val ? strtoul(val, NULL, 10) : -1;

    GC_FREE;
    return 0;
}

int libxl_devid_to_device_usbctrl(libxl_ctx *ctx, uint32_t domid,
                               int devid, libxl_device_usbctrl *usbctrl)
{
    GC_INIT(ctx);
    char* fe_path = NULL, *be_path = NULL, *tmp; 

    fe_path = libxl__sprintf(gc, "%s/device/vusb", libxl__xs_get_dompath(gc, domid));
    be_path = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/%d/backend", fe_path, devid));
    
    libxl_device_usbctrl_init(usbctrl);
    usbctrl->devid = devid;
    tmp = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/%d/backend-id",
                                                   fe_path, devid));
    if( tmp == NULL)
        return ERROR_FAIL;
    usbctrl->backend_domid = atoi(tmp);

    tmp = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/usb-ver", be_path));
    usbctrl->usb_version =  atoi(tmp);

    tmp = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/num-ports", be_path));
    usbctrl->num_ports = atoi(tmp);

    GC_FREE;
    return 0;
}

static int libxl__device_usb_add_xenstore(libxl__gc *gc, uint32_t domid, libxl_device_usb *usb)
{
    libxl_ctx *ctx = CTX; 
    char *be_path;
    
    be_path = libxl__sprintf(gc, "%s/backend/vusb/%d/%d", 
                        libxl__xs_get_dompath(gc, 0), domid, usb->ctrl);
    libxl_domain_type domtype = libxl__domain_type(gc, domid);
    if (domtype == LIBXL_DOMAIN_TYPE_INVALID)
        return ERROR_FAIL;

    if (domtype == LIBXL_DOMAIN_TYPE_PV) {
        if (libxl__wait_for_backend(gc, be_path, "4") < 0)
            return ERROR_FAIL;
    }

    be_path = libxl__sprintf(gc, "%s/port/%d", be_path, usb->port);
    LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, "Adding new usb device to xenstore"); 
    if (libxl__xs_write(gc, XBT_NULL, be_path, "%s", usb->intf) )
        return 1;
    
    return 0;
}

static int libxl__device_usb_remove_xenstore(libxl__gc *gc, uint32_t domid, libxl_device_usb *usb)
{
    libxl_ctx *ctx = CTX;
    char *be_path;

    be_path = libxl__sprintf(gc, "%s/backend/vusb/%d/%d",
                        libxl__xs_get_dompath(gc, 0), domid, usb->ctrl);
    libxl_domain_type domtype = libxl__domain_type(gc, domid);
    if (domtype == LIBXL_DOMAIN_TYPE_INVALID)
        return ERROR_FAIL;

    if (domtype == LIBXL_DOMAIN_TYPE_PV) {
        if (libxl__wait_for_backend(gc, be_path, "4") < 0)
            return ERROR_FAIL;
    }

    be_path = libxl__sprintf(gc, "%s/port/%d", be_path, usb->port);
    LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, "Removing USB device from xenstore");

    if (libxl__xs_write(gc,XBT_NULL, be_path, "") )
        return 1;

    return 0;
}

int libxl__device_usb_assigned_list(libxl__gc *gc, libxl_device_usb **list, int *num)
{
    char **domlist;
    unsigned int nd = 0, i, j;
    char *be_path;
    libxl_device_usb *usb;

    *list = NULL;
    *num = 0;

    domlist = libxl__xs_directory(gc, XBT_NULL, "/local/domain", &nd);
    be_path = libxl__sprintf(gc,"/local/domain/0/backend/vusb");
    for (i = 0; i < nd; i++) {
        char *path, *num_ports, **ctrl_list;
        unsigned int nc = 0;
        
        path = libxl__sprintf(gc, "%s/%s", be_path, domlist[i]);
        ctrl_list = libxl__xs_directory(gc, XBT_NULL, path , &nc);

        for (j = 0; j < nc; j++) {
            path = libxl__sprintf(gc, "%s/%s/%s/num-ports", be_path, domlist[i], ctrl_list[j]);
            num_ports = libxl__xs_read(gc, XBT_NULL, path);
            if ( num_ports ) {
                int nport = atoi(num_ports), k;
                char *devpath, *intf;

                for (k = 1; k <= nport; k++) {
                    devpath = libxl__sprintf(gc, "%s/%s/%s/port/%u", be_path, domlist[i], ctrl_list[j], k);
                    intf = libxl__xs_read(gc, XBT_NULL, devpath);
                    /* If there are USB device attached, add it to list */
                    if (intf && strcmp(intf, "") ) {
                        *list = realloc(*list, sizeof(libxl_device_usb) * ((*num) + 1));
                        if (*list == NULL)
                            return ERROR_NOMEM;
                        usb = *list + *num;
                        usb->ctrl = atoi(ctrl_list[j]);
                        usb->port = k;
                        usb->intf = strdup(intf);
                        (*num)++;
                    }
                }
            }
        }
    }
    libxl__ptr_add(gc, *list);

    return 0;
}

libxl_device_usb *libxl_device_usb_assigned_list(libxl_ctx *ctx, int *num)
{
    GC_INIT(ctx);
    libxl_device_usb *usbs;
    int rc;

    rc = libxl__device_usb_assigned_list(gc, &usbs, num);

    GC_FREE;
    return usbs;
}
    
static int is_usb_in_array(libxl_device_usb *assigned, int num_assigned, char *intf)
{
    int i;
    
    for (i = 0; i < num_assigned; i++) {
        if (!strcmp(assigned[i].intf, intf) )
            return 1;
    }

    return 0;
}

static int sysfs_write_intf(libxl__gc *gc, const char * sysfs_path,
                       libxl_device_usb *usb)
{
    libxl_ctx *ctx = CTX;
    char *buf;
    int rc, fd;
    
    fd = open(sysfs_path, O_WRONLY);
    if (fd < 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "Couldn't open %s",
                         sysfs_path);
        return ERROR_FAIL;
    }

    /* bind the usb device to usbback */
    buf = libxl__sprintf(gc,"%s:1.0", usb->intf);
    rc = write(fd, buf, strlen(buf));
    // Annoying to have two if's, but we need the errno 
    if (rc < 0)
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                         "write to %s returned %d", sysfs_path, rc);
    close(fd);

    if (rc < 0)
        return ERROR_FAIL;
    return 0;
}

static int get_usb_bDeviceClass(libxl__gc *gc, char *intf, char *buf)
{
    char *path;
    FILE *fd;
    int rc;

    path = libxl__sprintf(gc, SYSFS_USB_DEVS_PATH"/%s/bDeviceClass", intf);
    
    /* Check if this path exist, if not return 1 */
    if (access(path, R_OK) )
        return 1;
    
    path = libxl__sprintf(gc, "cat %s", path);
    fd = popen(path, "r");
    rc = fscanf(fd, "%s", buf);
    pclose(fd);

    if (rc)
        return 0;
    return 1;
}

static int is_usb_assignable(libxl__gc *gc, char *intf)
{
    int usb_classcode;
    char buf[5];

    if (!get_usb_bDeviceClass(gc, intf, buf) ){
        usb_classcode = atoi(buf);
        if (usb_classcode != USBHUB_CLASS_CODE)
            return 0;
    }
    
    return 1;
}

libxl_device_usb *libxl_device_usb_assignable_list(libxl_ctx *ctx, int *num)
{
    GC_INIT(ctx);
    libxl_device_usb *usbs = NULL, *new, *assigned;
    struct dirent *de;
    DIR *dir;
    int rc, num_assigned;

    *num = 0;

    rc = libxl__device_usb_assigned_list(gc, &assigned, &num_assigned);
    if ( rc )
        goto out;

    dir = opendir(SYSFS_USB_DEVS_PATH);
    if ( NULL == dir ) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "Couldn't open %s", SYSFS_USB_DEVS_PATH);
        goto out_closedir;
    }   
    
    while( (de = readdir(dir)) ) {
        if (!de->d_name)
            continue;

        if ( is_usb_in_array(assigned, num_assigned, de->d_name) )
            continue;

        if( is_usb_assignable(gc, de->d_name) )
            continue;
        new = realloc(usbs, ((*num) + 1) * sizeof(*new));
        if ( NULL == new )
            continue;

        usbs = new;
        new = usbs + *num;

        memset(new, 0, sizeof(*new));
        new->intf = strdup(de->d_name);

        (*num)++;
    }

out_closedir:
    closedir(dir);
out:
    GC_FREE;
    return usbs;
}

static int sysfs_dev_unbind(libxl__gc *gc, libxl_device_usb *usb, 
                                        char **driver_path)
{
    libxl_ctx *ctx = CTX;
    char * spath, *dp = NULL;
    struct stat st;

    spath = libxl__sprintf(gc, SYSFS_USB_DEVS_PATH"/%s:1.0/driver",
                          usb->intf);
    if ( !lstat(spath, &st) ) {
        /* Find the canousbctrlal path to the driver. */
        dp = libxl__zalloc(gc, PATH_MAX);
        dp = realpath(spath, dp);
        if ( !dp ) {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "realpath() failed");
            return -1;
        }

        LIBXL__LOG(ctx, LIBXL__LOG_DEBUG, "Driver re-plug path: %s",
                   dp);

        /* Unbind from the old driver */
        spath = libxl__sprintf(gc, "%s/unbind", dp);
        if ( sysfs_write_intf(gc, spath, usb) < 0 ) {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "Couldn't unbind device");
            return -1;
        }
    }

    if ( driver_path )
        *driver_path = dp;

    return 0;
}

static int usbback_dev_assign(libxl__gc *gc, libxl_device_usb *usb)
{
    libxl_ctx *ctx = CTX;

    if ( sysfs_write_intf(gc, SYSFS_USBBACK_DRIVER"/bind", usb) < 0 ) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                         "Couldn't bind device to usbback!");
        return ERROR_FAIL;
    }
    return 0;
}

static int usbback_dev_unassign(libxl__gc *gc, libxl_device_usb *usb)
{
    libxl_ctx *ctx = CTX;

    /* Remove from usbback */
    if ( sysfs_dev_unbind(gc, usb, NULL) < 0 ) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "Couldn't unbind device!");
        return ERROR_FAIL;
    }
    
    return 0;
}

#define USBBACK_INFO_PATH "/libxl/usbback"

/*cann't write '.' into Xenstore. So, change all '.' to '_' */
static void usb_interface_encode(char *interface)
{
    int i, len = strlen(interface);
    for (i = 0; i < len; i++) {
        if (interface[i] == '.')
            interface[i] = '_';
    }
}

static void usb_assignable_driver_path_write(libxl__gc *gc, libxl_device_usb *usb,
                                            char *driver_path)
{
    libxl_ctx *ctx = CTX;
    char *path, *intf;

    intf = libxl__strdup(gc, usb->intf);
    usb_interface_encode(intf);

    path = libxl__sprintf(gc, USBBACK_INFO_PATH"/%s/driver_path", intf);
    if ( libxl__xs_write(gc, XBT_NULL, path, "%s", driver_path) < 0 ) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_WARNING,
                         "Write of %s to node %s failed.",
                         driver_path, path);
    }
}

static char * usb_assignable_driver_path_read(libxl__gc *gc, libxl_device_usb *usb)
{
    char *intf;
    intf = libxl__strdup(gc, usb->intf);
    usb_interface_encode(intf);

    return libxl__xs_read(gc, XBT_NULL,libxl__sprintf(gc, 
                    USBBACK_INFO_PATH"/%s/driver_path", intf));
}

static void usb_assignable_driver_path_remove(libxl__gc *gc, libxl_device_usb *usb)
{
    libxl_ctx *ctx = CTX;
    char *intf;
    intf = libxl__strdup(gc, usb->intf);
    usb_interface_encode(intf);

    /*Remove the xenstore entry */
    xs_rm(ctx->xsh, XBT_NULL,
            libxl__sprintf(gc,USBBACK_INFO_PATH"/%s/driver_path", intf));
}

#undef USBBACK_INFO_PATH

static int do_usb_add (libxl__gc *gc, uint32_t domid, libxl_device_usb *usb)
{
    int rc = 0;

    switch (libxl__domain_type(gc, domid)) {
    case LIBXL_DOMAIN_TYPE_HVM:
        /* TO-DO */
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        rc = libxl__device_usb_add_xenstore(gc, domid, usb);
        if(rc) goto out;       
        /*  Only after usbback automatically add 
            inft:domid:controllerId:portId to "/sys/bus/usb/drivers/usbback/port_ids
            can we bind the usb device to usbback. So we need to wait for this to be completed.        
        */
        sleep(1);

        rc = usbback_dev_assign(gc, usb);
        if(rc) {
            libxl__device_usb_remove_xenstore(gc, domid, usb);
            goto out;
        }
        break;
    case LIBXL_DOMAIN_TYPE_INVALID:
        return ERROR_FAIL;
    }
    
out:
    return rc;
}

static int libxl__device_set_default_usbctrl(libxl__gc *gc, uint32_t domid, libxl_device_usb *usb)
{
    libxl_ctx *ctx = CTX;
    libxl_device_usbctrl *usbctrls;
    libxl_device_usb *usbs = NULL;
    int numctrl, numusb, i, j, rc;
    char *be_path, *tmp;

    usbctrls = libxl_device_usbctrl_list(ctx, domid, &numctrl);
    if ( !numctrl)
        goto out;

    for (i = 0; i < numctrl; i++) {
        rc = libxl__device_usb_list(gc, domid, &usbs, usbctrls[i].devid, &numusb);
        if (rc) goto out;
        if ( !usbctrls[i].num_ports || numusb == usbctrls[i].num_ports )
            continue;
        for (j = 1; i <= numusb; j++) {
            be_path = libxl__sprintf(gc, "%s/backend/vusb/%d/%d/port/%d",
                             libxl__xs_get_dompath(gc, 0), domid, usbctrls[i].devid, j);
            tmp = libxl__xs_read(gc, XBT_NULL, be_path);
            if ( tmp && !strcmp( tmp, "") ) {
                usb->ctrl = usbctrls[i].devid;
                usb->port = j;
                return 0;
            }
        }
        free(usbs);
    }

out:
    free(usbctrls);
    free(usbs);
    return 1;
} 

int libxl__device_usb_setdefault(libxl__gc *gc, uint32_t domid, libxl_device_usb *usb)
{
    int rc;

    switch (libxl__domain_type(gc, domid)) {
    case LIBXL_DOMAIN_TYPE_HVM:
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        if (usb->ctrl == -1) {
            if (usb->port != -1 ) {
                LOG(ERROR, "USB controller must be specified if you specify port ID");
                return ERROR_INVAL;
            }   
              
            rc = libxl__device_set_default_usbctrl(gc, domid, usb);
            /* If no existing ctrl to host this usb device, setup a new one */
            if (rc) {
                libxl_device_usbctrl usbctrl;
                libxl_device_usbctrl_init(&usbctrl);
                libxl__device_usbctrl_add(gc, domid, &usbctrl);
                usb->ctrl = usbctrl.devid;
                usb->port = 1;
                libxl_device_usbctrl_dispose(&usbctrl);
            } 
        }

        if (usb->port == -1) {
            char *be_path = libxl__sprintf(gc, "%s/backend/vusb/%d/path/%d",
                        libxl__xs_get_dompath(gc, 0), usb->ctrl, usb->port);
            char *tmp = libxl__xs_read(gc, XBT_NULL, be_path);
            if ( !tmp || strcmp(tmp, "") ){
                LOG(ERROR, "The controller port doesn't exist or it has been assigned to another device");
                return ERROR_INVAL;
            }
        }
        break;
    case LIBXL_DOMAIN_TYPE_INVALID:
        return ERROR_FAIL;
    default:
        abort();
    }

    return 0;
}

int libxl_device_usb_add(libxl_ctx *ctx, uint32_t domid, libxl_device_usb *usb,
                    const libxl_asyncop_how *ao_how)
{
    AO_CREATE(ctx, domid, ao_how);
    int rc;

    rc = libxl__device_usb_add(gc, domid, usb);
    libxl__ao_complete(egc, ao, rc);
    return AO_INPROGRESS;
}

int libxl__device_usb_add(libxl__gc *gc, uint32_t domid, libxl_device_usb *usb)
{
    libxl_ctx *ctx = CTX;
    libxl_device_usb *assigned;
    int rc, num_assigned;
    char *driver_path;

    rc = libxl__device_usb_setdefault(gc, domid, usb);
    if (rc) goto out;
    
    rc = libxl__device_usb_assigned_list(gc, &assigned, &num_assigned);
    if ( rc ) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, 
             "cannot determine if USB device is assigned, refusing to continue");
        goto out;
    }

    if ( is_usb_in_array(assigned, num_assigned, usb->intf) ) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "USB device already attached to a domain");
        rc = ERROR_FAIL;
        goto out;
    }
    
    /* Check to see if there's already a driver that we need to unbind from */
    if ( sysfs_dev_unbind(gc, usb, &driver_path ) ) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR,
                   "Couldn't unbind %s from driver", usb->intf);
        return ERROR_FAIL;
    }

    /* Store driver_path for rebinding to dom0 */
    if ( driver_path ) {
        usb_assignable_driver_path_write(gc, usb, driver_path);
    } else {
        LIBXL__LOG(ctx, LIBXL__LOG_WARNING,
                   "%s not bound to a driver, will not be rebound.", usb->intf);
    }
    
    if (do_usb_add(gc, domid, usb) ) 
       return ERROR_FAIL;
 
out:
    return rc;
}

static int do_usb_remove(libxl__gc *gc, uint32_t domid,
                        libxl_device_usb *usb, int force)
{

    libxl_ctx *ctx = CTX;
    libxl_device_usb *assigned;
    int rc, num;

    assigned = libxl_device_usb_list_all(gc, domid, &num);
    if ( assigned == NULL ) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "There is no USB device attached to this domain");
        return ERROR_FAIL;
    }

    rc = ERROR_INVAL;
    if ( !is_usb_in_array(assigned, num, usb->intf) ) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "USB device not attached to this domain");
        goto out_fail;
    }

    rc = ERROR_FAIL;
    switch (libxl__domain_type(gc, domid)) {
    case LIBXL_DOMAIN_TYPE_HVM:
        //TO-DO
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        //unbind USB device from usbback TO-DO
        rc = usbback_dev_unassign(gc, usb);

        if (rc) return rc;
        rc = libxl__device_usb_remove_xenstore(gc, domid, usb);
        if(rc) 
            return rc;
        break;
    default:
        abort();
    }
out_fail:
    return 0;
}

static int libxl__device_usb_remove_common(libxl__gc *gc, uint32_t domid,
                                libxl_device_usb *usb, int force)
{
    libxl_ctx *ctx = CTX;
    int rc;
    char *driver_path;

    rc = do_usb_remove(gc, domid, usb, force);

    if (rc)
        return ERROR_INVAL;
    
    /* Rebind if necessary */
    driver_path = usb_assignable_driver_path_read(gc, usb);

    if ( driver_path ) {
        LIBXL__LOG(ctx, LIBXL__LOG_INFO, "Rebinding USB device %s to driver at %s",
                                        usb->intf, driver_path);

        if ( sysfs_write_intf(gc, libxl__sprintf(gc, "%s/bind", driver_path),
                                 usb) < 0 ) {
            LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                             "Couldn't bind device to %s", driver_path);
            return -1;
        }
    }

    usb_assignable_driver_path_remove(gc, usb);

    return 0;
}

int libxl_device_usb_remove(libxl_ctx *ctx, uint32_t domid,
                            libxl_device_usb *usb, const libxl_asyncop_how *ao_how)

{
    AO_CREATE(ctx, domid, ao_how);
    int rc;

    rc = libxl__device_usb_remove_common(gc, domid, usb, 0);

    libxl__ao_complete(egc, ao, rc);
    return AO_INPROGRESS;
}
            
int libxl_device_usb_destroy(libxl_ctx *ctx, uint32_t domid,
                             libxl_device_usb *usb, const libxl_asyncop_how *ao_how)
{
    AO_CREATE(ctx, domid, ao_how);
    int rc;

    rc = libxl__device_usb_remove_common(gc, domid, usb, 1);

    libxl__ao_complete(egc, ao, rc);
    return AO_INPROGRESS;
}


int libxl__device_usb_list(libxl__gc *gc, uint32_t domid, libxl_device_usb **usbs, int usbctrl, int *num)
{
    char *be_path, *num_devs;
    int n, i;
    libxl_device_usb *usb;
    
    *usbs = NULL;
    *num = 0;

    be_path = libxl__sprintf(gc, "%s/backend/vusb/%d/%d", libxl__xs_get_dompath(gc, 0), domid, usbctrl);
    num_devs = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/num-ports", be_path));
    if (!num_devs)
        goto out;

    n = atoi(num_devs);
    *usbs = calloc(n, sizeof(libxl_device_usb));

    char *intf;
    for (i = 0; i < n; i++) {
        intf = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc,"%s/port/%d", be_path, i + 1));
        if ( intf && strcmp(intf, "") ) {
            usb = *usbs + *num;
            usb->intf = strdup(intf);
            usb->port = i + 1;
            usb->ctrl = usbctrl;
            (*num)++;
        }
    }
out:
    return 0;
}

libxl_device_usb *libxl_device_usb_list(libxl_ctx *ctx, uint32_t domid, int usbctrl, int *num)
{
    GC_INIT(ctx);
    libxl_device_usb *usbs;
    int rc;

    rc = libxl__device_usb_list(gc, domid, &usbs, usbctrl, num);

    if (rc)
        free(usbs);
    GC_FREE;
    return usbs;
}

libxl_device_usb *libxl_device_usb_list_all(libxl__gc *gc, uint32_t domid, int *num)
{
    char **usblist;
    unsigned int nd, i, j;
    char *be_path;
    int rc;
    libxl_device_usb *usbs = NULL;

    *num = 0;

    be_path = libxl__sprintf(gc,"/local/domain/0/backend/vusb/%d", domid);
    usblist = libxl__xs_directory(gc, XBT_NULL, be_path, &nd);

    for (i = 0; i < nd; i++) { 
        int nc = 0;
        libxl_device_usb *tmp;
        rc = libxl__device_usb_list(gc, domid, &tmp, atoi(usblist[i]), &nc);
        if (!nc) 
            continue;
        usbs = realloc(usbs, sizeof(libxl_device_usb)*((*num) + nc));
        /* TO-DO
        if (usbs == NULL)
            return ERROR_NOMEM;
        */
        for(j = 0; j < nc; j++) {
            usbs[*num].ctrl = tmp[j].ctrl;
            usbs[*num].port = tmp[j].port;
            usbs[*num].intf = strdup(tmp[j].intf);
            (*num)++;
        }
        free(tmp);
    }
    return usbs;
}

int libxl__device_usb_destroy_all(libxl__gc *gc, uint32_t domid)
{
    libxl_ctx *ctx = CTX;
    libxl_device_usbctrl *usbctrls;
    int num, i, rc = 0;

    usbctrls = libxl_device_usbctrl_list(ctx, domid, &num);
    if ( usbctrls == NULL )
        return 0;

    for (i = 0; i < num; i++) {
        /* Force remove on shutdown since, on HVM, qemu will not always
         * respond to SCI interrupt because the guest kernel has shut down the
         * devices by the time we even get here!
         */
        if (libxl__device_usbctrl_remove_common(ctx, domid, usbctrls + i, 0, 1) < 0)
            rc = ERROR_FAIL;
    }

    free(usbctrls);
    return 0;
}

/* TO-DO: map the "lsusb" bus:addr to the sysfs usb address
static int libxl_hostdev_to_intf(libxl__gc *gc, libxl_device_usb *usb)
{
    return 0;
}
*/

#define BUF_SIZE 20
/*use system call popen to get usb device information */
static int get_usb_devnum (libxl__gc *gc, const char *intf, char *buf)
{
    char *path;
    int rc;
    FILE *fd;

    path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/devnum", intf);
    fd = popen(path, "r");
    rc = fscanf(fd, "%s", buf);
    pclose(fd);

    if (rc) return 0;
    return 1;
}

static int get_usb_busnum(libxl__gc *gc, const char *intf, char *buf)
{
    char *path;
    int rc;
    FILE *fd;

    path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/busnum", intf);
    fd = popen(path, "r");
    rc = fscanf(fd, "%s", buf);
    pclose(fd);

    if (rc) return 0;
    return 1;
}

static int get_usb_idVendor(libxl__gc *gc, const char *intf, char *buf)
{
    char *path;
    int rc;
    FILE *fd;

    path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/idVendor", intf);
    fd = popen(path, "r");
    rc = fscanf(fd, "%s", buf);
    pclose(fd);

    if (rc) return 0;
    return 1;
}

static int get_usb_idProduct(libxl__gc *gc, const char *intf, char *buf)
{
    char *path;
    int rc;
    FILE *fd;

    path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/idProduct", intf);
    fd = popen(path, "r");
    rc = fscanf(fd, "%s", buf);
    pclose(fd);

    if (rc) return 0;
    return 1;
}

static int get_usb_manufacturer(libxl__gc *gc, const char *intf, char *buf)
{
    char *path;
    int rc;
    FILE *fd;

    path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/manufacturer", intf);
    fd = popen(path, "r");
    rc = fscanf(fd, "%s", buf);
    pclose(fd);

    if (rc) return 0;
    return 1;
}

static int get_usb_product(libxl__gc *gc, const char *intf, char *buf)
{
    char *path;
    int rc;
    FILE *fd;

    path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/product", intf);
    fd = popen(path, "r");
    rc = fscanf(fd, "%s", buf);
    pclose(fd);

    if (rc) return 0;
    return 1;
}

int libxl_device_usb_getinfo(libxl_ctx *ctx, char *intf, libxl_usbinfo *usbinfo)
{
    GC_INIT(ctx);
    char buf[20];

    //change usb, why!!!!????
    // maybe some better method to get this parameter

    if (!get_usb_devnum(gc, intf, buf) ) 
        usbinfo->devnum = atoi(buf);

    if ( !get_usb_busnum(gc, intf, buf))
        usbinfo->bus = atoi(buf);

    if (!get_usb_idVendor(gc, intf, buf) )
         usbinfo->idVendor = atoi(buf);

    if (!get_usb_idProduct(gc, intf, buf) )
        usbinfo->idProduct  = atoi(buf);

    if (!get_usb_manufacturer(gc, intf, buf) )
        usbinfo->manuf = strdup(buf);        
    
    if (!get_usb_product(gc, intf, buf) )
        usbinfo->prod = strdup(buf);
    
    GC_FREE;
    return 0; 
}

int libxl_hostdev_to_device_usb(libxl_ctx *ctx, uint32_t domid,
                               int devid, libxl_device_usb *usb)
{
    return 0;
}

//If specified by user, no need to convert; otherwise, search for it
int libxl_intf_to_device_usb(libxl_ctx *ctx, uint32_t domid,
                               char *intf, libxl_device_usb *usb)
{
    GC_INIT(ctx);
    libxl_device_usb *usbs;

    int rc, num, i;

    rc = libxl__device_usb_assigned_list(gc, &usbs, &num);
    if(rc) goto out; 
    
    for (i = 0; i < num; i++) {
        if (!strcmp(intf, usbs[i].intf) ) {
            usb->ctrl = usbs[i].ctrl;
            usb->port = usbs[i].port;
            usb->intf = strdup(usbs[i].intf);
            free(usbs);
            return 0;
        }
    }
out:
    GC_FREE;
    free(usbs);
    return 1;
}

/*
 * Local variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
