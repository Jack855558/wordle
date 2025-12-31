/*  WiFi softAP Example

   This example code is in the Public Domain (or CC0 licensed, at your option.)

   Unless required by applicable law or agreed to in writing, this
   software is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR
   CONDITIONS OF ANY KIND, either express or implied.
*/
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_http_server.h"
#include "cJSON.h"



#include "lwip/err.h"
#include "lwip/sys.h"

extern const char html_start[] asm("_binary_index_html_start");
extern const char html_end[] asm("_binary_index_html_end");

extern const char css_start[] asm("_binary_style_css_start");
extern const char css_end[] asm("_binary_style_css_end");

extern const char js_start[] asm("_binary_script_js_start");
extern const char js_end[] asm("_binary_script_js_end");

// Game state structure
typedef struct {
    int fd;              // File descriptor (WebSocket connection ID)
    bool connected;      // Is this player connected?
    char name[32];       // Player name (we can add this later)
} player_t;

typedef struct {
    player_t players[2];     // Two players
    int player_count;        // How many players connected
    bool game_active;        // Is a game currently running?
    char target_word[6];     // The word to guess (5 letters + null terminator)
} game_state_t;

// Global game state
static game_state_t game = {0};
static httpd_handle_t server = NULL;

/* The examples use WiFi configuration that you can set via project configuration menu.

   If you'd rather not, just change the below entries to strings with
   the config you want - ie #define EXAMPLE_WIFI_SSID "mywifissid"
*/
#define EXAMPLE_ESP_WIFI_SSID      CONFIG_ESP_WIFI_SSID
#define EXAMPLE_ESP_WIFI_PASS      CONFIG_ESP_WIFI_PASSWORD
#define EXAMPLE_ESP_WIFI_CHANNEL   CONFIG_ESP_WIFI_CHANNEL
#define EXAMPLE_MAX_STA_CONN       CONFIG_ESP_MAX_STA_CONN

#if CONFIG_ESP_GTK_REKEYING_ENABLE
#define EXAMPLE_GTK_REKEY_INTERVAL CONFIG_ESP_GTK_REKEY_INTERVAL
#else
#define EXAMPLE_GTK_REKEY_INTERVAL 0
#endif

static const char *TAG = "wifi softAP";

static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                    int32_t event_id, void* event_data)
{
    if (event_id == WIFI_EVENT_AP_STACONNECTED) {
        wifi_event_ap_staconnected_t* event = (wifi_event_ap_staconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" join, AID=%d",
                 MAC2STR(event->mac), event->aid);
    } else if (event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        wifi_event_ap_stadisconnected_t* event = (wifi_event_ap_stadisconnected_t*) event_data;
        ESP_LOGI(TAG, "station "MACSTR" leave, AID=%d, reason=%d",
                 MAC2STR(event->mac), event->aid, event->reason);
    }
}


void wifi_init_softap(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                        ESP_EVENT_ANY_ID,
                                                        &wifi_event_handler,
                                                        NULL,
                                                        NULL));

    wifi_config_t wifi_config = {
        .ap = {
            .ssid = EXAMPLE_ESP_WIFI_SSID,
            .ssid_len = strlen(EXAMPLE_ESP_WIFI_SSID),
            .channel = EXAMPLE_ESP_WIFI_CHANNEL,
            .password = EXAMPLE_ESP_WIFI_PASS,
            .max_connection = EXAMPLE_MAX_STA_CONN,
#ifdef CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT
            .authmode = WIFI_AUTH_WPA3_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
#else /* CONFIG_ESP_WIFI_SOFTAP_SAE_SUPPORT */
            .authmode = WIFI_AUTH_WPA2_PSK,
#endif
            .pmf_cfg = {
                    .required = true,
            },
#ifdef CONFIG_ESP_WIFI_BSS_MAX_IDLE_SUPPORT
            .bss_max_idle_cfg = {
                .period = WIFI_AP_DEFAULT_MAX_IDLE_PERIOD,
                .protected_keep_alive = 1,
            },
#endif
            .gtk_rekey_interval = EXAMPLE_GTK_REKEY_INTERVAL,
        },
    };
    if (strlen(EXAMPLE_ESP_WIFI_PASS) == 0) {
        wifi_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_softap finished. SSID:%s password:%s channel:%d",
             EXAMPLE_ESP_WIFI_SSID, EXAMPLE_ESP_WIFI_PASS, EXAMPLE_ESP_WIFI_CHANNEL);
}

// Handler for the main HTML page
static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, html_start, html_end - html_start);
    return ESP_OK;
}

// Handler for CSS
static esp_err_t css_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/css");
    httpd_resp_send(req, css_start, css_end - css_start);
    return ESP_OK;
}

// Handler for JavaScript
static esp_err_t js_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/javascript");
    httpd_resp_send(req, js_start, js_end - js_start);
    return ESP_OK;
}








//============================= JSON Game Logic ============================


// Check a guess and return result array
// 0 = wrong letter, 1 = correct letter wrong position, 2 = correct letter correct position
static void check_guess(const char *guess, const char *target, int *result)
{
    // First pass: mark exact matches
    bool target_used[5] = {false};
    bool guess_used[5] = {false};
    
    for (int i = 0; i < 5; i++) {
        if (guess[i] == target[i]) {
            result[i] = 2;  // Correct position
            target_used[i] = true;
            guess_used[i] = true;
        } else {
            result[i] = 0;  // Default to wrong
        }
    }
    
    // Second pass: mark letters in wrong position
    for (int i = 0; i < 5; i++) {
        if (guess_used[i]) continue;  // Already marked as correct position
        
        for (int j = 0; j < 5; j++) {
            if (!target_used[j] && guess[i] == target[j]) {
                result[i] = 1;  // Wrong position
                target_used[j] = true;
                break;
            }
        }
    }
}






// +++++++++++++++++++++++++++++++++ Websocket +++++++++++++++++++++++++++++++++++++

// Find a free player slot and add the connection
static int add_player(int fd)
{
    for (int i = 0; i < 2; i++) {
        if (!game.players[i].connected) {
            game.players[i].fd = fd;
            game.players[i].connected = true;
            game.player_count++;
            ESP_LOGI(TAG, "Player %d joined. Total players: %d", i + 1, game.player_count);
            return i;  // Return player index (0 or 1)
        }
    }
    return -1;  // No slots available
}

// Remove a player when they disconnect
static void remove_player(int fd)
{
    for (int i = 0; i < 2; i++) {
        if (game.players[i].connected && game.players[i].fd == fd) {
            game.players[i].connected = false;
            game.player_count--;
            ESP_LOGI(TAG, "Player %d left. Total players: %d", i + 1, game.player_count);
            
            // If a player leaves, end the game
            if (game.game_active) {
                game.game_active = false;
                ESP_LOGI(TAG, "Game ended because player left");
            }
            break;
        }
    }
}

// Send a message to a specific player
static void send_to_player(int player_index, const char *message)
{
    if (!game.players[player_index].connected) {
        return;  // Player not connected
    }
    
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    ws_pkt.payload = (uint8_t*)message;
    ws_pkt.len = strlen(message);
    ws_pkt.type = HTTPD_WS_TYPE_TEXT;
    
    // Send the message using the player's file descriptor
    httpd_ws_send_frame_async(server, game.players[player_index].fd, &ws_pkt);
    
    ESP_LOGI(TAG, "Sent to player %d: %s", player_index + 1, message);
}

// Broadcast a message to all connected players
static void broadcast_to_all(const char *message)
{
    ESP_LOGI(TAG, "Broadcasting: %s", message);
    for (int i = 0; i < 2; i++) {
        if (game.players[i].connected) {
            send_to_player(i, message);
        }
    }
}

// WebSocket handler - called when WebSocket receives a message
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        // This is the initial WebSocket handshake - NO frame receiving here!
        ESP_LOGI(TAG, "WebSocket handshake done, new connection opened");
        
        int fd = httpd_req_to_sockfd(req);
        int player_index = add_player(fd);
        
        if (player_index == -1) {
            ESP_LOGE(TAG, "Game full! Cannot accept more players");
            return ESP_FAIL;
        }
        
        // Send welcome message
        char welcome_msg[100];
        sprintf(welcome_msg, "Welcome! You are Player %d", player_index + 1);
        send_to_player(player_index, welcome_msg);
        
        // If we now have 2 players, start the game!
        if (game.player_count == 2) {
        broadcast_to_all("Game starting! Both players connected!");
        game.game_active = true;
        strcpy(game.target_word, "CRANE");  
        ESP_LOGI(TAG, "Target word set to: %s", game.target_word);  
        }
        
        return ESP_OK;
    }
    
    // If we get here, it's an actual WebSocket message (not the handshake)
    httpd_ws_frame_t ws_pkt;
    memset(&ws_pkt, 0, sizeof(httpd_ws_frame_t));
    
    // First, get the frame length
    esp_err_t ret = httpd_ws_recv_frame(req, &ws_pkt, 0);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "httpd_ws_recv_frame failed to get frame len with %d", ret);
        return ret;
    }
    
    ESP_LOGI(TAG, "Received packet with length %d", ws_pkt.len);
    
    // If length is 0, nothing to do
    if (ws_pkt.len) {
        // Allocate buffer for the message
        uint8_t *buf = calloc(1, ws_pkt.len + 1);
        if (buf == NULL) {
            ESP_LOGE(TAG, "Failed to allocate memory");
            return ESP_ERR_NO_MEM;
        }
        ws_pkt.payload = buf;
        
        // Actually receive the message
        ret = httpd_ws_recv_frame(req, &ws_pkt, ws_pkt.len);
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "httpd_ws_recv_frame failed with %d", ret);
            free(buf);
            return ret;
        }
        
        ESP_LOGI(TAG, "Got message: %s", ws_pkt.payload);

        // Parse the JSON message
        cJSON *json = cJSON_Parse((char*)ws_pkt.payload);
        if (json == NULL) {
            ESP_LOGE(TAG, "Failed to parse JSON");
            free(buf);
            return ESP_ERR_INVALID_ARG;
        }

        // Get the message type
        cJSON *type_item = cJSON_GetObjectItem(json, "type");
        if (type_item == NULL || !cJSON_IsString(type_item)) {
            ESP_LOGE(TAG, "No type field in JSON");
            cJSON_Delete(json);
            free(buf);
            return ESP_ERR_INVALID_ARG;
        }

        const char *msg_type = type_item->valuestring;
        ESP_LOGI(TAG, "Message type: %s", msg_type);

        // Handle different message types
        if (strcmp(msg_type, "guess") == 0) {
            // Get the guessed word
            cJSON *word_item = cJSON_GetObjectItem(json, "word");
            if (word_item != NULL && cJSON_IsString(word_item)) {
                const char *guess = word_item->valuestring;
                ESP_LOGI(TAG, "Player guessed: %s", guess);
                
                // Check the guess against target word
                int result[5];
                check_guess(guess, game.target_word, result);
                
                // Create response JSON
                cJSON *response = cJSON_CreateObject();
                cJSON_AddStringToObject(response, "type", "result");
                cJSON_AddStringToObject(response, "word", guess);
                cJSON *result_array = cJSON_CreateIntArray(result, 5);
                cJSON_AddItemToObject(response, "result", result_array);
                
                char *response_str = cJSON_Print(response);
                
                // Broadcast result to all players
                broadcast_to_all(response_str);
                
                free(response_str);
                cJSON_Delete(response);
            }
        }

        cJSON_Delete(json);
        free(buf);
    }
    
    return ret;
}

static httpd_handle_t start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.lru_purge_enable = true;

    ESP_LOGI(TAG, "Starting HTTP server on port: '%d'", config.server_port);
    
    if (httpd_start(&server, &config) == ESP_OK) {
        // Register URI handlers
        httpd_uri_t root = {
            .uri       = "/",
            .method    = HTTP_GET,
            .handler   = root_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &root);

        httpd_uri_t css = {
            .uri       = "/style.css",
            .method    = HTTP_GET,
            .handler   = css_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &css);

        httpd_uri_t js = {
            .uri       = "/script.js",
            .method    = HTTP_GET,
            .handler   = js_get_handler,
            .user_ctx  = NULL
        };
        httpd_register_uri_handler(server, &js);

        httpd_uri_t ws = {
            .uri       = "/ws",
            .method    = HTTP_GET,
            .handler   = ws_handler,
            .user_ctx  = NULL,
            .is_websocket = true  // This is the key difference!
        };
        httpd_register_uri_handler(server, &ws);
        ESP_LOGI(TAG, "WebSocket handler registered at /ws");

        ESP_LOGI(TAG, "Web server started successfully");
        return server;
    }

    ESP_LOGE(TAG, "Error starting server!");
    return NULL;
}








void app_main(void)
{
    //Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      ESP_ERROR_CHECK(nvs_flash_erase());
      ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_LOGI(TAG, "ESP_WIFI_MODE_AP");
    wifi_init_softap();
    
    // Start web server
    start_webserver();
    ESP_LOGI(TAG, "Server ready! Connect to WiFi and visit http://192.168.4.1");
}
