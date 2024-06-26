/******************************************************************************
 * Copyright (c) Huawei Technologies Co., Ltd. 2018-2019. All rights reserved.
 * iSulad licensed under the Mulan PSL v2.
 * You can use this software according to the terms and conditions of the Mulan PSL v2.
 * You may obtain a copy of Mulan PSL v2 at:
 *     http://license.coscl.org.cn/MulanPSL2
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND, EITHER EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT, MERCHANTABILITY OR FIT FOR A PARTICULAR
 * PURPOSE.
 * See the Mulan PSL v2 for more details.
 * Author: tanyifeng
 * Create: 2018-11-1
 * Description: provide container sha256 functions
 ********************************************************************************/

#ifndef UTILS_CUTILS_UTILS_ARRAY_H
#define UTILS_CUTILS_UTILS_ARRAY_H

#include <stdbool.h>
#include <stddef.h>
#include <isula_libutils/auto_cleanup.h>

#ifdef __cplusplus
extern "C" {
#endif

size_t util_array_len(const char **array);

void util_free_array_by_len(char **array, size_t len);

void util_free_array(char **array);

char **util_copy_array_by_len(char **array, size_t len);

int util_grow_array(char ***orig_array, size_t *orig_capacity, size_t size,
                    size_t increment);

int util_array_append(char ***array, const char *element);

bool util_array_contain(const char **array, const char *element);

// don't free element and set it null after call this function
int util_common_array_append_pointer(void ***array, void *element);

typedef struct string_array_t {
    char **items;
    size_t len;
    size_t cap;
} string_array;

string_array *util_string_array_new(size_t len);

void util_free_string_array(string_array *ptr);

int util_append_string_array(string_array *sarr, const char *val);

string_array *util_copy_string_array(string_array *sarr);

bool util_string_array_contain(const string_array *sarr, const char *elem);

void util_free_sensitive_array(char **array);

void util_free_sensitive_array_by_len(char **array, size_t len);

// define auto free function callback for char *
define_auto_cleanup_callback(util_free_array, char *);
// define auto free macro for char *
#define __isula_auto_array_t auto_cleanup_tag(util_free_array)

define_auto_cleanup_callback(util_free_string_array, string_array);
#define __isula_auto_string_array_t auto_cleanup_tag(util_free_string_array)

typedef void (*free_common_array_item_cb)(void *item);
typedef void *(*clone_common_array_item_cb)(void *item);
typedef struct common_array_t {
    void **items;
    size_t len;
    size_t cap;
    free_common_array_item_cb free_item_cb;
    clone_common_array_item_cb clone_item_cb;
} common_array;

void *util_clone_ptr(void *item);

common_array *util_common_array_new(size_t len, free_common_array_item_cb free_item_cb,
                                    clone_common_array_item_cb clone_item_cb);

void util_free_common_array(common_array *ptr);

int util_append_common_array(common_array *arr, void *val);

int util_merge_common_array(common_array *dest_arr, common_array *src_arr);

define_auto_cleanup_callback(util_free_common_array, common_array);
#define __isula_auto_common_array_t auto_cleanup_tag(util_free_common_array)

#ifdef __cplusplus
}
#endif

#endif // UTILS_CUTILS_UTILS_ARRAY_H

