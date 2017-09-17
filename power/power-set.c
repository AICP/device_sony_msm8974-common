/*
 * Copyright (C) 2017 OmniRom
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

static int saved_dcvs_cpu0_slack_max = -1;
static int saved_dcvs_cpu0_slack_min = -1;
static int saved_mpdecision_slack_max = -1;
static int saved_mpdecision_slack_min = -1;
static int slack_node_rw_failed = 0;
static int display_hint_sent;


int set_interactive_off(char * governor, int saved_interactive_mode) {

		struct video_encode_metadata_t video_encode_metadata;
		char tmp_str[NODE_MAX];
		int rc = 255;

        /* Display off. */
        if ((strncmp(governor, ONDEMAND_GOVERNOR, strlen(ONDEMAND_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(ONDEMAND_GOVERNOR))) {
            int resource_values[] = {DISPLAY_OFF, MS_500, THREAD_MIGRATION_SYNC_OFF};

            if (!display_hint_sent) {
                perform_hint_action(DISPLAY_STATE_HINT_ID,
                        resource_values, sizeof(resource_values)/sizeof(resource_values[0]));
                display_hint_sent = 1;
            }
        } else if ((strncmp(governor, INTERACTIVE_GOVERNOR, strlen(INTERACTIVE_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
            int resource_values[] = {TR_MS_50, THREAD_MIGRATION_SYNC_OFF};

            if (!display_hint_sent) {
                perform_hint_action(DISPLAY_STATE_HINT_ID,
                        resource_values, sizeof(resource_values)/sizeof(resource_values[0]));
                display_hint_sent = 1;
            }
        } else if ((strncmp(governor, MSMDCVS_GOVERNOR, strlen(MSMDCVS_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(MSMDCVS_GOVERNOR))) {
            if (saved_interactive_mode == 1){
                /* Display turned off. */
                if (sysfs_read(DCVS_CPU0_SLACK_MAX_NODE, tmp_str, NODE_MAX - 1)) {
                    if (!slack_node_rw_failed) {
                        ALOGE("Failed to read from %s", DCVS_CPU0_SLACK_MAX_NODE);
                    }

                    rc = 1;
                } else {
                    saved_dcvs_cpu0_slack_max = atoi(tmp_str);
                }

                if (sysfs_read(DCVS_CPU0_SLACK_MIN_NODE, tmp_str, NODE_MAX - 1)) {
                    if (!slack_node_rw_failed) {
                        ALOGE("Failed to read from %s", DCVS_CPU0_SLACK_MIN_NODE);
                    }

                    rc = 1;
                } else {
                    saved_dcvs_cpu0_slack_min = atoi(tmp_str);
                }

                if (sysfs_read(MPDECISION_SLACK_MAX_NODE, tmp_str, NODE_MAX - 1)) {
                    if (!slack_node_rw_failed) {
                        ALOGE("Failed to read from %s", MPDECISION_SLACK_MAX_NODE);
                    }

                    rc = 1;
                } else {
                    saved_mpdecision_slack_max = atoi(tmp_str);
                }

                if (sysfs_read(MPDECISION_SLACK_MIN_NODE, tmp_str, NODE_MAX - 1)) {
                    if(!slack_node_rw_failed) {
                        ALOGE("Failed to read from %s", MPDECISION_SLACK_MIN_NODE);
                    }

                    rc = 1;
                } else {
                    saved_mpdecision_slack_min = atoi(tmp_str);
                }

                /* Write new values. */
                if (saved_dcvs_cpu0_slack_max != -1) {
                    snprintf(tmp_str, NODE_MAX, "%d", 10 * saved_dcvs_cpu0_slack_max);

                    if (sysfs_write(DCVS_CPU0_SLACK_MAX_NODE, tmp_str) != 0) {
                        if (!slack_node_rw_failed) {
                            ALOGE("Failed to write to %s", DCVS_CPU0_SLACK_MAX_NODE);
                        }

                        rc = 1;
                    }
                }

                if (saved_dcvs_cpu0_slack_min != -1) {
                    snprintf(tmp_str, NODE_MAX, "%d", 10 * saved_dcvs_cpu0_slack_min);

                    if (sysfs_write(DCVS_CPU0_SLACK_MIN_NODE, tmp_str) != 0) {
                        if(!slack_node_rw_failed) {
                            ALOGE("Failed to write to %s", DCVS_CPU0_SLACK_MIN_NODE);
                        }

                        rc = 1;
                    }
                }

                if (saved_mpdecision_slack_max != -1) {
                    snprintf(tmp_str, NODE_MAX, "%d", 10 * saved_mpdecision_slack_max);

                    if (sysfs_write(MPDECISION_SLACK_MAX_NODE, tmp_str) != 0) {
                        if(!slack_node_rw_failed) {
                            ALOGE("Failed to write to %s", MPDECISION_SLACK_MAX_NODE);
                        }

                        rc = 1;
                    }
                }

                if (saved_mpdecision_slack_min != -1) {
                    snprintf(tmp_str, NODE_MAX, "%d", 10 * saved_mpdecision_slack_min);

                    if (sysfs_write(MPDECISION_SLACK_MIN_NODE, tmp_str) != 0) {
                        if(!slack_node_rw_failed) {
                            ALOGE("Failed to write to %s", MPDECISION_SLACK_MIN_NODE);
                        }

                        rc = 1;
                    }
                }
            }

            slack_node_rw_failed = rc;
        }
		return rc; 
}

int set_interactive_on(char * governor, int saved_interactive_mode) {

		struct video_encode_metadata_t video_encode_metadata;
		int rc = 255;
		char tmp_str[NODE_MAX];

        /* Display on. */
        if ((strncmp(governor, ONDEMAND_GOVERNOR, strlen(ONDEMAND_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(ONDEMAND_GOVERNOR))) {
            undo_hint_action(DISPLAY_STATE_HINT_ID);
            display_hint_sent = 0;
        } else if ((strncmp(governor, INTERACTIVE_GOVERNOR, strlen(INTERACTIVE_GOVERNOR)) == 0) &&
                (strlen(governor) == strlen(INTERACTIVE_GOVERNOR))) {
            undo_hint_action(DISPLAY_STATE_HINT_ID);
            display_hint_sent = 0;
        } else if ((strncmp(governor, MSMDCVS_GOVERNOR, strlen(MSMDCVS_GOVERNOR)) == 0) && 
                (strlen(governor) == strlen(MSMDCVS_GOVERNOR))) {
            if (saved_interactive_mode == -1 || saved_interactive_mode == 0) {
                /* Display turned on. Restore if possible. */
                if (saved_dcvs_cpu0_slack_max != -1) {
                    snprintf(tmp_str, NODE_MAX, "%d", saved_dcvs_cpu0_slack_max);

                    if (sysfs_write(DCVS_CPU0_SLACK_MAX_NODE, tmp_str) != 0) {
                        if (!slack_node_rw_failed) {
                            ALOGE("Failed to write to %s", DCVS_CPU0_SLACK_MAX_NODE);
                        }

                        rc = 1;
                    }
                }

                if (saved_dcvs_cpu0_slack_min != -1) {
                    snprintf(tmp_str, NODE_MAX, "%d", saved_dcvs_cpu0_slack_min);

                    if (sysfs_write(DCVS_CPU0_SLACK_MIN_NODE, tmp_str) != 0) {
                        if (!slack_node_rw_failed) {
                            ALOGE("Failed to write to %s", DCVS_CPU0_SLACK_MIN_NODE);
                        }

                        rc = 1;
                    }
                }

                if (saved_mpdecision_slack_max != -1) {
                    snprintf(tmp_str, NODE_MAX, "%d", saved_mpdecision_slack_max);

                    if (sysfs_write(MPDECISION_SLACK_MAX_NODE, tmp_str) != 0) {
                        if (!slack_node_rw_failed) {
                            ALOGE("Failed to write to %s", MPDECISION_SLACK_MAX_NODE);
                        }

                        rc = 1;
                    }
                }

                if (saved_mpdecision_slack_min != -1) {
                    snprintf(tmp_str, NODE_MAX, "%d", saved_mpdecision_slack_min);

                    if (sysfs_write(MPDECISION_SLACK_MIN_NODE, tmp_str) != 0) {
                        if (!slack_node_rw_failed) {
                            ALOGE("Failed to write to %s", MPDECISION_SLACK_MIN_NODE);
                        }

                        rc = 1;
                    }
                }
            }

            slack_node_rw_failed = rc;
        }

	return rc;
}

