#include "led.h"

#include "board.h"

#include "driver/rmt_tx.h"
#include "esp_log.h"

#include <string.h>

static const char *TAG = "led";
static rmt_channel_handle_t s_chan;
static rmt_encoder_handle_t s_enc;

void led_init(void)
{
    rmt_tx_channel_config_t tx = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .gpio_num = PIN_LED,
        .mem_block_symbols = 64,
        .resolution_hz = 10 * 1000 * 1000,
        .trans_queue_depth = 4,
    };
    rmt_copy_encoder_config_t copy = {};
    if (rmt_new_tx_channel(&tx, &s_chan) != ESP_OK) {
        ESP_LOGE(TAG, "rmt channel");
        return;
    }
    if (rmt_new_copy_encoder(&copy, &s_enc) != ESP_OK) {
        ESP_LOGE(TAG, "rmt encoder");
        return;
    }
    rmt_enable(s_chan);
    led_set(0, 0, 0);
}

void led_set(uint8_t r, uint8_t g, uint8_t b)
{
    rmt_symbol_word_t sym[25];
    uint8_t grb[3] = {g, r, b};
    int n = 0;
    int i;
    int bit;
    if (s_chan == NULL || s_enc == NULL) {
        return;
    }
    for (i = 0; i < 3; i++) {
        for (bit = 7; bit >= 0; bit--) {
            int one = (grb[i] >> bit) & 1;
            sym[n].level0 = 1;
            sym[n].duration0 = one ? 8 : 4;
            sym[n].level1 = 0;
            sym[n].duration1 = one ? 4 : 8;
            n++;
        }
    }
    sym[n].level0 = 0;
    sym[n].duration0 = 3000;
    sym[n].level1 = 0;
    sym[n].duration1 = 0;
    n++;
    rmt_transmit_config_t cfg = {.loop_count = 0};
    rmt_transmit(s_chan, s_enc, sym, (size_t)n * sizeof(sym[0]), &cfg);
    rmt_tx_wait_all_done(s_chan, 20);
}
