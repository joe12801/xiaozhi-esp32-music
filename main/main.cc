#include <esp_log.h>
#include <esp_err.h>
#include <nvs.h>
#include <nvs_flash.h>
#include <driver/gpio.h>
#include <esp_event.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "system_info.h"
#include "ssid_manager.h"

#define TAG "main"

// 内置兜底 WiFi 凭据：NVS 里没有任何记录时自动写入，避免刷机后必须重新配网
//
// 默认留空 —— 官方发布的固件不内置任何 WiFi，刷机后走正常配网流程。
// 想内置自己的 WiFi，就在下面填上，或用编译参数覆盖：
//   idf.py -DFALLBACK_WIFI_SSID=\"MyWiFi\" -DFALLBACK_WIFI_PASSWORD=\"12345678\" build
// 留空时这段逻辑会直接跳过，不会写入无效凭据。
#ifndef FALLBACK_WIFI_SSID
#define FALLBACK_WIFI_SSID ""
#endif
#ifndef FALLBACK_WIFI_PASSWORD
#define FALLBACK_WIFI_PASSWORD ""
#endif

extern "C" void app_main(void)
{
    // Initialize the default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Initialize NVS flash for WiFi configuration
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "Erasing NVS flash to fix corruption");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // 首次启动或 NVS 被清空时，自动写入内置 WiFi 凭据
    // FALLBACK_WIFI_SSID 为空表示未配置，跳过，交给正常配网流程
    auto& ssid_manager = SsidManager::GetInstance();
    if (FALLBACK_WIFI_SSID[0] != '\0' && ssid_manager.GetSsidList().empty()) {
        ESP_LOGW(TAG, "No saved WiFi, adding built-in credentials: %s", FALLBACK_WIFI_SSID);
        ssid_manager.AddSsid(FALLBACK_WIFI_SSID, FALLBACK_WIFI_PASSWORD);
    } else if (!ssid_manager.GetSsidList().empty()) {
        ESP_LOGI(TAG, "Saved WiFi count: %d", (int)ssid_manager.GetSsidList().size());
    } else {
        ESP_LOGI(TAG, "No built-in WiFi credentials, waiting for provisioning");
    }

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
