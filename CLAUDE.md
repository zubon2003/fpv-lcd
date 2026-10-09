# CLAUDE.md — FPV バンドモニタ 液晶版（ESP-SDR フォーク）

クラウドの Claude セッションから引き継いだプロジェクト。ユーザーとは日本語で話す（簡潔に。スマホで読むことが多い）。
詳しい仕様・配線・使い方は `README-fpv-lcd.md`。ここには作業に必要な前提と、決まったこと・未確認のことをまとめる。

## 何か

- ESP-SDR（ESPARGOS/esp-sdr, e74f2a4 から分岐）に、XIAO ESP32-C5 単体で動く FPV 5.8 GHz（FM-ATV）バンドモニタを追加したファーム。
- 対になる PC 版は別リポジトリ `fpv-band-monitor-c5`（ブランチ `lcd-firmware-stream`）。同じファームで PC 版にも液晶にも対応する。
- 追加コードは `main/fpv/`。ESP-SDR 側の変更は `main/families/c5_c6_c61/receiver.c`（`#if FPV_LCD_MONITOR` の `sdr_hw_*` と起動分岐、DC キャッシュの呼び出し）、`main/common/rx_recalibration*.{c,h}` と `rx_calibration_state.c`（DC キャッシュ、`#if FPV_LCD_MONITOR`）、CMake だけ。

## ビルド

- ESP-IDF は ESP-SDR が固定している版（`firmware-targets.json` の esp32c5、v6.2 系。クラウドでは commit 25fe69f を使った）。
- LovyanGFX は同梱していない：`git clone https://github.com/lovyan03/LovyanGFX components/LovyanGFX && git -C components/LovyanGFX checkout e2cc321`（1.2.32）。
- 2.8" ILI9341 版：
  ```
  idf.py -B build-fpv -D FPV_LCD_MONITOR=ON -D SDKCONFIG=build-fpv/sdkconfig \
    -D SDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.defaults.esp32c5;sdkconfig.defaults.fpv-lcd" set-target esp32c5 build
  ```
- 3.5" Waveshare Pico-ResTouch-LCD-3.5 版：上に `-D FPV_PANEL=RESTOUCH35`、ビルドフォルダは `build-fpv35`。
- 書き込み用の 1 ファイル：`cd build-fpv && python -m esptool --chip esp32c5 merge-bin -o ../fpv-lcd-xiao-c5-merged.bin @flash_args`（0x0 に書く）。
- 切り替えは Kconfig ではなく CMake のビルドプロパティ（`FPV_LCD_MONITOR`, `FPV_PANEL`）。Kconfig は requirements 展開時に見えないため。
- `-D FPV_LCD_MONITOR=ON` なしなら元の ESP-SDR。

## ウェブ書き込み（GitHub Pages）

- https://zubon2003.github.io/fpv-lcd/ … `web/`（ESP Web Tools 10.4.0、ESP32-C5 対応）。`.github/workflows/pages.yml` が `web/**` の push で公開する。
- upstream の CI（`firmware.yml`、`rebuild-site.yml`）はこのリポジトリでは GitHub 上で無効にしてある（ファイルは upstream と合わせるため残す）。
- ファームは CI でビルドしていない。手元で 2.8"（`build-fpv`）と 3.5"（`build-fpv35`）をビルドし、各ビルドフォルダで
  `python -m esptool --chip esp32c5 merge-bin -o <repo>/web/firmware/fpv-lcd-xiao-c5-{ili9341,restouch35}.bin @flash_args`、
  `web/manifest-*.json` と `index.html` の `version`（ソースのコミット）を書き換えてコミットする。

## 守ること（ハマった点）

- **C5 の .bss は 0x40820000 未満で終わる必要がある**（その上は RF ダンプ領域）。今は 0x4081ee38（2.8"）/ 0x4081f0b8（3.5"）で余裕は 3〜4 KB。大きいバッファは静的に置かず heap/PSRAM（`big_alloc`, `CONFIG_SPIRAM`, `SPIRAM_USE_CAPS_ALLOC`）。ビルド後に `grep "_bss_end = " build-*/esp_sdr.map` で確認。
- C5 には FPU が無い：DSP は整数（1024 点 FFT、Q31 係数、MULH、段ごとに 1/2）。旧 64bit 版は `FPV_FFT_REF` で残してある。
- `-Werror`：snprintf の切り詰め警告に注意（バッファは余裕を持たせる）。C++ では `gpio_config_t` を指定子初期化しない。
- monitor ビルドでは `common/spectrum.c`（SPEC）を外している。
- **DC キャッシュ**（`rx_recalibrate_cached`、FPV ビルドのみ）：upstream は周波数を変えるたびに DC を測り直す（AGC 発振対策、upstream #5/#6/#9）。5 GHz では 1 回 27〜30 ms で、掃引の 7 割を占めていた。LO ごとに `phy_param` の DC 部分（+288, +384〜639, +714, +912〜975 の 324 バイト）を heap に保存し、2 回目からは書き戻して `phy_set_rx_gain_table` だけ呼ぶ（約 1 ms）。PHY が裏で DC を測り直したら（自分以外で `dc_generation` が進んだら）と 30 秒で捨てる。範囲は CALPROBE で `phy_param` の差分から決めたもの：**ESP-IDF を上げたら `-D FPV_CALPROBE=ON` で測り直す**（`CALPROBE <MHz>` = 段階ごとの時間と変わったバイト、`CALCACHE 0|1` = A/B）。
- このPCの環境：ESP-IDF 25fe69f は `C:\esp\idf-25fe69f`（v6.0.2 の `C:\esp\v6.0.2` とは別）。`python` が 3.9 を指すので、export.ps1 の前に Python 3.13 を PATH の先頭に置く。

## テスト（ホスト、ESP-IDF 不要）

`tests/fpv_host/`（Python は numpy / Pillow 入りのシステム Python。PC 版リポジトリのパスを引数に渡す）：
- `python3 check_dsp.py <pc版>` … PSD・掃引・平均・リングを PC 版 Python と比較（≤0.011 dB）。
- `python3 check_link.py <pc版>` … 疑似端末で偽ファーム ↔ PC 版の受信（auto→fft、NOLCD、格子、重心）。
- `python3 render_previews.py <pc版>` … 画面 PNG（`previews/screen_*`=2.8"、`screen35_*`=3.5"）。2.8" は変更前とピクセル一致を保つこと。
- `cycles/` … unicorn で RV32 命令数を数える速度見積もり。
- 下駄基板：`hardware/xiao_restouch35_adapter/make_geta.py`（ezdxf, shapely, Pillow）。クリアランス 0.4 mm を自動確認。

## 決まった仕様（ユーザーの指定）

- モード：4CH = E2 E1 F3 F5、3CH = **E1 F1 F4**、1CH = ±15 MHz。1CH の ◀▶ は **E2→E1→F1→F3→F4→F5** だけ。
- 帯域マークは **9 MHz と 17 MHz**。
- 測定は **直近 10 掃引の単純平均**（10 掃引分をメモリに保存、Q58 の整数累積で正確）。
- モード切替と 1CH 送りはメイン画面からも、セットアップ画面からも（ゲイン・SNR 閾値・タッチ校正もセットアップ）。
- 電池：3.30 V 未満が 3 回でディープスリープ、起動は 3.40 V 以上、休止中は 60 秒ごとに測って 3.70 V で復帰。
- USB：`FPV` で始まる行が独自プロトコル（`main/fpv/link.h`）、それ以外は ESP-SDR の I/Q コマンド（5 秒のリース）。PC 版は **auto（FPV? に答えれば fft、だめなら iq）/ iq / fft** をボタンで選べる。`FPV NOLCD` で液晶を消して掃引だけ（STREAM OFF・送信失敗・FPV LCD・タップで復帰）。
- 3.5" 版のピンは下駄基板（片面・交差なし）に合わせて 2.8" 版と変えてある（`fpv_config.h` の `FPV_PANEL_RESTOUCH35` 側）。

## 実機で未確認（次にやること候補）

- 3.5" ResTouch：ILI9488 を 16bit 送信（`dlen_16bit`）+ RGB565 に固定した `Panel_ResTouch35`。色反転（`FPV_LCD_INVERT`）、SPI 20 MHz、初期化が実機で合うか。
- ResTouch の電源を電池（BAT+→VSYS）で取ったときの安定性とスリープ電流。必要なら VSYS のロードスイッチ。
- 下駄基板：Pico の寸法（列間 17.78 mm、1 番ピン端から 1.37 mm）と XIAO ピン配置は標準値からの作図。実物合わせ未確認。ResTouch 上の Pico の向き（USB 側が基板の外か）も未確認。
- 掃引速度：実測（2026-10-09、3.5"、PC の 4CH 相当 9 ホップ）は DC キャッシュ前 1.34 / NOLCD 2.47、**後 2.96 / NOLCD 6.3 sweeps/s**。液晶ありは描画（約 0.18 s/掃引）が次の律速。
- 3.5" ResTouch の表示・タッチ・ゲイン 25 は実機で動作（2026-10-10、2 台目の XIAO）。書き込み後に RTS リセットで抜けないときは watchdog リセットか RESET ボタン。
- 5833 MHz 以上の LO ではホップ端（±23〜27 MHz）が −2〜−4 dB 落ち、min 合成で LO 位置（5807, 5833, 5858 など）に谷が出る。upstream の C5 帯域は全幅約 48 MHz（±24）。対策案：近い方のホップを使い 6 dB 以上高いときだけ min（未実装）。
- 消費電流（見積もり：2.8" 150〜190 mA、3.5" 220〜280 mA、NOLCD 約 100 mA、スリープ 30〜100 µA）。
- さらに速くするなら FFT の radix-4 化が次の候補。

## コミット

- 2026-10-10 から作者はユーザーの git 設定（zubon2003）。それ以前は中立名 `fpv-lcd <fpv-lcd@localhost>`。
- ゲインの既定は 25（ユーザー指定）。
- GitHub：https://github.com/zubon2003/fpv-lcd（公開、ブランチ `main`。bundle の `master` を改名）。push はユーザーに確認してから。
- ライセンスは ESP-SDR に合わせて GPL-3.0。
