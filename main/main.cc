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
// 👇 改成你自己的 WiFi 名称和密码后再编译
// 也可以在编译时用 -DFALLBACK_WIFI_SSID=\"xxx\" -DFALLBACK_WIFI_PASSWORD=\"xxx\" 覆盖
#ifndef FALLBACK_WIFI_SSID
#define FALLBACK_WIFI_SSID "YOUR_WIFI_SSID"
#endif
#ifndef FALLBACK_WIFI_PASSWORD
#define FALLBACK_WIFI_PASSWORD "YOUR_WIFI_PASSWORD"
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
    auto& ssid_manager = SsidManager::GetInstance();
    if (ssid_manager.GetSsidList().empty()) {
        ESP_LOGW(TAG, "No saved WiFi, adding built-in credentials: %s", FALLBACK_WIFI_SSID);
        ssid_manager.AddSsid(FALLBACK_WIFI_SSID, FALLBACK_WIFI_PASSWORD);
    } else {
        ESP_LOGI(TAG, "Saved WiFi count: %d", (int)ssid_manager.GetSsidList().size());
    }

    // Launch the application
    auto& app = Application::GetInstance();
    app.Start();
}
