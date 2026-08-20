idf_component_register(SRCS "main.c"
                            "creator_mode.c"
                    INCLUDE_DIRS "."
                    REQUIRES
                        app_font
                        creator_core
                        smartscore_core
                        midi_parser
                        music_display
                        musicxml_renderer
                        usb_midi_input
                        ui-guider
                        wifi_manager
                        network
                        wt99p4c5_s1_board
                        lvgl
                        esp_hosted)
