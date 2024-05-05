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
 * Description: provide cni network plugin
 *********************************************************************************/
#include "cri_helpers.h"
#include "constants.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <sys/utsname.h>
#include <utility>

#include <isula_libutils/auto_cleanup.h>
#include <isula_libutils/log.h>
#include <isula_libutils/parse_common.h>

#include "cri_constants.h"
#include "cxxutils.h"
#include "path.h"
#include "utils.h"
#include "service_container_api.h"
#include "isulad_config.h"
#include "sha256.h"

namespace CRIHelpers {
const std::string Constants::POD_NETWORK_ANNOTATION_KEY { "network.alpha.kubernetes.io/network" };
const std::string Constants::CONTAINER_TYPE_LABEL_KEY { "cri.isulad.type" };
const std::string Constants::CONTAINER_TYPE_LABEL_SANDBOX { "podsandbox" };
const std::string Constants::CONTAINER_TYPE_LABEL_CONTAINER { "container" };
const std::string Constants::CONTAINER_LOGPATH_LABEL_KEY { "cri.container.logpath" };
const std::string Constants::CONTAINER_HUGETLB_ANNOTATION_KEY { "cri.container.hugetlblimit" };
const std::string Constants::SANDBOX_ID_LABEL_KEY { "cri.sandbox.id" };
const std::string Constants::POD_SANDBOX_KEY { "sandboxkey" };
const std::string Constants::KUBERNETES_CONTAINER_NAME_LABEL { "io.kubernetes.container.name" };
const std::string Constants::POD_INFRA_CONTAINER_NAME { "POD" };
const std::string Constants::DOCKER_IMAGEID_PREFIX { "docker://" };
const std::string Constants::DOCKER_PULLABLE_IMAGEID_PREFIX { "docker-pullable://" };
const std::string Constants::RUNTIME_READY { "RuntimeReady" };
const std::string Constants::NETWORK_READY { "NetworkReady" };
const std::string Constants::POD_CHECKPOINT_KEY { "cri.sandbox.isulad.checkpoint" };
const std::string Constants::CONTAINER_TYPE_ANNOTATION_KEY { "io.kubernetes.cri.container-type" };
const std::string Constants::CONTAINER_NAME_ANNOTATION_KEY { "io.kubernetes.cri.container-name" };
const std::string Constants::CONTAINER_ATTEMPT_ANNOTATION_KEY { "io.kubernetes.cri.container-attempt" };
const std::string Constants::CONTAINER_TYPE_ANNOTATION_CONTAINER { "container" };
const std::string Constants::CONTAINER_TYPE_ANNOTATION_SANDBOX { "sandbox" };
const std::string Constants::SANDBOX_ID_ANNOTATION_KEY { "io.kubernetes.cri.sandbox-id" };
const std::string Constants::SANDBOX_NAMESPACE_ANNOTATION_KEY { "io.kubernetes.cri.sandbox-namespace" };
const std::string Constants::SANDBOX_NAME_ANNOTATION_KEY { "io.kubernetes.cri.sandbox-name" };
const std::string Constants::SANDBOX_UID_ANNOTATION_KEY { "io.kubernetes.cri.sandbox-uid" };
const std::string Constants::SANDBOX_ATTEMPT_ANNOTATION_KEY { "io.kubernetes.cri.sandbox-attempt" };
const std::string Constants::NET_PLUGIN_EVENT_POD_CIDR_CHANGE { "pod-cidr-change" };
const std::string Constants::NET_PLUGIN_EVENT_POD_CIDR_CHANGE_DETAIL_CIDR { "pod-cidr" };
const std::string Constants::CNI_MUTL_NET_EXTENSION_KEY { "extension.network.kubernetes.io/cni" };
const std::string Constants::CNI_MUTL_NET_EXTENSION_ARGS_KEY { "CNI_MUTLINET_EXTENSION" };
const std::string Constants::CNI_ARGS_EXTENSION_PREFIX_KEY { "extension.network.kubernetes.io/cniargs/" };
const std::string Constants::CNI_CAPABILITIES_BANDWIDTH_INGRESS_KEY { "kubernetes.io/ingress-bandwidth" };
const std::string Constants::CNI_CAPABILITIES_BANDWIDTH_ENGRESS_KEY { "kubernetes.io/engress-bandwidth" };
const std::string Constants::IMAGE_NAME_ANNOTATION_KEY { "io.kubernetes.cri.image-name" };
// Usually, the format of level is "s0:c60,c525" or "s0-s0:c40.c23"
const std::string Constants::SELINUX_LABEL_LEVEL_PATTERN { "^s[0-9](-s[0-9])?(:c[0-9]{1,4}(\\.c[0-9]{1,4})?(,c[0-9]{1,4}(\\.c[0-9]{1,4})?)*)?$" };

const char *InternalLabelKeys[] = { CRIHelpers::Constants::CONTAINER_TYPE_LABEL_KEY.c_str(),
                                    CRIHelpers::Constants::CONTAINER_LOGPATH_LABEL_KEY.c_str(),
                                    CRIHelpers::Constants::SANDBOX_ID_LABEL_KEY.c_str(), nullptr
                                  };

auto GetDefaultSandboxImage(Errors &err) -> std::string
{
    const std::string defaultPodSandboxImageName { "pause" };
    const std::string defaultPodSandboxImageVersion { "3.0" };
    std::string machine;
    struct utsname uts {};

    if (uname(&uts) < 0) {
        err.SetError("Failed to read host arch.");
        return "";
    }

    if (strcasecmp("i386", uts.machine) == 0) {
        machine = "386";
    } else if ((strcasecmp("x86_64", uts.machine) == 0) || (strcasecmp("x86-64", uts.machine) == 0)) {
        machine = "amd64";
    } else if (strcasecmp("aarch64", uts.machine) == 0) {
        machine = "aarch64";
    } else if ((strcasecmp("armhf", uts.machine) == 0) || (strcasecmp("armel", uts.machine) == 0) ||
               (strcasecmp("arm", uts.machine) == 0)) {
        machine = "aarch";
    } else {
        machine = uts.machine;
    }
    return defaultPodSandboxImageName + "-" + machine + ":" + defaultPodSandboxImageVersion;
}

auto MakeLabels(const google::protobuf::Map<std::string, std::string> &mapLabels, Errors &error)
-> json_map_string_string *
{
    json_map_string_string *labels = (json_map_string_string *)util_common_calloc_s(sizeof(json_map_string_string));
    if (labels == nullptr) {
        ERROR("Out of memory");
        return nullptr;
    }

    if (!mapLabels.empty()) {
        if (mapLabels.size() > LIST_SIZE_MAX) {
            error.Errorf("Labels list is too long, the limit is %d", LIST_SIZE_MAX);
            goto cleanup;
        }
        for (auto &iter : mapLabels) {
            if (append_json_map_string_string(labels, iter.first.c_str(), iter.second.c_str()) != 0) {
                ERROR("Failed to append string");
                goto cleanup;
            }
        }
    }
    return labels;
cleanup:
    free_json_map_string_string(labels);
    return nullptr;
}

auto MakeAnnotations(const google::protobuf::Map<std::string, std::string> &mapAnnotations, Errors &error)
-> json_map_string_string *
{
    json_map_string_string *annotations =
        (json_map_string_string *)util_common_calloc_s(sizeof(json_map_string_string));
    if (annotations == nullptr) {
        ERROR("Out of memory");
        return nullptr;
    }

    if (!mapAnnotations.empty()) {
        if (mapAnnotations.size() > LIST_SIZE_MAX) {
            error.Errorf("Annotations list is too long, the limit is %d", LIST_SIZE_MAX);
            goto cleanup;
        }
        for (auto &iter : mapAnnotations) {
            if (append_json_map_string_string(annotations, iter.first.c_str(), iter.second.c_str()) != 0) {
                ERROR("Failed to append string");
                goto cleanup;
            }
        }
    }
    return annotations;
cleanup:
    free_json_map_string_string(annotations);
    return nullptr;
}

void ProtobufAnnoMapToStd(const google::protobuf::Map<std::string, std::string> &annotations,
                          std::map<std::string, std::string> &newAnnos)
{
    for (auto &iter : annotations) {
        newAnnos.insert(std::pair<std::string, std::string>(iter.first, iter.second));
    }
}

static auto IsSandboxLabel(json_map_string_string *input) -> bool
{
    bool is_sandbox_label { false };

    for (size_t j = 0; j < input->len; j++) {
        if (strcmp(input->keys[j], CRIHelpers::Constants::CONTAINER_TYPE_LABEL_KEY.c_str()) == 0 &&
            strcmp(input->values[j], CRIHelpers::Constants::CONTAINER_TYPE_LABEL_SANDBOX.c_str()) == 0) {
            is_sandbox_label = true;
            break;
        }
    }

    return is_sandbox_label;
}

void ExtractLabels(json_map_string_string *input, google::protobuf::Map<std::string, std::string> &labels)
{
    if (input == nullptr) {
        return;
    }

    for (size_t i = 0; i < input->len; i++) {
        bool internal = false;
        const char **internal_key = InternalLabelKeys;
        // Check if the key is used internally by the shim.
        while (*internal_key != nullptr) {
            if (strcmp(input->keys[i], *internal_key) == 0) {
                internal = true;
                break;
            }
            internal_key++;
        }
        if (internal) {
            continue;
        }

        // Delete the container name label for the sandbox. It is added
        // in the shim, should not be exposed via CRI.
        if (strcmp(input->keys[i], Constants::KUBERNETES_CONTAINER_NAME_LABEL.c_str()) == 0) {
            bool is_sandbox_label = IsSandboxLabel(input);
            if (is_sandbox_label) {
                continue;
            }
        }

        labels[input->keys[i]] = input->values[i];
    }
}

void ExtractAnnotations(json_map_string_string *input, google::protobuf::Map<std::string, std::string> &annotations)
{
    if (input == nullptr) {
        return;
    }

    for (size_t i = 0; i < input->len; i++) {
        annotations[input->keys[i]] = input->values[i];
    }
}

auto FiltersAdd(defs_filters *filters, const std::string &key, const std::string &value) -> int
{
    if (filters == nullptr) {
        return -1;
    }

    size_t len = filters->len + 1;
    char **keys = (char **)util_smart_calloc_s(sizeof(char *), len);
    if (keys == nullptr) {
        ERROR("Out of memory");
        return -1;
    }
    json_map_string_bool **vals = (json_map_string_bool **)util_smart_calloc_s(sizeof(json_map_string_bool *), len);
    if (vals == nullptr) {
        free(keys);
        ERROR("Out of memory");
        return -1;
    }

    if (filters->len != 0u) {
        (void)memcpy(keys, filters->keys, filters->len * sizeof(char *));

        (void)memcpy(vals, filters->values, filters->len * sizeof(json_map_string_bool *));
    }
    free(filters->keys);
    filters->keys = keys;
    free(filters->values);
    filters->values = vals;

    filters->values[filters->len] = (json_map_string_bool *)util_common_calloc_s(sizeof(json_map_string_bool));
    if (filters->values[filters->len] == nullptr) {
        ERROR("Out of memory");
        return -1;
    }
    if (append_json_map_string_bool(filters->values[filters->len], value.c_str(), true) != 0) {
        ERROR("Append failed");
        return -1;
    }

    filters->keys[filters->len] = util_strdup_s(key.c_str());
    filters->len++;
    return 0;
}

auto FiltersAddLabel(defs_filters *filters, const std::string &key, const std::string &value) -> int
{
    if (filters == nullptr) {
        return -1;
    }
    return FiltersAdd(filters, "label", key + "=" + value);
}

auto InspectImageByID(const std::string &imageID, Errors &err) -> imagetool_image_summary *
{
    im_summary_request *request { nullptr };
    im_summary_response *response { nullptr };
    imagetool_image_summary *image { nullptr };

    if (imageID.empty()) {
        err.SetError("Empty image ID");
        return nullptr;
    }

    request = (im_summary_request *)util_common_calloc_s(sizeof(im_summary_request));
    if (request == nullptr) {
        ERROR("Out of memory");
        err.SetError("Out of memory");
        return nullptr;
    }
    request->image.image = util_strdup_s(imageID.c_str());

    if (im_image_summary(request, &response) != 0) {
        if (response != nullptr && response->errmsg != nullptr) {
            err.SetError(response->errmsg);
        } else {
            err.SetError("Failed to call summary image");
        }
        goto cleanup;
    }

    if (response->image_summary != nullptr) {
        image = response->image_summary;
        response->image_summary = nullptr;
    }

cleanup:
    free_im_summary_request(request);
    free_im_summary_response(response);
    return image;
}

auto ToPullableImageID(const char *image_name, const char *image_ref) -> std::string
{
    // Default to the image ID, but if RepoDigests is not empty, use
    // the first digest instead.

    std::string imageID;

    if (image_name != nullptr) {
        imageID = Constants::DOCKER_IMAGEID_PREFIX + image_name;
    }

    if (image_ref != nullptr) {
        imageID = Constants::DOCKER_PULLABLE_IMAGEID_PREFIX + image_ref;
    }

    return imageID;
}

// IsContainerNotFoundError checks whether the error is container not found error.
auto IsContainerNotFoundError(const std::string &err) -> bool
{
    return err.find("No such container:") != std::string::npos ||
           err.find("No such image or container") != std::string::npos;
}

// IsImageNotFoundError checks whether the error is Image not found error.
auto IsImageNotFoundError(const std::string &err) -> bool
{
    return err.find("No such image:") != std::string::npos;
}

auto GetNetworkPlaneFromPodAnno(const std::map<std::string, std::string> &annotations, Errors &error)
-> cri_pod_network_container *
{
    auto iter = annotations.find(CRIHelpers::Constants::POD_NETWORK_ANNOTATION_KEY);

    cri_pod_network_container *result { nullptr };
    if (iter != annotations.end()) {
        parser_error err = nullptr;
        result = cri_pod_network_container_parse_data(iter->second.c_str(), nullptr, &err);
        if (err != nullptr) {
            error.Errorf("parse pod network json: %s failed: %s", iter->second.c_str(), err);
        }
        free(err);
    }

    return result;
}

auto CreateCheckpoint(CRI::PodSandboxCheckpoint &checkpoint, Errors &error) -> std::string
{
    cri_checkpoint *criCheckpoint { nullptr };
    struct parser_context ctx {
        OPT_GEN_SIMPLIFY, 0
    };
    parser_error err { nullptr };
    char *jsonStr { nullptr };
    char *digest { nullptr };
    std::string result;

    checkpoint.CheckpointToCStruct(&criCheckpoint, error);
    if (error.NotEmpty()) {
        goto out;
    }
    free(criCheckpoint->checksum);
    criCheckpoint->checksum = nullptr;
    jsonStr = cri_checkpoint_generate_json(criCheckpoint, &ctx, &err);
    if (jsonStr == nullptr) {
        error.Errorf("Generate cri checkpoint json failed: %s", err);
        goto out;
    }

    digest = sha256_digest_str(jsonStr);
    if (digest == nullptr) {
        error.Errorf("Failed to calculate digest");
        goto out;
    }

    checkpoint.SetCheckSum(digest);
    if (checkpoint.GetCheckSum().empty()) {
        error.SetError("checksum is empty");
        goto out;
    }
    criCheckpoint->checksum = util_strdup_s(checkpoint.GetCheckSum().c_str());

    free(jsonStr);
    jsonStr = cri_checkpoint_generate_json(criCheckpoint, &ctx, &err);
    if (jsonStr == nullptr) {
        error.Errorf("Generate cri checkpoint json failed: %s", err);
        goto out;
    }

    result = jsonStr;
out:
    free(digest);
    free(err);
    free(jsonStr);
    free_cri_checkpoint(criCheckpoint);
    return result;
}

void GetCheckpoint(const std::string &jsonCheckPoint, CRI::PodSandboxCheckpoint &checkpoint, Errors &error)
{
    cri_checkpoint *criCheckpoint { nullptr };
    struct parser_context ctx {
        OPT_GEN_SIMPLIFY, 0
    };
    parser_error err { nullptr };
    std::string tmpChecksum;
    char *jsonStr { nullptr };
    char *storeChecksum { nullptr };
    char *digest { nullptr };

    criCheckpoint = cri_checkpoint_parse_data(jsonCheckPoint.c_str(), &ctx, &err);
    if (criCheckpoint == nullptr) {
        ERROR("Failed to unmarshal checkpoint, removing checkpoint. ErrMsg: %s", err);
        error.SetError("Failed to unmarshal checkpoint");
        goto out;
    }

    tmpChecksum = criCheckpoint->checksum;
    storeChecksum = criCheckpoint->checksum;
    criCheckpoint->checksum = nullptr;
    jsonStr = cri_checkpoint_generate_json(criCheckpoint, &ctx, &err);
    criCheckpoint->checksum = storeChecksum;
    if (jsonStr == nullptr) {
        error.Errorf("Generate cri json str failed: %s", err);
        goto out;
    }

    digest = sha256_digest_str(jsonStr);
    if (digest == nullptr) {
        error.Errorf("Failed to calculate digest");
        goto out;
    }
    if (tmpChecksum != digest) {
        ERROR("Checksum of checkpoint is not valid");
        error.SetError("checkpoint is corrupted");
        goto out;
    }

    checkpoint.CStructToCheckpoint(criCheckpoint, error);
out:
    free(digest);
    free(jsonStr);
    free(err);
    free_cri_checkpoint(criCheckpoint);
}

auto InspectContainer(const std::string &Id, Errors &err, bool with_host_config) -> container_inspect *
{
    container_inspect *inspect_data { nullptr };
    inspect_data = inspect_container((const char *)Id.c_str(), INSPECT_TIMEOUT_SEC, with_host_config);
    if (inspect_data == nullptr) {
        err.Errorf("Failed to call inspect service %s", Id.c_str());
    }

    return inspect_data;
}

int32_t ToInt32Timeout(int64_t timeout)
{
    if (timeout > INT32_MAX) {
        return INT32_MAX;
    } else if (timeout < INT32_MIN) {
        return INT32_MIN;
    }

    return (int32_t)timeout;
}

void GetContainerLogPath(const std::string &containerID, std::string &path, std::string &realPath, Errors &error)
{
    container_inspect *info = InspectContainer(containerID, error, false);
    if (info == nullptr || error.NotEmpty()) {
        error.Errorf("failed to inspect container %s: %s", containerID.c_str(), error.GetCMessage());
        return;
    }

    if (info->config != nullptr && (info->config->labels != nullptr)) {
        for (size_t i = 0; i < info->config->labels->len; i++) {
            if (strcmp(info->config->labels->keys[i], CRIHelpers::Constants::CONTAINER_LOGPATH_LABEL_KEY.c_str()) == 0 &&
                strcmp(info->config->labels->values[i], "") != 0) {
                path = std::string(info->config->labels->values[i]);
                break;
            }
        }
    }

    if (info->log_path != nullptr && strcmp(info->log_path, "") != 0) {
        realPath = std::string(info->log_path);
    }
    free_container_inspect(info);
}

// CreateContainerLogSymlink creates the symlink for container log.
void RemoveContainerLogSymlink(const std::string &containerID, Errors &error)
{
    std::string path, realPath;
    GetContainerLogPath(containerID, path, realPath, error);
    if (error.NotEmpty()) {
        error.Errorf("Failed to get container %s log path: %s", containerID.c_str(), error.GetCMessage());
        return;
    }

    if (!path.empty()) {
        // Only remove the symlink when container log path is specified.
        if (util_path_remove(path.c_str()) != 0 && errno != ENOENT) {
            SYSERROR("Failed to remove container %s log symlink %s.", containerID.c_str(), path);
            error.Errorf("Failed to remove container %s log symlink %s.", containerID.c_str(), path);
        }
    }
}

void CreateContainerLogSymlink(const std::string &containerID, Errors &error)
{
    std::string path, realPath;

    GetContainerLogPath(containerID, path, realPath, error);
    if (error.NotEmpty()) {
        error.Errorf("failed to get container %s log path: %s", containerID.c_str(), error.GetCMessage());
        return;
    }
    if (path.empty()) {
        INFO("Container %s log path isn't specified, will not create the symlink", containerID.c_str());
        return;
    }
    if (realPath.empty()) {
        WARN("Cannot create symbolic link because container log file doesn't exist!");
        return;
    }
    if (util_path_remove(path.c_str()) == 0) {
        WARN("Deleted previously existing symlink file: %s", path.c_str());
    }
    if (symlink(realPath.c_str(), path.c_str()) != 0) {
        SYSERROR("failed to create symbolic link %s to the container log file %s for container %s", path.c_str(), realPath.c_str(),
                 containerID.c_str());
        error.Errorf("failed to create symbolic link %s to the container log file %s for container %s", path.c_str(),
                     realPath.c_str(), containerID.c_str());
    }
}

void GetContainerTimeStamps(const container_inspect *inspect, int64_t *createdAt, int64_t *startedAt,
                            int64_t *finishedAt, Errors &err)
{
    if (inspect == nullptr) {
        err.SetError("Invalid arguments");
        return;
    }
    if (createdAt != nullptr) {
        if (util_to_unix_nanos_from_str(inspect->created, createdAt) != 0) {
            err.Errorf("Parse createdAt failed: %s", inspect->created);
            return;
        }
    }
    if (inspect->state != nullptr) {
        if (startedAt != nullptr) {
            if (util_to_unix_nanos_from_str(inspect->state->started_at, startedAt) != 0) {
                err.Errorf("Parse startedAt failed: %s", inspect->state->started_at);
                return;
            }
        }
        if (finishedAt != nullptr) {
            if (util_to_unix_nanos_from_str(inspect->state->finished_at, finishedAt) != 0) {
                err.Errorf("Parse finishedAt failed: %s", inspect->state->finished_at);
                return;
            }
        }
    }
}

std::string GetRealContainerOrSandboxID(service_executor_t *cb, const std::string &id, bool isSandbox, Errors &error)
{
    std::string realID;

    if (cb == nullptr || cb->container.get_id == nullptr) {
        error.SetError("Unimplemented callback");
        return realID;
    }
    container_get_id_request *request { nullptr };
    container_get_id_response *response { nullptr };
    request = (container_get_id_request *)util_common_calloc_s(sizeof(container_get_id_request));
    if (request == nullptr) {
        error.SetError("Out of memory");
        goto cleanup;
    }
    request->id_or_name = util_strdup_s(id.c_str());
    if (isSandbox) {
        std::string label = CRIHelpers::Constants::CONTAINER_TYPE_LABEL_KEY + "=" +
                            CRIHelpers::Constants::CONTAINER_TYPE_LABEL_SANDBOX;
        request->label = util_strdup_s(label.c_str());
    } else {
        std::string label = CRIHelpers::Constants::CONTAINER_TYPE_LABEL_KEY + "=" +
                            CRIHelpers::Constants::CONTAINER_TYPE_LABEL_CONTAINER;
        request->label = util_strdup_s(label.c_str());
    }

    if (cb->container.get_id(request, &response) != 0) {
        if (response != nullptr && response->errmsg != nullptr) {
            error.SetError(response->errmsg);
            goto cleanup;
        } else {
            error.SetError("Failed to call get id callback");
            goto cleanup;
        }
    }
    if (strncmp(response->id, id.c_str(), id.length()) != 0) {
        error.Errorf("No such container with id: %s", id.c_str());
        goto cleanup;
    }

    realID = response->id;

cleanup:
    free_container_get_id_request(request);
    free_container_get_id_response(response);
    return realID;
}

void RemoveContainerHelper(service_executor_t *cb, const std::string &containerID, Errors &error)
{
    if (cb == nullptr || cb->container.remove == nullptr) {
        ERROR("Unimplemented callback");
        error.SetError("Unimplemented callback");
        return;
    }

    container_delete_response *response { nullptr };
    container_delete_request *request =
        (container_delete_request *)util_common_calloc_s(sizeof(container_delete_request));
    if (request == nullptr) {
        ERROR("Out of memory");
        error.SetError("Out of memory");
        goto cleanup;
    }
    request->id = util_strdup_s(containerID.c_str());
    request->force = true;

    RemoveContainerLogSymlink(containerID, error);
    if (error.NotEmpty()) {
        goto cleanup;
    }

    if (cb->container.remove(request, &response) != 0) {
        if (response != nullptr && response->errmsg != nullptr) {
            error.SetError(response->errmsg);
        } else {
            error.SetError("Failed to call remove container callback");
        }
        goto cleanup;
    }

cleanup:
    free_container_delete_request(request);
    free_container_delete_response(response);
}

void RemoveContainer(service_executor_t *cb, const std::string &containerID, Errors &error)
{
    if (containerID.empty()) {
        ERROR("Invalid empty container id.");
        error.SetError("Invalid empty container id.");
        return;
    }
    std::string realContainerID = GetRealContainerOrSandboxID(cb, containerID, false, error);
    if (error.NotEmpty()) {
        ERROR("Failed to find container id %s: %s", containerID.c_str(), error.GetCMessage());
        error.Errorf("Failed to find container id %s: %s", containerID.c_str(), error.GetCMessage());
        return;
    }

    RemoveContainerHelper(cb, realContainerID, error);
}

void StopContainerHelper(service_executor_t *cb, const std::string &containerID, int64_t timeout, Errors &error)
{
    int ret = 0;
    container_stop_request *request { nullptr };
    container_stop_response *response { nullptr };

    if (cb == nullptr || cb->container.stop == nullptr) {
        ERROR("Unimplemented callback");
        error.SetError("Unimplemented callback");
        return;
    }

    request = (container_stop_request *)util_common_calloc_s(sizeof(container_stop_request));
    if (request == nullptr) {
        ERROR("Out of memory");
        error.SetError("Out of memory");
        return;
    }
    request->id = util_strdup_s(containerID.c_str());
    request->timeout = timeout;

    ret = cb->container.stop(request, &response);
    if (ret != 0) {
        std::string msg = (response != nullptr && response->errmsg != nullptr) ? response->errmsg : "internal";
        ERROR("Failed to stop sandbox %s: %s", containerID.c_str(), msg.c_str());
        error.SetError(msg);
    }

    free_container_stop_request(request);
    free_container_stop_response(response);
}

void StopContainer(service_executor_t *cb, const std::string &containerID, int64_t timeout, Errors &error)
{
    if (containerID.empty()) {
        ERROR("Invalid empty container id.");
        error.SetError("Invalid empty container id.");
        return;
    }
    std::string realContainerID = CRIHelpers::GetRealContainerOrSandboxID(cb, containerID, false, error);
    if (error.NotEmpty()) {
        ERROR("Failed to find container id %s: %s", containerID.c_str(), error.GetCMessage());
        error.Errorf("Failed to find container id %s: %s", containerID.c_str(), error.GetCMessage());
        return;
    }

    StopContainerHelper(cb, containerID, timeout, error);
}

char *GenerateExecSuffix()
{
    char *exec_suffix = (char *)util_smart_calloc_s(sizeof(char), (CONTAINER_ID_MAX_LEN + 1));
    if (exec_suffix == nullptr) {
        ERROR("Out of memory");
        return nullptr;
    }

    if (util_generate_random_str(exec_suffix, (size_t)CONTAINER_ID_MAX_LEN)) {
        ERROR("Failed to generate exec suffix(id)");
        free(exec_suffix);
        return nullptr;
    }

    return exec_suffix;
}

std::string CRIRuntimeConvert(const std::string &runtime)
{
    std::string runtimeValue;
    json_map_string_string *criRuntimeList = nullptr;

    if (runtime.empty()) {
        return runtimeValue;
    }

    if (isulad_server_conf_rdlock()) {
        ERROR("Lock isulad server conf failed");
        return runtimeValue;
    }

    struct service_arguments *args = conf_get_server_conf();
    if (args == nullptr || args->json_confs == nullptr || args->json_confs->cri_runtimes == nullptr) {
        ERROR("Cannot get cri runtime list");
        goto out;
    }

    criRuntimeList = args->json_confs->cri_runtimes;
    for (size_t i = 0; i < criRuntimeList->len; i++) {
        if (criRuntimeList->keys[i] == nullptr || criRuntimeList->values[i] == nullptr) {
            WARN("CRI runtimes key or value is null");
            continue;
        }

        if (runtime == std::string(criRuntimeList->keys[i])) {
            runtimeValue = std::string(criRuntimeList->values[i]);
            break;
        }
    }

out:
    (void)isulad_server_conf_unlock();
    return runtimeValue;
}

bool ParseQuantitySuffix(const std::string &suffixStr, int64_t &base, int64_t &exponent)
{
    std::map<std::string, int16_t> binHandler {
        { "Ki", 10 }, { "Mi", 20 }, { "Gi", 30 }, { "Ti", 40 }, { "Pi", 50 }, { "Ei", 60 },
    };
    std::map<std::string, int16_t> dexHandler { { "n", -9 }, { "u", -6 }, { "m", -3 }, { "", 0 },   { "k", 3 },
        { "M", 6 },  { "G", 9 },  { "T", 12 }, { "P", 15 }, { "E", 18 } };

    if (suffixStr.empty()) {
        base = 10;
        exponent = 0;
        return true;
    }

    auto iter = dexHandler.find(suffixStr);
    if (iter != dexHandler.end()) {
        base = 10;
        exponent = iter->second;
        return true;
    }
    iter = binHandler.find(suffixStr);
    if (iter != binHandler.end()) {
        base = 2;
        exponent = iter->second;
        return true;
    }

    if (suffixStr.size() <= 1) {
        return false;
    }
    if (suffixStr[0] != 'E' && suffixStr[0] != 'e') {
        return false;
    }
    long long tmp = 0;
    if (util_safe_llong(suffixStr.substr(1).c_str(), &tmp) != 0) {
        return false;
    }
    base = 10;
    exponent = static_cast<int64_t>(tmp);
    return true;
}

int64_t ParseBinaryQuantity(bool positive, const std::string &numStr, const std::string &denomStr, int64_t &exponent,
                            Errors &error)
{
    int64_t result = 0;
    int64_t mult = 1 << exponent;
    long long tmp_num;
    double tmp_denom;
    int64_t work = 0;

    if (util_safe_llong(numStr.c_str(), &tmp_num) != 0) {
        if (errno != ERANGE) {
            error.Errorf("too large binary number: %s", numStr.c_str());
            return -1;
        }
        tmp_num = LONG_MAX;
    }
    // result = integer part
    work = static_cast<int64_t>(tmp_num);
    result = work * mult;
    if (result / mult != work) {
        error.Errorf("too large binary value: %s", numStr.c_str());
        return -1;
    }

    if (util_safe_strtod(("0." + denomStr).c_str(), &tmp_denom) != 0) {
        error.Errorf("invalid denom string: 0.%s", denomStr.c_str());
        return -1;
    }
    // result = integer part + demon part
    tmp_denom *= mult;
    work = static_cast<int64_t>(tmp_denom);
    if (positive && tmp_denom != static_cast<double>(work)) {
        if (work < INT64_MAX) {
            work += 1;
        }
    }

    if (work > 0 && result > INT64_MAX - work) {
        result = INT64_MAX;
    } else {
        result += work;
    }

    if (!positive) {
        result *= -1;
    }
    return result;
}

int64_t ParseDecimalQuantity(bool positive, const std::string &numStr, const std::string &denomStr, int64_t &exponent,
                             Errors &error)
{
    int64_t result = 0;
    int64_t mult = 1;
    long long tmp_num;
    double tmp_denom;
    int64_t work = 0;

    if (util_safe_llong(numStr.c_str(), &tmp_num) != 0) {
        if (errno != ERANGE) {
            error.Errorf("too large decimal number: %s", numStr.c_str());
            return -1;
        }
        tmp_num = LONG_MAX;
    }
    // result = integer part
    work = static_cast<int64_t>(tmp_num);
    if (exponent < 0) {
        bool has_denom = denomStr.size() > 0 ? true : false;
        for (int64_t i = 0; i < -exponent; i++) {
            if (work % 10 != 0) {
                has_denom = true;
            }
            work /= 10;
        }
        result = work;
        result = positive ? result : -result;
        if (has_denom && positive) {
            // if denom is not null, round up
            result = result + 1;
        }
        return result;
    }

    for (int64_t i = 0; i < exponent; i++) {
        mult *= 10;
    }

    result = work * mult;
    if (result / mult != work) {
        error.Errorf("too large decimal value: %s", numStr.c_str());
        return -1;
    }

    if (util_safe_strtod(("0." + denomStr).c_str(), &tmp_denom) != 0) {
        error.Errorf("invalid denom string: 0.%s", denomStr.c_str());
        return -1;
    }
    // result = integer part + demon part
    tmp_denom *= mult;
    work = static_cast<int64_t>(tmp_denom);
    if (denomStr.size() > static_cast<size_t>(exponent)) {
        // has denom part
        if (positive && work < INT64_MAX) {
            work += 1;
        }
    }

    if (work > 0 && result > INT64_MAX - work) {
        result = INT64_MAX;
    } else {
        result += work;
    }
    if (!positive) {
        result = -result;
    }
    return result;
}

int64_t ParseQuantity(const std::string &str, Errors &error)
{
    int64_t result = 0;

    if (str.empty()) {
        error.SetError("empty quantity string");
        return -1;
    }
    if (str == "0") {
        return 0;
    }
    bool positive = true;
    size_t pos = 0;
    size_t end = str.size();
    std::string numStr, denomStr;

    switch (str[pos]) {
        case '-':
            positive = false;
            pos++;
            break;
        case '+':
            pos++;
    }

    // strip zeros before number
    for (size_t i = pos;; i++) {
        if (i >= end) {
            return 0;
        }
        if (str[i] != '0') {
            break;
        }
        pos++;
    }

    // extract number
    for (size_t i = pos;; i++) {
        if (i >= end) {
            if (pos == end) {
                break;
            }
            numStr = str.substr(pos, end - pos);
            pos = end;
            break;
        }
        if (str[i] >= '0' && str[i] <= '9') {
            continue;
        }
        numStr = str.substr(pos, i - pos);
        pos = i;
        break;
    }

    if (numStr.empty()) {
        numStr = "0";
    }

    // extract denominator
    if (pos < end && str[pos] == '.') {
        pos++;
        for (size_t i = pos;; i++) {
            if (i >= end) {
                if (pos == end) {
                    break;
                }
                denomStr = str.substr(pos, end - pos);
                pos = end;
                break;
            }
            if (str[i] >= '0' && str[i] <= '9') {
                continue;
            }
            denomStr = str.substr(pos, i - pos);
            pos = i;
            break;
        }
        // allow 1.G now, but should not future.
    }

    // extract suffix
    int64_t base = 0;
    int64_t exponent = 0;
    if (!ParseQuantitySuffix(str.substr(pos), base, exponent)) {
        ERROR("Invalid suffix: %s", str.substr(pos).c_str());
        error.Errorf("Invalid suffix: %s", str.substr(pos).c_str());
        return -1;
    }

    // calculate result = suffix * (num + denom)
    if (base == 2) {
        result = ParseBinaryQuantity(positive, numStr, denomStr, exponent, error);
    } else {
        result = ParseDecimalQuantity(positive, numStr, denomStr, exponent, error);
    }
    if (error.NotEmpty()) {
        return -1;
    }
    DEBUG("parse quantity: %s to %ld", str.c_str(), result);
    return result;
}

auto fmtiSuladOpts(const std::vector<iSuladOpt> &opts, const char &sep) -> std::vector<std::string>
{
    std::vector<std::string> fmtOpts(opts.size());
    for (size_t i {}; i < opts.size(); i++) {
        fmtOpts[i] = opts.at(i).key + sep + opts.at(i).value;
    }
    return fmtOpts;
}

auto GetSeccompiSuladOptsByPath(const char *dstpath, Errors &error) -> std::vector<iSuladOpt>
{
    std::vector<iSuladOpt> ret { };
    __isula_auto_free parser_error err = nullptr;
    __isula_auto_free char *seccomp_json = nullptr;

    docker_seccomp *seccomp_spec = get_seccomp_security_opt_spec(dstpath);
    if (seccomp_spec == nullptr) {
        error.Errorf("failed to parse seccomp profile");
        return ret;
    }
    struct parser_context ctx = { OPT_GEN_SIMPLIFY, 0 };
    seccomp_json = docker_seccomp_generate_json(seccomp_spec, &ctx, &err);
    if (seccomp_json == nullptr) {
        error.Errorf("failed to generate seccomp json: %s", err);
    } else {
        ret = std::vector<iSuladOpt> { { "seccomp", seccomp_json, "" } };
    }

    free_docker_seccomp(seccomp_spec);
    return ret;
}

auto GetlegacySeccompiSuladOpts(const std::string &seccompProfile, Errors &error) -> std::vector<iSuladOpt>
{
    if (seccompProfile.empty() || seccompProfile == "unconfined") {
        DEBUG("Legacy seccomp is unconfined");
        return std::vector<iSuladOpt> { { "seccomp", "unconfined", "" } };
    }
    if (seccompProfile == "iSulad/default" || seccompProfile == "docker/default" ||
        seccompProfile == "runtime/default") {
        // return nil so iSulad will load the default seccomp profile
        return std::vector<iSuladOpt> {};
    }

    const std::string localHostStr("localhost/");
    if (seccompProfile.compare(0, localHostStr.length(), localHostStr) != 0) {
        error.Errorf("unknown seccomp profile option: %s", seccompProfile.c_str());
        return std::vector<iSuladOpt> {};
    }
    std::string fname = seccompProfile.substr(localHostStr.length(), seccompProfile.length());
    char dstpath[PATH_MAX] { 0 };
    if (util_clean_path(fname.c_str(), dstpath, sizeof(dstpath)) == nullptr) {
        error.Errorf("failed to get clean path");
        return std::vector<iSuladOpt> {};
    }
    if (dstpath[0] != '/') {
        error.Errorf("seccomp profile path must be absolute, but got relative path %s", fname.c_str());
        return std::vector<iSuladOpt> {};
    }

    return GetSeccompiSuladOptsByPath(dstpath, error);
}

auto GetPodSELinuxLabelOpts(const std::string &selinuxLabel, Errors &error)
-> std::vector<std::string>
{
    // security Opt Separator Change Version : k8s v1.23.0 (Corresponds to docker 1.11.x)
    // New version '=' , old version ':', iSulad cri is based on v18.09, so iSulad cri use new version separator
    const char securityOptSep { '=' };
    // LabeSep is consistent with the separator used when parsing labels
    const char labeSep { ':' };
    std::vector<iSuladOpt> selinuxOpts { };
    std::vector<std::string> opts = {"user", "role", "type", "level"};
    std::vector<std::string> vect;

    auto labelArr = CXXUtils::SplitN(selinuxLabel.c_str(), labeSep, opts.size());
    for (size_t i {0}; i < labelArr.size(); i++) {
        iSuladOpt tmp = { "label", opts[i] + std::string(1, labeSep) + std::string(labelArr[i]), "" };
        selinuxOpts.push_back(tmp);
    }

    return fmtiSuladOpts(selinuxOpts, securityOptSep);
}

} // namespace CRIHelpers
