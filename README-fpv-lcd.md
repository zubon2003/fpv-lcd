# FPV バンドモニタ 液晶版（XIAO ESP32-C5 + ILI9341）

[fpv-band-monitor-c5](https://github.com/zubon2003/fpv-band-monitor-c5)（PC 版）の解析と画面を、
ESP32-C5 単体で動くように [ESP-SDR](https://github.com/ESPARGOS/esp-sdr) ファームに組み込んだもの。
PC は不要で、2.8" ILI9341 + XPT2046 タッチ液晶にスペクトラム・ウォーターフォール・実測中心周波数を表示する。
**アナログ FPV の映像電波（FM-ATV）専用**。

追加したコードは `main/fpv/` にまとまっている。ESP-SDR 側の変更は
`main/families/c5_c6_c61/receiver.c` に、無線を直接操作する小さな関数群（`sdr_hw_*`）と、
起動時にモニタへ入る分岐を足しただけ。

## 表示モード

| モード | チャンネル | 掃引範囲 | ホップ数 |
|---|---|---|---|
| 4CH（既定） | E2 / E1 / F3 / F5 | 5670〜5835 MHz | 9 |
| 3CH | E1 / F1 / F3 | 5690〜5795 MHz | 7 |
| 1CH | 任意の 1ch（A/B/E/F/R/L 全 48ch） | 中心 ±15 MHz | 4 |

各チャンネルには **9 MHz と 17 MHz** の帯域マーク（内側が濃い）を描く。
ホップが少ないほど更新が速い。

## 画面（320×240）

- 上段：モード／更新速度（sw/s）とフロア／ゲイン／ピークホールド
- スペクトラム：灰色が最新の掃引、白が平均（測定に使う線）、点線がピークホールド。
  縦の実線が実測中心、点線が公称周波数
- 周波数目盛り
- ウォーターフォール：0.5 秒 1 行（各行はその間のピーク）、新しい行が上
- 下段：チャンネルごとの実測中心、ずれ（±0.5 MHz 以内は青、1 MHz 未満は黄、それ以上は赤）、
  ピーク dB と -3 dB 幅。信号なしのときは理由（SNR 不足など）
- 1CH では、大きな実測中心に加えてピーク・SNR・フロア・-3 dB／-20 dB 幅を表示

## タッチ操作

| 場所 | 動作 |
|---|---|
| 左上のモードボタン | 4CH → 3CH → 1CH の切替 |
| ゲインボタンの左半分／右半分 | ゲイン −5／+5（60 を超えると AGC、AGC から − で 60） |
| HOLD | ピークホールドの ON/OFF（ON にするたびリセット） |
| スペクトラムかウォーターフォールを長押し（0.8 秒） | 平均・ホールド・ウォーターフォールのクリア |
| 1CH：左右の ◀ ▶ | チャンネル送り（A1→…→A8→B1→…→L8） |
| 1CH：チャンネル名のあたり | バンド切替（番号はそのまま） |

モード・1CH のチャンネル・ゲインは保存され、次回もその状態で起動する。

**タッチの調整**：初回起動時に調整画面が出る（10 秒触らなければ既定値で起動）。
やり直すときは、起動直後の画面（約 1.5 秒）を押し続ける。

## 配線（XIAO ESP32-C5）

| 液晶基板のピン | XIAO | GPIO |
|---|---|---|
| SCK と T_CLK | D8 | 8 |
| SDI(MOSI) と T_DIN | D10 | 10 |
| T_DO | D9 | 9 |
| SDO(MISO) | 未接続 | – |
| CS | D0 | 1 |
| D/C | D1 | 0 |
| RESET | D4 | 23 |
| T_CS | D5 | 24 |
| T_IRQ | 未接続（ポーリング） | – |
| LED | 3V3 | – |
| VCC | 5V（J1 開放）または 3V3（J1 短絡） | – |
| GND | GND | – |

D2/D3（GPIO25/7）はストラップピン、D6/D7（GPIO11/12）は ESP-SDR の UART 用なので使っていない。
ピンや画面の向きは `main/fpv/fpv_config.h` で変えられる（`FPV_LCD_ROTATION` 1 か 3）。

## 書き込み

ビルド済みの `firmware/fpv-lcd-xiao-c5-merged.bin` を **オフセット 0x0** に書く（8 MB フラッシュ用）。

```
python -m esptool --chip esp32c5 write-flash 0x0 firmware/fpv-lcd-xiao-c5-merged.bin
```

ブラウザなら [ESP Tool](https://espressif.github.io/esptool-js/) で、Flash Address 0x0 に同じファイルを指定する。

## ビルド

ESP-IDF は ESP-SDR が固定している版（`firmware-targets.json` の esp32c5、6.2 系）を使う。
LovyanGFX は同梱していないので、先に取得する（動作確認した版は 1.2.32、コミット e2cc321）。

```
git clone https://github.com/lovyan03/LovyanGFX components/LovyanGFX
git -C components/LovyanGFX checkout e2cc321
idf.py -B build-fpv -D FPV_LCD_MONITOR=ON -D SDKCONFIG=build-fpv/sdkconfig \
  -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c5;sdkconfig.defaults.fpv-lcd" \
  set-target esp32c5 build
idf.py -B build-fpv -p PORT flash
```

`-D FPV_LCD_MONITOR=ON` を付けなければ、従来どおりの ESP-SDR（USB で PC 版やブラウザ版を使う）になる。

## PC 版との違い

- **FFT は 1024 点**（ビン幅 78.125 kHz）。C5 には FPU が無いので整数 FFT にし、2 のべき乗にした（PC 版は 800 点・100 kHz）。
- **平均は時定数 1.5 秒の指数平均**（PC 版は 3 秒の移動平均）。全掃引を 3 秒分保持するメモリが無いため。
- ホップ（25 MHz 間隔・片側 28 MHz 使用・LO ±500 kHz 除外・小さい方を採用）、
  重心による中心周波数（±7.5 MHz、フロア +3 dB 超のみ、再センタリング 1 回）、
  SNR 18 dB の在り判定、-3／-20 dB 幅、表示の平滑化（0.3）は PC 版と同じ。
- 液晶版では USB の I/Q 出力（ESP-WebSDR や PC 版との接続）は使えない。メモリの都合で、
  ESP-SDR のオンチップスペクトラム（SPEC コマンド）もこのビルドから外している。

## 動作確認

- PC 上で、液晶版の FFT・スティッチ・解析を PC 版の Python 実装と同じ疑似キャプチャで比較：
  スティッチ後のスペクトラムの差は最大 0.09 dB、実測中心の差は 0.001 MHz 以内、在り判定は全一致
  （`tests/fpv_host/check_dsp.py`）。
- 画面は同じ描画コードを PC で動かして確認（`tests/fpv_host/render_previews.py`）。
- **実機（XIAO ESP32-C5 + 液晶）ではまだ動かしていない。** 掃引速度（画面上段の sw/s）、
  PSRAM の認識、タッチの向きは実機で確かめる必要がある。

## ライセンス

ESP-SDR が GPL-3.0 なので、このファームも GPL-3.0。
LovyanGFX は FreeBSD ライセンス（`components/LovyanGFX`）。
