/*
 * Copyright (C) 2014 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#include <errno.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <dlfcn.h>
#include <cutils/uevent.h>
#include <errno.h>
#include <sys/poll.h>
#include <pthread.h>
#include <linux/netlink.h>
#include <stdlib.h>
#include <stdbool.h>

#define LOG_TAG "PowerHAL"
#include <utils/Log.h>

#include <hardware/hardware.h>
#include <hardware/power.h>

#include "power-set.h"
#include "utils.h"
#include "metadata-defs.h"
#include "hint-data.h"
#include "performance.h"
#include "power-common.h"

#define STATE_ON "state=1"
#define STATE_OFF "state=0"
#define STATE_HDR_ON "state=2"
#define STATE_HDR_OFF "state=3"

#define MAX_LENGTH         50

#define UEVENT_MSG_LEN 1024
#define TOTAL_CPUS 4
#define RETRY_TIME_CHANGING_FREQ 20
#define SLEEP_USEC_BETWN_RETRY 200
#define LOW_POWER_MAX_FREQ "729600"
#define LOW_POWER_MIN_FREQ "300000"
#define NORMAL_MAX_FREQ "2265600"
#define UEVENT_STRING "online@/devices/system/cpu/"

/* RPM runs at 19.2Mhz. Divide by 19200 for msec */
/*
 * TODO: Control those values
 */
#define RPM_CLK 19200
#define USINSEC 1000000L
#define NSINUS 1000L

static int client_sockfd;
static struct sockaddr_un client_addr;

static struct pollfd pfd;
static char *cpu_path_min[] = {
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu1/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu2/cpufreq/scaling_min_freq",
    "/sys/devices/system/cpu/cpu3/cpufreq/scaling_min_freq",
};
static char *cpu_path_max[] = {
    "/sys/devices/system/cpu/cpu0/cpufreq/scaling_max_freq",
    "/sys/devices/system/cpu/cpu1/cpufreq/scaling_max_freq",
    "/sys/devices/system/cpu/cpu2/cpufreq/scaling_max_freq",
    "/sys/devices/system/cpu/cpu3/cpufreq/scaling_max_freq",
};
static bool freq_set[TOTAL_CPUS];
static bool low_power_mode = false;
static pthread_mutex_t low_power_mode_lock = PTHREAD_MUTEX_INITIALIZER;

bool display_boost = false;
static int saved_interactive_mode = -1;

//interaction boost global variables
static pthread_mutex_t s_interaction_lock = PTHREAD_MUTEX_INITIALIZER;
static struct timespec s_previous_boost_timespec;


void set_feature(__attribute__((unused))struct power_module *module, feature_t feature, int state)
{
#ifdef TAP_TO_WAKE_NODE
    if (feature == POWER_FEATURE_DOUBLE_TAP_TO_WAKE) {
            ALOGI("Double tap to wake is %s.", state ? "enabled" : "disabled");
            sysfs_write(TAP_TO_WAKE_NODE, state ? "1" : "0");
        return;
    }
#endif
}

long long calc_timespan_us(struct timespec start, struct timespec end) {
    long long diff_in_us = 0;
    diff_in_us += (end.tv_sec - start.tv_sec) * USINSEC;
    diff_in_us += (end.tv_nsec - start.tv_nsec) / NSINUS;
    return diff_in_us;
}

int __attribute__ ((weak)) set_interactive_override(__attribute__((unused)) struct power_module *module, __attribute__((unused)) int on)
{   
    return HINT_NONE;
}

static void power_set_interactive(struct power_module *module, int on)
{
    char governor[80];
    int rc = 255;

    if (set_interactive_override(module, on) == HINT_HANDLED) {
        return;
    }

    ALOGI("Got set_interactive hint");

    if (get_scaling_governor(governor, sizeof(governor)) == -1) {
        ALOGE("Can't obtain scaling governor.");

        return;
    }

    if (!on) {
		rc = set_interactive_off(governor, saved_interactive_mode); 
    } else {
		rc = set_interactive_on(governor, saved_interactive_mode);
    }

    saved_interactive_mode = !!on;

    ALOGV("%s %s", __func__, (on ? "ON" : "OFF"));
	ALOGE("%s TODO", __func__);

}

static void process_video_encode_hint(void *metadata)
{
	ALOGE("%s TODO", __func__);
}

static void power_hint( __attribute__((unused)) struct power_module *module,
                      power_hint_t hint, __attribute__((unused)) void *data)
{
    int cpu, ret;

    switch (hint) {
        case POWER_HINT_INTERACTION:
            {
                ALOGV("POWER_HINT_INTERACTION");
			    int duration_hint = 0;

			    // little core freq bump for 1.5s
			    int resources[] = {0x20C};
			    int duration = 1500;
			    static int handle_little = 0;

			    // big core freq bump for 500ms
			    int resources_big[] = {0x2312, 0x1F08};
			    int duration_big = 500;
			    static int handle_big = 0;

			    // sched_downmigrate lowered to 10 for 1s at most
			    // should be half of upmigrate
			    int resources_downmigrate[] = {0x4F00};
			    int duration_downmigrate = 1000;
			    static int handle_downmigrate = 0;

			    // sched_upmigrate lowered to at most 20 for 500ms
			    // set threshold based on elapsed time since last boost
			    int resources_upmigrate[] = {0x4E00};
			    int duration_upmigrate = 500;
			    static int handle_upmigrate = 0;

			    // set duration hint
			    if (data) {
					duration_hint = *((int*)data);
			    }

			    struct timespec cur_boost_timespec;
			    clock_gettime(CLOCK_MONOTONIC, &cur_boost_timespec);

			    pthread_mutex_lock(&s_interaction_lock);
			    long long elapsed_time = calc_timespan_us(s_previous_boost_timespec, cur_boost_timespec);

			    if (elapsed_time > 750000)
					elapsed_time = 750000;
			    // don't hint if it's been less than 250ms since last boost
			    // also detect if we're doing anything resembling a fling
			    // support additional boosting in case of flings
			    else if (elapsed_time < 250000 && duration_hint <= 750) {
					pthread_mutex_unlock(&s_interaction_lock);
					return;
			    }

			    s_previous_boost_timespec = cur_boost_timespec;
			    pthread_mutex_unlock(&s_interaction_lock);

			    // 95: default upmigrate for phone
			    // 20: upmigrate for sporadic touch
			    // 750ms: a completely arbitrary threshold for last touch
			    int upmigrate_value = 95 - (int)(75. * ((elapsed_time*elapsed_time) / (750000.*750000.)));

			    // keep sched_upmigrate high when flinging
			    if (duration_hint >= 750)
					upmigrate_value = 20;

			    resources_upmigrate[0] = resources_upmigrate[0] | upmigrate_value;
			    resources_downmigrate[0] = resources_downmigrate[0] | (upmigrate_value / 2);

			    // modify downmigrate duration based on interaction data hint
			    // 1000 <= duration_downmigrate <= 5000
			    // extend little core freq bump past downmigrate to soften downmigrates
			    if (duration_hint > 1000) {
					if (duration_hint < 5000) {
					    duration_downmigrate = duration_hint;
					    duration = duration_hint + 750;
					} else {
					    duration_downmigrate = 5000;
					    duration = 5750;
					}
			    }

			    handle_little = interaction_with_handle(handle_little,duration, sizeof(resources)/sizeof(resources[0]), resources);
			    handle_big = interaction_with_handle(handle_big, duration_big, sizeof(resources_big)/sizeof(resources_big[0]), resources_big);
			    handle_downmigrate = interaction_with_handle(handle_downmigrate, duration_downmigrate, sizeof(resources_downmigrate)/sizeof(resources_downmigrate[0]), resources_downmigrate);
				handle_upmigrate = interaction_with_handle(handle_upmigrate, duration_upmigrate, sizeof(resources_upmigrate)/sizeof(resources_upmigrate[0]), resources_upmigrate);

            }
            break;

        case POWER_HINT_VIDEO_ENCODE:
            process_video_encode_hint(data);
            break;

        case POWER_HINT_LOW_POWER:
             pthread_mutex_lock(&low_power_mode_lock);
             if (data) {
                 low_power_mode = true;
                 for (cpu = 0; cpu < TOTAL_CPUS; cpu++) {
                     sysfs_write(cpu_path_min[cpu], LOW_POWER_MIN_FREQ);
                     ret = sysfs_write(cpu_path_max[cpu], LOW_POWER_MAX_FREQ);
                     if (!ret) {
                         freq_set[cpu] = true;
                     }
                 }
                 // reduces the refresh rate
                 system("service call SurfaceFlinger 1016");
             } else {
                 low_power_mode = false;
                 for (cpu = 0; cpu < TOTAL_CPUS; cpu++) {
                     ret = sysfs_write(cpu_path_max[cpu], NORMAL_MAX_FREQ);
                     if (!ret) {
                         freq_set[cpu] = false;
                     }
                 }
                 // restores the refresh rate
                 system("service call SurfaceFlinger 1017");
             }
             pthread_mutex_unlock(&low_power_mode_lock);
             break;
        case POWER_HINT_VSYNC:
            break;
        case POWER_HINT_CPU_BOOST:
             ALOGE("%s TODO: POWER_HINT_CPU_BOOST", __func__);
             break;
        case POWER_HINT_LAUNCH:
             ALOGE("%s TODO: POWER_HINT_LAUNCH", __func__);
             break;
        default:
			 ALOGE("%s TODO: hint id: %i", __func__, hint);
             break;
    }
}

static void power_init(__attribute__((unused)) struct power_module *module)
{
    ALOGI("%s", __func__);

    int fd;
    char buf[10] = {0};

    fd = open("/sys/devices/soc0/soc_id", O_RDONLY);
    if (fd >= 0) {
        if (read(fd, buf, sizeof(buf) - 1) == -1) {
            ALOGW("Unable to read soc_id");
        } else {
            int soc_id = atoi(buf);
            if (soc_id == 194 || (soc_id >= 208 && soc_id <= 218) || soc_id == 178) {
                display_boost = true;
				ALOGI("%s: Enabling display boost", __func__);
            } else {
                ALOGI("%s: Soc %d not in list, disabling display boost", __func__, soc_id);
            }
        }
        close(fd);
   }
}


static struct hw_module_methods_t power_module_methods = {
    .open = NULL,
};

struct power_module HAL_MODULE_INFO_SYM = {
    .common = {
        .tag = HARDWARE_MODULE_TAG,
        .module_api_version = POWER_MODULE_API_VERSION_0_2,
        .hal_api_version = HARDWARE_HAL_API_VERSION,
        .id = POWER_HARDWARE_MODULE_ID,
        .name = "Sony 8974 Power HAL",
        .author = "The Android Open Source Project",
        .methods = &power_module_methods,
    },

    .init = power_init,
    .setInteractive = power_set_interactive,
    .powerHint = power_hint,
    .setFeature = set_feature,
};

