/* Copyright (c) 2012-2013, The Linux Foundation. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are
 * met:
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above
 *       copyright notice, this list of conditions and the following
 *       disclaimer in the documentation and/or other materials provided
 *       with the distribution.
 *     * Neither the name of The Linux Foundation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED "AS IS" AND ANY EXPRESS OR IMPLIED
 * WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NON-INFRINGEMENT
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR
 * BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
 * WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE
 * OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN
 * IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 */

#include <ctype.h>
#include <cutils/properties.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <linux/msm_ion.h>
#include <sys/mman.h>

#include "mm_qcamera_dbg.h"
#include "mm_qcamera_app.h"

static pthread_mutex_t app_mutex;
static int thread_status = 0;
static pthread_cond_t app_cond_v;

#define MM_QCAMERA_APP_NANOSEC_SCALE 1000000000

int mm_camera_app_timedwait(uint8_t seconds)
{
    int rc = 0;
    pthread_mutex_lock(&app_mutex);
    if(FALSE == thread_status) {
        struct timespec tw;
        memset(&tw, 0, sizeof tw);
        tw.tv_sec = 0;
        tw.tv_nsec = time(0) + seconds * MM_QCAMERA_APP_NANOSEC_SCALE;

        rc = pthread_cond_timedwait(&app_cond_v, &app_mutex,&tw);
        thread_status = FALSE;
    }
    pthread_mutex_unlock(&app_mutex);
    return rc;
}

int mm_camera_app_wait()
{
    int rc = 0;
    pthread_mutex_lock(&app_mutex);
    if(FALSE == thread_status){
        pthread_cond_wait(&app_cond_v, &app_mutex);
        thread_status = FALSE;
    }
    pthread_mutex_unlock(&app_mutex);
    return rc;
}

void mm_camera_app_done()
{
  pthread_mutex_lock(&app_mutex);
  thread_status = TRUE;
  pthread_cond_signal(&app_cond_v);
  pthread_mutex_unlock(&app_mutex);
}

int mm_app_load_hal(mm_camera_app_t *my_cam_app)
{
    memset(&my_cam_app->hal_lib, 0, sizeof(hal_interface_lib_t));
    my_cam_app->hal_lib.ptr = dlopen("libmmcamera_interface.so", RTLD_NOW);
    my_cam_app->hal_lib.ptr_jpeg = dlopen("libmmjpeg_interface.so", RTLD_NOW);
    if (!my_cam_app->hal_lib.ptr || !my_cam_app->hal_lib.ptr_jpeg) {
        CDBG_ERROR("%s Error opening HAL library %s\n", __func__, dlerror());
        return -MM_CAMERA_E_GENERAL;
    }
    *(void **)&(my_cam_app->hal_lib.get_num_of_cameras) =
        dlsym(my_cam_app->hal_lib.ptr, "get_num_of_cameras");
    *(void **)&(my_cam_app->hal_lib.mm_camera_open) =
        dlsym(my_cam_app->hal_lib.ptr, "camera_open");
    *(void **)&(my_cam_app->hal_lib.jpeg_open) =
        dlsym(my_cam_app->hal_lib.ptr_jpeg, "jpeg_open");

    if (my_cam_app->hal_lib.get_num_of_cameras == NULL ||
        my_cam_app->hal_lib.mm_camera_open == NULL ||
        my_cam_app->hal_lib.jpeg_open == NULL) {
        CDBG_ERROR("%s Error loading HAL sym %s\n", __func__, dlerror());
        return -MM_CAMERA_E_GENERAL;
    }

    my_cam_app->num_cameras = my_cam_app->hal_lib.get_num_of_cameras();
    CDBG("%s: num_cameras = %d\n", __func__, my_cam_app->num_cameras);

    return MM_CAMERA_OK;
}

int mm_app_allocate_ion_memory(mm_camera_app_buf_t *buf, int ion_type)
{
    int rc = MM_CAMERA_OK;
    struct ion_handle_data handle_data;
    struct ion_allocation_data alloc;
    struct ion_fd_data ion_info_fd;
    int main_ion_fd = 0;
    void *data = NULL;

    main_ion_fd = open("/dev/ion", O_RDONLY);
    if (main_ion_fd <= 0) {
        CDBG_ERROR("Ion dev open failed %s\n", strerror(errno));
        goto ION_OPEN_FAILED;
    }

    memset(&alloc, 0, sizeof(alloc));
    alloc.len = buf->mem_info.size;
    /* to make it page size aligned */
    alloc.len = (alloc.len + 4095) & (~4095);
    alloc.align = 4096;
    alloc.flags = ION_FLAG_CACHED;
    alloc.heap_mask = ion_type;
    rc = ioctl(main_ion_fd, ION_IOC_ALLOC, &alloc);
    if (rc < 0) {
        CDBG_ERROR("ION allocation failed\n");
        goto ION_ALLOC_FAILED;
    }

    memset(&ion_info_fd, 0, sizeof(ion_info_fd));
    ion_info_fd.handle = alloc.handle;
    rc = ioctl(main_ion_fd, ION_IOC_SHARE, &ion_info_fd);
    if (rc < 0) {
        CDBG_ERROR("ION map failed %s\n", strerror(errno));
        goto ION_MAP_FAILED;
    }

    data = mmap(NULL,
                alloc.len,
                PROT_READ  | PROT_WRITE,
                MAP_SHARED,
                ion_info_fd.fd,
                0);

    if (data == MAP_FAILED) {
        CDBG_ERROR("ION_MMAP_FAILED: %s (%d)\n", strerror(errno), errno);
        goto ION_MAP_FAILED;
    }
    buf->mem_info.main_ion_fd = main_ion_fd;
    buf->mem_info.fd = ion_info_fd.fd;
    buf->mem_info.handle = ion_info_fd.handle;
    buf->mem_info.size = alloc.len;
    buf->mem_info.data = data;
    return MM_CAMERA_OK;

ION_MAP_FAILED:
    memset(&handle_data, 0, sizeof(handle_data));
    handle_data.handle = ion_info_fd.handle;
    ioctl(main_ion_fd, ION_IOC_FREE, &handle_data);
ION_ALLOC_FAILED:
    close(main_ion_fd);
ION_OPEN_FAILED:
    return -MM_CAMERA_E_GENERAL;
}

int mm_app_deallocate_ion_memory(mm_camera_app_buf_t *buf)
{
  struct ion_handle_data handle_data;
  int rc = 0;

  rc = munmap(buf->mem_info.data, buf->mem_info.size);

  if (buf->mem_info.fd > 0) {
      close(buf->mem_info.fd);
      buf->mem_info.fd = 0;
  }

  if (buf->mem_info.main_ion_fd > 0) {
      memset(&handle_data, 0, sizeof(handle_data));
      handle_data.handle = buf->mem_info.handle;
      ioctl(buf->mem_info.main_ion_fd, ION_IOC_FREE, &handle_data);
      close(buf->mem_info.main_ion_fd);
      buf->mem_info.main_ion_fd = 0;
  }
  return rc;
}

/* cmd = ION_IOC_CLEAN_CACHES, ION_IOC_INV_CACHES, ION_IOC_CLEAN_INV_CACHES */
int mm_app_cache_ops(mm_camera_app_meminfo_t *mem_info,
                     unsigned int cmd)
{
    struct ion_flush_data cache_inv_data;
    struct ion_custom_data custom_data;
    int ret = MM_CAMERA_OK;

#ifdef USE_ION
    if (NULL == mem_info) {
        CDBG_ERROR("%s: mem_info is NULL, return here", __func__);
        return -MM_CAMERA_E_GENERAL;
    }

    memset(&cache_inv_data, 0, sizeof(cache_inv_data));
    memset(&custom_data, 0, sizeof(custom_data));
    cache_inv_data.vaddr = mem_info->data;
    cache_inv_data.fd = mem_info->fd;
    cache_inv_data.handle = mem_info->handle;
    cache_inv_data.length = mem_info->size;
    custom_data.cmd = cmd;
    custom_data.arg = (unsigned long)&cache_inv_data;

    CDBG("addr = %p, fd = %d, handle = %p length = %d, ION Fd = %d",
         cache_inv_data.vaddr, cache_inv_data.fd,
         cache_inv_data.handle, cache_inv_data.length,
         mem_info->main_ion_fd);
    if(mem_info->main_ion_fd > 0) {
        if(ioctl(mem_info->main_ion_fd, ION_IOC_CUSTOM, &custom_data) < 0) {
            ALOGE("%s: Cache Invalidate failed\n", __func__);
            ret = -MM_CAMERA_E_GENERAL;
        }
    }
#endif

    return ret;
}

void mm_app_dump_frame(mm_camera_buf_def_t *frame,
                       char *name,
                       char *ext,
                       int frame_idx)
{
    char file_name[64];
    int file_fd;
    int i;
    int offset = 0;
    if ( frame != NULL) {
        snprintf(file_name, sizeof(file_name), "/data/test/%s_%04d.%s", name, frame_idx, ext);
        file_fd = open(file_name, O_RDWR | O_CREAT, 0777);
        if (file_fd < 0) {
            CDBG_ERROR("%s: cannot open file %s \n", __func__, file_name);
        } else {
            for (i = 0; i < frame->num_planes; i++) {
                CDBG("%s: saving file from address: 0x%x, data offset: %d, length: %d \n", __func__,
                          (uint32_t )frame->buffer, frame->planes[i].data_offset, frame->planes[i].length);
                write(file_fd,
                      (uint8_t *)frame->buffer + offset,
                      frame->planes[i].length);
                offset +=  frame->planes[i].length;
            }

            close(file_fd);
            CDBG("dump %s", file_name);
        }
    }
}

void mm_app_dump_jpeg_frame(const void * data, uint32_t size, char* name, char* ext, int index)
{
    char buf[32];
    int file_fd;
    if ( data != NULL) {
        snprintf(buf, sizeof(buf), "/data/test/%s_%d.%s", name, index, ext);
        CDBG("%s: %s size =%d, jobId=%d", __func__, buf, size, index);
        file_fd = open(buf, O_RDWR | O_CREAT, 0777);
        write(file_fd, data, size);
        close(file_fd);
    }
}

int mm_app_alloc_bufs(mm_camera_app_buf_t* app_bufs,
                      cam_frame_len_offset_t *frame_offset_info,
                      uint8_t num_bufs,
                      uint8_t is_streambuf,
                      size_t multipleOf)
{
    int i, j;
    int ion_type = 0x1 << CAMERA_ION_FALLBACK_HEAP_ID;

    if (is_streambuf) {
        ion_type |= 0x1 << CAMERA_ION_HEAP_ID;
    }

    for (i = 0; i < num_bufs ; i++) {
        if ( 0 < multipleOf ) {
            size_t m = frame_offset_info->frame_len / multipleOf;
            if ( ( frame_offset_info->frame_len % multipleOf ) != 0 ) {
                m++;
            }
            app_bufs[i].mem_info.size = m * multipleOf;
        } else {
            app_bufs[i].mem_info.size = frame_offset_info->frame_len;
        }
        mm_app_allocate_ion_memory(&app_bufs[i], ion_type);

        app_bufs[i].buf.buf_idx = i;
        app_bufs[i].buf.num_planes = frame_offset_info->num_planes;
        app_bufs[i].buf.fd = app_bufs[i].mem_info.fd;
        app_bufs[i].buf.frame_len = app_bufs[i].mem_info.size;
        app_bufs[i].buf.buffer = app_bufs[i].mem_info.data;
        app_bufs[i].buf.mem_info = (void *)&app_bufs[i].mem_info;

        /* Plane 0 needs to be set seperately. Set other planes
             * in a loop. */
        app_bufs[i].buf.planes[0].length = frame_offset_info->mp[0].len;
        app_bufs[i].buf.planes[0].m.userptr = app_bufs[i].buf.fd;
        app_bufs[i].buf.planes[0].data_offset = frame_offset_info->mp[0].offset;
        app_bufs[i].buf.planes[0].reserved[0] = 0;
        for (j = 1; j < frame_offset_info->num_planes; j++) {
            app_bufs[i].buf.planes[j].length = frame_offset_info->mp[j].len;
            app_bufs[i].buf.planes[j].m.userptr = app_bufs[i].buf.fd;
            app_bufs[i].buf.planes[j].data_offset = frame_offset_info->mp[j].offset;
            app_bufs[i].buf.planes[j].reserved[0] =
                app_bufs[i].buf.planes[j-1].reserved[0] +
                app_bufs[i].buf.planes[j-1].length;
        }
    }
    CDBG("%s: X", __func__);
    return MM_CAMERA_OK;
}

int mm_app_release_bufs(uint8_t num_bufs,
                        mm_camera_app_buf_t* app_bufs)
{
    int i, rc = MM_CAMERA_OK;

    CDBG("%s: E", __func__);

    for (i = 0; i < num_bufs; i++) {
        rc = mm_app_deallocate_ion_memory(&app_bufs[i]);
    }
    memset(app_bufs, 0, num_bufs * sizeof(mm_camera_app_buf_t));
    CDBG("%s: X", __func__);
    return rc;
}

int mm_app_stream_initbuf(cam_frame_len_offset_t *frame_offset_info,
                          uint8_t *num_bufs,
                          uint8_t **initial_reg_flag,
                          mm_camera_buf_def_t **bufs,
                          mm_camera_map_unmap_ops_tbl_t *ops_tbl,
                          void *user_data)
{
    mm_camera_stream_t *stream = (mm_camera_stream_t *)user_data;
    mm_camera_buf_def_t *pBufs = NULL;
    uint8_t *reg_flags = NULL;
    int i, rc;

    stream->offset = *frame_offset_info;

    CDBG("%s: alloc buf for stream_id %d, len=%d, num planes: %d, offset: %d",
         __func__,
         stream->s_id,
         frame_offset_info->frame_len,
         frame_offset_info->num_planes,
         frame_offset_info->mp[1].offset);

    pBufs = (mm_camera_buf_def_t *)malloc(sizeof(mm_camera_buf_def_t) * stream->num_of_bufs);
    reg_flags = (uint8_t *)malloc(sizeof(uint8_t) * stream->num_of_bufs);
    if (pBufs == NULL || reg_flags == NULL) {
        CDBG_ERROR("%s: No mem for bufs", __func__);
        if (pBufs != NULL) {
            free(pBufs);
        }
        if (reg_flags != NULL) {
            free(reg_flags);
        }
        return -1;
    }

    rc = mm_app_alloc_bufs(&stream->s_bufs[0],
                           frame_offset_info,
                           stream->num_of_bufs,
                           1,
                           stream->multipleOf);

    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: mm_stream_alloc_bufs err = %d", __func__, rc);
        free(pBufs);
        free(reg_flags);
        return rc;
    }

    for (i = 0; i < stream->num_of_bufs; i++) {
        /* mapping stream bufs first */
        pBufs[i] = stream->s_bufs[i].buf;
        reg_flags[i] = 1;
        rc = ops_tbl->map_ops(pBufs[i].buf_idx,
                              -1,
                              pBufs[i].fd,
                              pBufs[i].frame_len,
                              ops_tbl->userdata);
        if (rc != MM_CAMERA_OK) {
            CDBG_ERROR("%s: mapping buf[%d] err = %d", __func__, i, rc);
            break;
        }
    }

    if (rc != MM_CAMERA_OK) {
        int j;
        for (j=0; j>i; j++) {
            ops_tbl->unmap_ops(pBufs[j].buf_idx, -1, ops_tbl->userdata);
        }
        mm_app_release_bufs(stream->num_of_bufs, &stream->s_bufs[0]);
        free(pBufs);
        free(reg_flags);
        return rc;
    }

    *num_bufs = stream->num_of_bufs;
    *bufs = pBufs;
    *initial_reg_flag = reg_flags;

    CDBG("%s: X",__func__);
    return rc;
}

int32_t mm_app_stream_deinitbuf(mm_camera_map_unmap_ops_tbl_t *ops_tbl,
                                void *user_data)
{
    mm_camera_stream_t *stream = (mm_camera_stream_t *)user_data;
    int i;

    for (i = 0; i < stream->num_of_bufs ; i++) {
        /* mapping stream bufs first */
        ops_tbl->unmap_ops(stream->s_bufs[i].buf.buf_idx, -1, ops_tbl->userdata);
    }

    mm_app_release_bufs(stream->num_of_bufs, &stream->s_bufs[0]);

    CDBG("%s: X",__func__);
    return 0;
}

int32_t mm_app_stream_clean_invalidate_buf(int index, void *user_data)
{
    mm_camera_stream_t *stream = (mm_camera_stream_t *)user_data;
    return mm_app_cache_ops(&stream->s_bufs[index].mem_info,
      ION_IOC_CLEAN_INV_CACHES);
}

int32_t mm_app_stream_invalidate_buf(int index, void *user_data)
{
    mm_camera_stream_t *stream = (mm_camera_stream_t *)user_data;
    return mm_app_cache_ops(&stream->s_bufs[index].mem_info, ION_IOC_INV_CACHES);
}

static void notify_evt_cb(uint32_t camera_handle,
                          mm_camera_event_t *evt,
                          void *user_data)
{
    mm_camera_test_obj_t *test_obj =
        (mm_camera_test_obj_t *)user_data;
    if (test_obj == NULL || test_obj->cam->camera_handle != camera_handle) {
        CDBG_ERROR("%s: Not a valid test obj", __func__);
        return;
    }

    CDBG("%s:E evt = %d", __func__, evt->server_event_type);
    switch (evt->server_event_type) {
       case CAM_EVENT_TYPE_AUTO_FOCUS_DONE:
           CDBG("%s: rcvd auto focus done evt", __func__);
           break;
       case CAM_EVENT_TYPE_ZOOM_DONE:
           CDBG("%s: rcvd zoom done evt", __func__);
           break;
       default:
           break;
    }

    CDBG("%s:X", __func__);
}

int mm_app_open(mm_camera_app_t *cam_app,
                uint8_t cam_id,
                mm_camera_test_obj_t *test_obj)
{
    int32_t rc;
    cam_frame_len_offset_t offset_info;

    CDBG("%s:BEGIN\n", __func__);

    test_obj->cam = cam_app->hal_lib.mm_camera_open(cam_id);
    if(test_obj->cam == NULL) {
        CDBG_ERROR("%s:dev open error\n", __func__);
        return -MM_CAMERA_E_GENERAL;
    }

    CDBG("Open Camera id = %d handle = %d", cam_id, test_obj->cam->camera_handle);

    /* alloc ion mem for capability buf */
    memset(&offset_info, 0, sizeof(offset_info));
    offset_info.frame_len = sizeof(cam_capability_t);

    rc = mm_app_alloc_bufs(&test_obj->cap_buf,
                           &offset_info,
                           1,
                           0,
                           0);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s:alloc buf for capability error\n", __func__);
        goto error_after_cam_open;
    }

    /* mapping capability buf */
    rc = test_obj->cam->ops->map_buf(test_obj->cam->camera_handle,
                                     CAM_MAPPING_BUF_TYPE_CAPABILITY,
                                     test_obj->cap_buf.mem_info.fd,
                                     test_obj->cap_buf.mem_info.size);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s:map for capability error\n", __func__);
        goto error_after_cap_buf_alloc;
    }

    /* alloc ion mem for getparm buf */
    memset(&offset_info, 0, sizeof(offset_info));
    offset_info.frame_len = sizeof(parm_buffer_t);
    rc = mm_app_alloc_bufs(&test_obj->parm_buf,
                           &offset_info,
                           1,
                           0,
                           0);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s:alloc buf for getparm_buf error\n", __func__);
        goto error_after_cap_buf_map;
    }

    /* mapping getparm buf */
    rc = test_obj->cam->ops->map_buf(test_obj->cam->camera_handle,
                                     CAM_MAPPING_BUF_TYPE_PARM_BUF,
                                     test_obj->parm_buf.mem_info.fd,
                                     test_obj->parm_buf.mem_info.size);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s:map getparm_buf error\n", __func__);
        goto error_after_getparm_buf_alloc;
    }

    rc = test_obj->cam->ops->register_event_notify(test_obj->cam->camera_handle,
                                                   notify_evt_cb,
                                                   test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: failed register_event_notify", __func__);
        rc = -MM_CAMERA_E_GENERAL;
        goto error_after_getparm_buf_map;
    }

    rc = test_obj->cam->ops->query_capability(test_obj->cam->camera_handle);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: failed query_capability", __func__);
        rc = -MM_CAMERA_E_GENERAL;
        goto error_after_getparm_buf_map;
    }

    memset(&test_obj->jpeg_ops, 0, sizeof(mm_jpeg_ops_t));
    test_obj->jpeg_hdl = cam_app->hal_lib.jpeg_open(&test_obj->jpeg_ops);
    if (test_obj->jpeg_hdl == 0) {
        CDBG_ERROR("%s: jpeg lib open err", __func__);
        rc = -MM_CAMERA_E_GENERAL;
        goto error_after_getparm_buf_map;
    }

    return rc;

error_after_getparm_buf_map:
    test_obj->cam->ops->unmap_buf(test_obj->cam->camera_handle,
                                  CAM_MAPPING_BUF_TYPE_PARM_BUF);
error_after_getparm_buf_alloc:
    mm_app_release_bufs(1, &test_obj->parm_buf);
error_after_cap_buf_map:
    test_obj->cam->ops->unmap_buf(test_obj->cam->camera_handle,
                                  CAM_MAPPING_BUF_TYPE_CAPABILITY);
error_after_cap_buf_alloc:
    mm_app_release_bufs(1, &test_obj->cap_buf);
error_after_cam_open:
    test_obj->cam->ops->close_camera(test_obj->cam->camera_handle);
    test_obj->cam = NULL;
    return rc;
}

int mm_app_close(mm_camera_test_obj_t *test_obj)
{
    uint32_t rc = MM_CAMERA_OK;

    if (test_obj == NULL || test_obj->cam ==NULL) {
        CDBG_ERROR("%s: cam not opened", __func__);
        return -MM_CAMERA_E_GENERAL;
    }

    /* unmap capability buf */
    rc = test_obj->cam->ops->unmap_buf(test_obj->cam->camera_handle,
                                       CAM_MAPPING_BUF_TYPE_CAPABILITY);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: unmap capability buf failed, rc=%d", __func__, rc);
    }

    /* unmap parm buf */
    rc = test_obj->cam->ops->unmap_buf(test_obj->cam->camera_handle,
                                       CAM_MAPPING_BUF_TYPE_PARM_BUF);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: unmap setparm buf failed, rc=%d", __func__, rc);
    }

    rc = test_obj->cam->ops->close_camera(test_obj->cam->camera_handle);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: close camera failed, rc=%d", __func__, rc);
    }
    test_obj->cam = NULL;

    /* close jpeg client */
    if (test_obj->jpeg_hdl && test_obj->jpeg_ops.close) {
        rc = test_obj->jpeg_ops.close(test_obj->jpeg_hdl);
        test_obj->jpeg_hdl = 0;
        if (rc != MM_CAMERA_OK) {
            CDBG_ERROR("%s: close jpeg failed, rc=%d", __func__, rc);
        }
    }

    /* dealloc capability buf */
    rc = mm_app_release_bufs(1, &test_obj->cap_buf);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: release capability buf failed, rc=%d", __func__, rc);
    }

    /* dealloc parm buf */
    rc = mm_app_release_bufs(1, &test_obj->parm_buf);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: release setparm buf failed, rc=%d", __func__, rc);
    }

    return MM_CAMERA_OK;
}

mm_camera_channel_t * mm_app_add_channel(mm_camera_test_obj_t *test_obj,
                                         mm_camera_channel_type_t ch_type,
                                         mm_camera_channel_attr_t *attr,
                                         mm_camera_buf_notify_t channel_cb,
                                         void *userdata)
{
    uint32_t ch_id = 0;
    mm_camera_channel_t *channel = NULL;

    ch_id = test_obj->cam->ops->add_channel(test_obj->cam->camera_handle,
                                            attr,
                                            channel_cb,
                                            userdata);
    if (ch_id == 0) {
        CDBG_ERROR("%s: add channel failed", __func__);
        return NULL;
    }
    channel = &test_obj->channels[ch_type];
    channel->ch_id = ch_id;
    return channel;
}

int mm_app_del_channel(mm_camera_test_obj_t *test_obj,
                       mm_camera_channel_t *channel)
{
    test_obj->cam->ops->delete_channel(test_obj->cam->camera_handle,
                                       channel->ch_id);
    memset(channel, 0, sizeof(mm_camera_channel_t));
    return MM_CAMERA_OK;
}

mm_camera_stream_t * mm_app_add_stream(mm_camera_test_obj_t *test_obj,
                                       mm_camera_channel_t *channel)
{
    mm_camera_stream_t *stream = NULL;
    int rc = MM_CAMERA_OK;
    cam_frame_len_offset_t offset_info;

    stream = &(channel->streams[channel->num_streams++]);
    stream->s_id = test_obj->cam->ops->add_stream(test_obj->cam->camera_handle,
                                                  channel->ch_id);
    if (stream->s_id == 0) {
        CDBG_ERROR("%s: add stream failed", __func__);
        return NULL;
    }

    stream->multipleOf = test_obj->slice_size;

    /* alloc ion mem for stream_info buf */
    memset(&offset_info, 0, sizeof(offset_info));
    offset_info.frame_len = sizeof(cam_stream_info_t);

    rc = mm_app_alloc_bufs(&stream->s_info_buf,
                           &offset_info,
                           1,
                           0,
                           0);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s:alloc buf for stream_info error\n", __func__);
        test_obj->cam->ops->delete_stream(test_obj->cam->camera_handle,
                                          channel->ch_id,
                                          stream->s_id);
        stream->s_id = 0;
        return NULL;
    }

    /* mapping streaminfo buf */
    rc = test_obj->cam->ops->map_stream_buf(test_obj->cam->camera_handle,
                                            channel->ch_id,
                                            stream->s_id,
                                            CAM_MAPPING_BUF_TYPE_STREAM_INFO,
                                            0,
                                            -1,
                                            stream->s_info_buf.mem_info.fd,
                                            stream->s_info_buf.mem_info.size);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s:map setparm_buf error\n", __func__);
        mm_app_deallocate_ion_memory(&stream->s_info_buf);
        test_obj->cam->ops->delete_stream(test_obj->cam->camera_handle,
                                          channel->ch_id,
                                          stream->s_id);
        stream->s_id = 0;
        return NULL;
    }

    return stream;
}

int mm_app_del_stream(mm_camera_test_obj_t *test_obj,
                      mm_camera_channel_t *channel,
                      mm_camera_stream_t *stream)
{
    test_obj->cam->ops->unmap_stream_buf(test_obj->cam->camera_handle,
                                         channel->ch_id,
                                         stream->s_id,
                                         CAM_MAPPING_BUF_TYPE_STREAM_INFO,
                                         0,
                                         -1);
    mm_app_deallocate_ion_memory(&stream->s_info_buf);
    test_obj->cam->ops->delete_stream(test_obj->cam->camera_handle,
                                      channel->ch_id,
                                      stream->s_id);
    memset(stream, 0, sizeof(mm_camera_stream_t));
    return MM_CAMERA_OK;
}

mm_camera_channel_t *mm_app_get_channel_by_type(mm_camera_test_obj_t *test_obj,
                                                mm_camera_channel_type_t ch_type)
{
    return &test_obj->channels[ch_type];
}

int mm_app_config_stream(mm_camera_test_obj_t *test_obj,
                         mm_camera_channel_t *channel,
                         mm_camera_stream_t *stream,
                         mm_camera_stream_config_t *config)
{
    return test_obj->cam->ops->config_stream(test_obj->cam->camera_handle,
                                             channel->ch_id,
                                             stream->s_id,
                                             config);
}

int mm_app_start_channel(mm_camera_test_obj_t *test_obj,
                         mm_camera_channel_t *channel)
{
    return test_obj->cam->ops->start_channel(test_obj->cam->camera_handle,
                                             channel->ch_id);
}

int mm_app_stop_channel(mm_camera_test_obj_t *test_obj,
                        mm_camera_channel_t *channel)
{
    return test_obj->cam->ops->stop_channel(test_obj->cam->camera_handle,
                                            channel->ch_id);
}

int initBatchUpdate(mm_camera_test_obj_t *test_obj)
{
    parm_buffer_t *parm_buf = ( parm_buffer_t * ) test_obj->parm_buf.mem_info.data;
    memset(parm_buf, 0, sizeof(parm_buffer_t));
    parm_buf->first_flagged_entry = CAM_INTF_PARM_MAX;
    return MM_CAMERA_OK;
}

int AddSetParmEntryToBatch(mm_camera_test_obj_t *test_obj,
                           cam_intf_parm_type_t paramType,
                           uint32_t paramLength,
                           void *paramValue)
{
    int position = paramType;
    int current, next;

    parm_buffer_t *p_table = ( parm_buffer_t * ) test_obj->parm_buf.mem_info.data;
    /*************************************************************************
    *                 Code to take care of linking next flags                *
    *************************************************************************/
    current = GET_FIRST_PARAM_ID(p_table);
    if (position == current){
        //DO NOTHING
    } else if (position < current){
        SET_NEXT_PARAM_ID(position, p_table, current);
        SET_FIRST_PARAM_ID(p_table, position);
    } else {
        /* Search for the position in the linked list where we need to slot in*/
        while (position > GET_NEXT_PARAM_ID(current, p_table))
            current = GET_NEXT_PARAM_ID(current, p_table);

        /*If node already exists no need to alter linking*/
        if (position != GET_NEXT_PARAM_ID(current, p_table)) {
            next = GET_NEXT_PARAM_ID(current, p_table);
            SET_NEXT_PARAM_ID(current, p_table, position);
            SET_NEXT_PARAM_ID(position, p_table, next);
        }
    }

    /*************************************************************************
    *                   Copy contents into entry                             *
    *************************************************************************/

    if (paramLength > sizeof(parm_type_t)) {
        ALOGE("%s:Size of input larger than max entry size",__func__);
        return MM_CAMERA_E_GENERAL;
    }
    memcpy(POINTER_OF(paramType,p_table), paramValue, paramLength);
    return MM_CAMERA_OK;
}

int ReadSetParmEntryToBatch(mm_camera_test_obj_t *test_obj,
                           cam_intf_parm_type_t paramType,
                           uint32_t paramLength,
                           void *paramValue)
{
    parm_buffer_t *p_table = ( parm_buffer_t * ) test_obj->parm_buf.mem_info.data;
    memcpy(paramValue, POINTER_OF(paramType,p_table), paramLength);
    return MM_CAMERA_OK;
}

int commitSetBatch(mm_camera_test_obj_t *test_obj)
{
    int rc = MM_CAMERA_OK;
    parm_buffer_t *p_table = ( parm_buffer_t * ) test_obj->parm_buf.mem_info.data;
    if (p_table->first_flagged_entry < CAM_INTF_PARM_MAX) {
        rc = test_obj->cam->ops->set_parms(test_obj->cam->camera_handle, p_table);
    }
    return rc;
}

int setAecLock(mm_camera_test_obj_t *test_obj, int value)
{
    int rc = MM_CAMERA_OK;

    rc = initBatchUpdate(test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: Batch camera parameter update failed\n", __func__);
        goto ERROR;
    }

    printf("%s: Setting AECLock value %d \n", __func__, value);
    rc = AddSetParmEntryToBatch(test_obj,
                                CAM_INTF_PARM_AEC_LOCK,
                                sizeof(value),
                                &value);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: AEC Lock parameter not added to batch\n", __func__);
        goto ERROR;
    }

    rc = commitSetBatch(test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: Batch parameters commit failed\n", __func__);
        goto ERROR;
    }

ERROR:
    return rc;
}

int setAwbLock(mm_camera_test_obj_t *test_obj, int value)
{
    int rc = MM_CAMERA_OK;

    rc = initBatchUpdate(test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: Batch camera parameter update failed\n", __func__);
        goto ERROR;
    }

    printf("%s: Setting AWB Lock value %d \n", __func__, value);
    rc = AddSetParmEntryToBatch(test_obj,
                                CAM_INTF_PARM_AWB_LOCK,
                                sizeof(value),
                                &value);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: AWB Lock parameter not added to batch\n", __func__);
        goto ERROR;
    }

    rc = commitSetBatch(test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: Batch parameters commit failed\n", __func__);
        goto ERROR;
    }

ERROR:
    return rc;
}

int setFocusMode(mm_camera_test_obj_t *test_obj, cam_focus_mode_type mode)
{
    int rc = MM_CAMERA_OK;

    rc = initBatchUpdate(test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: Batch camera parameter update failed\n", __func__);
        goto ERROR;
    }

    uint32_t value = mode;

    rc = AddSetParmEntryToBatch(test_obj,
                                CAM_INTF_PARM_FOCUS_MODE,
                                sizeof(value),
                                &value);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: Focus mode parameter not added to batch\n", __func__);
        goto ERROR;
    }

    rc = commitSetBatch(test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: Batch parameters commit failed\n", __func__);
        goto ERROR;
    }

ERROR:
    return rc;
}

/** tuneserver_capture
 *    @lib_handle: the camera handle object
 *
 *  makes JPEG capture
 *
 *  Return: >=0 on success, -1 on failure.
 **/
int tuneserver_capture(mm_camera_lib_handle *lib_handle)
{
  int rc = 0;

  printf("Take jpeg snapshot\n");

  if ( lib_handle->stream_running ) {
      lib_handle->test_obj.encodeJpeg = 1;
      mm_camera_app_wait();
  }

  return rc;
}

int main(int argc, char **argv)
{
    int c;
    int rc;
    int run_tc = 0;
    int run_dual_tc = 0;
    mm_camera_app_t my_cam_app;

    CDBG("\nCamera Test Application\n");

    while ((c = getopt(argc, argv, "tdh")) != -1) {
        switch (c) {
           case 't':
               run_tc = 1;
               break;
           case 'd':
               run_dual_tc = 1;
               break;
           case 'h':
           default:
               printf("usage: %s [-t] [-d] \n", argv[0]);
               printf("-t:   Unit test        \n");
               printf("-d:   Dual camera test \n");
               return 0;
        }
    }

    memset(&my_cam_app, 0, sizeof(mm_camera_app_t));
    if((mm_app_load_hal(&my_cam_app) != MM_CAMERA_OK)) {
        CDBG_ERROR("%s:mm_app_init err\n", __func__);
        return -1;
    }

    if(run_tc) {
        printf("\tRunning unit test engine only\n");
        rc = mm_app_unit_test_entry(&my_cam_app);
        printf("\tUnit test engine. EXIT(%d)!!!\n", rc);
        return rc;
    }
#if 0
    if(run_dual_tc) {
        printf("\tRunning Dual camera test engine only\n");
        rc = mm_app_dual_test_entry(&my_cam_app);
        printf("\t Dual camera engine. EXIT(%d)!!!\n", rc);
        exit(rc);
    }
#endif
    /* Clean up and exit. */
    CDBG("Exiting test app\n");
    return 0;
}

int mm_camera_lib_open(mm_camera_lib_handle *handle, int cam_id)
{
    int rc = MM_CAMERA_OK;

    if ( NULL == handle ) {
        CDBG_ERROR(" %s : Invalid handle", __func__);
        rc = MM_CAMERA_E_INVALID_INPUT;
        goto EXIT;
    }

    memset(handle, 0, sizeof(mm_camera_lib_handle));
    rc = mm_app_load_hal(&handle->app_ctx);
    if( MM_CAMERA_OK != rc ) {
        CDBG_ERROR("%s:mm_app_init err\n", __func__);
        goto EXIT;
    }

    handle->test_obj.buffer_width = DEFAULT_PREVIEW_WIDTH;
    handle->test_obj.buffer_height = DEFAULT_PREVIEW_HEIGHT;
    handle->test_obj.buffer_format = DEFAULT_SNAPSHOT_FORMAT;
    handle->current_params.stream_width = DEFAULT_SNAPSHOT_WIDTH;
    handle->current_params.stream_height = DEFAULT_SNAPSHOT_HEIGHT;
    rc = mm_app_open(&handle->app_ctx, cam_id, &handle->test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s:mm_app_open() cam_idx=%d, err=%d\n",
                   __func__, cam_id, rc);
        goto EXIT;
    }

    rc = mm_app_initialize_fb(&handle->test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: mm_app_initialize_fb() cam_idx=%d, err=%d\n",
                   __func__, cam_id, rc);
        goto EXIT;
    }

EXIT:

    return rc;
}

int mm_camera_lib_start_stream(mm_camera_lib_handle *handle)
{
    int rc = MM_CAMERA_OK;

    if ( NULL == handle ) {
        CDBG_ERROR(" %s : Invalid handle", __func__);
        rc = MM_CAMERA_E_INVALID_INPUT;
        goto EXIT;
    }

    rc = mm_app_start_preview_zsl(&handle->test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: mm_app_start_preview_zsl() err=%d\n",
                   __func__, rc);
        goto EXIT;
    }

    handle->stream_running = 1;

EXIT:
    return rc;
}

int mm_camera_lib_stop_stream(mm_camera_lib_handle *handle)
{
    int rc = MM_CAMERA_OK;

    if ( NULL == handle ) {
        CDBG_ERROR(" %s : Invalid handle", __func__);
        rc = MM_CAMERA_E_INVALID_INPUT;
        goto EXIT;
    }

    rc = mm_app_stop_preview_zsl(&handle->test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s: mm_app_stop_preview_zsl() err=%d\n",
                   __func__, rc);
        goto EXIT;
    }

    handle->stream_running = 0;

EXIT:
    return rc;
}

int mm_camera_lib_get_caps(mm_camera_lib_handle *handle,
                           cam_capability_t *caps)
{
    int rc = MM_CAMERA_OK;

    if ( NULL == handle ) {
        CDBG_ERROR(" %s : Invalid handle", __func__);
        rc = MM_CAMERA_E_INVALID_INPUT;
        goto EXIT;
    }

    if ( NULL == caps ) {
        CDBG_ERROR(" %s : Invalid capabilities structure", __func__);
        rc = MM_CAMERA_E_INVALID_INPUT;
        goto EXIT;
    }

    *caps = *( (cam_capability_t *) handle->test_obj.cap_buf.mem_info.data );

EXIT:

    return rc;
}


int mm_camera_lib_send_command(mm_camera_lib_handle *handle,
                               mm_camera_lib_commands cmd,
                               void *in_data)
{
    int width, height;
    int rc = MM_CAMERA_OK;

    if ( NULL == handle ) {
        CDBG_ERROR(" %s : Invalid handle", __func__);
        rc = MM_CAMERA_E_INVALID_INPUT;
        goto EXIT;
    }

    if ( 0 == handle->stream_running ) {
        CDBG_ERROR(" %s : Streaming is not enabled!", __func__);
        rc = MM_CAMERA_E_INVALID_OPERATION;
        goto EXIT;
    }

    switch(cmd) {
        case MM_CAMERA_LIB_RAW_CAPTURE:
            rc = mm_app_stop_preview_zsl(&handle->test_obj);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s: mm_app_stop_preview_zsl() err=%d\n",
                           __func__, rc);
                goto EXIT;
            }
            mm_app_close_fb(&handle->test_obj);

            width = handle->test_obj.buffer_width;
            height = handle->test_obj.buffer_height;
            handle->test_obj.buffer_width = DEFAULT_RAW_WIDTH;
            handle->test_obj.buffer_height = DEFAULT_RAW_HEIGHT;
            handle->test_obj.buffer_format = DEFAULT_RAW_FORMAT;
            rc = mm_app_initialize_fb(&handle->test_obj);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s: mm_app_initialize_fb() err=%d\n",
                           __func__, rc);
                goto EXIT;
            }

            rc = mm_app_start_capture_raw(&handle->test_obj, 1);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s: mm_app_start_capture() err=%d\n",
                           __func__, rc);
                goto EXIT;
            }

            mm_camera_app_wait();

            rc = mm_app_stop_capture_raw(&handle->test_obj);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s: mm_app_stop_capture() err=%d\n",
                           __func__, rc);
                goto EXIT;
            }
            mm_app_close_fb(&handle->test_obj);

            handle->test_obj.buffer_width = width;
            handle->test_obj.buffer_height = height;
            handle->test_obj.buffer_format = DEFAULT_SNAPSHOT_FORMAT;
            rc = mm_app_initialize_fb(&handle->test_obj);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s: mm_app_initialize_fb() err=%d\n",
                           __func__, rc);
                goto EXIT;
            }

            rc = mm_app_start_preview_zsl(&handle->test_obj);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s: mm_app_start_preview_zsl() err=%d\n",
                           __func__, rc);
                goto EXIT;
            }
            break;

        case MM_CAMERA_LIB_JPEG_CAPTURE:
        {
             tuneserver_capture(handle);
             break;
        }
        case MM_CAMERA_LIB_SET_FOCUS_MODE: {
            cam_focus_mode_type mode = *((cam_focus_mode_type *)in_data);
            rc = setFocusMode(&handle->test_obj, mode);
            if (rc != MM_CAMERA_OK) {
              CDBG_ERROR("%s:autofocus error\n", __func__);
              goto EXIT;
            }
            break;
        }

        case MM_CAMERA_LIB_DO_AF:
            rc = handle->test_obj.cam->ops->do_auto_focus(handle->test_obj.cam->camera_handle);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s:autofocus error\n", __func__);
                goto EXIT;
            }

            break;

        case MM_CAMERA_LIB_CANCEL_AF:
            rc = handle->test_obj.cam->ops->cancel_auto_focus(handle->test_obj.cam->camera_handle);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s:autofocus error\n", __func__);
                goto EXIT;
            }

            break;

        case MM_CAMERA_LIB_LOCK_AWB:
            rc = setAwbLock(&handle->test_obj, 1);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s: AWB locking failed\n", __func__);
                goto EXIT;
            }

            printf("AWB lock active\n");
            break;

        case MM_CAMERA_LIB_UNLOCK_AWB:
            rc = setAwbLock(&handle->test_obj, 0);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s: AE unlocking failed\n", __func__);
                goto EXIT;
            }

            printf("AWB lock disabled\n");
            break;

        case MM_CAMERA_LIB_LOCK_AE:
            rc = setAecLock(&handle->test_obj, 1);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s: AE locking failed\n", __func__);
                goto EXIT;
            }

            printf("AE lock active\n");
            break;

        case MM_CAMERA_LIB_UNLOCK_AE:
            rc = setAecLock(&handle->test_obj, 0);
            if (rc != MM_CAMERA_OK) {
                CDBG_ERROR("%s: AE unlocking failed\n", __func__);
                goto EXIT;
            }

            printf("AE lock disabled\n");
            break;

        case MM_CAMERA_LIB_NO_ACTION:
        default:
            break;
    };

EXIT:

    return rc;
}

int mm_camera_lib_close(mm_camera_lib_handle *handle)
{
    int rc = MM_CAMERA_OK;

    if ( NULL == handle ) {
        CDBG_ERROR(" %s : Invalid handle", __func__);
        rc = MM_CAMERA_E_INVALID_INPUT;
        goto EXIT;
    }

    rc = mm_app_close_fb(&handle->test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s:mm_app_close_fb() err=%d\n",
                   __func__, rc);
        goto EXIT;
    }

    rc = mm_app_close(&handle->test_obj);
    if (rc != MM_CAMERA_OK) {
        CDBG_ERROR("%s:mm_app_close() err=%d\n",
                   __func__, rc);
        goto EXIT;
    }

EXIT:
    return rc;
}
