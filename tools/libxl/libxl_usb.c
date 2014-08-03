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

static int do_pvusbctrl_add(libxl__egc *egc, uint32_t domid,
                        libxl_device_usbctrl *usbctrl, libxl__ao_device *aodev) 
{
    STATE_AO_GC(aodev->ao);
    flexarray_t *front;
    flexarray_t *back;
    libxl__device *device;
    unsigned int rc;

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
    aodev->dev = device;
    aodev->action = LIBXL__DEVICE_ACTION_ADD;
    libxl__wait_device_connection(egc, aodev);

    rc = 0;
out:
    aodev->rc = rc;
    if (rc) aodev->callback(egc, aodev);
    return 0;
}

// TO BE Improved
static int libxl_port_add_xenstore(libxl__gc *gc, uint32_t domid,
                                    libxl_device_usbctrl *usbctrl) {
    libxl_ctx *ctx = libxl__gc_owner(gc);
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
                              
//do some check
int libxl__device_usbctrl_add(libxl__gc *gc, libxl__egc *egc, uint32_t domid,
                           libxl_device_usbctrl *usbctrl, libxl__ao_device *aodev) 
{
    //libxl_ctx *ctx = libxl__gc_owner(gc);
    int rc;
    
    //chech device status
    
    //do_usbctrl_add
    switch (libxl__domain_type(gc, domid)) {
    case LIBXL_DOMAIN_TYPE_HVM:
         //TO_DO
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        if (do_pvusbctrl_add(egc, domid, usbctrl, aodev)) {
            rc = ERROR_FAIL;
            goto out;
        }
        //Add sub-port
        if (libxl_port_add_xenstore(gc, domid, usbctrl)) {
            rc = ERROR_FAIL;
            goto out;
        }
        break;
    case LIBXL_DOMAIN_TYPE_INVALID:
            return ERROR_FAIL;
    }
out:
    return rc;
}

//according to usbctrl,usbctrl add, 
int libxl_device_usbctrl_add(libxl_ctx *ctx, uint32_t domid, 
                                libxl_device_usbctrl *usbctrl, 
                                const libxl_asyncop_how *ao_how)
{
    AO_CREATE(ctx, domid, ao_how);
    libxl__ao_device *aodev;
    
    GCNEW(aodev);
    libxl__prepare_ao_device(ao, aodev);
    aodev->callback = device_addrm_aocomplete;
    libxl__device_usbctrl_add(gc, egc, domid, usbctrl, aodev);
    
    return AO_INPROGRESS; 
}

libxl_device_usbctrl *libxl_device_usbctrl_list(libxl_ctx *ctx, uint32_t domid, int *num)
{
    GC_INIT(ctx);
    
    libxl_device_usbctrl* usbctrls = NULL;
    //int rc;
    char* fe_path = NULL;
    char** dir = NULL;
    unsigned int ndirs = 0;
    
    *num = 0;
    
    fe_path = libxl__sprintf(gc, "%s/device/vusb", libxl__xs_get_dompath(gc, domid));
    dir = libxl__xs_directory(gc, XBT_NULL, fe_path, &ndirs);

    if (dir && ndirs) {
        usbctrls = malloc(sizeof(*usbctrls) * ndirs);
        libxl_device_usbctrl* usbctrl;
        libxl_device_usbctrl* end = usbctrls + ndirs;
        for(usbctrl = usbctrls; usbctrl < end; ++usbctrl, ++dir, (*num)++) {
            char* tmp;
            const char* be_path = libxl__xs_read(gc, XBT_NULL,
                                    GCSPRINTF("%s/%s/backend", fe_path, *dir));

            libxl_device_usbctrl_init(usbctrl);

            usbctrl->devid = atoi(*dir);

            tmp = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/%s/backend-id",
                                                   fe_path, *dir));
            if( tmp == NULL)
                goto outerr;
            usbctrl->backend_domid = atoi(tmp);

            tmp = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/usb-ver", be_path));
            usbctrl->usb_version =  atoi(tmp);

            tmp = libxl__xs_read(gc, XBT_NULL, GCSPRINTF("%s/num-ports", be_path));
            usbctrl->num_ports = atoi(tmp);
       }
    }
    *num = ndirs;
    
    GC_FREE;
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
        //TO_DO
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        //Remove USB devives first
        usbs = libxl_device_usb_list(ctx, domid, usbctrl->devid, &numusb);
        for (i = 0; i < numusb; i++) {
            if (libxl__device_usb_remove_common(gc, domid, &usbs[i], 0)) {
                fprintf(stderr, "libxl_device_usb_remove failed.\n");
                return 1;
            }
        }
        //Remove usbctrl
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

    return 0;
}

/*
int libxl_name_to_device_usbctrl(libxl_ctx *ctx,
                               uint32_t domid,
                               int devid,
                               libxl_device_usbctrl *usbctrl)
{
    return 0;    
}

*/

static int libxl__device_usb_add_xenstore(libxl__gc *gc, uint32_t domid, libxl_device_usb *usb)
{
    libxl_ctx *ctx = libxl__gc_owner(gc); 
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
    libxl_ctx *ctx = libxl__gc_owner(gc);
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
                    //If there are USB device attached, add it to list 
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
    libxl_ctx *ctx = libxl__gc_owner(gc);
    int rc;
    char *buf;

    int fd = open(sysfs_path, O_WRONLY);
    if (fd < 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "Couldn't open %s",
                         sysfs_path);
        return ERROR_FAIL;
    }

    //Add the usb-controller to usbback driver
    buf = libxl__sprintf(gc,"%s:1.0", usb->intf);
    rc = write(fd, buf, strlen(buf));
    /* Annoying to have two if's, but we need the errno */
    if (rc < 0)
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                         "write to %s returned %d", sysfs_path, rc);
    close(fd);

    if (rc < 0)
        return ERROR_FAIL;

    return 0;
}

/*Get assignable usb devices*/
static int get_usb_bDeviceClass(libxl__gc *gc, char *intf, char *buf)
{
    char *path = libxl__sprintf(gc, SYSFS_USB_DEVS_PATH"/%s/bDeviceClass", intf);
    
    //Check if this path exist, if not return 1
    if (access(path, R_OK) )
        return 1;
    
    path = libxl__sprintf(gc, "cat %s", path);
    FILE *fd = popen(path, "r");
    
    int rc = fscanf(fd, "%s", buf);
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
    libxl_ctx *ctx = libxl__gc_owner(gc);
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
    libxl_ctx *ctx = libxl__gc_owner(gc);

    if ( sysfs_write_intf(gc, SYSFS_USBBACK_DRIVER"/bind", usb) < 0 ) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR,
                         "Couldn't bind device to usbback!");
        return ERROR_FAIL;
    }
    return 0;
}

static int usbback_dev_unassign(libxl__gc *gc, libxl_device_usb *usb)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);

    /* Remove from usbback */
    if ( sysfs_dev_unbind(gc, usb, NULL) < 0 ) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "Couldn't unbind device!");
        return ERROR_FAIL;
    }
    
    return 0;
}
/*
#define USBBACK_INFO_PATH "/libxl/usbback"

static void usb_driver_path_write(libxl__gc *gc, libxl_device_usb *usb,
                                            char *driver_path)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    char *path;

    path = libxl__sprintf(gc, USBBACK_INFO_PATH"/%s/driver_path",
                          usb->intf);
    if ( libxl__xs_write(gc, XBT_NULL, path, "%s", driver_path) < 0 ) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_WARNING,
                         "Write of %s to node %s failed.",
                         driver_path, path);
    }
}

static char * usb_driver_path_read(libxl__gc *gc, libxl_device_usb *usb)
{
    return libxl__xs_read(gc, XBT_NULL,libxl__sprintf(gc, 
                    USBBACK_INFO_PATH"/%s/driver_path", usb->intf));
}

static void usb_driver_path_remove(libxl__gc *gc, libxl_device_usb *usb)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);

    Remove the xenstore entry
    xs_rm(ctx->xsh, XBT_NULL,
            libxl__sprintf(gc,USBBACK_INFO_PATH"/%s/driver_path",
                                        usb->intf));
}
*/
static int do_usb_add (libxl__gc *gc, uint32_t domid, libxl_device_usb *usb)
{
    int rc = 0, hvm = 0;

    switch (libxl__domain_type(gc, domid)) {
    case LIBXL_DOMAIN_TYPE_HVM:
        hvm = 1;
        if (libxl__wait_for_device_model_deprecated(gc, domid, "running",
                                         NULL, NULL, NULL) < 0) {
            return ERROR_FAIL;
        }
        switch (libxl__device_model_version_running(gc, domid)) {
            case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN_TRADITIONAL:
                //rc = qemu_usb_add_xenstore(gc, domid, usb);
                break;
            /*
            case LIBXL_DEVICE_MODEL_VERSION_QEMU_XEN:
                rc = libxl__qmp_usb_add(gc, domid, usb);
                break;
            default:
            */
                return ERROR_INVAL;
        }
        if ( rc )
            return ERROR_FAIL;
        break;
    case LIBXL_DOMAIN_TYPE_PV:
        rc = libxl__device_usb_add_xenstore(gc, domid, usb);
        if(rc) goto out;       
        //maybe fail if writing to port_ids too slow ....
        sleep(1);
        //rc = libxl__device_bind_intf(gc, domid, usb);
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

static int libxl__device_ctrl_default(libxl__gc *gc, uint32_t domid,  libxl_device_usb *usb)
{
    libxl_ctx *ctx = CTX;
    libxl_device_usbctrl *usbctrls;
    libxl_device_usb *usbs = NULL;
    int numctrl, numusb, i, j;
    char *be_path, *tmp;

    usbctrls = libxl_device_usbctrl_list(ctx, domid, &numctrl);
    if ( !numctrl)
        goto out;

    for (i = 0; i < numctrl; i++) {
        usbs = libxl_device_usb_list(ctx, domid, usbctrls[i].devid, &numusb);
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

int libxl__device_usb_setdefault(libxl__gc *gc, libxl_device_usb *usb,
                            uint32_t domid)
{
    int rc;
    //usb->type = LIBXL_USB_TYPE_HOSTDEV;

    switch (libxl__domain_type(gc, domid)) {
        case LIBXL_DOMAIN_TYPE_HVM:
            break;
        case LIBXL_DOMAIN_TYPE_PV:{
            //default ctrl and port
            if (usb->ctrl == -1) {
                if (usb->port != -1 ) {
                    LOG(ERROR, "USB controller must be specified if you specify port ID");
                    return ERROR_INVAL;
                }   
                
                rc = libxl__device_ctrl_default(gc, domid, usb);
                //If no ctrl exists, setup new ctrl;
                if (rc) {
                    LOG(ERROR, "Please add controller first: xl usb-controller-attach %d", domid);
                    /* How to call libxl_device_usbctrl_add inside libxl ??
                    libxl_device_usbctrl usbctrl;
                    libxl_device_usbctrl_init(&usbctrl);
                    libxl_device_usbctrl_add(CTX, domid, &usbctrl, 0);

                    usb->ctrl = usbctrl.devid;
                    usb->port = 1;
                    libxl_device_usbctrl_dispose(&usbctrl);
                    break;
                    */
                }
                // if not, create new ctrl;
            }
            if (usb->port == -1) {
                //check if it's assigned
                char *be_path = libxl__sprintf(gc, "%s/backend/vusb/%d/path/%d",
                             libxl__xs_get_dompath(gc, 0), usb->ctrl, usb->port);
                char *tmp = libxl__xs_read(gc, XBT_NULL, be_path);
                if ( !tmp || strcmp(tmp, "") ){
                    LOG(ERROR, "The controller port doesn't exist or it has been assigned to another device");
                    return ERROR_INVAL;
                }
            }
            break;
        }
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
    if (rc) {
        LOG(ERROR, "unable to add usb device");
        goto out;
    }

out:
    libxl__ao_complete(egc, ao, rc);
    return AO_INPROGRESS;
}

/* Maybe not needed
static int libxl_usb_assignable(libxl_ctx *ctx, libxl_device_usb *usb)
{
    libxl_device_usb *usbs;
    int num, i;
    
    usbs = libxl_device_usb_assignable_list(ctx, &num);
    for (i = 0; i < num; i++) {
        if (strcmp(usbs[i].intf, usb->intf) )
            break;
    }
    free(usbs);
    return i != num;
}
*/

int libxl__device_usb_add(libxl__gc *gc, uint32_t domid,
                             libxl_device_usb *usb)
{
    libxl_ctx *ctx = libxl__gc_owner(gc);
    libxl_device_usb *assigned;
    int rc, num_assigned;

    rc = libxl__device_usb_setdefault(gc, usb, domid);
    if (rc) goto out;
    
    // do some check, maybe not need to usbback
    /*
    if (!usbback_dev_is_assigned(gc, usb)) {
        rc = libxl__device_usb_assignable_add(gc, usb, 1);
        if ( rc )
            goto out;
    }
    
    if (!libxl_usb_assignable(ctx, usb)) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "USB device %s is not assignable",
                                           usb->intf);
        rc = ERROR_FAIL;
        goto out;
    }
    */

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
    
    if (do_usb_add(gc, domid, usb) ) 
       return ERROR_FAIL;
 
out:
    return rc;
}

static int qemu_usb_remove_xenstore(libxl__gc *gc, uint32_t domid,
                                 libxl_device_usb *usb)
{
    return 0;
}

static int do_usb_remove(libxl__gc *gc, uint32_t domid,
                        libxl_device_usb *usb, int force)
{

    libxl_ctx *ctx = libxl__gc_owner(gc);
    libxl_device_usb *assigned;
    int hvm = 0, rc, num;

    assigned = libxl_device_usb_list_all(ctx, domid, &num);
    if ( assigned == NULL )
        return ERROR_FAIL;

    rc = ERROR_INVAL;
    if ( !is_usb_in_array(assigned, num, usb->intf) ) {
        LIBXL__LOG(ctx, LIBXL__LOG_ERROR, "USB device not attached to this domain");
        goto out_fail;
    }

    rc = ERROR_FAIL;
    switch (libxl__domain_type(gc, domid)) {
    case LIBXL_DOMAIN_TYPE_HVM:
        hvm = 1;
        //TO-DO
        rc = qemu_usb_remove_xenstore(gc, domid, usb);
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
    if (do_usb_remove(gc, domid, usb, force) ) 
        return 1;
    return 0;
}

int libxl_device_usb_remove(libxl_ctx *ctx, uint32_t domid,
                            libxl_device_usb *usb,
                            const libxl_asyncop_how *ao_how)

{
    AO_CREATE(ctx, domid, ao_how);
    int rc;

    rc = libxl__device_usb_remove_common(gc, domid, usb, 0);

    libxl__ao_complete(egc, ao, rc);
    return AO_INPROGRESS;
}
            
int libxl_device_usb_destroy(libxl_ctx *ctx, uint32_t domid,
                             libxl_device_usb *usb,
                             const libxl_asyncop_how *ao_how)
{
    AO_CREATE(ctx, domid, ao_how);
    int rc;

    rc = libxl__device_usb_remove_common(gc, domid, usb, 1);

    libxl__ao_complete(egc, ao, rc);
    return AO_INPROGRESS;
}

libxl_device_usb *libxl_device_usb_list(libxl_ctx *ctx, uint32_t domid, int usbctrl, int *num)
{
    GC_INIT(ctx);
    char *be_path, *num_devs;
    int n, i;
    libxl_device_usb *usbs = NULL;

    *num = 0;

    be_path = libxl__sprintf(gc, "%s/backend/vusb/%d/%d", libxl__xs_get_dompath(gc, 0), domid, usbctrl);
    num_devs = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc, "%s/num-ports", be_path));
    if (!num_devs)
        goto out;

    n = atoi(num_devs);
    usbs = calloc(n, sizeof(libxl_device_usb));

    char *intf;
    for (i = 0; i < n; i++) {
        intf = libxl__xs_read(gc, XBT_NULL, libxl__sprintf(gc,"%s/port/%d", be_path, i + 1));
        if ( intf && strcmp(intf, "") ) {
            usbs[*num].intf = strdup(intf);
            usbs[*num].port = i + 1;
            usbs[*num].ctrl = usbctrl; 
            (*num)++;
        }
    } 
out:
    GC_FREE;
    return usbs;
}

libxl_device_usb *libxl_device_usb_list_all(libxl_ctx *ctx, uint32_t domid, int *num)
{
   //usb libxl_device_usb_list to list all usb devices for a domid
    GC_INIT(ctx);
    char ** vusblist;
    unsigned int nd, i, j;
    char *be_path;
    libxl_device_usb *usbs = NULL;

    *num = 0;

    be_path = libxl__sprintf(gc,"/local/domain/0/backend/vusb/%d", domid);
    vusblist = libxl__xs_directory(gc, XBT_NULL, be_path, &nd);

    for (i = 0; i < nd; i++) { 
        int nc = 0;
        libxl_device_usb *tmp;
        tmp = libxl_device_usb_list(ctx, domid, atoi(vusblist[i]), &nc);
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
    libxl_ctx *ctx = libxl__gc_owner(gc);
    libxl_device_usb *usbs;
    int num, i, rc = 0;

    usbs = libxl_device_usb_list_all(ctx, domid, &num);
    if ( usbs == NULL )
        return 0;

    for (i = 0; i < num; i++) {
        /* Force remove on shutdown since, on HVM, qemu will not always
         * respond to SCI interrupt because the guest kernel has shut down the
         * devices by the time we even get here!
         */
        if (libxl__device_usb_remove_common(gc, domid, usbs + i, 1) < 0)
            rc = ERROR_FAIL;
    }

    free(usbs);
    return 0;
}

/*
static int libxl_hostdev_to_intf(libxl__gc *gc, libxl_device_usb *usb)
{
    return 0;
}
*/
#define BUF_SIZE 20
//use system call open and write to manipulate usb device
    
static int get_usb_devnum (libxl__gc *gc, const char *intf, char *buf)
{
    //GC_INIT(ctx);
    //int rc, fd;
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/devnum", intf);

    /*
    fd = open(path, O_RDONLY);
    if (fd < 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "Couldn't open %s",
                         path);
        return ERROR_FAIL;
    }
    
    rc = read(fd, buf, 10);
    */
    FILE *fd = popen(path, "r");
    char *tmp = fgets(buf, BUF_SIZE, fd);
    pclose(fd);
    if(!tmp)
        return 1;
    return 0;
}

static int get_usb_busnum(libxl__gc *gc, const char *intf, char *buf)
{
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/busnum", intf);

    FILE *fd = popen(path, "r");
    char *tmp = fgets(buf, BUF_SIZE, fd );
    pclose(fd);
    if(!tmp)
        return 1;
    return 0;
}

static int get_usb_idVendor(libxl__gc *gc, const char *intf, char *buf)
{
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/idVendor", intf);

    char *tmp = fgets(buf, BUF_SIZE, popen(path, "r") );
    if(!tmp)
        return 1;
    return 0; 
}

static int get_usb_idProduct(libxl__gc *gc, const char *intf, char *buf)
{
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/idProduct", intf);

    char *tmp = fgets(buf, BUF_SIZE, popen(path, "r") );
    if(!tmp)
        return 1;
    return 0;
}

static int get_usb_manufacturer(libxl__gc *gc, const char *intf, char *buf)
{
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/manufacturer", intf);

    /*
    char *tmp = fgets(buf, BUF_SIZE, popen(path, "r") );
    if(!tmp)
        return 1;
    */
    FILE *fd = popen(path, "r");
    int rc = fscanf(fd, "%s", buf);
    pclose(fd);

    if (rc) return 0;
    return 1;
}

static int get_usb_product(libxl__gc *gc, const char *intf, char *buf)
{
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/product", intf);
/*
    char *tmp = fgets(buf, BUF_SIZE, popen(path, "r") );
    if(!tmp)
        return 1;

    return 0;    
*/
    FILE *fd = popen(path, "r");
    int rc = fscanf(fd, "%s", buf);

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
/*
//use system call open and write to manipulate usb device
static int get_usb_devnum (libxl__gc *gc, libxl_device_usb *usb, char *buf)
{
    //GC_INIT(ctx);
    //int rc, fd;
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/devnum", usb->intf);

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        LIBXL__LOG_ERRNO(ctx, LIBXL__LOG_ERROR, "Couldn't open %s",
                         path);
        return ERROR_FAIL;
    }
    
    rc = read(fd, buf, 10);
    FILE *fd = popen(path, "r");
    char *tmp = fgets(buf, BUF_SIZE, fd);
    pclose(fd);
    if(!tmp)
        return 1;
    return 0;
}

static int get_usb_busnum(libxl__gc *gc, const libxl_device_usb *usb, char *buf)
{
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/busnum", usb->intf);

    char *tmp = fgets(buf, BUF_SIZE, popen(path, "r") );
    if(!tmp)
        return 1;
    return 0;
}

static int get_usb_idVendor(libxl__gc *gc, libxl_device_usb *usb, char *buf)
{
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/idVendor", usb->intf);

    char *tmp = fgets(buf, BUF_SIZE, popen(path, "r") );
    if(!tmp)
        return 1;
    return 0; 
}

static int get_usb_idProduct(libxl__gc *gc, libxl_device_usb *usb, char *buf)
{
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/idProduct", usb->intf);

    char *tmp = fgets(buf, BUF_SIZE, popen(path, "r") );
    if(!tmp)
        return 1;
    return 0;
}

static int get_usb_manufacturer(libxl__gc *gc, libxl_device_usb *usb, char *buf)
{
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/manufacturer", usb->intf);

    char *tmp = fgets(buf, BUF_SIZE, popen(path, "r") );
    if(!tmp)
        return 1;

    return 0;
}

static int get_usb_product(libxl__gc *gc, libxl_device_usb *usb, char *buf)
{
    char *path = libxl__sprintf(gc, "cat "SYSFS_USB_DEVS_PATH"/%s/product", usb->intf);

    char *tmp = fgets(buf, BUF_SIZE, popen(path, "r") );
    if(!tmp)
        return 1;

    return 0;    
}
*/
