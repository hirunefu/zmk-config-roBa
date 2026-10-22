/*
 * ディープスリープに入る直前に、LED インジケーターの RGB LED（zmk-rgbled-widget が使う
 * led-red・led-green・led-blue の別名が指す GPIO）を切り離して消す。左右どちらでも使う。
 * 有効化は CONFIG_ROBA_LED_OFF_ON_SLEEP（Kconfig と config/roBa_R.conf・config/roBa_L.conf を参照）。
 *
 * 要る理由（cormoran/zmk@e5c9b69、zmkfirmware/zephyr@10ba6d0、caksoylar/zmk-rgbled-widget@e6b4677 を読んだ結果）:
 *   - ZMK は眠るとき、ZMK_ACTIVITY_SLEEP を通知してから、PM に対応したデバイスを止めて電源を切る
 *     （app/src/activity.c:141-157 の set_state → zmk_pm_suspend_devices → sys_poweroff）。
 *     通知は同じスレッドでその場で配られるので、ここの処理は電源を切る前に必ず終わる
 *   - LED の gpio-leds ドライバは PM に対応していない（drivers/led/led_gpio.c:97 で PM 無しに登録）ので、
 *     眠る前に止められず、ピンはそのまま残る
 *   - nRF52840 は電源を切っても GPIO の出力を保つ。LED が点いている瞬間に眠ると、起きるまで点いたままになる
 *   - zmk-rgbled-widget は、レイヤーの色を出す設定のときだけ、この通知で LED を消す（src/widget.c:287-313）。
 *     左手（その設定が無い）では消さない。右手でも、点滅を処理するスレッド（同:354-390）は止めないので、
 *     デバイスを止めている間にそのスレッドが動くと、点け直すおそれがある（推定）
 *
 * やること:
 *   ZMK_ACTIVITY_SLEEP の通知で、3 つのピンを GPIO_DISCONNECTED にする（出力も入力も切る）。
 *   LED はピンを Low にすると点く（GPIO_ACTIVE_LOW）ので、ピンを切り離すと電流が流れず消える。
 *   そのあと LED のドライバが出力の値を書き換えても、ピンは駆動されないので点かない。
 *   起きると再起動するので、LED のドライバがピンを設定し直す
 */

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/util.h>

#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

/* 別名は rgbled_adapter シールドが付ける（XIAO では led0〜led2 = P0.26・P0.30・P0.06）。
 * 別名が無ければ zmk-rgbled-widget の BUILD_ASSERT でビルドが止まるので、ここでは確かめない */
static const struct gpio_dt_spec leds[] = {
    GPIO_DT_SPEC_GET(DT_ALIAS(led_red), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led_green), gpios),
    GPIO_DT_SPEC_GET(DT_ALIAS(led_blue), gpios),
};

static int led_off_on_sleep_listener(const zmk_event_t *eh) {
    const struct zmk_activity_state_changed *ev = as_zmk_activity_state_changed(eh);

    if (ev == NULL || ev->state != ZMK_ACTIVITY_SLEEP) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    for (size_t i = 0; i < ARRAY_SIZE(leds); i++) {
        int err = gpio_pin_configure_dt(&leds[i], GPIO_DISCONNECTED);

        if (err) {
            LOG_WRN("LED pin %u was not disconnected before sleep (err %d)", (unsigned int)i, err);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(roba_led_off_on_sleep, led_off_on_sleep_listener);
ZMK_SUBSCRIPTION(roba_led_off_on_sleep, zmk_activity_state_changed);
