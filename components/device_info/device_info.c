/**
 * @file device_info.c
 * @brief Firmware version (app descriptor), hardware revision (NVS), reset reason
 *
 * @author Nirmal Lad <nirmal.lad@acldigital.com>
 * @date 2026-10-19
 *
 * @copyright Copyright (c) 2026 ACL Digital Pvt Ltd. All rights reserved.
 *
 * CONFIDENTIALITY NOTICE:
 * This software and documentation are the confidential and proprietary
 * information of ACL Digital Pvt Ltd. Unauthorized copying, distribution,
 * modification, or reverse engineering of this file, via any medium,
 * is strictly prohibited.
 *
 * REQ-FUNC-20..28, REQ-FUNC-33. The hardware revision lives in NVS namespace
 * "devinfo", key "hw_rev". NOTHING is compiled in (G2-02): a freshly erased
 * board reports ESP_ERR_NOT_FOUND. The handle is opened per call (read-only
 * for reads) so a failed or absent namespace is simply reported, never cached.
 */
#include "device_info.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs.h"

#include "devinfo_validate.h"
#include "reset_reason_map.h"

_Static_assert(RESET_REASON_MAP_COUNT == (int)ESP_RST_CPU_LOCKUP + 1,
               "reset_reason_map.c must cover every esp_reset_reason_t value (IDF upgrade?)");

static const char *TAG = "devinfo";

#define NVS_NS      "devinfo"
#define NVS_KEY_HW  "hw_rev"
#define HW_BUF_SIZE (DEVICE_INFO_HW_REV_MAX_LEN + 1)

static bool s_nvs_ok;
static int  s_reset_code;
static bool s_inited;

esp_err_t device_info_init(bool nvs_ok)
{
    s_nvs_ok = nvs_ok;
    s_reset_code = (int)esp_reset_reason();
    s_inited = true;
    return ESP_OK;
}

static esp_err_t copy_out(const char *src, char *buf, size_t buf_len)
{
    size_t n = strlen(src);
    if (n + 1 > buf_len) {
        buf[0] = '\0';
        return ESP_ERR_INVALID_SIZE;
    }
    memcpy(buf, src, n + 1);
    return ESP_OK;
}

esp_err_t device_info_get_fw_version(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    const esp_app_desc_t *desc = esp_app_get_description();
    if (desc == NULL) {
        return ESP_ERR_NOT_FOUND;
    }
    if (!devinfo_validate_fw_version(desc->version, sizeof(desc->version), DEVICE_INFO_FW_VER_MAX_LEN)) {
        return ESP_ERR_NOT_FOUND; /* empty, "1" placeholder or invalid characters */
    }
    return copy_out(desc->version, buf, buf_len);
}

esp_err_t device_info_get_hw_revision(char *buf, size_t buf_len)
{
    if (buf == NULL || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = '\0';
    if (!s_nvs_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND; /* namespace never written: absent */
    }
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    char tmp[HW_BUF_SIZE];
    size_t len = sizeof(tmp);
    err = nvs_get_str(h, NVS_KEY_HW, tmp, &len);
    nvs_close(h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_ERR_NOT_FOUND;
    }
    if (err == ESP_ERR_NVS_INVALID_LENGTH) {
        return ESP_ERR_INVALID_RESPONSE; /* stored string longer than the 32-character limit */
    }
    if (err != ESP_OK) {
        return ESP_ERR_INVALID_STATE;
    }
    tmp[sizeof(tmp) - 1] = '\0';
    if (!devinfo_validate_hw_rev(tmp)) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return copy_out(tmp, buf, buf_len);
}

bool device_info_hw_revision_is_valid(const char *rev)
{
    return devinfo_validate_hw_rev(rev);
}

esp_err_t device_info_set_hw_revision(const char *rev)
{
    if (rev == NULL || !devinfo_validate_hw_rev(rev)) {
        return ESP_ERR_INVALID_ARG; /* stored value unchanged */
    }
    if (!s_nvs_ok) {
        return ESP_ERR_INVALID_STATE;
    }
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed (%s)", esp_err_to_name(err));
        return ESP_ERR_INVALID_STATE;
    }

    char old[HW_BUF_SIZE];
    size_t len = sizeof(old);
    esp_err_t gerr = nvs_get_str(h, NVS_KEY_HW, old, &len);
    if (gerr != ESP_OK) {
        memcpy(old, "none", sizeof("none"));
    } else {
        old[sizeof(old) - 1] = '\0';
    }

    err = nvs_set_str(h, NVS_KEY_HW, rev);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "hw revision write failed (%s)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "DEVINFO_HW_REV_SET old=%s new=%s", old, rev);
    return ESP_OK;
}

esp_err_t device_info_get_reset_reason(const char **token, int *code)
{
    if (!s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    if (token != NULL) {
        *token = reset_reason_token(s_reset_code);
    }
    if (code != NULL) {
        *code = s_reset_code;
    }
    return ESP_OK;
}

const char *device_info_reset_reason_token(int code)
{
    return reset_reason_token(code);
}
