# この個体のコード（src/ の C ソース）。zephyr/module.yml の build.cmake で読み込まれ、ZMK 本体の app ターゲットに加える
# （cormoran の各モジュールと同じやり方）。roBa_L や settings_reset のビルドでも評価されるが、
# それぞれの設定が無効なら何も加えない。
target_sources_ifdef(CONFIG_ROBA_BLE_CONN_PARAM_RETRY app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/ble_conn_param_retry.c)
# LED インジケーター（caksoylar/zmk-rgbled-widget を取り込んで直したもの。src/rgbled_widget/README.md）
if(CONFIG_RGBLED_WIDGET)
  target_sources(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/rgbled_widget/widget.c)
  target_include_directories(app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/rgbled_widget)
endif()
