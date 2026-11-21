#include "device_manager.h"
#include <esp_log.h>
#include <esp_mac.h>
#include <nvs_flash.h>
#include <nvs.h>
#include <cJSON.h>
#include "board.h"
#include "server_config.h"

#define TAG "DeviceManager"

// 本文件内的 URL 编码（与 esp32_music.cc 的 url_encode 行为一致）
static std::string esp32_url_encode(const std::string& str) {
    std::string encoded;
    char hex[4];
    for (size_t i = 0; i < str.length(); i++) {
        unsigned char c = str[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            encoded += c;
        } else if (c == ' ') {
            encoded += '+';
        } else {
            snprintf(hex, sizeof(hex), "%%%02X", c);
            encoded += hex;
        }
    }
    return encoded;
}

DeviceManager& DeviceManager::GetInstance() {
    static DeviceManager instance;
    return instance;
}

DeviceManager::DeviceManager() : is_bound_(false) {
    // 初始化NVS（如果尚未初始化）
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);
    
    // 获取MAC地址
    uint8_t mac[6];
    esp_err_t err = esp_efuse_mac_get_default(mac);
    if (err == ESP_OK) {
        char mac_str[18];
        snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        mac_address_ = std::string(mac_str);
        ESP_LOGI(TAG, "Device MAC Address: %s", mac_address_.c_str());
    } else {
        ESP_LOGE(TAG, "Failed to get MAC address");
    }
    
    // 从NVS加载配置
    LoadFromNVS();
    
    // 如果没有 token，尝试从服务器自动获取（可能已在网页端绑定）
    if (device_token_.empty() && !mac_address_.empty()) {
        ESP_LOGI(TAG, "No token found, trying to fetch from server...");
        TryFetchTokenFromServer();
    }
}

DeviceManager::~DeviceManager() {
}

std::string DeviceManager::GetMACAddress() {
    return mac_address_;
}

void DeviceManager::LoadFromNVS() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &nvs_handle);
    
    if (err == ESP_OK) {
        // 读取Token
        size_t token_len = 0;
        err = nvs_get_str(nvs_handle, NVS_KEY_TOKEN, nullptr, &token_len);
        if (err == ESP_OK && token_len > 0) {
            char* token_buf = new char[token_len];
            err = nvs_get_str(nvs_handle, NVS_KEY_TOKEN, token_buf, &token_len);
            if (err == ESP_OK) {
                device_token_ = std::string(token_buf);
                is_bound_ = true;
                ESP_LOGI(TAG, "Loaded device token from NVS (length: %d)", token_len - 1);
            }
            delete[] token_buf;
        }
        
        // 读取用户名
        size_t username_len = 0;
        err = nvs_get_str(nvs_handle, NVS_KEY_USERNAME, nullptr, &username_len);
        if (err == ESP_OK && username_len > 0) {
            char* username_buf = new char[username_len];
            err = nvs_get_str(nvs_handle, NVS_KEY_USERNAME, username_buf, &username_len);
            if (err == ESP_OK) {
                bound_username_ = std::string(username_buf);
                ESP_LOGI(TAG, "Device bound to user: %s", bound_username_.c_str());
            }
            delete[] username_buf;
        }
        
        nvs_close(nvs_handle);
    } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No device binding found in NVS");
    } else {
        ESP_LOGE(TAG, "Failed to open NVS: %s", esp_err_to_name(err));
    }
}

bool DeviceManager::SaveDeviceToken(const std::string& token) {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for writing: %s", esp_err_to_name(err));
        return false;
    }
    
    err = nvs_set_str(nvs_handle, NVS_KEY_TOKEN, token.c_str());
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to write token to NVS: %s", esp_err_to_name(err));
        nvs_close(nvs_handle);
        return false;
    }
    
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        device_token_ = token;
        is_bound_ = true;
        ESP_LOGI(TAG, "Device token saved to NVS");
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to commit NVS: %s", esp_err_to_name(err));
        return false;
    }
}

std::string DeviceManager::GetDeviceToken() {
    return device_token_;
}

bool DeviceManager::ClearDeviceToken() {
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle);
    
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to open NVS for erasing: %s", esp_err_to_name(err));
        return false;
    }
    
    nvs_erase_key(nvs_handle, NVS_KEY_TOKEN);
    nvs_erase_key(nvs_handle, NVS_KEY_USERNAME);
    err = nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
    
    if (err == ESP_OK) {
        device_token_.clear();
        bound_username_.clear();
        is_bound_ = false;
        ESP_LOGI(TAG, "Device token cleared");
        return true;
    } else {
        ESP_LOGE(TAG, "Failed to clear device token: %s", esp_err_to_name(err));
        return false;
    }
}

bool DeviceManager::BindDevice(const std::string& binding_code, const std::string& device_name) {
    ESP_LOGI(TAG, "Starting device binding with code: %s", binding_code.c_str());
    
    if (mac_address_.empty()) {
        ESP_LOGE(TAG, "MAC address not available");
        return false;
    }
    
    // 构建请求JSON
    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "mac", mac_address_.c_str());
    cJSON_AddStringToObject(request, "binding_code", binding_code.c_str());
    if (!device_name.empty()) {
        cJSON_AddStringToObject(request, "device_name", device_name.c_str());
    } else {
        cJSON_AddStringToObject(request, "device_name", "ESP32音乐播放器");
    }
    
    char* request_str = cJSON_PrintUnformatted(request);
    std::string request_body(request_str);
    cJSON_Delete(request);
    cJSON_free(request_str);
    
    // 发送HTTP请求到服务器
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    
    std::string bind_url = DEVICE_BIND_API_URL;
    
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    
    ESP_LOGI(TAG, "Sending bind request to: %s", bind_url.c_str());
    ESP_LOGD(TAG, "Request body: %s", request_body.c_str());
    
    if (!http->Open("POST", bind_url)) {
        ESP_LOGE(TAG, "Failed to connect to bind API");
        return false;
    }
    
    // 发送请求体
    http->Write(request_body.c_str(), request_body.length());
    
    int status_code = http->GetStatusCode();
    ESP_LOGI(TAG, "Bind request status code: %d", status_code);
    
    if (status_code != 200) {
        ESP_LOGE(TAG, "Bind request failed with status: %d", status_code);
        http->Close();
        return false;
    }
    
    // 读取响应
    std::string response = http->ReadAll();
    http->Close();
    
    ESP_LOGD(TAG, "Bind response: %s", response.c_str());
    
    // 解析响应JSON
    cJSON* response_json = cJSON_Parse(response.c_str());
    if (!response_json) {
        ESP_LOGE(TAG, "Failed to parse bind response");
        return false;
    }
    
    cJSON* success = cJSON_GetObjectItem(response_json, "success");
    if (!success || !cJSON_IsBool(success) || !cJSON_IsTrue(success)) {
        ESP_LOGE(TAG, "Bind request was not successful");
        cJSON_Delete(response_json);
        return false;
    }
    
    cJSON* token = cJSON_GetObjectItem(response_json, "token");
    cJSON* username = cJSON_GetObjectItem(response_json, "username");
    
    if (!token || !cJSON_IsString(token)) {
        ESP_LOGE(TAG, "No token in bind response");
        cJSON_Delete(response_json);
        return false;
    }
    
    std::string token_str = token->valuestring;
    std::string username_str = username && cJSON_IsString(username) ? username->valuestring : "";
    
    cJSON_Delete(response_json);
    
    // 保存Token到NVS
    if (!SaveDeviceToken(token_str)) {
        ESP_LOGE(TAG, "Failed to save token");
        return false;
    }
    
    // 保存用户名到NVS
    if (!username_str.empty()) {
        nvs_handle_t nvs_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_set_str(nvs_handle, NVS_KEY_USERNAME, username_str.c_str());
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            bound_username_ = username_str;
        }
    }
    
    ESP_LOGI(TAG, "Device successfully bound to user: %s", username_str.c_str());
    
    return true;
}

bool DeviceManager::VerifyDevice() {
    if (device_token_.empty()) {
        ESP_LOGW(TAG, "No device token available for verification");
        return false;
    }
    
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    
    std::string verify_url = DEVICE_VERIFY_API_URL;
    
    http->SetHeader("X-Device-Token", device_token_.c_str());
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    
    if (!http->Open("GET", verify_url)) {
        ESP_LOGE(TAG, "Failed to connect to verify API");
        return false;
    }
    
    int status_code = http->GetStatusCode();
    http->Close();
    
    if (status_code == 200) {
        ESP_LOGI(TAG, "Device verification successful");
        return true;
    } else {
        ESP_LOGW(TAG, "Device verification failed with status: %d", status_code);
        return false;
    }
}

std::string DeviceManager::GetFavorites() {
    if (device_token_.empty()) {
        return "";
    }
    
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    
    std::string url = FAVORITE_LIST_API_URL;
    
    http->SetHeader("X-Device-Token", device_token_.c_str());
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to connect to favorites API");
        return "";
    }
    
    int status_code = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    
    if (status_code == 200) {
        return response;
    } else {
        ESP_LOGW(TAG, "GetFavorites failed with status: %d", status_code);
        return "";
    }
}

std::string DeviceManager::GetUserPlaylists() {
    if (device_token_.empty()) {
        return "";
    }
    
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    
    std::string url = PLAYLIST_LIST_API_URL;
    
    http->SetHeader("X-Device-Token", device_token_.c_str());
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    
    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "Failed to connect to playlist API");
        return "";
    }
    
    int status_code = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    
    if (status_code == 200) {
        return response;
    } else {
        ESP_LOGW(TAG, "GetUserPlaylists failed with status: %d", status_code);
        return "";
    }
}

std::string DeviceManager::GetPlaylistSongs(const std::string& name) {
    if (device_token_.empty() || name.empty()) {
        return "";
    }

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);

    // 用 GET query 传歌单名（中文用 esp32_url_encode 编码），避免 POST body
    // 在该 HttpClient 上走 chunked 导致服务端读不到 name、设备等响应头超时30秒。
    std::string url = std::string(PLAYLIST_SONGS_API_URL) + "?name=" + esp32_url_encode(name);

    http->SetHeader("X-Device-Token", device_token_.c_str());
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");

    if (!http->Open("GET", url)) {
        ESP_LOGE(TAG, "GetPlaylistSongs: failed to connect");
        http->Close();
        return "";
    }

    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();

    if (status == 200) {
        ESP_LOGI(TAG, "GetPlaylistSongs('%s'): %u bytes", name.c_str(), response.size());
        return response;
    }
    ESP_LOGW(TAG, "GetPlaylistSongs failed status=%d", status);
    return "";
}

std::string DeviceManager::CreateUserPlaylist(const std::string& name, const std::string& description) {
    if (device_token_.empty() || name.empty()) {
        return "";
    }

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);

    // 构造请求体 {"name": "...", "description": "..."}
    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "name", name.c_str());
    cJSON_AddStringToObject(req, "description", description.c_str());
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return "";
    }
    std::string request_body(body);
    cJSON_free(body);

    http->SetHeader("X-Device-Token", device_token_.c_str());
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Content-Type", "application/json");
    // 必须在 Open 之前 SetContent：否则该 HttpClient 走 chunked，服务端读不到 body
    http->SetContent(std::move(request_body));

    if (!http->Open("POST", PLAYLIST_CREATE_API_URL)) {
        ESP_LOGE(TAG, "CreateUserPlaylist: failed to connect");
        http->Close();
        return "";
    }

    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();

    if (status == 200 || status == 201) {
        ESP_LOGI(TAG, "CreateUserPlaylist: created '%s'", name.c_str());
        return response;
    }
    ESP_LOGW(TAG, "CreateUserPlaylist failed status=%d resp=%s", status, response.c_str());
    return "";
}

bool DeviceManager::AddSongToUserPlaylist(const std::string& playlist_id,
                                          const std::string& title,
                                          const std::string& artist) {
    if (device_token_.empty() || playlist_id.empty() || title.empty()) {
        return false;
    }

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);

    // 构造 MusicItem 请求体。url 用 /stream_live 现场构造，服务端原样存储，
    // 播放时走已验证的多源链路（与搜索/播放一致）。
    std::string base_url = MUSIC_SERVER_URL;
    std::string play_url = base_url + "/stream_live?song=" +
                           esp32_url_encode(title) + "&singer=" + esp32_url_encode(artist);
    cJSON* req = cJSON_CreateObject();
    cJSON_AddStringToObject(req, "title", title.c_str());
    cJSON_AddStringToObject(req, "artist", artist.c_str());
    cJSON_AddStringToObject(req, "audio_url", play_url.c_str());
    cJSON_AddStringToObject(req, "audio_full_url", play_url.c_str());
    char* body = cJSON_PrintUnformatted(req);
    cJSON_Delete(req);
    if (!body) {
        return false;
    }
    std::string request_body(body);
    cJSON_free(body);

    std::string url = std::string(PLAYLIST_ADD_SONG_API_URL) + "?playlist_id=" + esp32_url_encode(playlist_id);

    http->SetHeader("X-Device-Token", device_token_.c_str());
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    http->SetHeader("Content-Type", "application/json");
    // 必须在 Open 之前 SetContent：否则该 HttpClient 走 chunked，服务端读不到 body
    http->SetContent(std::move(request_body));

    if (!http->Open("POST", url)) {
        ESP_LOGE(TAG, "AddSongToPlaylist: failed to connect");
        http->Close();
        return false;
    }

    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();

    if (status == 200) {
        ESP_LOGI(TAG, "AddSongToPlaylist: added '%s' - '%s'", title.c_str(), artist.c_str());
        return true;
    }
    ESP_LOGW(TAG, "AddSongToPlaylist failed status=%d resp=%s", status, response.c_str());
    return false;
}

bool DeviceManager::DeleteUserPlaylist(const std::string& playlist_id) {
    if (device_token_.empty() || playlist_id.empty()) {
        return false;
    }

    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);

    std::string url = std::string(PLAYLIST_DELETE_API_URL) + "?playlist_id=" + esp32_url_encode(playlist_id);

    http->SetHeader("X-Device-Token", device_token_.c_str());
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");

    if (!http->Open("DELETE", url)) {
        ESP_LOGE(TAG, "DeleteUserPlaylist: failed to connect");
        http->Close();
        return false;
    }
    int status = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();

    if (status == 200) {
        ESP_LOGI(TAG, "DeleteUserPlaylist: deleted '%s'", playlist_id.c_str());
        return true;
    }
    ESP_LOGW(TAG, "DeleteUserPlaylist failed status=%d resp=%s", status, response.c_str());
    return false;
}

bool DeviceManager::IsDeviceBound() {
    return is_bound_ && !device_token_.empty();
}

std::string DeviceManager::GetBoundUsername() {
    return bound_username_;
}

bool DeviceManager::TryFetchTokenFromServer() {
    ESP_LOGI(TAG, "Attempting to fetch token from server using MAC: %s", mac_address_.c_str());
    
    auto& board = Board::GetInstance();
    auto network = board.GetNetwork();
    auto http = network->CreateHttp(0);
    
    // 构建请求 JSON
    cJSON* request = cJSON_CreateObject();
    cJSON_AddStringToObject(request, "mac", mac_address_.c_str());
    char* request_str = cJSON_PrintUnformatted(request);
    std::string request_body(request_str);
    cJSON_Delete(request);
    cJSON_free(request_str);
    
    std::string sync_url = MUSIC_SERVER_URL;
    sync_url += "/api/esp32/sync";
    
    http->SetHeader("Content-Type", "application/json");
    http->SetHeader("User-Agent", "ESP32-Music-Player/1.0");
    
    if (!http->Open("POST", sync_url)) {
        ESP_LOGW(TAG, "Failed to connect to sync API");
        return false;
    }
    
    http->Write(request_body.c_str(), request_body.length());
    
    int status_code = http->GetStatusCode();
    std::string response = http->ReadAll();
    http->Close();
    
    if (status_code == 404) {
        ESP_LOGI(TAG, "Device not bound on server yet");
        return false;
    }
    
    if (status_code != 200) {
        ESP_LOGW(TAG, "Token sync failed with status: %d", status_code);
        return false;
    }
    
    // 解析响应
    cJSON* response_json = cJSON_Parse(response.c_str());
    if (!response_json) {
        ESP_LOGE(TAG, "Failed to parse sync response");
        return false;
    }
    
    cJSON* token = cJSON_GetObjectItem(response_json, "token");
    cJSON* username = cJSON_GetObjectItem(response_json, "username");
    
    if (!token || !cJSON_IsString(token)) {
        ESP_LOGE(TAG, "Invalid sync response: missing token");
        cJSON_Delete(response_json);
        return false;
    }
    
    std::string token_str = token->valuestring;
    std::string username_str = username && cJSON_IsString(username) ? username->valuestring : "";
    
    cJSON_Delete(response_json);
    
    // 保存 Token 到 NVS
    if (!SaveDeviceToken(token_str)) {
        ESP_LOGE(TAG, "Failed to save synced token");
        return false;
    }
    
    // 保存用户名到 NVS
    if (!username_str.empty()) {
        nvs_handle_t nvs_handle;
        if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &nvs_handle) == ESP_OK) {
            nvs_set_str(nvs_handle, NVS_KEY_USERNAME, username_str.c_str());
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            bound_username_ = username_str;
            is_bound_ = true;
        }
    }
    
    ESP_LOGI(TAG, "✅ Token synced successfully for user: %s", username_str.c_str());
    
    return true;
}
