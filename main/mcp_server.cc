/*
 * MCP Server Implementation
 * Reference: https://modelcontextprotocol.io/specification/2024-11-05
 */

#include "mcp_server.h"
#include <esp_log.h>
#include <esp_app_desc.h>
#include <algorithm>
#include <cstring>
#include <esp_pthread.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "application.h"
#include "display.h"
#include "oled_display.h"
#include "board.h"
#include "settings.h"
#include "lvgl_theme.h"
#include "lvgl_display.h"
#include "boards/common/esp32_music.h"
#include "device_manager.h"
#define TAG "MCP"

// 按歌单名在用户的歌单列表中查找 id；找不到返回空字符串。
// 让语音交互里用户说歌单名（而非 id）也能加歌/删歌。
static std::string find_playlist_id_by_name(const std::string& name) {
    auto& dm = DeviceManager::GetInstance();
    std::string list_json = dm.GetUserPlaylists();
    if (list_json.empty()) {
        return "";
    }
    cJSON* arr = cJSON_Parse(list_json.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return "";
    }
    std::string id;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON* item = cJSON_GetArrayItem(arr, i);
        cJSON* jname = cJSON_GetObjectItem(item, "name");
        cJSON* jid = cJSON_GetObjectItem(item, "id");
        if (cJSON_IsString(jname) && cJSON_IsString(jid) &&
            std::string(jname->valuestring) == name) {
            id = jid->valuestring;
            break;
        }
    }
    cJSON_Delete(arr);
    return id;
}

// 按歌单名取出该歌单的 songs 数组 JSON 字符串（供语音"播放XX歌单"直接播放，
// 避免让大模型先把全部歌单JSON读进去、再拼一个大JSON回来，那会长时间阻塞音频任务导致卡死）。
// 找到返回 songs 数组的 JSON；找不到返回空字符串。调用方负责解析。
static std::string find_playlist_songs_json_by_name(const std::string& name) {
    auto& dm = DeviceManager::GetInstance();

    // 优先走服务端精简接口：只返回该歌单的 [{title,artist}]（约1KB），
    // 避免拉全部歌单60首歌的30KB大JSON——那会在ESP32同步解析时饿死音频任务导致卡死。
    std::string slim = dm.GetPlaylistSongs(name);
    if (!slim.empty()) {
        return slim;  // 直接就是精简歌曲数组，正是工具需要的格式
    }

    // 回退：老服务端没有精简接口时，仍用全量歌单本地筛选
    std::string list_json = dm.GetUserPlaylists();
    if (list_json.empty()) {
        return "";
    }
    cJSON* arr = cJSON_Parse(list_json.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return "";
    }
    std::string result;
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON* item = cJSON_GetArrayItem(arr, i);
        cJSON* jname = cJSON_GetObjectItem(item, "name");
        if (cJSON_IsString(jname) && std::string(jname->valuestring) == name) {
            cJSON* jsongs = cJSON_GetObjectItem(item, "songs");
            if (cJSON_IsArray(jsongs)) {
                char* printed = cJSON_PrintUnformatted(jsongs);
                if (printed) {
                    result = printed;
                    cJSON_free(printed);
                }
            }
            break;
        }
    }
    cJSON_Delete(arr);
    return result;
}

// ========== 歌单分页拉取 ==========
// 单页条数：50 首的精简 JSON 约 2.3KB，解析峰值远低于一次性 198 首（约 9KB）。
static constexpr int kPlaylistPageSize = 50;

// 把一页响应追加解析到 out。兼容两种格式：
//   {"total":N,"songs":[{title,artist},...]}  —— 新服务端分页
//   [{title,artist},...]                      —— 老服务端一次性全量
// 返回本页新增条数（<0 表示解析失败）；*total 返回服务端声明的总条数，-1 表示未知。
static int append_playlist_page(const std::string& payload, std::vector<SongInfo>& out, int* total) {
    if (total) *total = -1;
    cJSON* root = cJSON_Parse(payload.c_str());
    if (!root) {
        return -1;
    }
    cJSON* songs = root;
    if (cJSON_IsObject(root)) {
        cJSON* jtotal = cJSON_GetObjectItem(root, "total");
        if (total && cJSON_IsNumber(jtotal)) {
            *total = jtotal->valueint;
        }
        songs = cJSON_GetObjectItem(root, "songs");
    }
    if (!cJSON_IsArray(songs)) {
        cJSON_Delete(root);
        return -1;
    }
    int added = 0;
    int n = cJSON_GetArraySize(songs);
    for (int i = 0; i < n; i++) {
        cJSON* item = cJSON_GetArrayItem(songs, i);
        if (!item) continue;
        cJSON* title = cJSON_GetObjectItem(item, "title");
        cJSON* artist = cJSON_GetObjectItem(item, "artist");
        if (cJSON_IsString(title) && cJSON_IsString(artist)) {
            out.emplace_back(title->valuestring, artist->valuestring);
            added++;
        }
    }
    cJSON_Delete(root);
    return added;
}

// 分页拉取整个歌单：每页 50 首，解析完立即释放该页文本与 cJSON 节点，再拉下一页。
// 这样内存峰值只有"一页"大小，200/500 首的歌单都能稳定拉全。
// 原来是「一次性拉整包」——198 首时解析失败，表现为歌单拉到了却一个音频请求都不发。
static bool load_playlist_songs(const std::string& name, bool is_favorite, std::vector<SongInfo>& out) {
    auto& dm = DeviceManager::GetInstance();
    out.clear();

    int offset = 0;
    int page = 0;
    while (true) {
        // payload 在循环体内声明，每轮结束即析构，不会累积占用
        std::string payload = is_favorite
            ? dm.GetFavorites(offset, kPlaylistPageSize)
            : dm.GetPlaylistSongs(name, offset, kPlaylistPageSize);
        if (payload.empty()) {
            ESP_LOGW(TAG, "load_playlist_songs('%s'): page %d offset=%d empty", name.c_str(), page, offset);
            return offset == 0 ? false : !out.empty();
        }

        int total = -1;
        int added = append_playlist_page(payload, out, &total);
        if (added < 0) {
            ESP_LOGW(TAG, "load_playlist_songs('%s'): page %d parse failed", name.c_str(), page);
            return offset == 0 ? false : !out.empty();
        }

        offset += added;
        page++;
        ESP_LOGI(TAG, "load_playlist_songs('%s'): page %d +%d, have %d/%d",
                 name.c_str(), page, added, (int)out.size(), total);

        if (total < 0) break;           // 老服务端：这一页就是全量
        if (added == 0) break;          // 空页，防死循环
        if (offset >= total) break;     // 已拉完
        vTaskDelay(pdMS_TO_TICKS(30));  // 让出 CPU，避免连续 HTTP 饿死音频任务
    }
    return !out.empty();
}

// 把"全部歌单+每首歌详情"的大JSON瘦身成"只有歌单名+歌曲数"的小JSON。
// 用于 my_playlists（只查看歌单名时），避免把60首歌的详情灌进大模型导致卡死。
static std::string slim_playlist_names_json(const std::string& list_json) {
    cJSON* arr = cJSON_Parse(list_json.c_str());
    if (!arr || !cJSON_IsArray(arr)) {
        if (arr) cJSON_Delete(arr);
        return "[]";
    }
    cJSON* out = cJSON_CreateArray();
    int n = cJSON_GetArraySize(arr);
    for (int i = 0; i < n; i++) {
        cJSON* item = cJSON_GetArrayItem(arr, i);
        cJSON* jname = cJSON_GetObjectItem(item, "name");
        if (!cJSON_IsString(jname)) continue;
        cJSON* obj = cJSON_CreateObject();
        cJSON_AddStringToObject(obj, "name", jname->valuestring);
        cJSON* jsongs = cJSON_GetObjectItem(item, "songs");
        int count = cJSON_IsArray(jsongs) ? cJSON_GetArraySize(jsongs) : 0;
        cJSON_AddNumberToObject(obj, "song_count", count);
        cJSON_AddItemToArray(out, obj);
    }
    cJSON_Delete(arr);
    char* printed = cJSON_PrintUnformatted(out);
    std::string result = printed ? printed : "[]";
    if (printed) cJSON_free(printed);
    cJSON_Delete(out);
    return result;
}

McpServer::McpServer() {
}

McpServer::~McpServer() {
    for (auto tool : tools_) {
        delete tool;
    }
    tools_.clear();
}

void McpServer::AddCommonTools() {
    // *Important* To speed up the response time, we add the common tools to the beginning of
    // the tools list to utilize the prompt cache.
    // **重要** 为了提升响应速度，我们把常用的工具放在前面，利用 prompt cache 的特性。

    // Backup the original tools list and restore it after adding the common tools.
    auto original_tools = std::move(tools_);
    auto& board = Board::GetInstance();

    // Do not add custom tools here.
    // Custom tools must be added in the board's InitializeTools function.

    AddTool("self.get_device_status",
        "Provides the real-time information of the device, including the current status of the audio speaker, screen, battery, network, etc.\n"
        "Use this tool for: \n"
        "1. Answering questions about current condition (e.g. what is the current volume of the audio speaker?)\n"
        "2. As the first step to control the device (e.g. turn up / down the volume of the audio speaker, etc.)",
        PropertyList(),
        [&board](const PropertyList& properties) -> ReturnValue {
            return board.GetDeviceStatusJson();
        });

    AddTool("self.audio_speaker.set_volume", 
        "Set the volume of the audio speaker. If the current volume is unknown, you must call `self.get_device_status` tool first and then call this tool.",
        PropertyList({
            Property("volume", kPropertyTypeInteger, 0, 100)
        }), 
        [&board](const PropertyList& properties) -> ReturnValue {
            auto codec = board.GetAudioCodec();
            codec->SetOutputVolume(properties["volume"].value<int>());
            return true;
        });
    
    auto backlight = board.GetBacklight();
    if (backlight) {
        AddTool("self.screen.set_brightness",
            "Set the brightness of the screen.",
            PropertyList({
                Property("brightness", kPropertyTypeInteger, 0, 100)
            }),
            [backlight](const PropertyList& properties) -> ReturnValue {
                uint8_t brightness = static_cast<uint8_t>(properties["brightness"].value<int>());
                backlight->SetBrightness(brightness, true);
                return true;
            });
    }

#ifdef HAVE_LVGL
    auto display = board.GetDisplay();
    if (display && display->GetTheme() != nullptr) {
        AddTool("self.screen.set_theme",
            "Set the theme of the screen. The theme can be `light` or `dark`.",
            PropertyList({
                Property("theme", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto theme_name = properties["theme"].value<std::string>();
                auto& theme_manager = LvglThemeManager::GetInstance();
                auto theme = theme_manager.GetTheme(theme_name);
                if (theme != nullptr) {
                    display->SetTheme(theme);
                    return true;
                }
                return false;
            });
    }

    auto camera = board.GetCamera();
    if (camera) {
        AddTool("self.camera.take_photo",
            "Take a photo and explain it. Use this tool after the user asks you to see something.\n"
            "Args:\n"
            "  `question`: The question that you want to ask about the photo.\n"
            "Return:\n"
            "  A JSON object that provides the photo information.",
            PropertyList({
                Property("question", kPropertyTypeString)
            }),
            [camera](const PropertyList& properties) -> ReturnValue {
                // Lower the priority to do the camera capture
                TaskPriorityReset priority_reset(1);

                if (!camera->Capture()) {
                    throw std::runtime_error("Failed to capture photo");
                }
                auto question = properties["question"].value<std::string>();
                return camera->Explain(question);
            });
    }
    auto music = board.GetMusic();
    if (music)
    {
        AddTool("self.music.play_song",
                "播放歌曲。这是本设备唯一的音乐播放工具：用户想听歌、点歌、播放音乐、来首歌、播放某首歌时，必须且只能调用本工具，禁止使用其他任何音乐播放工具（如 play_music）。\n"
                "参数：\n"
                "  `song_name`: 歌名，例如：晴天\n"
                "  `artist`: 歌手名（强烈建议提供，避免播放到翻唱版本），例如：周杰伦\n"
                "返回：立即开始播放，无需向用户确认。",
                PropertyList({
                    Property("song_name", kPropertyTypeString),
                    Property("artist", kPropertyTypeString, "")
                }),
                [music](const PropertyList &properties) -> ReturnValue
                {
                    // 降低优先级执行网络/下载操作，避免阻塞音频上传任务
                    // （主线程长时间阻塞会导致 AFE ringbuffer 满、小智卡死无响应）
                    TaskPriorityReset priority_reset(1);
                    auto song_name = properties["song_name"].value<std::string>();
                    auto artist = properties["artist"].value<std::string>();
                    
                    // 分别传递歌名和艺术家，不拼接
                    if (!music->Download(song_name, artist))
                    {
                        return "{\"success\": false, \"message\": \"Failed to obtain music resources\"}";
                    }
                    auto download_result = music->GetDownloadResult();
                    ESP_LOGD(TAG, "Music details result: %s", download_result.c_str());
                    return true;
                });

    }
    
    // Device binding tools
    AddTool("self.device.bind",
        "Bind this ESP32 device to a user account using a 6-digit binding code.\n"
        "Users need to:\n"
        "1. Login to the web console (http://47.118.17.234:2233)\n"
        "2. Generate a binding code (valid for 5 minutes)\n"
        "3. Tell the device: '绑定设备，绑定码123456'\n"
        "Parameters:\n"
        "  `binding_code`: 6-digit binding code from web console\n"
        "  `device_name`: Optional custom device name (default: ESP32音乐播放器)\n"
        "Returns:\n"
        "  Success message with bound username, or error message.",
        PropertyList({
            Property("binding_code", kPropertyTypeString),
            Property("device_name", kPropertyTypeString, "")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            TaskPriorityReset priority_reset(1);  // 避免同步HTTP阻塞音频任务
            auto& device_manager = DeviceManager::GetInstance();
            
            std::string binding_code = properties["binding_code"].value<std::string>();
            std::string device_name = properties["device_name"].value<std::string>();
            
            if (binding_code.empty()) {
                return "错误：绑定码不能为空";
            }
            
            if (binding_code.length() != 6) {
                return "错误：绑定码必须是6位数字";
            }
            
            // Check if device is already bound
            if (device_manager.IsDeviceBound()) {
                std::string username = device_manager.GetBoundUsername();
                return "设备已绑定到用户: " + username + "\n如需重新绑定，请先解绑。";
            }
            
            // Attempt to bind
            bool success = device_manager.BindDevice(binding_code, device_name);
            
            if (success) {
                std::string username = device_manager.GetBoundUsername();
                return "✅ 设备绑定成功！\n已绑定到用户: " + username;
            } else {
                return "❌ 绑定失败！请检查：\n"
                       "1. 绑定码是否正确\n"
                       "2. 绑定码是否已过期（有效期5分钟）\n"
                       "3. 网络连接是否正常";
            }
        });

    AddTool("self.device.unbind",
        "Unbind this device from the current user account.\n"
        "This will remove the device binding and require re-binding to use personalized features.\n"
        "Returns:\n"
        "  Success or error message.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            TaskPriorityReset priority_reset(1);  // 避免同步HTTP阻塞音频任务
            auto& device_manager = DeviceManager::GetInstance();
            
            if (!device_manager.IsDeviceBound()) {
                return "设备未绑定，无需解绑";
            }
            
            std::string username = device_manager.GetBoundUsername();
            bool success = device_manager.ClearDeviceToken();
            
            if (success) {
                return "✅ 设备已解绑\n之前绑定的用户: " + username;
            } else {
                return "❌ 解绑失败，请稍后重试";
            }
        });

    AddTool("self.device.status",
        "Get the current device binding status and information.\n"
        "Returns:\n"
        "  Device binding status, MAC address, bound username, etc.",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            auto& device_manager = DeviceManager::GetInstance();
            
            std::string result = "📱 设备信息:\n\n";
            result += "MAC地址: " + device_manager.GetMACAddress() + "\n";
            
            if (device_manager.IsDeviceBound()) {
                result += "绑定状态: ✅ 已绑定\n";
                result += "绑定用户: " + device_manager.GetBoundUsername() + "\n";
                
                // Try to verify with server
                bool verified = device_manager.VerifyDevice();
                result += "服务器验证: " + std::string(verified ? "✅ 通过" : "❌ 失败") + "\n";
            } else {
                result += "绑定状态: ❌ 未绑定\n";
                result += "\n💡 提示: 使用 '绑定设备' 功能来绑定账号";
            }
            
            return result;
        });

    // 歌单相关工具
    AddTool("self.music.favorite_list",
        "获取我的'我喜欢'歌单中的歌曲列表。\n"
        "Returns:\n"
        "  歌曲列表JSON数组，每首歌包含：\n"
        "  - title: 歌曲名\n"
        "  - artist: 艺术家名\n"
        "  - duration: 时长\n"
        "  **播放选项**:\n"
        "  1. 播放单首歌：使用 play_song 工具，同时传递 song_name 和 artist 参数\n"
        "  2. 播放整个歌单：直接调用 play_playlist_by_name 工具，playlist_name 传'我喜欢'即可。不要自己拼歌曲JSON数组！",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            TaskPriorityReset priority_reset(1);  // 避免同步HTTP阻塞音频任务
            auto& device_manager = DeviceManager::GetInstance();
            if (!device_manager.IsDeviceBound()) {
                return "错误：设备未绑定，请先绑定账号";
            }
            
            std::string result = device_manager.GetFavorites();
            if (result.empty()) {
                return "获取歌单失败或歌单为空";
            }
            return result;
        });

    AddTool("self.music.my_playlists",
        "只用于查看用户有哪些歌单（歌单名称和歌曲数量）。\n"
        "注意：本工具【不用于播放】。用户说'播放歌单'时不要调用本工具，直接调用 play_playlist_by_name 播放。\n"
        "Returns: 歌单列表，每项只有 name（歌单名）和 song_count（歌曲数），不含歌曲详情。",
        PropertyList(),
        [](const PropertyList& properties) -> ReturnValue {
            TaskPriorityReset priority_reset(1);  // 避免同步HTTP阻塞音频任务
            auto& device_manager = DeviceManager::GetInstance();
            if (!device_manager.IsDeviceBound()) {
                return "错误：设备未绑定，请先绑定账号";
            }
            
            std::string result = device_manager.GetUserPlaylists();
            if (result.empty()) {
                return "获取歌单失败或没有歌单";
            }
            return slim_playlist_names_json(result);  // 只返回歌单名，避免大JSON灌入大模型
        });

    // 创建新歌单
    AddTool("self.music.create_playlist",
        "创建一个新的歌单。用户说'创建歌单''新建歌单''建一个叫XX的歌单'时调用本工具。\n"
        "参数：\n"
        "  `name`: 歌单名称，例如：通勤路上的歌\n"
        "  `description`: 歌单简介，可留空\n"
        "返回：新歌单信息（含 id）。请记住返回的 id 与 name，后续加歌时要用。",
        PropertyList({
            Property("name", kPropertyTypeString),
            Property("description", kPropertyTypeString, "")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            TaskPriorityReset priority_reset(1);  // 避免同步HTTP阻塞音频任务
            auto& dm = DeviceManager::GetInstance();
            if (!dm.IsDeviceBound()) {
                return "{\"success\": false, \"message\": \"设备未绑定，请先绑定账号\"}";
            }
            auto name = properties["name"].value<std::string>();
            auto desc = properties["description"].value<std::string>();
            if (name.empty()) {
                return "{\"success\": false, \"message\": \"歌单名不能为空\"}";
            }
            std::string result = dm.CreateUserPlaylist(name, desc);
            if (result.empty()) {
                return "{\"success\": false, \"message\": \"创建歌单失败，请稍后重试\"}";
            }
            return "{\"success\": true, \"message\": \"歌单创建成功\", \"playlist\": " + result + "}";
        });

    // 往歌单加歌
    AddTool("self.music.add_song_to_playlist",
        "把歌曲加入指定歌单。用户说'把这首歌加到XX歌单''往XX歌单加一首晴天'时调用本工具。\n"
        "参数：\n"
        "  `song_name`: 歌名，例如：晴天\n"
        "  `playlist_name`: 歌单名称，例如：通勤路上的歌（优先用歌单名；也可用 playlist_id 传 id）\n"
        "  `artist`: 歌手名，可选，建议提供以避免加错版本\n"
        "  `playlist_id`: 歌单ID，可选（若已提供 playlist_name 可留空）\n"
        "返回：是否添加成功。",
        PropertyList({
            Property("song_name", kPropertyTypeString),
            Property("playlist_name", kPropertyTypeString, ""),
            Property("artist", kPropertyTypeString, ""),
            Property("playlist_id", kPropertyTypeString, "")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            TaskPriorityReset priority_reset(1);  // 避免同步HTTP阻塞音频任务
            auto& dm = DeviceManager::GetInstance();
            if (!dm.IsDeviceBound()) {
                return "{\"success\": false, \"message\": \"设备未绑定，请先绑定账号\"}";
            }
            auto song = properties["song_name"].value<std::string>();
            auto artist = properties["artist"].value<std::string>();
            auto pl_name = properties["playlist_name"].value<std::string>();
            auto pl_id = properties["playlist_id"].value<std::string>();
            if (song.empty()) {
                return "{\"success\": false, \"message\": \"歌名不能为空\"}";
            }
            // 优先用显式 id；否则按名字查
            if (pl_id.empty() && !pl_name.empty()) {
                pl_id = find_playlist_id_by_name(pl_name);
            }
            if (pl_id.empty()) {
                return "{\"success\": false, \"message\": \"找不到该歌单，请先用 my_playlists 查看歌单列表\"}";
            }
            if (dm.AddSongToUserPlaylist(pl_id, song, artist)) {
                return "{\"success\": true, \"message\": \"已把《" + song + "》加入歌单\"}";
            }
            return "{\"success\": false, \"message\": \"加歌失败（可能歌曲已在歌单中或服务异常）\"}";
        });

    // 删除歌单
    AddTool("self.music.delete_playlist",
        "删除一个歌单。用户说'删除歌单XX''把XX歌单删掉'时调用本工具。\n"
        "参数：\n"
        "  `playlist_name`: 歌单名称（优先）；也可用 playlist_id 传 id\n"
        "  `playlist_id`: 歌单ID，可选\n"
        "返回：是否删除成功。",
        PropertyList({
            Property("playlist_name", kPropertyTypeString, ""),
            Property("playlist_id", kPropertyTypeString, "")
        }),
        [](const PropertyList& properties) -> ReturnValue {
            TaskPriorityReset priority_reset(1);  // 避免同步HTTP阻塞音频任务
            auto& dm = DeviceManager::GetInstance();
            if (!dm.IsDeviceBound()) {
                return "{\"success\": false, \"message\": \"设备未绑定，请先绑定账号\"}";
            }
            auto pl_name = properties["playlist_name"].value<std::string>();
            auto pl_id = properties["playlist_id"].value<std::string>();
            if (pl_id.empty() && !pl_name.empty()) {
                pl_id = find_playlist_id_by_name(pl_name);
            }
            if (pl_id.empty()) {
                return "{\"success\": false, \"message\": \"找不到该歌单，请先用 my_playlists 查看歌单列表\"}";
            }
            if (dm.DeleteUserPlaylist(pl_id)) {
                return "{\"success\": true, \"message\": \"歌单已删除\"}";
            }
            return "{\"success\": false, \"message\": \"删除歌单失败\"}";
        });

    // 播放整个歌单工具
    AddTool("self.music.play_playlist",
        "播放整个歌单，连续播放歌单中的所有歌曲。\n"
        "parameters:\n"
        "  `songs`: JSON格式的歌曲数组，每首歌必须包含 title 和 artist 字段\n"
        "return:\n"
        "  开始播放歌单的状态信息",
        PropertyList({
            Property("songs", kPropertyTypeString)
        }),
        [music](const PropertyList &properties) -> ReturnValue
        {
            TaskPriorityReset priority_reset(1);  // 避免启动播放时阻塞音频任务
            auto songs_json = properties["songs"].value<std::string>();
            
            // 解析歌曲JSON数组
            cJSON* json = cJSON_Parse(songs_json.c_str());
            if (!json || !cJSON_IsArray(json)) {
                if (json) cJSON_Delete(json);
                return "{\"success\": false, \"message\": \"Invalid songs JSON format\"}";
            }
            
            std::vector<SongInfo> playlist;
            int array_size = cJSON_GetArraySize(json);
            
            for (int i = 0; i < array_size; i++) {
                cJSON* song_item = cJSON_GetArrayItem(json, i);
                if (!song_item) continue;
                
                cJSON* title = cJSON_GetObjectItem(song_item, "title");
                cJSON* artist = cJSON_GetObjectItem(song_item, "artist");
                
                if (cJSON_IsString(title) && cJSON_IsString(artist)) {
                    playlist.emplace_back(title->valuestring, artist->valuestring);
                }
            }
            
            cJSON_Delete(json);
            
            if (playlist.empty()) {
                return "{\"success\": false, \"message\": \"No valid songs found in playlist\"}";
            }
            
            // 开始播放歌单
            if (music->PlayPlaylist(playlist)) {
                return "{\"success\": true, \"message\": \"Started playing playlist with " + 
                       std::to_string(playlist.size()) + " songs\"}";
            } else {
                return "{\"success\": false, \"message\": \"Failed to start playlist\"}";
            }
        });

    // 按歌单名播放（语音推荐工具）：用户说"播放XX歌单""播放我的XX歌单""播放XX歌单里的歌"时，
    // 直接传歌单名即可。设备自己读取歌单并播放，无需大模型拼大JSON，避免长文本阻塞导致卡死。
    AddTool("self.music.play_playlist_by_name",
        "播放整个歌单的唯一工具：连续播放某个歌单里的所有歌。\n"
        "用户说'播放我的歌单''播放XX歌单''播放XX歌单里的歌''放一下XX歌单''播放周杰伦歌单'时，"
        "必须且只能调用本工具，直接把歌单名传给 playlist_name，禁止先调用 my_playlists，也禁止用 play_playlist 工具。\n"
        "设备会自己读取歌单并播放，你不需要知道歌单里有哪些歌。\n"
        "参数：\n"
        "  `playlist_name`: 歌单名称，例如：Beyond、周杰伦、王杰、我喜欢\n"
        "返回：开始播放歌单的状态。",
        PropertyList({
            Property("playlist_name", kPropertyTypeString)
        }),
        [music](const PropertyList &properties) -> ReturnValue
        {
            TaskPriorityReset priority_reset(1);  // 读取歌单HTTP + 启动播放，避免阻塞音频任务
            auto& dm = DeviceManager::GetInstance();
            if (!dm.IsDeviceBound()) {
                return "{\"success\": false, \"message\": \"设备未绑定，请先绑定账号\"}";
            }
            auto pl_name = properties["playlist_name"].value<std::string>();
            if (pl_name.empty()) {
                return "{\"success\": false, \"message\": \"请告诉我歌单名称\"}";
            }

            // "我喜欢"走收藏接口，其余走自建歌单接口；两者都按 50 首分页拉全
            const bool is_fav = (pl_name == "我喜欢" || pl_name == "我喜欢的" ||
                                 pl_name == "收藏" || pl_name == "favorite");
            std::vector<SongInfo> playlist;
            bool loaded = load_playlist_songs(pl_name, is_fav, playlist);
            if (!loaded) {
                // 兜底：服务端还没有精简/分页接口时，用全量歌单 JSON 本地筛
                std::string songs_json = find_playlist_songs_json_by_name(pl_name);
                if (!songs_json.empty()) {
                    int total = -1;
                    append_playlist_page(songs_json, playlist, &total);
                }
            }
            if (playlist.empty()) {
                return "{\"success\": false, \"message\": \"找不到歌单《" + pl_name + "》，请先说'我的歌单'查看列表\"}";
            }
            if (music->PlayPlaylist(playlist)) {
                ESP_LOGI(TAG, "Play playlist '%s': %d songs", pl_name.c_str(), (int)playlist.size());
                return "{\"success\": true, \"message\": \"开始播放歌单《" + pl_name + "》，共 " +
                       std::to_string(playlist.size()) + " 首歌\"}";
            }
            return "{\"success\": false, \"message\": \"播放歌单失败\"}";
        });

    // 播放队列控制工具
    AddTool("self.music.next_song",
        "播放下一首歌曲（仅在播放歌单时有效）。\n"
        "return:\n"
        "  切换到下一首歌的状态信息",
        PropertyList(),
        [music](const PropertyList &properties) -> ReturnValue
        {
            if (!music->IsPlaylistMode()) {
                return "{\"success\": false, \"message\": \"Not in playlist mode\"}";
            }
            
            if (music->NextSong()) {
                return "{\"success\": true, \"message\": \"Switched to next song\"}";
            } else {
                return "{\"success\": false, \"message\": \"Already at last song or playlist ended\"}";
            }
        });

    AddTool("self.music.previous_song",
        "播放上一首歌曲（仅在播放歌单时有效）。\n"
        "return:\n"
        "  切换到上一首歌的状态信息",
        PropertyList(),
        [music](const PropertyList &properties) -> ReturnValue
        {
            if (!music->IsPlaylistMode()) {
                return "{\"success\": false, \"message\": \"Not in playlist mode\"}";
            }
            
            if (music->PreviousSong()) {
                return "{\"success\": true, \"message\": \"Switched to previous song\"}";
            } else {
                return "{\"success\": false, \"message\": \"Already at first song\"}";
            }
        });

    AddTool("self.music.stop_playlist",
        "停止播放歌单。\n"
        "return:\n"
        "  停止播放歌单的状态信息",
        PropertyList(),
        [music](const PropertyList &properties) -> ReturnValue
        {
            music->StopPlaylist();
            return "{\"success\": true, \"message\": \"Playlist stopped\"}";
        });
    
    // 闹钟功能工具
    AddTool("self.alarm.add",
        "Set a new alarm with music playback. When users request to set an alarm, this tool will create the alarm with specified parameters.\n"
        "🎵 Music Feature: If no specific music is provided, the system will randomly select from 40+ popular songs including Chinese pop, classics, and international hits.\n"
        "Parameters:\n"
        "  `hour`: Hour of the alarm (0-23)\n"
        "  `minute`: Minute of the alarm (0-59)\n"
        "  `repeat_mode`: Repeat mode (0=once, 1=daily, 2=weekdays, 3=weekends)\n"
        "  `label`: Optional label/description for the alarm\n"
        "  `music_name`: Optional specific music to play (leave empty for random selection)\n"
        "Returns:\n"
        "  Alarm ID if successful, error message if failed.",
        PropertyList({
            Property("hour", kPropertyTypeInteger, 0, 23),
            Property("minute", kPropertyTypeInteger, 0, 59),
            Property("repeat_mode", kPropertyTypeInteger, 0, 0, 3),
            Property("label", kPropertyTypeString, ""),
            Property("music_name", kPropertyTypeString, "")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& alarm_manager = AlarmManager::GetInstance();
            
            int hour = properties["hour"].value<int>();
            int minute = properties["minute"].value<int>();
            AlarmRepeatMode repeat_mode = (AlarmRepeatMode)properties["repeat_mode"].value<int>();
            std::string label = properties["label"].value<std::string>();
            std::string music_name = properties["music_name"].value<std::string>();
            
            int alarm_id = alarm_manager.AddAlarm(hour, minute, repeat_mode, label, music_name);
            
            if (alarm_id > 0) {
                std::string result = "已设置闹钟: " + AlarmManager::FormatTime(hour, minute);
                if (!label.empty()) {
                    result += " - " + label;
                }
                if (!music_name.empty()) {
                    result += " (音乐: " + music_name + ")";
                }
                
                // 显示重复模式
                switch (repeat_mode) {
                    case kAlarmOnce: result += " (一次性)"; break;
                    case kAlarmDaily: result += " (每日)"; break;
                    case kAlarmWeekdays: result += " (工作日)"; break;
                    case kAlarmWeekends: result += " (周末)"; break;
                    case kAlarmCustom: result += " (自定义)"; break;
                }
                
                return result;
            } else {
                return "设置闹钟失败，请检查时间格式";
            }
        });

    AddTool("self.alarm.list",
        "List all alarms and show their status.\n"
        "Returns:\n"
        "  List of all alarms with their details.",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& alarm_manager = AlarmManager::GetInstance();
            auto alarms = alarm_manager.GetAllAlarms();
            
            if (alarms.empty()) {
                return "没有设置任何闹钟";
            }
            
            std::string result = "闹钟列表:\n";
            for (const auto& alarm : alarms) {
                result += "ID " + std::to_string(alarm.id) + ": ";
                result += AlarmManager::FormatAlarmTime(alarm);
                
                if (!alarm.label.empty()) {
                    result += " - " + alarm.label;
                }
                
                switch (alarm.status) {
                    case kAlarmEnabled: result += " [启用]"; break;
                    case kAlarmDisabled: result += " [禁用]"; break;
                    case kAlarmTriggered: result += " [正在响铃]"; break;
                    case kAlarmSnoozed: result += " [贪睡中]"; break;
                }
                
                if (!alarm.music_name.empty()) {
                    result += " (音乐: " + alarm.music_name + ")";
                }
                result += "\n";
            }
            
            result += "\n" + alarm_manager.GetNextAlarmInfo();
            return result;
        });

    AddTool("self.alarm.remove",
        "Remove/delete an alarm by ID.\n"
        "Parameters:\n"
        "  `alarm_id`: ID of the alarm to remove\n"
        "Returns:\n"
        "  Success or error message.",
        PropertyList({
            Property("alarm_id", kPropertyTypeInteger)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& alarm_manager = AlarmManager::GetInstance();
            int alarm_id = properties["alarm_id"].value<int>();
            
            if (alarm_manager.RemoveAlarm(alarm_id)) {
                return "已删除闹钟 ID " + std::to_string(alarm_id);
            } else {
                return "未找到闹钟 ID " + std::to_string(alarm_id);
            }
        });

    AddTool("self.alarm.toggle",
        "Enable or disable an alarm by ID.\n"
        "Parameters:\n"
        "  `alarm_id`: ID of the alarm to toggle\n"
        "  `enabled`: True to enable, false to disable\n"
        "Returns:\n"
        "  Success or error message.",
        PropertyList({
            Property("alarm_id", kPropertyTypeInteger),
            Property("enabled", kPropertyTypeBoolean, true)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& alarm_manager = AlarmManager::GetInstance();
            int alarm_id = properties["alarm_id"].value<int>();
            bool enabled = properties["enabled"].value<bool>();
            
            if (alarm_manager.EnableAlarm(alarm_id, enabled)) {
                return "闹钟 ID " + std::to_string(alarm_id) + (enabled ? " 已启用" : " 已禁用");
            } else {
                return "未找到闹钟 ID " + std::to_string(alarm_id);
            }
        });

    AddTool("self.alarm.snooze",
        "Snooze the currently active alarm.\n"
        "Parameters:\n"
        "  `alarm_id`: ID of the alarm to snooze (optional, will snooze first active alarm if not specified)\n"
        "Returns:\n"
        "  Success or error message.",
        PropertyList({
            Property("alarm_id", kPropertyTypeInteger, -1)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& alarm_manager = AlarmManager::GetInstance();
            int alarm_id = properties["alarm_id"].value<int>();
            
            if (alarm_id == -1) {
                // 贪睡第一个活动的闹钟
                auto active_alarms = alarm_manager.GetActiveAlarms();
                if (!active_alarms.empty()) {
                    alarm_id = active_alarms[0].id;
                } else {
                    return "没有正在响铃的闹钟";
                }
            }
            
            if (alarm_manager.SnoozeAlarm(alarm_id)) {
                return "闹钟已贪睡5分钟";
            } else {
                return "无法贪睡闹钟，可能已达到最大贪睡次数";
            }
        });

    AddTool("self.alarm.stop",
        "Stop the currently active alarm.\n"
        "Parameters:\n"
        "  `alarm_id`: ID of the alarm to stop (optional, will stop first active alarm if not specified)\n"
        "Returns:\n"
        "  Success or error message.",
        PropertyList({
            Property("alarm_id", kPropertyTypeInteger, -1)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& alarm_manager = AlarmManager::GetInstance();
            int alarm_id = properties["alarm_id"].value<int>();
            
            if (alarm_id == -1) {
                // 停止第一个活动的闹钟
                auto active_alarms = alarm_manager.GetActiveAlarms();
                if (!active_alarms.empty()) {
                    alarm_id = active_alarms[0].id;
                } else {
                    return "没有正在响铃的闹钟";
                }
            }
            
            if (alarm_manager.StopAlarm(alarm_id)) {
                return "闹钟已关闭";
            } else {
                return "未找到活动的闹钟";
            }
        });

    AddTool("self.alarm.music_list",
        "Show the list of default alarm music. Users can reference this list when setting custom alarm music.\n"
        "Returns:\n"
        "  List of available alarm music songs.",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            auto music_list = app.GetDefaultAlarmMusicList();
            
            if (music_list.empty()) {
                return "暂无可用的闹钟音乐";
            }
            
            std::string result = "🎵 可用的闹钟音乐列表:\n\n";
            result += "📝 使用说明: 设置闹钟时可以指定以下任意一首歌曲作为闹钟铃声\n";
            result += "🎲 如果不指定音乐，系统会随机播放其中一首\n\n";
            
            // 分类显示音乐
            result += "🇨🇳 中文流行:\n";
            std::vector<std::string> chinese_songs = {
                "晴天", "七里香", "青花瓷", "稻香", "彩虹", "告白气球", "说好不哭",
                "夜曲", "花海", "简单爱", "听妈妈的话", "东风破", "菊花台",
                "起风了", "红豆", "好久不见", "匆匆那年", "老男孩", "那些年",
                "小幸运", "成都", "南山南", "演员", "体面", "盗将行", "大鱼"
            };
            
            for (size_t i = 0; i < chinese_songs.size() && i < 15; i++) {
                result += "  • " + chinese_songs[i] + "\n";
            }
            
            result += "\n🎼 经典怀旧:\n";
            std::vector<std::string> classic_songs = {
                "新不了情", "月亮代表我的心", "甜蜜蜜", "我只在乎你",
                "友谊之光", "童年", "海阔天空", "光辉岁月", "真的爱你", "喜欢你"
            };
            
            for (const auto& song : classic_songs) {
                result += "  • " + song + "\n";
            }
            
            result += "\n🌍 国际流行:\n";
            std::vector<std::string> international_songs = {
                "closer", "sugar", "shape of you", "despacito", 
                "perfect", "happier", "someone like you"
            };
            
            for (const auto& song : international_songs) {
                result += "  • " + song + "\n";
            }
            
            result += "\n💡 示例: \"明天早上7点播放青花瓷叫我起床\"";
            return result;
        });

    AddTool("self.alarm.test_music_ui",
        "Test the new vinyl record music UI interface. This tool will simulate a music playback to showcase the new rotating vinyl record interface.\n"
        "Parameters:\n"
        "  `song_name`: Name of the song to display (optional)\n"
        "  `duration`: Test duration in seconds (default 10 seconds)\n"
        "Returns:\n"
        "  Status message about the UI test.",
        PropertyList({
            Property("song_name", kPropertyTypeString, "晴天"),
            Property("duration", kPropertyTypeInteger, 10, 5, 60)
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            auto display = board.GetDisplay();
            
            if (!display) {
                return "显示器不可用，无法测试音乐界面";
            }
            
            std::string song_name = properties["song_name"].value<std::string>();
            int duration = properties["duration"].value<int>();
            
            if (song_name.empty()) {
                song_name = "UI测试 - 旋转唱片界面";
            }
            
            // 显示音乐界面
            display->SetMusicProgress(song_name.c_str(), 0, duration, 0.0f);
            
            return "🎵 已启动音乐界面测试！\n"
                   "✨ 特色功能展示:\n"
                   "  🎵 旋转唱片 - 黑胶唱片持续旋转\n"
                   "  📡 唱片臂 - 自动放下/收起动画\n" 
                   "  📊 进度条 - 实时显示播放进度\n"
                   "  ⏰ 时间显示 - 当前时间/总时长\n"
                   "  🌊 音波装饰 - 动态音乐波形\n"
                   "测试时长: " + std::to_string(duration) + " 秒\n"
                   "歌曲: " + song_name;
        });
#endif

    // Restore the original tools list to the end of the tools list
    tools_.insert(tools_.end(), original_tools.begin(), original_tools.end());
}

void McpServer::AddUserOnlyTools() {
    // System tools
    AddUserOnlyTool("self.get_system_info",
        "Get the system information",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& board = Board::GetInstance();
            return board.GetSystemInfoJson();
        });

    AddUserOnlyTool("self.reboot", "Reboot the system",
        PropertyList(),
        [this](const PropertyList& properties) -> ReturnValue {
            auto& app = Application::GetInstance();
            app.Schedule([&app]() {
                ESP_LOGW(TAG, "User requested reboot");
                vTaskDelay(pdMS_TO_TICKS(1000));

                app.Reboot();
            });
            return true;
        });

    // Firmware upgrade
    AddUserOnlyTool("self.upgrade_firmware", "Upgrade firmware from a specific URL. This will download and install the firmware, then reboot the device.",
        PropertyList({
            Property("url", kPropertyTypeString, "The URL of the firmware binary file to download and install")
        }),
        [this](const PropertyList& properties) -> ReturnValue {
            auto url = properties["url"].value<std::string>();
            ESP_LOGI(TAG, "User requested firmware upgrade from URL: %s", url.c_str());
            
            auto& app = Application::GetInstance();
            app.Schedule([url, &app]() {
                auto ota = std::make_unique<Ota>();
                
                bool success = app.UpgradeFirmware(*ota, url);
                if (!success) {
                    ESP_LOGE(TAG, "Firmware upgrade failed");
                }
            });
            
            return true;
        });

    // Display control
#ifdef HAVE_LVGL
    auto display = dynamic_cast<LvglDisplay*>(Board::GetInstance().GetDisplay());
    if (display) {
        AddUserOnlyTool("self.screen.get_info", "Information about the screen, including width, height, etc.",
            PropertyList(),
            [display](const PropertyList& properties) -> ReturnValue {
                cJSON *json = cJSON_CreateObject();
                cJSON_AddNumberToObject(json, "width", display->width());
                cJSON_AddNumberToObject(json, "height", display->height());
                if (dynamic_cast<OledDisplay*>(display)) {
                    cJSON_AddBoolToObject(json, "monochrome", true);
                } else {
                    cJSON_AddBoolToObject(json, "monochrome", false);
                }
                return json;
            });

#if CONFIG_LV_USE_SNAPSHOT
        AddUserOnlyTool("self.screen.snapshot", "Snapshot the screen and upload it to a specific URL",
            PropertyList({
                Property("url", kPropertyTypeString),
                Property("quality", kPropertyTypeInteger, 80, 1, 100)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto quality = properties["quality"].value<int>();

                std::string jpeg_data;
                if (!display->SnapshotToJpeg(jpeg_data, quality)) {
                    throw std::runtime_error("Failed to snapshot screen");
                }

                ESP_LOGI(TAG, "Upload snapshot %u bytes to %s", jpeg_data.size(), url.c_str());
                
                // 构造multipart/form-data请求体
                std::string boundary = "----ESP32_SCREEN_SNAPSHOT_BOUNDARY";
                
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);
                http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
                if (!http->Open("POST", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                {
                    // 文件字段头部
                    std::string file_header;
                    file_header += "--" + boundary + "\r\n";
                    file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"screenshot.jpg\"\r\n";
                    file_header += "Content-Type: image/jpeg\r\n";
                    file_header += "\r\n";
                    http->Write(file_header.c_str(), file_header.size());
                }

                // JPEG数据
                http->Write((const char*)jpeg_data.data(), jpeg_data.size());

                {
                    // multipart尾部
                    std::string multipart_footer;
                    multipart_footer += "\r\n--" + boundary + "--\r\n";
                    http->Write(multipart_footer.c_str(), multipart_footer.size());
                }
                http->Write("", 0);

                if (http->GetStatusCode() != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(http->GetStatusCode()));
                }
                std::string result = http->ReadAll();
                http->Close();
                ESP_LOGI(TAG, "Snapshot screen result: %s", result.c_str());
                return true;
            });
        
        AddUserOnlyTool("self.screen.preview_image", "Preview an image on the screen",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [display](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                auto http = Board::GetInstance().GetNetwork()->CreateHttp(3);

                if (!http->Open("GET", url)) {
                    throw std::runtime_error("Failed to open URL: " + url);
                }
                int status_code = http->GetStatusCode();
                if (status_code != 200) {
                    throw std::runtime_error("Unexpected status code: " + std::to_string(status_code));
                }

                size_t content_length = http->GetBodyLength();
                char* data = (char*)heap_caps_malloc(content_length, MALLOC_CAP_8BIT);
                if (data == nullptr) {
                    throw std::runtime_error("Failed to allocate memory for image: " + url);
                }
                size_t total_read = 0;
                while (total_read < content_length) {
                    int ret = http->Read(data + total_read, content_length - total_read);
                    if (ret < 0) {
                        heap_caps_free(data);
                        throw std::runtime_error("Failed to download image: " + url);
                    }
                    if (ret == 0) {
                        break;
                    }
                    total_read += ret;
                }
                http->Close();

                auto image = std::make_unique<LvglAllocatedImage>(data, content_length);
                display->SetPreviewImage(std::move(image));
                return true;
            });
#endif // CONFIG_LV_USE_SNAPSHOT
    }
#endif // HAVE_LVGL

    // Assets download url
    auto& assets = Assets::GetInstance();
    if (assets.partition_valid()) {
        AddUserOnlyTool("self.assets.set_download_url", "Set the download url for the assets",
            PropertyList({
                Property("url", kPropertyTypeString)
            }),
            [](const PropertyList& properties) -> ReturnValue {
                auto url = properties["url"].value<std::string>();
                Settings settings("assets", true);
                settings.SetString("download_url", url);
                return true;
            });
    }
}

void McpServer::AddTool(McpTool* tool) {
    // Prevent adding duplicate tools
    if (std::find_if(tools_.begin(), tools_.end(), [tool](const McpTool* t) { return t->name() == tool->name(); }) != tools_.end()) {
        ESP_LOGW(TAG, "Tool %s already added", tool->name().c_str());
        return;
    }

    ESP_LOGI(TAG, "Add tool: %s%s", tool->name().c_str(), tool->user_only() ? " [user]" : "");
    tools_.push_back(tool);
}

void McpServer::AddTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    AddTool(new McpTool(name, description, properties, callback));
}

void McpServer::AddUserOnlyTool(const std::string& name, const std::string& description, const PropertyList& properties, std::function<ReturnValue(const PropertyList&)> callback) {
    auto tool = new McpTool(name, description, properties, callback);
    tool->set_user_only(true);
    AddTool(tool);
}

void McpServer::ParseMessage(const std::string& message) {
    cJSON* json = cJSON_Parse(message.c_str());
    if (json == nullptr) {
        ESP_LOGE(TAG, "Failed to parse MCP message: %s", message.c_str());
        return;
    }
    ParseMessage(json);
    cJSON_Delete(json);
}

void McpServer::ParseCapabilities(const cJSON* capabilities) {
    auto vision = cJSON_GetObjectItem(capabilities, "vision");
    if (cJSON_IsObject(vision)) {
        auto url = cJSON_GetObjectItem(vision, "url");
        auto token = cJSON_GetObjectItem(vision, "token");
        if (cJSON_IsString(url)) {
            auto camera = Board::GetInstance().GetCamera();
            if (camera) {
                std::string url_str = std::string(url->valuestring);
                std::string token_str;
                if (cJSON_IsString(token)) {
                    token_str = std::string(token->valuestring);
                }
                camera->SetExplainUrl(url_str, token_str);
            }
        }
    }
}

void McpServer::ParseMessage(const cJSON* json) {
    // Check JSONRPC version
    auto version = cJSON_GetObjectItem(json, "jsonrpc");
    if (version == nullptr || !cJSON_IsString(version) || strcmp(version->valuestring, "2.0") != 0) {
        ESP_LOGE(TAG, "Invalid JSONRPC version: %s", version ? version->valuestring : "null");
        return;
    }
    
    // Check method
    auto method = cJSON_GetObjectItem(json, "method");
    if (method == nullptr || !cJSON_IsString(method)) {
        ESP_LOGE(TAG, "Missing method");
        return;
    }
    
    auto method_str = std::string(method->valuestring);
    if (method_str.find("notifications") == 0) {
        return;
    }
    
    // Check params
    auto params = cJSON_GetObjectItem(json, "params");
    if (params != nullptr && !cJSON_IsObject(params)) {
        ESP_LOGE(TAG, "Invalid params for method: %s", method_str.c_str());
        return;
    }

    auto id = cJSON_GetObjectItem(json, "id");
    if (id == nullptr || !cJSON_IsNumber(id)) {
        ESP_LOGE(TAG, "Invalid id for method: %s", method_str.c_str());
        return;
    }
    auto id_int = id->valueint;
    
    if (method_str == "initialize") {
        if (cJSON_IsObject(params)) {
            auto capabilities = cJSON_GetObjectItem(params, "capabilities");
            if (cJSON_IsObject(capabilities)) {
                ParseCapabilities(capabilities);
            }
        }
        auto app_desc = esp_app_get_description();
        std::string message = "{\"protocolVersion\":\"2024-11-05\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"" BOARD_NAME "\",\"version\":\"";
        message += app_desc->version;
        message += "\"}}";
        ReplyResult(id_int, message);
    } else if (method_str == "tools/list") {
        std::string cursor_str = "";
        bool list_user_only_tools = false;
        if (params != nullptr) {
            auto cursor = cJSON_GetObjectItem(params, "cursor");
            if (cJSON_IsString(cursor)) {
                cursor_str = std::string(cursor->valuestring);
            }
            auto with_user_tools = cJSON_GetObjectItem(params, "withUserTools");
            if (cJSON_IsBool(with_user_tools)) {
                list_user_only_tools = with_user_tools->valueint == 1;
            }
        }
        GetToolsList(id_int, cursor_str, list_user_only_tools);
    } else if (method_str == "tools/call") {
        if (!cJSON_IsObject(params)) {
            ESP_LOGE(TAG, "tools/call: Missing params");
            ReplyError(id_int, "Missing params");
            return;
        }
        auto tool_name = cJSON_GetObjectItem(params, "name");
        if (!cJSON_IsString(tool_name)) {
            ESP_LOGE(TAG, "tools/call: Missing name");
            ReplyError(id_int, "Missing name");
            return;
        }
        auto tool_arguments = cJSON_GetObjectItem(params, "arguments");
        if (tool_arguments != nullptr && !cJSON_IsObject(tool_arguments)) {
            ESP_LOGE(TAG, "tools/call: Invalid arguments");
            ReplyError(id_int, "Invalid arguments");
            return;
        }
        DoToolCall(id_int, std::string(tool_name->valuestring), tool_arguments);
    } else {
        ESP_LOGE(TAG, "Method not implemented: %s", method_str.c_str());
        ReplyError(id_int, "Method not implemented: " + method_str);
    }
}

void McpServer::ReplyResult(int id, const std::string& result) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id) + ",\"result\":";
    payload += result;
    payload += "}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::ReplyError(int id, const std::string& message) {
    std::string payload = "{\"jsonrpc\":\"2.0\",\"id\":";
    payload += std::to_string(id);
    payload += ",\"error\":{\"message\":\"";
    payload += message;
    payload += "\"}}";
    Application::GetInstance().SendMcpMessage(payload);
}

void McpServer::GetToolsList(int id, const std::string& cursor, bool list_user_only_tools) {
    const int max_payload_size = 8000;
    std::string json = "{\"tools\":[";
    
    bool found_cursor = cursor.empty();
    auto it = tools_.begin();
    std::string next_cursor = "";
    
    while (it != tools_.end()) {
        // 如果我们还没有找到起始位置，继续搜索
        if (!found_cursor) {
            if ((*it)->name() == cursor) {
                found_cursor = true;
            } else {
                ++it;
                continue;
            }
        }

        if (!list_user_only_tools && (*it)->user_only()) {
            ++it;
            continue;
        }
        
        // 添加tool前检查大小
        std::string tool_json = (*it)->to_json() + ",";
        if (json.length() + tool_json.length() + 30 > max_payload_size) {
            // 如果添加这个tool会超出大小限制，设置next_cursor并退出循环
            next_cursor = (*it)->name();
            break;
        }
        
        json += tool_json;
        ++it;
    }
    
    if (json.back() == ',') {
        json.pop_back();
    }
    
    if (json.back() == '[' && !tools_.empty()) {
        // 如果没有添加任何tool，返回错误
        ESP_LOGE(TAG, "tools/list: Failed to add tool %s because of payload size limit", next_cursor.c_str());
        ReplyError(id, "Failed to add tool " + next_cursor + " because of payload size limit");
        return;
    }

    if (next_cursor.empty()) {
        json += "]}";
    } else {
        json += "],\"nextCursor\":\"" + next_cursor + "\"}";
    }
    
    ReplyResult(id, json);
}

void McpServer::DoToolCall(int id, const std::string& tool_name, const cJSON* tool_arguments) {
    auto tool_iter = std::find_if(tools_.begin(), tools_.end(), 
                                 [&tool_name](const McpTool* tool) { 
                                     return tool->name() == tool_name; 
                                 });
    
    if (tool_iter == tools_.end()) {
        ESP_LOGE(TAG, "tools/call: Unknown tool: %s", tool_name.c_str());
        ReplyError(id, "Unknown tool: " + tool_name);
        return;
    }

    PropertyList arguments = (*tool_iter)->properties();
    try {
        for (auto& argument : arguments) {
            bool found = false;
            if (cJSON_IsObject(tool_arguments)) {
                auto value = cJSON_GetObjectItem(tool_arguments, argument.name().c_str());
                if (argument.type() == kPropertyTypeBoolean && cJSON_IsBool(value)) {
                    argument.set_value<bool>(value->valueint == 1);
                    found = true;
                } else if (argument.type() == kPropertyTypeInteger && cJSON_IsNumber(value)) {
                    argument.set_value<int>(value->valueint);
                    found = true;
                } else if (argument.type() == kPropertyTypeString && cJSON_IsString(value)) {
                    argument.set_value<std::string>(value->valuestring);
                    found = true;
                }
            }

            if (!argument.has_default_value() && !found) {
                ESP_LOGE(TAG, "tools/call: Missing valid argument: %s", argument.name().c_str());
                ReplyError(id, "Missing valid argument: " + argument.name());
                return;
            }
        }
    } catch (const std::exception& e) {
        ESP_LOGE(TAG, "tools/call: %s", e.what());
        ReplyError(id, e.what());
        return;
    }

    // Use main thread to call the tool
    auto& app = Application::GetInstance();
    app.Schedule([this, id, tool_iter, arguments = std::move(arguments)]() {
        try {
            ReplyResult(id, (*tool_iter)->Call(arguments));
        } catch (const std::exception& e) {
            ESP_LOGE(TAG, "tools/call: %s", e.what());
            ReplyError(id, e.what());
        }
    });
}
