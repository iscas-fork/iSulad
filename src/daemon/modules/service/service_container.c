/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2017-2019. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: tanyifeng
 * Create: 2017-11-22
 * Description: provide container supervisor functions
 ******************************************************************************/
#define _GNU_SOURCE
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/eventfd.h>
#include <sys/epoll.h>

#include <isula_libutils/container_config.h>
#include <isula_libutils/container_config_v2.h>
#include <isula_libutils/container_exec_request.h>
#include <isula_libutils/container_exec_response.h>
#include <isula_libutils/defs.h>
#include <isula_libutils/host_config.h>
#include <isula_libutils/oci_runtime_spec.h>
#include <isula_libutils/log.h>
#include <isula_libutils/auto_cleanup.h>

#include "service_container_api.h"
#include "utils.h"
#include "err_msg.h"
#include "events_sender_api.h"
#include "image_api.h"
#include "specs_api.h"
#include "specs_mount.h"
#include "specs_extend.h"
#include "isulad_config.h"
#include "verify.h"
#include "plugin_api.h"
#include "container_api.h"
#include "namespace.h"
#include "runtime_api.h"
#include "error.h"
#include "io_handler.h"
#include "mainloop.h"
#include "constants.h"
#include "event_type.h"
#include "utils_array.h"
#include "utils_file.h"
#include "utils_fs.h"
#include "utils_string.h"
#include "utils_verify.h"
#include "utils_network.h"
#include "volume_api.h"
#include "utils_network.h"
#include "network_namespace.h"
#ifdef ENABLE_NATIVE_NETWORK
#include "service_network_api.h"
#endif
#include "id_name_manager.h"
#ifdef ENABLE_CRI_API_V1
#include "sandbox_ops.h"
#include "vsock_io_handler.h"
#endif

#define KATA_RUNTIME "kata-runtime"

int set_container_to_removal(const container_t *cont)
{
    int ret = 0;
    char *id = NULL;

    if (cont == NULL) {
        ERROR("Invalid input arguments");
        ret = -1;
        goto out;
    }

    id = cont->common_config->id;

    bool removal_progress = container_state_set_removal_in_progress(cont->state);
    if (removal_progress) {
        isulad_set_error_message("Container:%s was already in removal progress", id);
        ERROR("Container:%s was already in removal progress", id);
        ret = -1;
        goto out;
    }
out:
    return ret;
}

static bool save_after_auto_remove(container_t *cont)
{
    if (cont->hostconfig != NULL && cont->hostconfig->auto_remove) {
        int nret = set_container_to_removal(cont);
        if (nret != 0) {
            ERROR("Failed to set container %s state to removal", cont->common_config->id);
            return true;
        }
        container_unlock(cont);
        nret = delete_container(cont, true);
        container_lock(cont);
        if (nret != 0) {
            ERROR("Failed to cleanup container %s", cont->common_config->id);
            return true;
        }
        return false; /* do not save container if already auto removed */
    }

    return true;
}

static int create_mtab_link(const oci_runtime_spec *oci_spec)
{
    char *pathname = "/proc/mounts";
    char *slink = NULL;
    char *dir = NULL;
    int ret = 0;

    if (oci_spec->root == NULL || oci_spec->root->path == NULL) {
        ERROR("Root path is NULL, can not create link /etc/mtab for target /proc/mounts");
        return -1;
    }

    slink = util_path_join(oci_spec->root->path, "/etc/mtab");
    if (slink == NULL) {
        ERROR("Failed to join path:%s with /etc/mtab", oci_spec->root->path);
        ret = -1;
        goto out;
    }

    dir = util_path_dir(slink);
    if (dir == NULL) {
        ERROR("Failed to get dir %s", slink);
        ret = -1;
        goto out;
    }
    // When dir is symbol link, unlink dir to assure creating dir success following
    (void)unlink(dir);

    if (!util_dir_exists(dir)) {
        ret = util_mkdir_p(dir, ETC_FILE_MODE);
        if (ret != 0) {
            ERROR("Unable to create mtab directory %s.", dir);
            goto out;
        }
    }

    if (util_fileself_exists(slink)) {
        goto out;
    }

    ret = symlink(pathname, slink);
    if (ret < 0 && errno != EEXIST) {
        if (errno == EROFS) {
            WARN("Failed to create link %s for target %s. Read-only filesystem", slink, pathname);
        } else {
            SYSERROR("Failed to create \"%s\"", slink);
            ret = -1;
            goto out;
        }
    }

    ret = 0;

out:
    free(slink);
    free(dir);
    return ret;
}

static int generate_user_and_groups_conf(const container_t *cont, defs_process_user **puser)
{
    int ret = -1;
    char *username = NULL;

    if (cont == NULL || cont->common_config == NULL) {
        ERROR("Can not found container config");
        return -1;
    }

    *puser = util_common_calloc_s(sizeof(defs_process_user));
    if (*puser == NULL) {
        ERROR("Out of memory");
        return -1;
    }

    if (cont->common_config->config != NULL) {
        username = cont->common_config->config->user;
    }

    /* username may be NULL, we will handle it as UID 0 in im_get_user_conf */
    ret = im_get_user_conf(cont->common_config->image_type, cont->common_config->base_fs, cont->hostconfig, username,
                           *puser);
    if (ret != 0) {
        ERROR("Get user failed with '%s'", username ? username : "");
        free_defs_process_user(*puser);
        *puser = NULL;
    }

    return ret;
}

static int update_process_user(const container_t *cont, const oci_runtime_spec *oci_spec)
{
    int ret = 0;
    defs_process_user *puser = NULL;

    if (generate_user_and_groups_conf(cont, &puser) != 0) {
        ret = -1;
        goto out;
    }

    free_defs_process_user(oci_spec->process->user);
    oci_spec->process->user = puser;
    puser = NULL;

out:
    free_defs_process_user(puser);
    return ret;
}

static int renew_oci_config(const container_t *cont, oci_runtime_spec *oci_spec)
{
    int ret = 0;

    ret = update_process_user(cont, oci_spec);
    if (ret != 0) {
        ERROR("Failed to update process user");
        goto out;
    }

    ret = merge_share_namespace(oci_spec, cont->hostconfig, cont->common_config, cont->network_settings);
    if (ret != 0) {
        ERROR("Failed to merge share ns");
        goto out;
    }

out:
    return ret;
}

static void clean_resources_on_failure(const container_t *cont, const char *engine_log_path, const char *loglevel)
{
    int ret = 0;
    const char *id = cont->common_config->id;
    const char *runtime = cont->runtime;
    rt_clean_params_t params = { 0 };

    params.rootpath = cont->root_path;
    params.statepath = cont->state_path;
    params.logpath = engine_log_path;
    params.loglevel = loglevel;
    params.pid = 0;

    ret = runtime_clean_resource(id, runtime, &params);
    if (ret != 0) {
        ERROR("Failed to clean failed started container %s", id);
    }

    return;
}

static int do_post_start_on_success(container_t *cont, int exit_fifo_fd,
                                    const char *exit_fifo, const pid_ppid_info_t *pid_info)
{
    int ret = 0;

    // exit_fifo_fd was closed in container_supervisor_add_exit_monitor
    if (container_supervisor_add_exit_monitor(exit_fifo_fd, exit_fifo, pid_info, cont)) {
        ERROR("Failed to add exit monitor to supervisor");
        ret = -1;
    }
    return ret;
}

static int create_env_path_dir(const char *env_path)
{
    int ret = 0;
    size_t len = 0;
    size_t i = 0;
    char *dir = NULL;

    len = strlen(env_path);
    if (len == 0) {
        return 0;
    }
    dir = util_strdup_s(env_path);
    for (i = len - 1; i > 0; i--) {
        if (dir[i] == '/') {
            dir[i] = '\0';
            break;
        }
    }
    if (strlen(dir) == 0) {
        free(dir);
        return 0;
    }
    if (!util_dir_exists(dir)) {
        ret = util_mkdir_p(dir, DEFAULT_SECURE_DIRECTORY_MODE);
    }
    free(dir);
    return ret;
}

static int write_env_content(const char *env_path, const char **env, size_t env_len)
{
    int ret = 0;
    int fd = -1;
    size_t i = 0;
    ssize_t nret = 0;

    ret = create_env_path_dir(env_path);
    if (ret < 0) {
        ERROR("Failed to create env path dir");
        return ret;
    }
    fd = util_open(env_path, O_WRONLY | O_CREAT | O_TRUNC, DEFAULT_SECURE_FILE_MODE);
    if (fd < 0) {
        SYSERROR("Failed to create env file: %s", env_path);
        ret = -1;
        goto out;
    }
    if (env != NULL) {
        for (i = 0; i < env_len; i++) {
            size_t env_max = 4096;
            if (strlen(env[i]) > env_max) {
                ERROR("Env is too long");
                ret = -1;
                goto out;
            }
            size_t len = strlen(env[i]) + strlen("\n") + 1;
            char *env_content = NULL;
            env_content = util_common_calloc_s(len);
            if (env_content == NULL) {
                ERROR("Out of memory");
                ret = -1;
                goto out;
            }
            nret = snprintf(env_content, len, "%s\n", env[i]);
            if (nret < 0 || (size_t)nret >= len) {
                ERROR("Out of memory");
                free(env_content);
                ret = -1;
                goto out;
            }
            nret = util_write_nointr(fd, env_content, strlen(env_content));
            if (nret < 0 || (size_t)nret != strlen(env_content)) {
                SYSERROR("Write env file failed");
                free(env_content);
                ret = -1;
                goto out;
            }
            free(env_content);
        }
    }
out:
    if (fd >= 0) {
        close(fd);
    }
    return ret;
}

static int write_env_to_target_file(const container_t *cont, const oci_runtime_spec *oci_spec)
{
    int ret = 0;
    char *env_path = NULL;

    if (cont->hostconfig->env_target_file == NULL || oci_spec->process == NULL) {
        return 0;
    }
    if (!cont->hostconfig->system_container || cont->hostconfig->external_rootfs == NULL) {
        return 0;
    }
    env_path = util_path_join(cont->common_config->base_fs, cont->hostconfig->env_target_file);
    if (env_path == NULL) {
        ERROR("Failed to get env target file path: %s", cont->hostconfig->env_target_file);
        return -1;
    }
    ret = write_env_content(env_path, (const char **)oci_spec->process->env, oci_spec->process->env_len);
    free(env_path);
    return ret;
}

static int mount_host_channel(const host_config_host_channel *host_channel, const char *user_remap)
{
    char properties[MOUNT_PROPERTIES_SIZE] = { 0 };

    if (host_channel == NULL) {
        return 0;
    }
    if (util_detect_mounted(host_channel->path_on_host)) {
        return 0;
    }
    int nret =
        snprintf(properties, sizeof(properties), "mode=1777,size=%llu", (long long unsigned int)host_channel->size);
    if (nret < 0 || (size_t)nret >= sizeof(properties)) {
        ERROR("Failed to generate mount properties");
        return -1;
    }
    if (mount("tmpfs", host_channel->path_on_host, "tmpfs", MS_NOEXEC | MS_NOSUID | MS_NODEV, (void *)properties)) {
        ERROR("Failed to mount host path '%s'", host_channel->path_on_host);
        return -1;
    }
    if (user_remap != NULL) {
        unsigned int host_uid = 0;
        unsigned int host_gid = 0;
        unsigned int size = 0;
        if (util_parse_user_remap(user_remap, &host_uid, &host_gid, &size)) {
            ERROR("Failed to split string '%s'.", user_remap);
            return -1;
        }
        if (chown(host_channel->path_on_host, host_uid, host_gid) != 0) {
            ERROR("Failed to chown host path '%s'.", host_channel->path_on_host);
            return -1;
        }
    }
    return 0;
}

static int prepare_user_remap_config(const container_t *cont)
{
    if (cont == NULL) {
        return 0;
    }

    if (cont->hostconfig == NULL) {
        return 0;
    }

    if (cont->hostconfig->host_channel != NULL) {
        if (mount_host_channel(cont->hostconfig->host_channel, cont->hostconfig->user_remap)) {
            ERROR("Failed to mount host channel");
            return -1;
        }
    }
    return 0;
}

static int mount_dev_tmpfs_for_system_container(const container_t *cont)
{
    char rootfs_dev_path[PATH_MAX] = { 0 };

    if (cont == NULL || cont->hostconfig == NULL || cont->common_config == NULL) {
        return 0;
    }
    if (!cont->hostconfig->system_container || cont->hostconfig->external_rootfs == NULL) {
        return 0;
    }
    int nret = snprintf(rootfs_dev_path, sizeof(rootfs_dev_path), "%s/dev", cont->common_config->base_fs);
    if (nret < 0 || (size_t)nret >= sizeof(rootfs_dev_path)) {
        ERROR("Out of memory");
        return -1;
    }
    if (!util_dir_exists(rootfs_dev_path)) {
        if (util_mkdir_p(rootfs_dev_path, CONFIG_DIRECTORY_MODE)) {
            ERROR("Failed to mkdir '%s'", rootfs_dev_path);
            return -1;
        }
    }
    /* set /dev mount size to half of container memory limit */
    if (cont->hostconfig->memory > 0) {
        char mnt_opt[MOUNT_PROPERTIES_SIZE] = { 0 };
        nret = snprintf(mnt_opt, sizeof(mnt_opt), "size=%lld,mode=755", (long long int)(cont->hostconfig->memory / 2));
        if (nret < 0 || (size_t)nret >= sizeof(mnt_opt)) {
            ERROR("Out of memory");
            return -1;
        }
        if (mount("tmpfs", rootfs_dev_path, "tmpfs", 0, mnt_opt) != 0) {
            ERROR("Failed to mount dev tmpfs on '%s'", rootfs_dev_path);
            return -1;
        }
    } else {
        if (mount("tmpfs", rootfs_dev_path, "tmpfs", 0, "mode=755") != 0) {
            ERROR("Failed to mount dev tmpfs on '%s'", rootfs_dev_path);
            return -1;
        }
    }
    if (cont->hostconfig->user_remap != NULL) {
        unsigned int host_uid = 0;
        unsigned int host_gid = 0;
        unsigned int size = 0;
        if (util_parse_user_remap(cont->hostconfig->user_remap, &host_uid, &host_gid, &size)) {
            ERROR("Failed to split string '%s'.", cont->hostconfig->user_remap);
            return -1;
        }
        if (chown(rootfs_dev_path, host_uid, host_gid) != 0) {
            ERROR("Failed to chown host path '%s'.", rootfs_dev_path);
            return -1;
        }
    }
    return 0;
}

static void umount_rootfs_on_failure(const container_t *cont)
{
    const char *id = cont->common_config->id;
    int nret = im_umount_container_rootfs(cont->common_config->image_type, cont->common_config->image, id);
    if (nret != 0) {
        ERROR("Failed to umount rootfs for container %s", id);
    }
}

static int prepare_start_state_files(const container_t *cont, char **exit_fifo, int *exit_fifo_fd, char **pid_file)
{
    int ret = 0;
    int nret = 0;
    char container_state[PATH_MAX] = { 0 };
    char pidfile[PATH_MAX] = { 0 };
    const char *id = cont->common_config->id;

    nret = snprintf(container_state, sizeof(container_state), "%s/%s", cont->state_path, id);
    if (nret < 0 || (size_t)nret >= sizeof(container_state)) {
        ERROR("Failed to sprintf container_state");
        ret = -1;
        goto out;
    }

    nret = util_mkdir_p(container_state, TEMP_DIRECTORY_MODE);
    if (nret < 0) {
        ERROR("Unable to create container state directory %s.", container_state);
        ret = -1;
        goto out;
    }

    nret = snprintf(pidfile, sizeof(pidfile), "%s/pid.file", container_state);
    if (nret < 0 || (size_t)nret >= sizeof(pidfile)) {
        ERROR("Failed to sprintf pidfile");
        ret = -1;
        goto out;
    }
    *pid_file = util_strdup_s(pidfile);
    if (*pid_file == NULL) {
        ERROR("Failed to dup pid file in state directory %s", container_state);
        ret = -1;
        goto out;
    }

    *exit_fifo = container_exit_fifo_create(container_state);
    if (*exit_fifo == NULL) {
        ERROR("Failed to create exit FIFO in state directory %s", container_state);
        ret = -1;
        goto out;
    }

    *exit_fifo_fd = container_exit_fifo_open(*exit_fifo);
    if (*exit_fifo_fd < 0) {
        ERROR("Failed to open exit FIFO %s", *exit_fifo);
        ret = -1;
        goto out;
    }

out:
    return ret;
}

static int umount_dev_tmpfs_for_system_container(const container_t *cont)
{
    if (cont->hostconfig != NULL && cont->hostconfig->system_container && cont->hostconfig->external_rootfs != NULL) {
        char rootfs_dev_path[PATH_MAX] = { 0 };
        int nret = snprintf(rootfs_dev_path, sizeof(rootfs_dev_path), "%s/dev", cont->common_config->base_fs);
        if ((size_t)nret >= sizeof(rootfs_dev_path) || nret < 0) {
            ERROR("Out of memory");
            return -1;
        }
        if (umount(rootfs_dev_path) < 0 && errno != ENOENT) {
            SYSWARN("Failed to umount dev tmpfs: %s", rootfs_dev_path);
        }
    }
    return 0;
}

static int valid_mount_point(container_config_v2_common_config_mount_points_element *mp)
{
    struct stat st;
    // ignore checking nonexist mount point
    if (mp == NULL || mp->type == NULL || mp->source == NULL) {
        return 0;
    }

    // check volumes only currently
    if (strcmp(mp->type, MOUNT_TYPE_VOLUME) != 0) {
        return 0;
    }

    if (lstat(mp->source, &st) != 0) {
        SYSERROR("lstat %s failed", mp->source);
        isulad_set_error_message("Check %s failed, get more information from log.", mp->source);
        return -1;
    }

    return 0;
}

static int verify_mounts(const container_t *cont)
{
    size_t i = 0;
    container_config_v2_common_config_mount_points *mount_points = NULL;
    container_config_v2_common_config_mount_points_element *mp = NULL;

    if (cont->common_config == NULL || cont->common_config->mount_points == NULL) {
        return 0;
    }

    mount_points = cont->common_config->mount_points;
    for (i = 0; i < mount_points->len; i++) {
        mp = mount_points->values[i];
        if (valid_mount_point(mp) != 0) {
            return -1;
        }
    }

    return 0;
}

static int wait_exit_fifo_epoll_callback(int fd, uint32_t events, void *cbdata, struct epoll_descr *descr)
{
    int ret = EPOLL_LOOP_HANDLE_CLOSE;
    int exit_code = 0;
    char *container_id = cbdata;

    if (util_read_nointr(fd, &exit_code, sizeof(int)) <= 0) {
        ERROR("Failed to read exit fifo fd for container %s", container_id);
        return ret;
    }

    ERROR("The container %s 's monitor on fd %d has exited: %d", container_id, fd, exit_code);
    return ret;
}

static void wait_exit_fifo_timeout_callback(void *cbdata)
{
    char *container_id = NULL;

    if (cbdata == NULL) {
        ERROR("Invalid cbdata");
        return;
    }
    container_id = (char *)cbdata;

    // timeout
    // maybe monitor still cleanup cgroup and processes,
    // or monitor doesn't step in cleanup at all
    ERROR("Wait container %s 's monitor timeout", container_id);
}

static void wait_exit_fifo(const char *id, const int exit_fifo_fd)
{
    int nret = 0;
    const int WAIT_TIMEOUT = 3000;
    char *container_id = NULL;
    struct epoll_descr descr = { 0 };

    nret = epoll_loop_open(&descr);
    if (nret != 0) {
        ERROR("Failed to create epoll for container %s", id);
        return;
    }

    container_id = util_strdup_s(id);
    nret = epoll_loop_add_handler(&descr, exit_fifo_fd, wait_exit_fifo_epoll_callback, container_id);
    if (nret != 0) {
        ERROR("Failed to add epoll handler for container %s", id);
        goto out;
    }

    descr.timeout_cb = wait_exit_fifo_timeout_callback;
    descr.timeout_cbdata = container_id;
    nret = epoll_loop(&descr, WAIT_TIMEOUT);
    if (nret != 0) {
        SYSERROR("Wait container %s 's monitor on fd %d failed", id, exit_fifo_fd);
        goto out;
    }

out:
    free(container_id);
    epoll_loop_close(&descr);
}

static int do_oci_spec_update(const char *id, oci_runtime_spec *oci_spec, container_config *container_spec, host_config *hostconfig)
{
    int ret;

    // Renew annotations for oci spec, cgroup path only,
    // since lxc uses the "cgroup.dir" in oci annotations to create cgroup
    // should ensure that container spec has the same annotations as oci spec
    ret = update_spec_annotations(oci_spec, container_spec, hostconfig);
    if (ret < 0) {
        return -1;
    }

    // If isulad daemon cgroup parent updated, we should update this config into oci spec
    ret = update_oci_container_cgroups_path(id, oci_spec, hostconfig);
    if (ret < 0) {
        return -1;
    }

    // For Linux.Resources, isula update will save changes into oci spec;
    // so we just skip it;

    // Remove old devices and update all devices
    ret = update_devcies_for_oci_spec(oci_spec, hostconfig);
    if (ret != 0) {
        ERROR("Failed to do update devices for oci spec");
        return -1;
    }

    // If isulad daemon ulimit updated, we should update this config into oci spec.
    ret = update_oci_ulimit(oci_spec, hostconfig);
    if (ret < 0) {
        return -1;
    }

    // renew_oci_config() will update process->user and share namespace after.

    return 0;
}

static int do_start_container(container_t *cont, const char *console_fifos[], bool reset_rm, pid_ppid_info_t *pid_info)
{
    int ret = 0;
    int nret = 0;
    int exit_fifo_fd = -1;
    bool tty = false;
    bool open_stdin = false;
    unsigned int start_timeout = 0;
    char *engine_log_path = NULL;
    char *loglevel = NULL;
    char *logdriver = NULL;
    char *exit_fifo = NULL;
    char *pidfile = NULL;
    char bundle[PATH_MAX] = { 0 };
    const char *runtime = cont->runtime;
    const char *id = cont->common_config->id;
    oci_runtime_spec *oci_spec = NULL;
    rt_create_params_t create_params = { 0 };
    rt_start_params_t start_params = { 0 };

    nret = snprintf(bundle, sizeof(bundle), "%s/%s", cont->root_path, id);
    if (nret < 0 || (size_t)nret >= sizeof(bundle)) {
        ERROR("Failed to print bundle string");
        ret = -1;
        goto out;
    }
    DEBUG("bd:%s, state:%s", bundle, cont->state_path);
    if (mount_dev_tmpfs_for_system_container(cont) < 0) {
        ret = -1;
        goto out;
    }

    if (prepare_user_remap_config(cont) != 0) {
        ret = -1;
        goto out;
    }

    if (reset_rm && !container_reset_restart_manager(cont, true)) {
        ERROR("Failed to reset restart manager");
        isulad_set_error_message("Failed to reset restart manager");
        ret = -1;
        goto out;
    }

    if (conf_get_daemon_log_config(&loglevel, &logdriver, &engine_log_path) != 0) {
        ret = -1;
        goto out;
    }

    nret = prepare_start_state_files(cont, &exit_fifo, &exit_fifo_fd, &pidfile);
    if (nret != 0) {
        ret = -1;
        goto out;
    }

    oci_spec = load_oci_config(cont->root_path, id);
    if (oci_spec == NULL) {
        ERROR("Failed to load oci config");
        ret = -1;
        goto close_exit_fd;
    }

    if (write_env_to_target_file(cont, oci_spec) < 0) {
        ret = -1;
        goto close_exit_fd;
    }

    nret = im_mount_container_rootfs(cont->common_config->image_type, cont->common_config->image, id);
    if (nret != 0) {
        ERROR("Failed to mount rootfs for container %s", id);
        ret = -1;
        goto close_exit_fd;
    }

    // Update possible changes
    nret = do_oci_spec_update(id, oci_spec, cont->common_config->config, cont->hostconfig);
    if (nret != 0) {
        ERROR("Failed to update possible changes for oci spec");
        ret = -1;
        goto close_exit_fd;
    }

    nret = container_to_disk(cont);
    if (nret != 0) {
        ERROR("Failed to save container info to disk");
        ret = -1;
        goto close_exit_fd;
    }

    nret = setup_ipc_dirs(cont->hostconfig, cont->common_config);
    if (nret != 0) {
        ERROR("Failed to setup ipc dirs");
        ret = -1;
        goto close_exit_fd;
    }

    // embedded conainter is readonly, create mtab link will fail
    // kata-runtime container's qemu donot support to create mtab in host
    if (strcmp(IMAGE_TYPE_EMBEDDED, cont->common_config->image_type) != 0 && strcmp(KATA_RUNTIME, cont->runtime) != 0) {
        nret = create_mtab_link(oci_spec);
        if (nret != 0) {
            ERROR("Failed to create link /etc/mtab for target /proc/mounts");
            ret = -1;
            goto close_exit_fd;
        }
    }

    if (verify_mounts(cont)) {
        ret = -1;
        goto close_exit_fd;
    }

    if (renew_oci_config(cont, oci_spec) != 0) {
        ret = -1;
        goto close_exit_fd;
    }

    if (verify_container_settings_start(oci_spec) != 0) {
        ret = -1;
        goto close_exit_fd;
    }

    if (save_oci_config(id, cont->root_path, oci_spec) != 0) {
        ERROR("Failed to save container settings");
        ret = -1;
        goto close_exit_fd;
    }

    start_timeout = conf_get_start_timeout();
    if (cont->common_config->config != NULL) {
        tty = cont->common_config->config->tty;
        open_stdin = cont->common_config->config->open_stdin;
    }

#ifdef ENABLE_PLUGIN
    if (plugin_event_container_pre_start(cont)) {
        ERROR("Plugin event pre start failed ");
        plugin_event_container_post_stop(cont); /* ignore error */
        ret = -1;
        goto close_exit_fd;
    }
#endif

#ifdef ENABLE_CRI_API_V1
    if (cont->common_config->sandbox_info != NULL &&
        sandbox_prepare_container(cont->common_config,
                                  oci_spec, console_fifos, tty) != 0) {
        ERROR("Failed to prepare in sandbox");
        ret = -1;
        goto close_exit_fd;
    }
#endif

    create_params.bundle = bundle;
    create_params.state = cont->state_path;
    create_params.oci_config_data = oci_spec;
    create_params.terminal = tty;
    create_params.stdin = console_fifos[0];
    create_params.stdout = console_fifos[1];
    create_params.stderr = console_fifos[2];
    create_params.exit_fifo = exit_fifo;
    create_params.tty = tty;
    create_params.open_stdin = open_stdin;
#ifdef ENABLE_CRI_API_V1
    if (cont->common_config->sandbox_info != NULL) {
        create_params.task_addr = cont->common_config->sandbox_info->task_address;
    }
#endif

    if (runtime_create(id, runtime, &create_params) != 0) {
        ret = -1;
        goto close_exit_fd;
    }

    start_params.rootpath = cont->root_path;
    start_params.state = cont->state_path;
    start_params.tty = tty;
    start_params.open_stdin = open_stdin;
    start_params.logpath = engine_log_path;
    start_params.loglevel = loglevel;
    start_params.console_fifos = console_fifos;
    start_params.start_timeout = start_timeout;
    start_params.container_pidfile = pidfile;
    start_params.exit_fifo = exit_fifo;
    start_params.image_type_oci = false;
    if (strcmp(IMAGE_TYPE_OCI, cont->common_config->image_type) == 0) {
        start_params.image_type_oci = true;
    }

    ret = runtime_start(id, runtime, &start_params, pid_info);
    if (ret == 0) {
        if (do_post_start_on_success(cont, exit_fifo_fd, exit_fifo, pid_info) != 0) {
            ERROR("Failed to do post start on runtime start success");
            ret = -1;
            goto clean_resources;
        }
    } else {
        // wait monitor cleanup cgroup and processes finished
        wait_exit_fifo(id, exit_fifo_fd);
        goto close_exit_fd;
    }
    goto out;

close_exit_fd:
    close(exit_fifo_fd);

clean_resources:
    clean_resources_on_failure(cont, engine_log_path, loglevel);

out:
    free(loglevel);
    free(engine_log_path);
    free(logdriver);
    free(exit_fifo);
    free(pidfile);
    free_oci_runtime_spec(oci_spec);
    if (ret != 0) {
        umount_rootfs_on_failure(cont);
        (void)umount_dev_tmpfs_for_system_container(cont);
    }
    return ret;
}

static int force_kill(container_t *cont);

int start_container(container_t *cont, const char *console_fifos[], bool reset_rm)
{
    int ret = 0;
    pid_ppid_info_t pid_info = { 0 };
    int exit_code = 125;

    if (cont == NULL || console_fifos == NULL) {
        ERROR("Invalid input arguments");
        ret = -1;
        goto out;
    }

    container_lock(cont);

    if (reset_rm && container_is_running(cont->state)) {
        ret = 0;
        goto out;
    }

    if (container_is_paused(cont->state)) {
        ERROR("Cannot start a paused container, try unpause instead");
        isulad_set_error_message("Cannot start a paused container, try unpause instead.");
        ret = -1;
        goto out;
    }

    if (container_is_removal_in_progress(cont->state) || container_is_dead(cont->state)) {
        ERROR("Container is marked for removal and cannot be started.");
        isulad_set_error_message("Container is marked for removal and cannot be started.");
        ret = -1;
        goto out;
    }

    if (container_is_in_gc_progress(cont->common_config->id)) {
        isulad_set_error_message("You cannot start container %s in garbage collector progress.",
                                 cont->common_config->id);
        ERROR("You cannot start container %s in garbage collector progress.", cont->common_config->id);
        ret = -1;
        goto out;
    }

#ifdef ENABLE_NATIVE_NETWORK
    if (util_native_network_checker(cont->hostconfig->network_mode)) {
        if (!validate_native_network(cont->hostconfig, cont->network_settings)) {
            ERROR("Invalid native network");
            ret = -1;
            goto out;
        }

        if (!util_post_setup_network(cont->hostconfig->user_remap) && prepare_native_network(cont) != 0) {
            isulad_set_error_message("Failed to prepare container network.");
            ERROR("Failed to prepare container network");
            ret = -1;
            goto out;
        }
    }
#endif

    ret = do_start_container(cont, console_fifos, reset_rm, &pid_info);
    if (ret != 0) {
        ERROR("Runtime start container failed");
        ret = -1;
        goto set_stopped;
    }
    container_state_set_running(cont->state, &pid_info, true);

#ifdef ENABLE_NATIVE_NETWORK
    if (util_native_network_checker(cont->hostconfig->network_mode)) {
        // if isolate container with a user namespace, setup network after container running
        // otherwise the network namespace is owned by a wrong user namespace
        if (util_post_setup_network(cont->hostconfig->user_remap) && prepare_native_network(cont) != 0) {
            isulad_append_error_message("Failed to prepare container network. ");
            ERROR("Failed to prepare container network");
            ret = -1;
            goto stop_container;
        }
    }
#endif

    container_state_reset_has_been_manual_stopped(cont->state);
    container_init_health_monitor(cont->common_config->id);
    goto save_container;

#ifdef ENABLE_NATIVE_NETWORK
stop_container:
    // set AutoRemove flag to false before kill so the container won't be removed
    cont->hostconfig->auto_remove = false;
    if (force_kill(cont) != 0) {
        ERROR("Failed to force kill container %s", cont->common_config->id);
    }
    cont->hostconfig->auto_remove = cont->hostconfig->auto_remove_bak;
#endif
set_stopped:
#ifdef ENABLE_NATIVE_NETWORK
    if (util_native_network_checker(cont->hostconfig->network_mode)) {
        if (!util_post_setup_network(cont->hostconfig->user_remap) && remove_native_network(cont) != 0) {
            ERROR("Failed to remove cont network");
        }
    }
#endif

    container_state_set_error(cont->state, (const char *)g_isulad_errmsg);
    util_contain_errmsg(g_isulad_errmsg, &exit_code);
    container_state_set_stopped(cont->state, exit_code);
    container_wait_stop_cond_broadcast(cont);
    if (!save_after_auto_remove(cont)) {
        goto out;
    }

save_container:
    if (container_state_to_disk(cont)) {
        ERROR("Failed to save container \"%s\" to disk", cont->common_config->id);
        ret = -1;
        goto out;
    }
out:
    container_unlock(cont);
    return ret;
}

static int do_clean_container(const container_t *cont, pid_t pid)
{
    int ret = 0;
    char *engine_log_path = NULL;
    char *loglevel = NULL;
    char *logdriver = NULL;
    const char *id = cont->common_config->id;
    const char *runtime = cont->runtime;
    rt_clean_params_t params = { 0 };

    if (conf_get_daemon_log_config(&loglevel, &logdriver, &engine_log_path) != 0) {
        ERROR("Failed to get log config");
        ret = -1;
        goto out;
    }

    params.rootpath = cont->root_path;
    params.statepath = cont->state_path;
    params.logpath = engine_log_path;
    params.loglevel = loglevel;
    params.pid = pid;

    ret = runtime_clean_resource(id, runtime, &params);
    if (ret != 0) {
        ERROR("Failed to clean failed started container %s", id);
        ret = -1;
        goto out;
    }

    if (im_umount_container_rootfs(cont->common_config->image_type, cont->common_config->image, id)) {
        ERROR("Failed to umount rootfs for container %s", id);
        ret = -1;
        goto out;
    }

    if (umount_dev_tmpfs_for_system_container(cont) < 0) {
        ret = -1;
        goto out;
    }

out:
    free(loglevel);
    free(engine_log_path);
    free(logdriver);
    return ret;
}

int clean_container_resource(const char *id, const char *runtime, pid_t pid)
{
    int ret = 0;
    container_t *cont = NULL;

    if (id == NULL || runtime == NULL) {
        ERROR("Invalid input arguments");
        ret = -1;
        goto out;
    }

    cont = containers_store_get(id);
    if (cont == NULL) {
        WARN("No such container:%s", id);
        goto out;
    }

    container_lock(cont);
    ret = do_clean_container(cont, pid);
    if (ret != 0) {
        ERROR("Runtime clean container resource failed");
        ret = -1;
        goto unlock;
    }

#ifdef ENABLE_NATIVE_NETWORK
    if (util_native_network_checker(cont->hostconfig->network_mode)) {
        if (cont->skip_remove_network) {
            WARN("skip remove container %s network when restarting", cont->common_config->id);
        } else if (remove_native_network(cont) != 0) {
            // ignore remove network error
            ERROR("Failed to remove container %s network", cont->common_config->id);
        }
    }
#endif

unlock:
    container_unlock(cont);

out:
    container_unref(cont);
    return ret;
}

static int do_runtime_rm_helper(const char *id, const char *runtime, const char *rootpath)
{
    int ret = 0;
    rt_rm_params_t params = { 0 };

    params.rootpath = rootpath;

    if (runtime_rm(id, runtime, &params)) {
        ERROR("Runtime remove container failed");
        ret = -1;
        goto out;
    }

out:
    return ret;
}

int release_volumes(container_config_v2_common_config_mount_points *mount_points, char *id, bool rm_anonymous_volumes)
{
    int ret = 0;
    size_t i = 0;

    // no mount point is valid
    if (mount_points == NULL) {
        return 0;
    }

    for (i = 0; i < mount_points->len; i++) {
        // only volume have name
        if (mount_points->values[i]->name == NULL) {
            continue;
        }

        // release reference to this volume
        if (volume_del_ref(mount_points->values[i]->name, id) != 0) {
            ERROR("delete reference %s to volume %s failed", id, mount_points->values[i]->name);
            ret = -1;
            continue;
        }

        // --rm delete anonymous volumes only
        if (!mount_points->values[i]->named && rm_anonymous_volumes) {
            ret = volume_remove(mount_points->values[i]->name);
            if (ret != 0 && ret != VOLUME_ERR_NOT_EXIST) {
                ERROR("remove anonymous volume %s failed", mount_points->values[i]->name);
                ret = -1;
            }
        }
    }

    return ret;
}

static void do_delete_network(container_t *cont)
{
    if (cont->network_settings == NULL || cont->network_settings->sandbox_key == NULL) {
        return;
    }

#ifdef ENABLE_NATIVE_NETWORK
    if (util_native_network_checker(cont->hostconfig->network_mode)) {
        if (remove_native_network(cont) != 0) {
            WARN("Failed to remove network when delete container %s, maybe it has been cleaned up",
                 cont->common_config->id);
        }
        if (remove_network_namespace_file(cont->network_settings->sandbox_key) != 0) {
            ERROR("Failed to remove network ns file when deleting container %s", cont->common_config->id);
        }

        return;
    }
#endif

    if (!namespace_is_cni(cont->hostconfig->network_mode)) {
        return;
    }

#ifdef ENABLE_CRI_API_V1
    // Under cri_api_v1, only that sandbox is not nil and is shim and network mode is cni
    // Indicates this is a pause container under cni network mode from sandbox.
    // Sandbox will maintain sandboxkey, so we will not delete sandboxkey here.
    if (is_sandbox_container(cont->common_config->sandbox_info)) {
        return;
    }
#endif

    if (remove_network_namespace(cont->network_settings->sandbox_key) != 0) {
        WARN("Failed to remove network ns when deleting container %s, maybe it has been cleaned up",
             cont->common_config->id);
    }
    if (remove_network_namespace_file(cont->network_settings->sandbox_key) != 0) {
        ERROR("Failed to remove network ns file when deleting container %s", cont->common_config->id);
    }
}

static int delete_client_fifo_home_dir(const char *name)
{
    char *client_fifo_home_dir = NULL;

    client_fifo_home_dir = util_path_join(CLIENT_RUNDIR, name);
    if (client_fifo_home_dir == NULL) {
        ERROR("Fail to get fifo home dir");
        return -1;
    }

    // Do not delete if the directory does not exist.
    if (!util_file_exists(client_fifo_home_dir)) {
        free(client_fifo_home_dir);
        return 0;
    }

    if (util_recursive_rmdir(client_fifo_home_dir, 0)) {
        WARN("Failed to delete client fifo home path:%s", client_fifo_home_dir);
    }

    free(client_fifo_home_dir);
    return 0;
}

static int do_delete_container(container_t *cont)
{
    int ret = 0;
    char *id = NULL;
    char *name = NULL;
    char *statepath = NULL;
    char container_state[PATH_MAX] = { 0 };
    const char *runtime = NULL;
    const char *rootpath = NULL;
    container_t *cont_tmp = NULL;
    bool rm_anonymous_volumes = false;
    bool skip_id_name_manage = false;

    container_lock(cont);

    id = cont->common_config->id;
    name = cont->common_config->name;
    statepath = cont->state_path;
    runtime = cont->runtime;
    rootpath = cont->root_path;
    rm_anonymous_volumes = cont->rm_anonymous_volumes || ((cont->hostconfig != NULL) && cont->hostconfig->auto_remove);
#ifdef ENABLE_CRI_API_V1
    skip_id_name_manage = is_sandbox_container(cont->common_config->sandbox_info);
#endif

    /* check if container was deregistered by previous rm already */
    cont_tmp = containers_store_get(id);
    if (cont_tmp == NULL) {
        ret = 0;
        goto out;
    }
    container_unref(cont_tmp);

    (void)container_state_to_disk(cont);

    if (container_is_in_gc_progress(id)) {
        isulad_set_error_message("You cannot remove container %s in garbage collector progress.", id);
        ERROR("You cannot remove container %s in garbage collector progress.", id);
        ret = -1;
        goto out;
    }

    do_delete_network(cont);

    ret = snprintf(container_state, sizeof(container_state), "%s/%s", statepath, id);
    if (ret < 0 || (size_t)ret >= sizeof(container_state)) {
        ERROR("Failed to sprintf container_state");
        ret = -1;
        goto out;
    }
    ret = util_recursive_rmdir(container_state, 0);
    if (ret != 0) {
        SYSERROR("Failed to delete container's state directory %s", container_state);
        ret = -1;
        goto out;
    }

    umount_share_shm(cont);

    umount_host_channel(cont->hostconfig->host_channel);

    // clean residual mount points
    cleanup_mounts_by_id(id, rootpath);

    if (do_runtime_rm_helper(id, runtime, rootpath) != 0) {
        ret = -1;
        goto out;
    }

    if (im_remove_container_rootfs(cont->common_config->image_type, id)) {
        ERROR("Failed to remove rootfs for container %s", id);
        ret = -1;
        goto out;
    }

    ret = release_volumes(cont->common_config->mount_points, id, rm_anonymous_volumes);
    if (ret != 0) {
        ERROR("Failed to release volumes of container %s", name);
        goto out;
    }

    /* broadcast remove condition */
    container_wait_rm_cond_broadcast(cont);

    if (!containers_store_remove(id)) {
        ERROR("Failed to remove container '%s' from containers store", id);
        ret = -1;
        goto out;
    }

    if (!container_name_index_remove(name)) {
        ERROR("Failed to remove '%s' from name index", name);
        ret = -1;
    }

    if (!skip_id_name_manage && !id_name_manager_remove_entry(id, name)) {
        ERROR("Failed to remove %s and %s from id name manager", id, name);
        ret = -1;
    }

out:
    // when container is auto-remove, it will be deleted when stopped.
    // isula has no suitable time to delete fifo dir, so isulad delete it here.
    // Whether the delete container operation fails or not, delete the client's fifo dir to avoid it residual.
    // When isula and isulad use tcp to connect, fifo files will not be created.
    // Because restart will set auto_remove to false, using auto_remove_bak to ensure delete Policy.
    if (cont->hostconfig != NULL && cont->hostconfig->auto_remove_bak && delete_client_fifo_home_dir(id) != 0) {
        WARN("Failed to delete client fifo home dir");
    }
    container_unlock(cont);
    return ret;
}

int delete_container(container_t *cont, bool force)
{
    int ret = 0;
    char *id = NULL;

    if (cont == NULL) {
        ERROR("Invalid input arguments");
        ret = -1;
        goto out;
    }

    id = cont->common_config->id;

    if (container_is_running(cont->state)) {
        if (!force) {
            if (container_is_paused(cont->state)) {
                isulad_set_error_message("You cannot remove a paused container %s. "
                                         "Unpause and then stop the container before "
                                         "attempting removal or force remove",
                                         id);
                ERROR("You cannot remove a paused container %s. Unpause and then stop the container before "
                      "attempting removal or force remove",
                      id);
            } else {
                isulad_set_error_message("You cannot remove a running container %s. "
                                         "Stop the container before attempting removal or use -f",
                                         id);
                ERROR("You cannot remove a running container %s."
                      " Stop the container before attempting removal or use -f",
                      id);
            }
            ret = -1;
            goto reset_removal_progress;
        }

        ret = stop_container(cont, 3, force, false);
        if (ret != 0) {
            isulad_append_error_message("Could not stop running container %s, cannot remove. ", id);
            ERROR("Could not stop running container %s, cannot remove", id);
            ret = -1;
            goto reset_removal_progress;
        }
    }

#ifdef ENABLE_PLUGIN
    plugin_event_container_post_remove(cont);
#endif

    ret = do_delete_container(cont);
    if (ret != 0) {
        goto reset_removal_progress;
    }

    goto out;

reset_removal_progress:
    container_state_reset_removal_in_progress(cont->state);
out:
    return ret;
}

static int send_signal_to_process(pid_t pid, unsigned long long start_time, uint32_t stop_signal, uint32_t signal)
{
    if (util_process_alive(pid, start_time) == false) {
        if (signal == stop_signal || signal == SIGKILL) {
            WARN("Process %d is not alive", pid);
            return 0;
        } else {
            ERROR("Process (pid=%d) is not alive, can not kill with signal %u", pid, signal);
            return -1;
        }
    } else {
        int ret = kill(pid, (int)signal);
        if (ret < 0) {
            SYSERROR("Can not kill process (pid=%d) with signal %u", pid, signal);
            return -1;
        }
    }

    return 0;
}

static uint32_t container_stop_signal(container_t *cont)
{
    int signal = 0;

    if (cont->common_config->config != NULL && cont->common_config->config->stop_signal != NULL) {
        signal = util_sig_parse(cont->common_config->config->stop_signal);
    }

    if (signal <= 0) {
        signal = SIGTERM;
    }

    return (uint32_t)signal;
}

static int kill_with_signal(container_t *cont, uint32_t signal)
{
    int ret = 0;
    int nret = 0;
    const char *id = cont->common_config->id;
    uint32_t stop_signal = container_stop_signal(cont);
    bool need_unpause = container_is_paused(cont->state);
    rt_resume_params_t params = { 0 };
    char annotations[EVENT_EXTRA_ANNOTATION_MAX] = { 0 };

    if (container_exit_on_next(cont)) {
        ERROR("Failed to cancel restart manager");
        ret = -1;
        goto out;
    }
    container_state_set_has_been_manual_stopped(cont->state);
    (void)container_state_to_disk(cont);

    if (!container_is_running(cont->state)) {
        INFO("Container %s is already stopped", id);
        ret = 0;
        goto out;
    }
    if (container_is_restarting(cont->state)) {
        INFO("Container %s is currently restarting we do not need to send the signal to the process", id);
        ret = 0;
        goto out;
    }

    rt_kill_params_t kill_params = {
        .pid = cont->state->state->pid,
        .start_time = cont->state->state->start_time,
        .signal = signal,
        .stop_signal = stop_signal,
    };
    ret = runtime_kill(id, cont->runtime, &kill_params);
    if (ret != 0) {
        ERROR("Failed to send signal to container %s with signal %u", id, signal);
    }
    if (signal == SIGKILL && need_unpause) {
        params.rootpath = cont->root_path;
        params.state = cont->state_path;
        if (runtime_resume(id, cont->runtime, &params) != 0) {
            ERROR("Cannot unpause container: %s", id);
            ret = -1;
            goto out;
        }
    }

    nret = snprintf(annotations, sizeof(annotations), "signal=%u", signal);
    if (nret < 0 || (size_t)nret >= sizeof(annotations)) {
        ERROR("Failed to get signal string");
        ret = -1;
        goto out;
    }

    (void)isulad_monitor_send_container_event(id, KILL, -1, 0, NULL, annotations);

out:
    return ret;
}

static int force_kill(container_t *cont)
{
    int ret = 0;
    const char *id = cont->common_config->id;
    uint32_t stop_signal = container_stop_signal(cont);

    ret = kill_with_signal(cont, SIGKILL);
    if (ret != 0) {
        WARN("Failed to stop Container(%s), try to wait 'STOPPED' for 90 seconds", id);
    }
    ret = container_wait_stop(cont, 90);
    if (ret != 0) {
        ERROR("Container(%s) stuck for 90 seconds, try to kill the monitor of container", id);
        ret = send_signal_to_process(cont->state->state->p_pid, cont->state->state->p_start_time, stop_signal, SIGKILL);
        if (ret != 0) {
            ERROR("Container stuck for 90 seconds and failed to kill the monitor of container, "
                  "please check the config");
            isulad_set_error_message("Container stuck for 90 seconds "
                                     "and failed to kill the monitor of container, please check configuration files");
            goto out;
        }
        ret = container_wait_stop(cont, -1);
    }
out:
    return ret;
}

int stop_container(container_t *cont, int timeout, bool force, bool restart)
{
    int ret = 0;
    char *id = NULL;
    uint32_t stop_signal = 0;

    if (cont == NULL) {
        ERROR("Invalid input arguments");
        return -1;
    }

    id = cont->common_config->id;

    container_lock(cont);

    if (!container_is_running(cont->state)) {
        INFO("Container %s is already stopped", id);
        ret = 0;
        goto out;
    }

    container_stop_health_checks(cont);

    // set AutoRemove flag to false before stop so the container won't be
    // removed during restart process
    if (restart) {
        cont->hostconfig->auto_remove = false;
    }

    stop_signal = container_stop_signal(cont);

    if (!force) {
        ret = kill_with_signal(cont, stop_signal);
        if (ret) {
            ERROR("Failed to grace shutdown container %s", id);
        }
        ret = container_wait_stop(cont, timeout);
        if (ret != 0) {
            ERROR("Failed to wait Container(%s) 'STOPPED' for %d seconds, force killing", id, timeout);
            ret = force_kill(cont);
            if (ret != 0) {
                ERROR("Failed to force kill container %s", id);
                goto out;
            }
        }
    } else {
        ret = force_kill(cont);
        if (ret != 0) {
            ERROR("Failed to force kill container %s", id);
            goto out;
        }
    }

#ifdef ENABLE_CRI_API_V1
    if (cont->common_config->sandbox_info != NULL &&
        sandbox_purge_container(cont->common_config) != 0) {
        ERROR("Failed to remove container %s from sandbox", id);
        ret = -1;
        goto out;
    }
#endif

out:
    if (restart) {
        cont->hostconfig->auto_remove = cont->hostconfig->auto_remove_bak;
    }
    container_unlock(cont);
    return ret;
}

int kill_container(container_t *cont, uint32_t signal)
{
    int ret = 0;
    char *id = NULL;

    id = cont->common_config->id;

    container_lock(cont);

    if (!container_is_running(cont->state)) {
        ERROR("Cannot kill container: Container %s is not running", id);
        isulad_set_error_message("Cannot kill container: Container %s is not running", id);
        ret = -1;
        goto out;
    }

    if (signal == 0 || signal == SIGKILL) {
        ret = force_kill(cont);
    } else {
        ret = kill_with_signal(cont, signal);
    }

    if (ret != 0) {
        ret = -1;
        goto out;
    }

out:
    container_unlock(cont);
    return ret;
}

int cleanup_mounts_by_id(const char *id, const char *engine_root_path)
{
    char target[PATH_MAX] = { 0 };
    int nret = 0;

    nret = snprintf(target, PATH_MAX, "%s/%s", engine_root_path, id);
    if (nret < 0 || (size_t)nret >= PATH_MAX) {
        ERROR("Sprintf failed");
        return -1;
    }

    if (!util_deal_with_mount_info(util_umount_residual_shm, target)) {
        ERROR("Cleanup mounts failed");
        return -1;
    }

    return 0;
}

void umount_share_shm(container_t *cont)
{
    if (container_has_mount_for(cont, "/dev/shm")) {
        return;
    }
    if (cont->hostconfig == NULL) {
        return;
    }
    // ignore shm of system container
    if (cont->hostconfig->system_container) {
        return;
    }
    if (cont->hostconfig->ipc_mode == NULL || namespace_is_shareable(cont->hostconfig->ipc_mode)) {
        if (cont->common_config == NULL || cont->common_config->shm_path == NULL) {
            return;
        }

        INFO("Umounting share shm: %s", cont->common_config->shm_path);
        if (umount2(cont->common_config->shm_path, MNT_DETACH)) {
            SYSERROR("Failed to umount the target: %s", cont->common_config->shm_path);
        }
    }
}

void umount_host_channel(const host_config_host_channel *host_channel)
{
    if (host_channel == NULL) {
        return;
    }

    if (util_detect_mounted(host_channel->path_on_host)) {
        if (umount2(host_channel->path_on_host, MNT_DETACH)) {
            ERROR("Failed to umount the target: %s", host_channel->path_on_host);
        }
    }
    if (util_recursive_rmdir(host_channel->path_on_host, 0)) {
        ERROR("Failed to delete host path: %s", host_channel->path_on_host);
    }
}

static int do_append_process_exec_env(const char **default_env, defs_process *spec)
{
    int ret = 0;
    size_t new_size = 0;
    size_t old_size = 0;
    size_t i = 0;
    size_t j = 0;
    char **temp = NULL;
    char **default_kv = NULL;
    char **custom_kv = NULL;
    size_t default_env_len = util_array_len(default_env);

    if (default_env_len == 0) {
        return 0;
    }

    if (default_env_len > LIST_ENV_SIZE_MAX - spec->env_len) {
        ERROR("The length of envionment variables is too long, the limit is %lld", LIST_ENV_SIZE_MAX);
        isulad_set_error_message("The length of envionment variables is too long, the limit is %d", LIST_ENV_SIZE_MAX);
        ret = -1;
        goto out;
    }
    // new_size = old_size + default_env_len + 1(null as terminator)
    new_size = (spec->env_len + default_env_len + 1) * sizeof(char *);
    old_size = spec->env_len * sizeof(char *);
    ret = util_mem_realloc((void **)&temp, new_size, spec->env, old_size);
    if (ret != 0) {
        ERROR("Failed to realloc memory for envionment variables");
        ret = -1;
        goto out;
    }

    spec->env = temp;
    for (i = 0; i < default_env_len; i++) {
        bool found = false;
        default_kv = util_string_split(default_env[i], '=');
        if (default_kv == NULL) {
            continue;
        }

        for (j = 0; j < spec->env_len; j++) {
            custom_kv = util_string_split(spec->env[i], '=');
            if (custom_kv == NULL) {
                continue;
            }
            if (strcmp(default_kv[0], custom_kv[0]) == 0) {
                found = true;
            }
            util_free_array(custom_kv);
            custom_kv = NULL;
            if (found) {
                break;
            }
        }

        if (!found) {
            spec->env[spec->env_len] = util_strdup_s(default_env[i]);
            spec->env_len++;
        }
        util_free_array(default_kv);
        default_kv = NULL;
    }
out:
    return ret;
}

static int append_necessary_process_env(bool tty, const container_config *container_spec, defs_process *spec)
{
    int ret = 0;
    int nret = 0;
    char **default_env = NULL;
    char host_name_str[MAX_HOST_NAME_LEN + 10] = { 0 };

    if (util_array_append(&default_env, "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin") != 0) {
        ERROR("Failed to append default exec env");
        ret = -1;
        goto out;
    }

    if (container_spec != NULL && container_spec->hostname != NULL) {
        nret = snprintf(host_name_str, sizeof(host_name_str), "HOSTNAME=%s", container_spec->hostname);
        if (nret < 0 || (size_t)nret >= sizeof(host_name_str)) {
            ERROR("hostname is too long");
            ret = -1;
            goto out;
        }
        if (util_array_append(&default_env, host_name_str) != 0) {
            ERROR("Failed to append default exec env");
            ret = -1;
            goto out;
        }
    }

    if (tty) {
        if (util_array_append(&default_env, "TERM=xterm") != 0) {
            ERROR("Failed to append default exec env");
            ret = -1;
            goto out;
        }
    }

    ret = do_append_process_exec_env((const char **)default_env, spec);

out:
    util_free_array(default_env);
    return ret;
}

static int merge_exec_from_container_env(defs_process *spec, const container_config *container_spec)
{
    int ret = 0;
    size_t i = 0;

    if (container_spec == NULL) {
        return 0;
    }

    if (container_spec->env_len > LIST_ENV_SIZE_MAX - spec->env_len) {
        ERROR("The length of envionment variables is too long, the limit is %lld", LIST_ENV_SIZE_MAX);
        isulad_set_error_message("The length of envionment variables is too long, the limit is %d", LIST_ENV_SIZE_MAX);
        ret = -1;
        goto out;
    }

    for (i = 0; i < container_spec->env_len; i++) {
        ret = util_array_append(&(spec->env), container_spec->env[i]);
        if (ret != 0) {
            ERROR("Failed to append container env to exec process env");
            goto out;
        }
        spec->env_len++;
    }

out:
    return ret;
}

static int merge_envs_from_request_env(defs_process *spec, const char **envs, size_t env_len)
{
    int ret = 0;
    size_t i = 0;

    if (env_len > LIST_ENV_SIZE_MAX - spec->env_len) {
        ERROR("The length of envionment variables is too long, the limit is %lld", LIST_ENV_SIZE_MAX);
        isulad_set_error_message("The length of envionment variables is too long, the limit is %d", LIST_ENV_SIZE_MAX);
        ret = -1;
        goto out;
    }

    for (i = 0; i < env_len; i++) {
        ret = util_array_append(&(spec->env), envs[i]);
        if (ret != 0) {
            ERROR("Failed to append request env to exec process env");
            goto out;
        }
        spec->env_len++;
    }

out:
    return ret;
}

static int dup_defs_process_user(defs_process_user *src, defs_process_user **dst)
{
    int ret = 0;
    size_t i;

    if (src == NULL) {
        return 0;
    }

    *dst = (defs_process_user *)util_common_calloc_s(sizeof(defs_process_user));
    if (*dst == NULL) {
        ERROR("Out of memory");
        return -1;
    }

    (*dst)->username = util_strdup_s(src->username);
    (*dst)->uid = src->uid;
    (*dst)->gid = src->gid;

    if (src->additional_gids_len != 0) {
        (*dst)->additional_gids = util_smart_calloc_s(sizeof(gid_t), src->additional_gids_len);
        if ((*dst)->additional_gids == NULL) {
            ERROR("Out of memory");
            ret = -1;
            goto out;
        }
        (*dst)->additional_gids_len = src->additional_gids_len;
        for (i = 0; i < src->additional_gids_len; i++) {
            (*dst)->additional_gids[i] = src->additional_gids[i];
        }
    }

out:
    return ret;
}

static int append_rlimit_from_oci_spec(defs_process *spec, const oci_runtime_spec *oci_spec)
{
    size_t j;

    spec->rlimits = (defs_process_rlimits_element **)util_smart_calloc_s(sizeof(defs_process_rlimits_element *),
                                                                         (size_t)oci_spec->process->rlimits_len);
    if (spec->rlimits == NULL) {
        ERROR("Out of memory");
        return -1;
    }

    for (j = 0; j < oci_spec->process->rlimits_len; j++) {
        spec->rlimits[j] = util_common_calloc_s(sizeof(defs_process_rlimits_element));
        if (spec->rlimits[j] == NULL) {
            ERROR("Out of memory");
            return -1;
        }
        spec->rlimits[j]->type = util_strdup_s(oci_spec->process->rlimits[j]->type);
        spec->rlimits[j]->hard = oci_spec->process->rlimits[j]->hard;
        spec->rlimits[j]->soft = oci_spec->process->rlimits[j]->soft;
        spec->rlimits_len++;
    }

    return 0;
}

static defs_process *make_exec_process_spec(const container_config *container_spec, defs_process_user *puser,
                                            const char *runtime, const container_exec_request *request, const oci_runtime_spec *oci_spec)
{
    int ret = 0;
    defs_process *spec = NULL;

    spec = util_common_calloc_s(sizeof(defs_process));
    if (spec == NULL) {
        return NULL;
    }

    if (strcasecmp(runtime, "lcr") != 0) {
        // for oci runtime:
        // step 1: merge env from container;
        ret = merge_exec_from_container_env(spec, container_spec);
        if (ret != 0) {
            ERROR("Failed to dup args for exec process spec");
            goto err_out;
        }

        // step 2: merge process env including PATH, HOATNAME and TERM(if tty is true);
        ret = append_necessary_process_env(request->tty, container_spec, spec);
        if (ret != 0) {
            ERROR("Failed to append necessary for exec process spec");
            goto err_out;
        }

        ret = append_rlimit_from_oci_spec(spec, oci_spec);
        if (ret != 0) {
            ERROR("Failed to append rlimit for exec process spec");
            goto err_out;
        }

        spec->no_new_privileges = oci_spec->process->no_new_privileges;
    }

    // for oci runtime:
    // step 3 : Finally, merge env from request to ensure that the env in the request is not overwritten;
    // for lcr:
    // since the container env and the process env have been stored in the config file, lcr only needs to merge the env in the request.
    ret = merge_envs_from_request_env(spec, (const char **)request->env, request->env_len);
    if (ret != 0) {
        ERROR("Failed to dup args for exec process spec");
        goto err_out;
    }

    ret = util_dup_array_of_strings((const char **)request->argv, request->argv_len, &(spec->args), &(spec->args_len));
    if (ret != 0) {
        ERROR("Failed to dup envs for exec process spec");
        goto err_out;
    }

    ret = dup_defs_process_user(puser, &(spec->user));
    if (ret != 0) {
        ERROR("Failed to dup process user for exec process spec");
        goto err_out;
    }

    spec->terminal = request->tty;

    if (container_spec != NULL && util_valid_str(container_spec->working_dir)) {
        spec->cwd = util_strdup_s(container_spec->working_dir);
    } else {
        spec->cwd = util_strdup_s("/");
    }

    return spec;

err_out:
    free_defs_process(spec);
    return NULL;
}

static int do_exec_container(const container_t *cont, const char *runtime, char * const console_fifos[],
                             defs_process_user *puser, const container_exec_request *request, int *exit_code)
{
    int ret = 0;
    char *engine_log_path = NULL;
    char *loglevel = NULL;
    char *logdriver = NULL;
    const char *id = cont->common_config->id;
    oci_runtime_spec *oci_spec = NULL;
    defs_process *process_spec = NULL;
    rt_exec_params_t params = { 0 };

    loglevel = conf_get_isulad_loglevel();
    if (loglevel == NULL) {
        ERROR("Exec: failed to get log level");
        ret = -1;
        goto out;
    }
    logdriver = conf_get_isulad_logdriver();
    if (logdriver == NULL) {
        ERROR("Exec: Failed to get log driver");
        ret = -1;
        goto out;
    }
    engine_log_path = conf_get_engine_log_file();
    if (strcmp(logdriver, "file") == 0 && engine_log_path == NULL) {
        ERROR("Exec: Log driver is file, but engine log path is NULL");
        ret = -1;
        goto out;
    }

    // lcr reads the config from the file and will not lose it.
    // so there is no need to get the config from oci_spec.
    if (strcasecmp(runtime, "lcr") != 0) {
        oci_spec = load_oci_config(cont->root_path, id);
        if (oci_spec == NULL) {
            ERROR("Failed to load oci config");
            ret = -1;
            goto out;
        }
    }

    process_spec = make_exec_process_spec(cont->common_config->config, puser, runtime, request, oci_spec);
    if (process_spec == NULL) {
        ERROR("Exec: Failed to make process spec");
        ret = -1;
        goto out;
    }

#ifdef ENABLE_CRI_API_V1
    if (cont->common_config->sandbox_info != NULL &&
        sandbox_prepare_exec(cont->common_config, request->suffix,
                             process_spec, (const char **)console_fifos, request->tty) != 0) {
        ERROR("Failed to prepare exec for container %s", id);
        ret = -1;
        goto out;
    }
#endif

    params.loglevel = loglevel;
    params.logpath = engine_log_path;
    params.console_fifos = (const char **)console_fifos;
    params.rootpath = cont->root_path;
    params.timeout = request->timeout;
    params.suffix = request->suffix;
    params.state = cont->state_path;
    params.spec = process_spec;
    params.attach_stdin = request->attach_stdin;
    params.workdir = request->workdir;

    if (runtime_exec(cont->common_config->id, runtime, &params, exit_code)) {
        ERROR("Runtime exec container failed");
        ret = -1;
        goto out;
    }

out:
    free(loglevel);
    free(engine_log_path);
    free(logdriver);
    free_defs_process(process_spec);
    free_oci_runtime_spec(oci_spec);

    return ret;
}

#ifdef ENABLE_CRI_API_V1
static int exec_prepare_vsock(const container_t *cont, const container_exec_request *request, int stdinfd,
                              struct io_write_wrapper *stdout_handler, struct io_write_wrapper *stderr_handler,
                              char **vsockpaths, int *sync_fd, pthread_t *thread_id)
{
    uint32_t cid;
    const char *task_address = cont->common_config->sandbox_info->task_address;
    if (!parse_vsock_path(task_address, &cid, NULL)) {
        ERROR("Failed to parse vsock path %s", task_address);
        return -1;
    }

    if (!request->attach_stdin && !request->attach_stdout && !request->attach_stderr) {
        return 0;
    }

    if (create_daemon_vsockpaths(cont->common_config->sandbox_info->id, cid, request->attach_stdin, request->attach_stdout,
                                 request->attach_stderr, vsockpaths) != 0) {
        return -1;
    }

    *sync_fd = eventfd(0, EFD_CLOEXEC);
    if (*sync_fd < 0) {
        SYSERROR("Failed to create eventfd");
        return -1;
    }

    return start_vsock_io_copy(request->suffix, *sync_fd, false, request->stdin, request->stdout, request->stderr, stdinfd,
                               stdout_handler, stderr_handler, (const char **)vsockpaths, thread_id);
}
#endif

static int exec_prepare_fifo(const container_t *cont, const container_exec_request *request, int stdinfd,
                             struct io_write_wrapper *stdout_handler, struct io_write_wrapper *stderr_handler,
                             char **fifos, char **fifopath, int *sync_fd, pthread_t *thread_id)
{
    int ret = 0;
    const char *id = cont->common_config->id;

    if (request->attach_stdin || request->attach_stdout || request->attach_stderr) {
        if (create_daemon_fifos(id, cont->runtime, request->attach_stdin, request->attach_stdout,
                                request->attach_stderr, "exec", fifos, fifopath)) {
            ret = -1;
            goto out;
        }

        *sync_fd = eventfd(0, EFD_CLOEXEC);
        if (*sync_fd < 0) {
            SYSERROR("Failed to create eventfd");
            ret = -1;
            goto out;
        }
        if (ready_copy_io_data(*sync_fd, false, request->stdin, request->stdout, request->stderr, stdinfd,
                               stdout_handler, stderr_handler, (const char **)fifos, thread_id)) {
            ret = -1;
            goto out;
        }
    }
out:
    return ret;
}

#ifdef ENABLE_CRI_API_V1
static bool is_vsock_supported(const container_t *cont)
{
    return cont->common_config->sandbox_info != NULL &&
           is_vsock_path(cont->common_config->sandbox_info->task_address);
}
#endif

static int exec_prepare_console(const container_t *cont, const container_exec_request *request, int stdinfd,
                                struct io_write_wrapper *stdout_handler, struct io_write_wrapper *stderr_handler,
                                char **io_addresses, char **iopath, int *sync_fd, pthread_t *thread_id)
{
#ifdef ENABLE_CRI_API_V1
    if (is_vsock_supported(cont)) {
        return exec_prepare_vsock(cont, request, stdinfd, stdout_handler, stderr_handler, io_addresses, sync_fd, thread_id);
    }
#endif
    return exec_prepare_fifo(cont, request, stdinfd, stdout_handler, stderr_handler, io_addresses, iopath, sync_fd,
                             thread_id);
}

static void exec_container_end(container_exec_response *response, const container_t *cont,
                               const char *exec_id, uint32_t cc,
                               int exit_code, int sync_fd, pthread_t thread_id)
{
#ifdef ENABLE_CRI_API_V1
    if (cont->common_config->sandbox_info != NULL &&
        sandbox_purge_exec(cont->common_config, exec_id) != 0) {
        ERROR("Failed to purge container for exec %s", exec_id);
    }
#endif
    if (response != NULL) {
        response->cc = cc;
        response->exit_code = (uint32_t)exit_code;
        if (g_isulad_errmsg != NULL) {
            response->errmsg = util_strdup_s(g_isulad_errmsg);
            DAEMON_CLEAR_ERRMSG();
        }
    }
    if (sync_fd >= 0 && cc != ISULAD_SUCCESS) {
        if (eventfd_write(sync_fd, 1) < 0) {
            SYSERROR("Failed to write eventfd");
        }
    }
    if (thread_id > 0) {
        if (pthread_join(thread_id, NULL) != 0) {
            ERROR("Failed to join thread: 0x%lx", thread_id);
        }
    }
    if (sync_fd >= 0) {
        close(sync_fd);
    }
}

static void cleanup_exec_console_io(const container_t *cont, const char *fifopath, const char *io_addresses[])
{
#ifdef ENABLE_CRI_API_V1
    if (is_vsock_supported(cont)) {
        delete_daemon_vsockpaths(cont->common_config->sandbox_info->id, io_addresses);
        return;
    }
#endif
    delete_daemon_fifos(fifopath, (const char **)io_addresses);
}

static int get_exec_user_info(const container_t *cont, const char *username, defs_process_user **puser)
{
    int ret = 0;

    *puser = util_common_calloc_s(sizeof(defs_process_user));
    if (*puser == NULL) {
        ERROR("Out of memory");
        return -1;
    }
    ret = im_get_user_conf(cont->common_config->image_type, cont->common_config->base_fs, cont->hostconfig, username,
                           *puser);
    if (ret != 0) {
        ERROR("Get user failed with '%s'", username ? username : "");
        ret = -1;
        goto out;
    }
out:
    return ret;
}
static void get_exec_command(const container_exec_request *request, char *exec_command, size_t len)
{
    size_t i;
    bool should_abbreviated = false;
    size_t start = 0;
    size_t end = 0;

    for (i = 0; i < request->argv_len; i++) {
        if (strlen(request->argv[i]) < len - strlen(exec_command)) {
            (void)strcat(exec_command, request->argv[i]);
            if (i != (request->argv_len - 1) && len - strlen(exec_command) > 1) {
                (void)strcat(exec_command, " ");
            }
        } else {
            should_abbreviated = true;
            goto out;
        }
    }

out:
    if (should_abbreviated) {
        if (strlen(exec_command) <= len - 1 - 3) {
            start = strlen(exec_command);
            end = start + 3;
        } else {
            start = len - 1 - 3;
            end = len - 1;
        }

        for (i = start; i < end; i++) {
            exec_command[i] = '.';
        }
    }
}

int exec_container(const container_t *cont, const container_exec_request *request, container_exec_response *response,
                   int stdinfd, struct io_write_wrapper *stdout_handler, struct io_write_wrapper *stderr_handler)
{
    int exit_code = 0;
    int sync_fd = -1;
    uint32_t cc = ISULAD_SUCCESS;
    char *id = NULL;
    char *io_addresses[3] = { NULL, NULL, NULL };
    char *iopath = NULL;
    pthread_t thread_id = 0;
    defs_process_user *puser = NULL;
    char exec_command[EVENT_ARGS_MAX] = { 0x00 };

    if (cont == NULL || request == NULL || response == NULL) {
        ERROR("Invalid NULL input");
        return -1;
    }

    id = cont->common_config->id;
    WARN("Event: {Object: %s, Type: execing}", id);

    get_exec_command(request, exec_command, sizeof(exec_command));
    (void)isulad_monitor_send_container_event(id, EXEC_CREATE, -1, 0, exec_command, NULL);

    if (container_is_in_gc_progress(id)) {
        isulad_set_error_message("You cannot exec container %s in garbage collector progress.", id);
        ERROR("You cannot exec container %s in garbage collector progress.", id);
        cc = ISULAD_ERR_EXEC;
        goto pack_response;
    }

    if (!container_is_running(cont->state)) {
        ERROR("Container %s is not running", id);
        isulad_set_error_message("Container %s is not running", id);
        cc = ISULAD_ERR_EXEC;
        goto pack_response;
    }

    if (container_is_paused(cont->state)) {
        ERROR("Container %s ispaused, unpause the container before exec", id);
        isulad_set_error_message("Container %s paused, unpause the container before exec", id);
        cc = ISULAD_ERR_EXEC;
        goto pack_response;
    }

    if (container_is_restarting(cont->state)) {
        ERROR("Container %s is currently restarting, wait until the container is running", id);
        isulad_set_error_message("Container %s is currently restarting, wait until the container is running", id);
        cc = ISULAD_ERR_EXEC;
        goto pack_response;
    }

    if (request->user != NULL) {
        if (get_exec_user_info(cont, request->user, &puser) != 0) {
            cc = ISULAD_ERR_EXEC;
            goto pack_response;
        }
    } else {
        if (cont->common_config->config != NULL && cont->common_config->config->user != NULL) {
            if (get_exec_user_info(cont, cont->common_config->config->user, &puser) != 0) {
                cc = ISULAD_ERR_EXEC;
                goto pack_response;
            }
        }
    }

    if (exec_prepare_console(cont, request, stdinfd, stdout_handler, stderr_handler, io_addresses, &iopath, &sync_fd,
                             &thread_id)) {
        cc = ISULAD_ERR_EXEC;
        goto pack_response;
    }
    (void)isulad_monitor_send_container_event(id, EXEC_START, -1, 0, exec_command, NULL);
    if (do_exec_container(cont, cont->runtime, (char * const *)io_addresses, puser, request, &exit_code)) {
        cc = ISULAD_ERR_EXEC;
        goto pack_response;
    }

    WARN("Event: {Object: %s, Type: execed with exit code %d}", id, exit_code);
    (void)isulad_monitor_send_container_event(id, EXEC_DIE, -1, 0, NULL, NULL);

pack_response:
    exec_container_end(response, cont, request->suffix, cc, exit_code, sync_fd, thread_id);
    cleanup_exec_console_io(cont, iopath, (const char **)io_addresses);
    free(io_addresses[0]);
    free(io_addresses[1]);
    free(io_addresses[2]);
    free(iopath);
    free_defs_process_user(puser);

    return (cc == ISULAD_SUCCESS) ? 0 : -1;
}
