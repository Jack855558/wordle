idf_component_register(SRCS "softap_example_main.c"
                    PRIV_REQUIRES esp_wifi nvs_flash esp_http_server json
                    INCLUDE_DIRS "."
                    EMBED_FILES "assets/index.html"
                                "assets/style.css"
                                "assets/script.js"
                    )
