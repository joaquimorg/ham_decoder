#include "es8311.h"

#if defined(BOARD_FNK0104S) && (AUDIO_SOURCE == AUDIO_SRC_ES8311 || AUDIO_MONITOR)

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_log.h"

#include "board_i2c.h"

static const char *TAG = "ES8311";

// 32-bit stereo frames: 3 KB per direction, from the internal RAM that the
// Wi-Fi buffers also need (the DMA buffers cannot live in the PSRAM).
#define ES_DMA_FRAMES   ES8311_DMA_FRAMES
#define ES_DMA_DESC     ES8311_DMA_DESC

// ES8311 registers
#define REG_RESET       0x00
#define REG_CLK1        0x01
#define REG_CLK2        0x02
#define REG_CLK3        0x03
#define REG_CLK4        0x04
#define REG_CLK5        0x05
#define REG_CLK6        0x06
#define REG_CLK7        0x07
#define REG_CLK8        0x08
#define REG_SDPIN       0x09    // DAC serial port format
#define REG_SDPOUT      0x0A    // ADC serial port format
#define REG_SYS0D       0x0D
#define REG_SYS0E       0x0E
#define REG_SYS12       0x12
#define REG_SYS13       0x13
#define REG_SYS14       0x14    // input select + PGA gain
#define REG_ADC15       0x15
#define REG_ADC16       0x16    // ADC scale (gain)
#define REG_ADC17       0x17    // ADC volume
#define REG_ADC1B       0x1B
#define REG_ADC1C       0x1C
#define REG_DAC31       0x31    // DAC mute
#define REG_DAC32       0x32    // DAC volume
#define REG_DAC37       0x37
#define REG_CHIP_ID1    0xFD    // 0x83
#define REG_CHIP_ID2    0xFE    // 0x11

static i2s_chan_handle_t rx_handle = nullptr;
static i2s_chan_handle_t tx_handle = nullptr;
static i2c_master_dev_handle_t codec = nullptr;
static bool started = false;
static bool codec_ok = false;
static int volume_pct = MONITOR_DEFAULT_VOL;

static volatile uint32_t rx_overflows = 0;

static bool IRAM_ATTR on_recv_overflow(i2s_chan_handle_t, i2s_event_data_t *, void *)
{
    rx_overflows = rx_overflows + 1;
    return false;
}

uint32_t es8311_take_rx_overflows()
{
    const uint32_t n = rx_overflows;
    rx_overflows = 0;
    return n;
}

i2s_chan_handle_t es8311_rx() { return rx_handle; }
i2s_chan_handle_t es8311_tx() { return tx_handle; }

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    const uint8_t b[2] = { reg, val };
    return i2c_master_transmit(codec, b, sizeof(b), 50);
}

static esp_err_t rd(uint8_t reg, uint8_t *val)
{
    return i2c_master_transmit_receive(codec, &reg, 1, val, 1, 50);
}

// Speaker amplifier (SC8002B): GPIO low = on, the external pull-up = off.
static void pa_enable(bool on)
{
#if AUDIO_MONITOR
    gpio_set_level((gpio_num_t)ES8311_PA_GPIO, on ? 0 : 1);
#else
    (void)on;
#endif
}

static void apply_volume()
{
    if (!codec_ok)
        return;
#if AUDIO_MONITOR
    if (volume_pct <= 0) {
        wr(REG_DAC31, 0x60);    // mute the DAC
        pa_enable(false);
        return;
    }
    // 0.5 dB per step, 0xBF = 0 dB: 1 % = -59.4 dB ... 100 % = 0 dB.
    const int reg = 0xBF - (int)((100 - volume_pct) * 1.2f + 0.5f);
    wr(REG_DAC32, reg < 0 ? 0 : reg);
    wr(REG_DAC31, 0x00);
    pa_enable(true);
#endif
}

void es8311_set_volume(int percent)
{
    volume_pct = percent < 0 ? 0 : percent > 100 ? 100 : percent;
    apply_volume();
}

static bool codec_init()
{
    i2c_device_config_t dev = {};
    dev.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    dev.device_address = ES8311_ADDR;
    dev.scl_speed_hz = 400000;
    ESP_ERROR_CHECK(i2c_master_bus_add_device(board_i2c_bus(), &dev, &codec));

    uint8_t id1 = 0, id2 = 0;
    if (rd(REG_CHIP_ID1, &id1) != ESP_OK || rd(REG_CHIP_ID2, &id2) != ESP_OK) {
        ESP_LOGE(TAG, "codec (0x%02x) nao responde no I2C", ES8311_ADDR);
        return false;
    }
    ESP_LOGI(TAG, "chip id %02X%02X", id1, id2);

    esp_err_t e = ESP_OK;
    e |= wr(REG_RESET, 0x1F);               // reset
    vTaskDelay(pdMS_TO_TICKS(20));
    e |= wr(REG_RESET, 0x00);
    e |= wr(REG_RESET, 0x80);               // power on, slave
    e |= wr(0x44, 0x08);                    // ignore I2C glitches
    e |= wr(0x0B, 0x00);                    // power-up sequence of the analog blocks
    e |= wr(0x0C, 0x00);
    e |= wr(0x10, 0x1F);                    // bias / reference of the ADC and DAC
    e |= wr(0x11, 0x7F);
    e |= wr(REG_CLK1, 0x3F);                // all clocks on, MCLK from the pin

    // MCLK = 256 fs (3.072 MHz at 12 kHz): pre_div 1, pre_mult 1, adc/dac div 1,
    // OSR 0x10, LRCK divider 256, BCLK divider 4. Only the ratio matters.
    uint8_t v = 0;
    rd(REG_CLK2, &v);
    e |= wr(REG_CLK2, (v & 0x07) | (0 << 5) | (0 << 3));
    e |= wr(REG_CLK5, 0x00);
    e |= wr(REG_CLK3, 0x10);                // single speed, ADC OSR
    e |= wr(REG_CLK4, 0x10);                // DAC OSR
    rd(REG_CLK7, &v);
    e |= wr(REG_CLK7, (v & 0xC0) | 0x00);   // LRCK divider high
    e |= wr(REG_CLK8, 0xFF);                // LRCK divider low
    rd(REG_CLK6, &v);
    e |= wr(REG_CLK6, (v & 0xE0) | (4 - 1));

    // I2S, 24-bit words inside the 32-bit slots, unmuted.
    e |= wr(REG_SDPIN, 0x00);
    e |= wr(REG_SDPOUT, 0x00);

    e |= wr(REG_SYS0D, 0x01);               // power up the analog circuitry
    e |= wr(REG_SYS0E, 0x02);               // PGA and ADC modulator on
    e |= wr(REG_SYS12, 0x00);               // DAC on
    e |= wr(REG_SYS13, 0x10);               // output driver on
    e |= wr(REG_ADC1B, 0x0A);               // ADC high-pass
    e |= wr(REG_ADC1C, 0x6A);               // ADC EQ bypass, digital DC cancel
    e |= wr(REG_DAC37, 0x08);               // DAC EQ bypass

    // Input: MIC1P/MIC1N analog (the radio audio replaces the MEMS microphone).
    e |= wr(REG_SYS14, 0x10 | (ES8311_MIC_PGA & 0x0F));
    e |= wr(REG_ADC15, 0x40);               // ADC ramp rate
    e |= wr(REG_ADC16, ES8311_ADC_SCALE & 0x07);
    e |= wr(REG_ADC17, 0xBF);               // ADC volume 0 dB

    // Output stays muted until the monitor volume is applied.
    e |= wr(REG_DAC31, 0x60);
    e |= wr(REG_DAC32, 0x00);

    if (e != ESP_OK) {
        ESP_LOGE(TAG, "erro ao configurar o codec");
        return false;
    }
    return true;
}

bool es8311_start()
{
    if (started)
        return codec_ok;
    started = true;

#if AUDIO_MONITOR
    // The amplifier stays off until the volume is applied.
    gpio_config_t pa = {};
    pa.pin_bit_mask = 1ULL << ES8311_PA_GPIO;
    pa.mode = GPIO_MODE_OUTPUT;
    gpio_set_level((gpio_num_t)ES8311_PA_GPIO, 1);
    gpio_config(&pa);
#endif

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = ES_DMA_DESC;
    chan_cfg.dma_frame_num = ES_DMA_FRAMES;
    chan_cfg.auto_clear = true;    // zeros, not a repeated buffer, when the monitor has no data
    i2s_chan_handle_t *rx = AUDIO_SOURCE == AUDIO_SRC_ES8311 ? &rx_handle : nullptr;
    i2s_chan_handle_t *tx = AUDIO_MONITOR ? &tx_handle : nullptr;
    ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, tx, rx));

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = (gpio_num_t)ES8311_MCLK_GPIO,
            .bclk = (gpio_num_t)ES8311_BCK_GPIO,
            .ws   = (gpio_num_t)ES8311_WS_GPIO,
            .dout = I2S_GPIO_UNUSED,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = {},
        },
    };
    std_cfg.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;

    if (rx_handle) {
        std_cfg.gpio_cfg.din = (gpio_num_t)ES8311_DIN_GPIO;
        std_cfg.gpio_cfg.dout = I2S_GPIO_UNUSED;
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
    }
    if (tx_handle) {
        std_cfg.gpio_cfg.din = I2S_GPIO_UNUSED;
        std_cfg.gpio_cfg.dout = (gpio_num_t)ES8311_DOUT_GPIO;
        ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
    }
    if (rx_handle) {
        i2s_event_callbacks_t cbs = {};
        cbs.on_recv_q_ovf = on_recv_overflow;
        ESP_ERROR_CHECK(i2s_channel_register_event_callback(rx_handle, &cbs, nullptr));
        ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
    }
    if (tx_handle)
        ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));

    // Headroom for the output: the monitor writes in lock-step with the capture,
    // so a late capture block would otherwise leave the DMA empty (audible gap).
    if (tx_handle) {
        static int32_t silence[ES_DMA_FRAMES * 2];
        size_t w;
        for (int i = 0; i < ES_DMA_DESC / 2; i++)
            i2s_channel_write(tx_handle, silence, sizeof(silence), &w, pdMS_TO_TICKS(50));
    }

    // The codec needs MCLK running while its registers are written.
    vTaskDelay(pdMS_TO_TICKS(10));
    codec_ok = codec_init();
    apply_volume();

    ESP_LOGI(TAG, "I2S %d Hz, MCLK %d Hz; entrada %s, monitor %s",
             AUDIO_SAMPLE_RATE, AUDIO_SAMPLE_RATE * 256, rx_handle ? "codec" : "-",
             tx_handle ? "ligado" : "-");
    return codec_ok;
}

#endif
