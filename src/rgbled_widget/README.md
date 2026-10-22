# LED インジケーター（取り込んだ zmk-rgbled-widget）

XIAO 本体の RGB LED で、電池の残り・接続・いちばん上のレイヤーを色で示すコード。
caksoylar 氏の [zmk-rgbled-widget](https://github.com/caksoylar/zmk-rgbled-widget) の main（e6b467792a1d、2026-07-09）を、このディレクトリに取り込んで直したもの。ライセンスは上流と同じ MIT（[LICENSE](LICENSE)）。

## 取り込んだ理由

上流は LED を点けるか消すかしかできず（gpio-leds）、XIAO の LED では明るすぎた（2026-10-22）。明るさを下げるには PWM で点ける必要があり、上流のコードに手を入れる必要がある。フォークを作る案もあったが、ユーザーの判断で、新しいリポジトリを作らずにこのリポジトリへ取り込んだ。

## 上流との違い

- `widget.c`: 「roBa:」のコメントを付けた 3 か所だけ
  - LED のデバイスを、`led-red` の別名が指すノードの親にした（上流は gpio-leds の互換を持つノードを探す）。gpio-leds でも pwm-leds でも動く
  - 3 色が同じ親にあることを `BUILD_ASSERT` で確かめる
  - `led_on`（100%）の代わりに `led_set_brightness`（`CONFIG_RGBLED_WIDGET_BRIGHTNESS`）で点ける
- `Kconfig`: `RGBLED_WIDGET_BRIGHTNESS` を足し、キーマップ用の behavior（`&ind_bat`・`&ind_con`）の項目を外した
- 取り込んでいないもの: behavior（`dts/`・`src/behaviors/`）と `rgbled_adapter` シールド。LED の定義と別名は `boards/shields/roBa/roBa.dtsi` にある

項目名は上流のまま `RGBLED_WIDGET_` で始める（このリポジトリの `ROBA_` の決まりの例外。上流と比べやすくするため）。

## 上流が更新されたときの上げ直し方

1. 上流の新しいコミットで `src/widget.c`・`include/zmk_rgbled_widget/widget.h`・`Kconfig` を取ってくる
2. 「roBa:」の 3 か所と、`Kconfig` の違い（明るさの項目を足す、behavior の項目を外す）を入れ直す
3. このファイルと `widget.c`・`Kconfig` の先頭にあるコミットを書き換える
4. CI のビルドと実機（起動時の点滅、レイヤーの色、ディープスリープ中の消灯）で確かめる
