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
#include "words.h"


#include "lwip/err.h"
#include "lwip/sys.h"



extern const char html_start[] asm("_binary_index_html_start");
extern const char html_end[] asm("_binary_index_html_end");

extern const char css_start[] asm("_binary_style_css_start");
extern const char css_end[] asm("_binary_style_css_end");

extern const char js_start[] asm("_binary_script_js_start");
extern const char js_end[] asm("_binary_script_js_end");

typedef struct {
    int fd;
    bool connected;
    char name[32];
    int guesses_used;
    bool has_won;
    int score;
    bool waiting_for_opponent;  //Has the opponent submitted a guess
} player_t;

typedef struct {
    player_t players[2];
    int player_count;
    bool game_active;
    char target_word[6];
    int round_number;        // Current round
    time_t round_start_time; // When the round started (for 45s timer)
    bool round_over;         // Has this round ended?
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

// Initialize a new round
static void start_new_round(void)
{
    game.round_number++;
    game.round_over = false;
    game.round_start_time = time(NULL);
    
    // Reset player states for new round
    for (int i = 0; i < 2; i++) {
        if (game.players[i].connected) {
            game.players[i].guesses_used = 0;
            game.players[i].has_won = false;
            game.players[i].waiting_for_opponent = false;  
        }
    }
    
    // Pick a random word from the list
    srand(time(NULL) + game.round_number);
    int random_index = rand() % WORD_LIST_SIZE;
    strcpy(game.target_word, WORD_LIST[random_index]);
    
    ESP_LOGI(TAG, "Round %d started! Target word: %s", game.round_number, game.target_word);

     // Calculate hints based on CURRENT scores and NEW word
    int score_diff = abs(game.players[0].score - game.players[1].score);
    int losing_player = -1;
    
    if (game.players[0].score < game.players[1].score) {
        losing_player = 0;
    } else if (game.players[1].score < game.players[0].score) {
        losing_player = 1;
    }
    
    // Send round start message to both players
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "round_start");
    cJSON_AddNumberToObject(msg, "round", game.round_number);
    cJSON_AddNumberToObject(msg, "time_limit", 45);
    
     // Add hint for losing player (from NEW word)
    if (losing_player >= 0 && score_diff >= 2) {
        int hint_position = rand() % 5;
        char hint_letter = game.target_word[hint_position];
        
        cJSON_AddNumberToObject(msg, "hint_player", losing_player);
        cJSON_AddNumberToObject(msg, "hint_position", hint_position);
        cJSON_AddStringToObject(msg, "hint_letter", (char[]){hint_letter, '\0'});
        
        if (score_diff >= 4) {
            cJSON_AddStringToObject(msg, "hint_type", "green");
        } else {
            cJSON_AddStringToObject(msg, "hint_type", "yellow");
        }
        
        ESP_LOGI(TAG, "Giving hint to player %d for new word '%s': letter '%c' at position %d (%s)", 
                 losing_player + 1, game.target_word, hint_letter, hint_position, 
                 (score_diff >= 4) ? "green" : "yellow");
    }

    char *msg_str = cJSON_Print(msg);
    broadcast_to_all(msg_str);
    
    free(msg_str);
    cJSON_Delete(msg);
}

// End the current round and determine winner
static void end_round(void)
{
    if (game.round_over) return; // Already ended
    
    game.round_over = true;
    
    ESP_LOGI(TAG, "Round %d ended!", game.round_number);
    
    // Determine winner(s)
    int winner = -1;  // -1 = tie/no winner, 0 = player 1, 1 = player 2
    
    // Check if anyone won
    bool p1_won = game.players[0].connected && game.players[0].has_won;
    bool p2_won = game.players[1].connected && game.players[1].has_won;
    
    if (p1_won && p2_won) {
        // Both won - check who used fewer guesses
        if (game.players[0].guesses_used < game.players[1].guesses_used) {
            winner = 0;
            game.players[0].score++;
        } else if (game.players[1].guesses_used < game.players[0].guesses_used) {
            winner = 1;
            game.players[1].score++;
        } else {
            winner = -1; // Tie - same number of guesses
        }
    } else if (p1_won) {
        winner = 0;
        game.players[0].score++;
    } else if (p2_won) {
        winner = 1;
        game.players[1].score++;
    }
    
    // Send round results to both players
    cJSON *msg = cJSON_CreateObject();
    cJSON_AddStringToObject(msg, "type", "round_end");
    cJSON_AddNumberToObject(msg, "winner", winner);
    cJSON_AddStringToObject(msg, "target_word", game.target_word);
    cJSON_AddNumberToObject(msg, "player1_score", game.players[0].score);
    cJSON_AddNumberToObject(msg, "player2_score", game.players[1].score);

    char *msg_str = cJSON_Print(msg);
    broadcast_to_all(msg_str);
    free(msg_str);
    cJSON_Delete(msg);
}


// Check if 45 seconds have passed since round start
// static bool is_time_up(void)
// {
//     if (!game.game_active || game.round_over) return false;
    
//     time_t current_time = time(NULL);
//     double elapsed = difftime(current_time, game.round_start_time);
    
//     return elapsed >= 45.0;
// }

// Task that checks if round time has expired
// static void timer_check_task(void *pvParameters)
// {
//     while (1) {
//         vTaskDelay(1000 / portTICK_PERIOD_MS);
        
//         if (game.game_active && !game.round_over && game.round_start_time > 0) {
//             time_t current_time = time(NULL);
//             double elapsed = difftime(current_time, game.round_start_time);
            
//             // Check if 45 seconds passed
//             if (elapsed >= 45.0) {
//                 ESP_LOGI(TAG, "Guess timer expired!");
                
//                 // Force submit for any player who hasn't guessed
//                 bool anyone_timeout = false;
//                 for (int i = 0; i < 2; i++) {
//                     if (game.players[i].connected && 
//                         !game.players[i].waiting_for_opponent &&
//                         !game.players[i].has_won) {
                        
//                         ESP_LOGI(TAG, "Player %d timed out", i + 1);
//                         game.players[i].waiting_for_opponent = true;
//                         anyone_timeout = true;
                        
//                         // Notify that player they timed out
//                         cJSON *timeout_msg = cJSON_CreateObject();
//                         cJSON_AddStringToObject(timeout_msg, "type", "timeout");
//                         char *timeout_str = cJSON_Print(timeout_msg);
//                         send_to_player(i, timeout_str);
//                         free(timeout_str);
//                         cJSON_Delete(timeout_msg);
//                     }
//                 }
                
//                 if (anyone_timeout) {
//                     // Reset flags and continue
//                     game.players[0].waiting_for_opponent = false;
//                     game.players[1].waiting_for_opponent = false;
                    
//                     // Check if round should end
//                     if (game.players[0].guesses_used >= 5 && game.players[1].guesses_used >= 5) {
//                         end_round();
//                     } else {
//                         // Broadcast to continue
//                         cJSON *continue_msg = cJSON_CreateObject();
//                         cJSON_AddStringToObject(continue_msg, "type", "both_guessed");
//                         char *continue_str = cJSON_Print(continue_msg);
//                         broadcast_to_all(continue_str);
//                         free(continue_str);
//                         cJSON_Delete(continue_msg);
//                     }
                    
//                     // Reset timer for next guess
//                     game.round_start_time = time(NULL);
//                 }
//             }
//         }
//     }
// }








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

// WebSocket handler - called when WebSocket receives a message
static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        ESP_LOGI(TAG, "WebSocket handshake done, new connection opened");
        
        int fd = httpd_req_to_sockfd(req);
        int player_index = add_player(fd);

        if (player_index == -1) {
            ESP_LOGE(TAG, "Game full! Cannot accept more players");
            return ESP_FAIL;
        }
        
        // Just send welcome message - that's it!
        cJSON *welcome = cJSON_CreateObject();
        cJSON_AddStringToObject(welcome, "type", "welcome");
        cJSON_AddNumberToObject(welcome, "player_index", player_index);
        char *welcome_str = cJSON_Print(welcome);
        send_to_player(player_index, welcome_str);
        free(welcome_str);
        cJSON_Delete(welcome);
        
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
            cJSON *word_item = cJSON_GetObjectItem(json, "word");
            if (word_item != NULL && cJSON_IsString(word_item)) {
                const char *guess = word_item->valuestring;
                
                // Find which player made this guess
                int fd = httpd_req_to_sockfd(req);
                int player_index = -1;
                for (int i = 0; i < 2; i++) {
                    if (game.players[i].connected && game.players[i].fd == fd) {
                        player_index = i;
                        break;
                    }
                }
                
                if (player_index == -1) {
                    ESP_LOGE(TAG, "Could not find player for guess");
                    cJSON_Delete(json);
                    free(buf);
                    return ESP_ERR_INVALID_ARG;
                }
                
                // Check if they're already waiting or round is over
                if (game.players[player_index].waiting_for_opponent) {
                    ESP_LOGI(TAG, "Player %d already submitted, waiting for opponent", player_index + 1);
                    cJSON_Delete(json);
                    free(buf);
                    return ESP_OK;
                }
                
                if (game.round_over || game.players[player_index].has_won) {
                    ESP_LOGI(TAG, "Player %d tried to guess but round is over or they won", player_index + 1);
                    cJSON_Delete(json);
                    free(buf);
                    return ESP_OK;
                }
                
                ESP_LOGI(TAG, "Player %d guessed: %s", player_index + 1, guess);
                
                game.players[player_index].guesses_used++;
                game.players[player_index].waiting_for_opponent = true;
                
                // Check the guess
                int result[5];
                check_guess(guess, game.target_word, result);
                
                bool is_correct = true;
                for (int i = 0; i < 5; i++) {
                    if (result[i] != 2) {
                        is_correct = false;
                        break;
                    }
                }
                
                if (is_correct) {
                    game.players[player_index].has_won = true;
                    ESP_LOGI(TAG, "Player %d won the round!", player_index + 1);
                }
                
                // Send result to this player
                cJSON *response = cJSON_CreateObject();
                cJSON_AddStringToObject(response, "type", "result");
                cJSON_AddStringToObject(response, "word", guess);
                cJSON_AddNumberToObject(response, "player", player_index);
                cJSON *result_array = cJSON_CreateIntArray(result, 5);
                cJSON_AddItemToObject(response, "result", result_array);
                cJSON_AddBoolToObject(response, "is_correct", is_correct);
                
                char *response_str = cJSON_Print(response);
                send_to_player(player_index, response_str);
                free(response_str);
                cJSON_Delete(response);
                
                // Notify opponent that this player submitted
                int opponent_index = (player_index == 0) ? 1 : 0;
                cJSON *waiting_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(waiting_msg, "type", "opponent_submitted");
                cJSON_AddNumberToObject(waiting_msg, "opponent", player_index);
                char *waiting_str = cJSON_Print(waiting_msg);
                send_to_player(opponent_index, waiting_str);
                free(waiting_str);
                cJSON_Delete(waiting_msg);
                
                // Check if both players have submitted
                bool both_submitted = game.players[0].waiting_for_opponent && 
                                    game.players[1].waiting_for_opponent;
                
                if (both_submitted) {
                    ESP_LOGI(TAG, "Both players submitted their guesses");
                    
                    // Reset waiting flags for next guess
                    game.players[0].waiting_for_opponent = false;
                    game.players[1].waiting_for_opponent = false;
                    
                    // Broadcast both results to both players
                    cJSON *both_results = cJSON_CreateObject();
                    cJSON_AddStringToObject(both_results, "type", "both_guessed");
                    char *both_str = cJSON_Print(both_results);
                    broadcast_to_all(both_str);
                    free(both_str);
                    cJSON_Delete(both_results);
                    
                    // Check if round should end
                    if (game.players[0].has_won || game.players[1].has_won) {
                        end_round();
                    } else if (game.players[0].guesses_used >= 5 && game.players[1].guesses_used >= 5) {
                        end_round();
                    }
                }
            }
        }else if (strcmp(msg_type, "next_round") == 0) {
            ESP_LOGI(TAG, "Next round requested");
            
            if (game.round_over) {
                start_new_round();
            } else {
                ESP_LOGE(TAG, "Cannot start next round - current round not over");
            }
        }
        else if (strcmp(msg_type, "join") == 0) {
            // Get player name
            cJSON *name_item = cJSON_GetObjectItem(json, "name");
            if (name_item != NULL && cJSON_IsString(name_item)) {
                const char *name = name_item->valuestring;
                
                // Find which player this is
                int fd = httpd_req_to_sockfd(req);
                int player_index = -1;
                for (int i = 0; i < 2; i++) {
                    if (game.players[i].connected && game.players[i].fd == fd) {
                        player_index = i;
                        strncpy(game.players[i].name, name, 31);
                        game.players[i].name[31] = '\0';
                        ESP_LOGI(TAG, "Player %d (%s) joined the lobby", i + 1, name);
                        break;
                    }
                }
                
                if (player_index == -1) {
                    ESP_LOGE(TAG, "Could not find player");
                    cJSON_Delete(json);
                    free(buf);
                    return ESP_ERR_INVALID_ARG;
                }
                
                // Send lobby update to all players
                cJSON *lobby_msg = cJSON_CreateObject();
                cJSON_AddStringToObject(lobby_msg, "type", "lobby_update");
                cJSON_AddNumberToObject(lobby_msg, "player_count", game.player_count);
                char *lobby_str = cJSON_Print(lobby_msg);
                broadcast_to_all(lobby_str);
                free(lobby_str);
                cJSON_Delete(lobby_msg);
                
                // Check if both players have names (both clicked join)
                bool both_ready = (strlen(game.players[0].name) > 0 && 
                                strlen(game.players[1].name) > 0 &&
                                game.players[0].connected && 
                                game.players[1].connected);
                
                if (both_ready && game.player_count == 2) {
                    ESP_LOGI(TAG, "Both players ready! Starting game...");
                    
                    // Send game_starting message
                    cJSON *start_msg = cJSON_CreateObject();
                    cJSON_AddStringToObject(start_msg, "type", "game_starting");
                    char *start_str = cJSON_Print(start_msg);
                    broadcast_to_all(start_str);
                    free(start_str);
                    cJSON_Delete(start_msg);
                    
                    game.game_active = true;
                    
                    // Start first round after a short delay
                    vTaskDelay(1000 / portTICK_PERIOD_MS);
                    start_new_round();
                }
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

    // Start timer check task
    // xTaskCreate(timer_check_task, "timer_check", 2048, NULL, 5, NULL);
}
