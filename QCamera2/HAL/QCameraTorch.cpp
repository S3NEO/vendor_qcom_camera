/* Copyright (c) 2012-2013, The Linux Foundataion. All rights reserved.
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

#define LOG_TAG "QCameraTorch"

#include <utils/Errors.h>
#include "QCamera2HWI.h"
#include "QCameraStream.h"
#include <cutils/log.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <sys/types.h>

#define CAMERA_TORCH_MAX_BRIGHTNESS	255
#define CAMERA_TORCH_MIN_BRIGHTNESS	1
#define CAMERA_TORCH_OFF		0
#define CAMERA_TORCH_ON			CAMERA_TORCH_MAX_BRIGHTNESS

#define CAMERA_TORCH_PATH		"/sys/devices/leds-qpnp-e8a2ca00/leds/led:flash_torch/brightness"

namespace qcamera {

int32_t getMode()
{
    int fd, value;
    char buffer[2];

    fd = open(CAMERA_TORCH_PATH, O_RDONLY);

    if (fd >= 0) {
        read(fd, buffer, 3);
    }
    close(fd);

    value = atoi(buffer);

    if (value > 255 || value > 1) {
	ALOGE("%s: unknown torch mode", __func__);
	return -1;
    } else {
	return 0;
    }
}

int32_t get_mode()
{
    if (getMode() != 0) {
	ALOGE("%s: torch already in use", __func__);
	return -1;
    } else {
	return getMode();
    }
}

int32_t setMode(int value)
{
    int fd;
    static int already_warned;

    already_warned = 0;

    ALOGV("%s: path %s, value %d",__func__, path, value);
    fd = open(CAMERA_TORCH_PATH, O_RDWR);

    if (fd >= 0) {
        char buffer[20];
        int bytes = sprintf(buffer, "%d\n", value);
        int amt = write(fd, buffer, bytes);
        close(fd);
        return amt == -1 ? -errno : 0;
    } else {
        if (already_warned == 0) {
            ALOGE("%s: failed to open %s\n",__func__, CAMERA_TORCH_PATH);
            already_warned = 1;
        }
        return -errno;
    }
}

int32_t set_mode(const char* id, int value)
{
	ALOGE("%s: setting torch mode %d for camera id %s", __func__, value, id);
	if (getMode() != 0) {
		ALOGE("%s: torch already in use", __func__);
		return -1;
	} else {
		if (value == 1) {
			return setMode(CAMERA_TORCH_ON);
		} else if (value == 0) {
			return setMode(CAMERA_TORCH_OFF);
		} else {
			ALOGE("%s: unknown mode %d for camera id %s", __func__, value, id);
			return -1;
		}
	}
}

}; // namespace qcamera
