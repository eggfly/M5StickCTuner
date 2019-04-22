#include <Arduino.h>
#include <M5StickC.h>
#include <driver/i2s.h>
#include "notes.h"

// こいつの大きさで正確性と速度が変わってくるっちゃ。
#define FRAME_LEN 2048

/*
以下のセクションはM5StickCのサンプルソースを改変。
https://github.com/m5stack/M5StickC/tree/master/examples/Basics/Micophone
*/
#define PIN_CLK 0
#define PIN_DATA 34
#define READ_LEN (2 * FRAME_LEN)
#define SAMPLE_RATE 44100

uint8_t BUFFER[READ_LEN] = {0};
int16_t *adcBuffer = (int16_t *)BUFFER;

void i2sInit() {
    i2s_config_t i2s_config = {
        .mode = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_RX | I2S_MODE_PDM),
        .sample_rate = SAMPLE_RATE,
        .bits_per_sample =
            I2S_BITS_PER_SAMPLE_16BIT,  // is fixed at 12bit, stereo, MSB
        .channel_format = I2S_CHANNEL_FMT_ALL_RIGHT,
        .communication_format = I2S_COMM_FORMAT_I2S,
        .intr_alloc_flags = ESP_INTR_FLAG_LEVEL1,
        .dma_buf_count = 2,
        .dma_buf_len = 128,
    };

    i2s_pin_config_t pin_config;
    pin_config.bck_io_num = I2S_PIN_NO_CHANGE;
    pin_config.ws_io_num = PIN_CLK;
    pin_config.data_out_num = I2S_PIN_NO_CHANGE;
    pin_config.data_in_num = PIN_DATA;

    i2s_driver_install(I2S_NUM_0, &i2s_config, 0, NULL);
    i2s_set_pin(I2S_NUM_0, &pin_config);
    i2s_set_clk(I2S_NUM_0, SAMPLE_RATE, I2S_BITS_PER_SAMPLE_16BIT,
                I2S_CHANNEL_MONO);
}

/*
以下のセクションは
https://github.com/luolongzhi/llz_pitch
にあるソースを理解せぬまま雑に移植。
*/
float yin_x[FRAME_LEN * sizeof(float) * 2];
float yin_d[FRAME_LEN * sizeof(float)];
float yin_tol;

float quadfrac(float s0, float s1, float s2, float pf) {
    float tmp =
        s0 + (pf / 2.) * (pf * (s0 - 2. * s1 + s2) - 3. * s0 + 4. * s1 - s2);
    return tmp;
}

float quadint_min(float *data, int len, int pos, int span) {
    float step = 1. / 200.;
    /* init resold to - something (in case x[pos+-span]<0)) */
    float res, frac, s0, s1, s2, exactpos = (float)pos, resold = 100000.;
    if ((pos > span) && (pos < len - span)) {
        s0 = data[pos - span];
        s1 = data[pos];
        s2 = data[pos + span];
        /* increase frac */
        for (frac = 0.; frac < 2.; frac = frac + step) {
            res = quadfrac(s0, s1, s2, frac);
            if (res < resold) {
                resold = res;
            } else {
                exactpos += (frac - step) * span - span / 2.;
                break;
            }
        }
    }

    return exactpos;
}

float yin_getpitch() {
    int j;
    int tau = 0;
    int period = 0;

    float diff;
    float cum;

    int min_pos;
    float tmp;

    for (j = 0; j < FRAME_LEN; j++) {
        yin_x[j] = yin_x[j + FRAME_LEN];
        yin_x[j + FRAME_LEN] = (float)adcBuffer[j];
    }

    memset(yin_d, 0, FRAME_LEN * sizeof(float));

    yin_d[0] = 1;
    cum = 0.0;
    for (tau = 1; tau < FRAME_LEN; tau++) {
        yin_d[tau] = 0.0;

        for (j = 0; j < FRAME_LEN; j++) {
            diff = yin_x[j] - yin_x[j + tau];
            yin_d[tau] += diff * diff;
        }
        cum += yin_d[tau];
        yin_d[tau] *= tau / cum;

        period = tau - 3;
        if (tau > 3 && yin_d[period] < yin_tol &&
            yin_d[period] < yin_d[period + 1])

            return quadint_min(yin_d, FRAME_LEN, period, 1);
    }

    min_pos = 0;
    tmp = yin_d[0];
    for (j = 0; j < FRAME_LEN; j++) {
        if (yin_d[j] < tmp) {
            tmp = yin_d[j];
            min_pos = j;
        }
    }

    return quadint_min(yin_d, FRAME_LEN, min_pos, 1);
}

/*
取るに足らない表示セクション
*/

#define CORRECT_MARGIN 15

int last_disp_notes_index = 0;
bool last_correct = false;

// どの音かなあ？（戻り値は配列notesのインデックス。 判定不能なら -1 を返す。）
int getNotesIndex(float frequency) {
    const int notes_len = sizeof(notes) / sizeof(notes[0]);
    if (frequency < notes[0].frequency ||
        notes[notes_len - 1].frequency < frequency) {
        return -1;
    }

    for (int i = 1; i < notes_len - 1; i++) {
        if (notes[i].frequency > frequency) {
            float border = notes[i - 1].frequency +
                           (notes[i].frequency - notes[i - 1].frequency) / 2;
            if (border < frequency) {
                return i;
            } else {
                return i - 1;
            }
        }
    }
    return -1;
}

// その音の範囲
bool getNoteRange(int notes_index, float *range_min, float *range_max) {
    if (notes_index < 1 ||
        notes_index >= (sizeof(notes) / sizeof(notes[0]) - 2)) {
        return false;
    }
    *range_min =
        notes[notes_index - 1].frequency +
        (notes[notes_index].frequency - notes[notes_index - 1].frequency) / 2;
    *range_max =
        notes[notes_index].frequency +
        (notes[notes_index + 1].frequency - notes[notes_index].frequency) / 2;
    return true;
}

void displayNote(int notes_index, float frequency) {
    float range_min, range_max;
    if (!getNoteRange(notes_index, &range_min, &range_max)) return;
    // 正確なピッチかどうかの正確性にかける判定
    float pitch_x = (frequency - range_min) * 160 / (range_max - range_min);
    bool correct =
        (pitch_x >= (80 - CORRECT_MARGIN) && pitch_x <= (80 + CORRECT_MARGIN));

    // pitch indicator ...
    M5.Lcd.fillRect(0, 0, 160, 16, correct ? WHITE : BLACK);
    M5.Lcd.fillCircle(pitch_x, 7, 8, RED);

    // 音名表示（前回と同じなら描画しない）
    if (notes_index != last_disp_notes_index || last_correct != correct) {
        M5.Lcd.fillRect(0, 16, 160, 64, correct ? WHITE : BLACK);
        M5.Lcd.setCursor(strlen(notes[notes_index].name) < 3 ? 40 : 16, 24);
        M5.Lcd.setTextSize(8);
        M5.Lcd.setTextColor(correct ? BLACK : WHITE);
        M5.Lcd.print(notes[notes_index].name);
        last_disp_notes_index = notes_index;
    }

    last_correct = correct;
}

void setup() {
    M5.begin();
    M5.Axp.ScreenBreath(11);
    M5.Lcd.setRotation(3);
    M5.Lcd.fillScreen(BLACK);
    M5.Lcd.setTextColor(WHITE);

    i2sInit();
}

void loop() {
    // マイクからのデータ取得
    // examplesでは生成した別タスクで実行していたが、なんだか難しいのでloop()に持ってきた。
    int bytes = i2s_read_bytes(I2S_NUM_0, (char *)BUFFER, READ_LEN,
                               (100 / portTICK_RATE_MS));

    if (bytes) {
        // 基本周波数検出
        float pitch = yin_getpitch();
        float frequency = (float)SAMPLE_RATE / pitch;
        Serial.printf("%f Hz\n", frequency);
        // 表示
        int notes_index = getNotesIndex(frequency);
        if (notes_index >= 0) {
            displayNote(notes_index, frequency);
        }
    }
}
