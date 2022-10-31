#include "i2s_audio.h"

#define USE_LEGACY_I2S 1

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/i2c.h"
#ifdef USE_LEGACY_I2S
#include "driver/i2s.h"

/* Required for I2S driver workaround */
#include "esp_rom_gpio.h"
#include "hal/gpio_hal.h"
#include "hal/i2s_ll.h"
#else
#include "driver/i2s_std.h"
#endif
#include "esp_system.h"
#include "esp_check.h"
#include "es7210.h"
#include "es8311.h"

//    ESP_ERROR_CHECK(bsp_i2s_init(I2S_NUM_0, 16000));
//    ESP_ERROR_CHECK(bsp_codec_init(AUDIO_HAL_16K_SAMPLES));
//    .CODEC_I2C_ADDR = 0,
//    .AUDIO_ADC_I2C_ADDR = 0,

/**
 * Look at
 * https://github.com/espressif/esp-idf/blob/master/examples/peripherals/i2s/i2s_codec/i2s_es8311/main/i2s_es8311_example.c
 * and
 * https://github.com/espressif/esp-box/blob/master/components/bsp/src/peripherals/bsp_i2s.c
 */

/* I2C port and GPIOs */
#define I2C_NUM         (I2C_NUM_0)
#define I2C_SCL_IO      (GPIO_NUM_18)
#define I2C_SDA_IO      (GPIO_NUM_8)
#define I2C_FREQ_HZ          400000                     /*!< I2C master clock frequency */
#define I2C_TIMEOUT_MS       1000

/* I2S port and GPIOs */
#define I2S_NUM         (I2S_NUM_0)
#define I2S_MCK_IO      (GPIO_NUM_2)
#define I2S_BCK_IO      (GPIO_NUM_17)
#define I2S_WS_IO       (GPIO_NUM_47)
#define I2S_DO_IO       (GPIO_NUM_15)
#define I2S_DI_IO       (GPIO_NUM_16)

/* Example configurations */
#define EXAMPLE_SAMPLE_RATE     (16000) // 16k in esp32_s3_box.c, but nofrendo used 32k
#define EXAMPLE_MCLK_MULTIPLE   (I2S_MCLK_MULTIPLE_512) // If not using 24-bit data width, 256 should be enough
#define EXAMPLE_MCLK_FREQ_HZ    (EXAMPLE_SAMPLE_RATE * EXAMPLE_MCLK_MULTIPLE)
#define EXAMPLE_VOLUME          (100) // percent

#ifdef USE_LEGACY_I2S
#define I2S_CONFIG_DEFAULT() {                                          \
    .mode                   = (i2s_mode_t)(I2S_MODE_MASTER | I2S_MODE_TX | I2S_MODE_RX), \
    .sample_rate            = sample_rate,                              \
    .bits_per_sample        = I2S_BITS_PER_SAMPLE_32BIT,                \
    .channel_format         = I2S_CHANNEL_FMT_RIGHT_LEFT,               \
    .communication_format   = I2S_COMM_FORMAT_STAND_I2S,                \
    .intr_alloc_flags       = ESP_INTR_FLAG_LEVEL1,                     \
    .dma_buf_count          = 6,                                        \
    .dma_buf_len            = 160,                                      \
    .use_apll               = false,                                    \
    .tx_desc_auto_clear     = true,                                     \
    .fixed_mclk             = 0,                                        \
    .mclk_multiple          = EXAMPLE_MCLK_MULTIPLE,                    \
    .bits_per_chan          = I2S_BITS_PER_CHAN_32BIT,                  \
  }
#else
static i2s_chan_handle_t tx_handle = NULL;
static i2s_chan_handle_t rx_handle = NULL;
#endif

static esp_err_t i2s_driver_init(void)
{
  printf("initializing i2s driver...\n");
  auto ret_val = ESP_OK;
#ifdef USE_LEGACY_I2S
  printf("Using LEGACY_I2S\n");
  uint32_t sample_rate = EXAMPLE_SAMPLE_RATE;
  i2s_config_t i2s_config = I2S_CONFIG_DEFAULT();
  i2s_pin_config_t pin_config;
  memset(&pin_config, 0, sizeof(pin_config));
  pin_config.bck_io_num = I2S_BCK_IO;
  pin_config.ws_io_num = I2S_WS_IO;
  pin_config.data_out_num = I2S_DO_IO;
  pin_config.data_in_num = I2S_DI_IO;
  pin_config.mck_io_num = I2S_MCK_IO;
  ret_val |= i2s_driver_install(I2S_NUM, &i2s_config, 0, NULL);
  ret_val |= i2s_set_pin(I2S_NUM, &pin_config);
  ret_val |= i2s_zero_dma_buffer(I2S_NUM);
#else
  printf("Using newer I2S standard\n");
  i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM, I2S_ROLE_MASTER);
  chan_cfg.auto_clear = true; // Auto clear the legacy data in the DMA buffer
  ESP_ERROR_CHECK(i2s_new_channel(&chan_cfg, &tx_handle, &rx_handle));
  i2s_std_clk_config_t clock_cfg = I2S_STD_CLK_DEFAULT_CONFIG(EXAMPLE_SAMPLE_RATE);
  i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIP_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT, I2S_SLOT_MODE_STEREO);
  i2s_std_config_t std_cfg = {
    .clk_cfg = clock_cfg,
    .slot_cfg = slot_cfg,
    .gpio_cfg = {
      .mclk = I2S_MCK_IO,
      .bclk = I2S_BCK_IO,
      .ws = I2S_WS_IO,
      .dout = I2S_DO_IO,
      .din = I2S_DI_IO,
      .invert_flags = {
        .mclk_inv = false,
        .bclk_inv = false,
        .ws_inv = false,
      },
    },
  };

  ESP_ERROR_CHECK(i2s_channel_init_std_mode(tx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_init_std_mode(rx_handle, &std_cfg));
  ESP_ERROR_CHECK(i2s_channel_enable(tx_handle));
  ESP_ERROR_CHECK(i2s_channel_enable(rx_handle));
#endif
  return ret_val;
}

static void i2c_driver_init(void)
{
  printf("initializing i2c driver...\n");
  i2c_config_t es_i2c_cfg;
  memset(&es_i2c_cfg, 0, sizeof(es_i2c_cfg));
  es_i2c_cfg.sda_io_num = I2C_SDA_IO;
  es_i2c_cfg.scl_io_num = I2C_SCL_IO;
  es_i2c_cfg.mode = I2C_MODE_MASTER;
  es_i2c_cfg.sda_pullup_en = GPIO_PULLUP_ENABLE;
  es_i2c_cfg.scl_pullup_en = GPIO_PULLUP_ENABLE;
  es_i2c_cfg.master.clk_speed = I2C_FREQ_HZ;
  auto err = i2c_param_config(I2C_NUM, &es_i2c_cfg);
  if (err != ESP_OK) printf("config i2c failed");
  err = i2c_driver_install(I2C_NUM, I2C_MODE_MASTER,  0, 0, 0);
  if (err != ESP_OK) printf("install i2c driver failed");
}

// es7210 is for audio input codec
static esp_err_t es7210_init_default(void)
{
  printf("initializing es7210 codec...\n");
  esp_err_t ret_val = ESP_OK;
  audio_hal_codec_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.codec_mode = AUDIO_HAL_CODEC_MODE_ENCODE;
  cfg.adc_input = AUDIO_HAL_ADC_INPUT_ALL;
  cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_32BITS;
  cfg.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
  cfg.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
  cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;
  ret_val |= es7210_adc_init(&cfg);
  ret_val |= es7210_adc_config_i2s(cfg.codec_mode, &cfg.i2s_iface);
  ret_val |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC1 | ES7210_INPUT_MIC2), GAIN_37_5DB);
  ret_val |= es7210_adc_set_gain((es7210_input_mics_t)(ES7210_INPUT_MIC3 | ES7210_INPUT_MIC4), GAIN_0DB);
  ret_val |= es7210_adc_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);

  if (ESP_OK != ret_val) {
    printf("Failed initialize codec");
  }

  return ret_val;
}

// es8311 is for audio output codec
static esp_err_t es8311_init_default(void)
{
  printf("initializing es8311 codec...\n");
  esp_err_t ret_val = ESP_OK;
  audio_hal_codec_config_t cfg;
  memset(&cfg, 0, sizeof(cfg));
  cfg.codec_mode = AUDIO_HAL_CODEC_MODE_DECODE;
  cfg.dac_output = AUDIO_HAL_DAC_OUTPUT_LINE1;
  cfg.i2s_iface.bits = AUDIO_HAL_BIT_LENGTH_32BITS;
  cfg.i2s_iface.fmt = AUDIO_HAL_I2S_NORMAL;
  cfg.i2s_iface.mode = AUDIO_HAL_MODE_SLAVE;
  cfg.i2s_iface.samples = AUDIO_HAL_16K_SAMPLES;

  ret_val |= es8311_codec_init(&cfg);
  ret_val |= es8311_set_bits_per_sample(cfg.i2s_iface.bits);
  ret_val |= es8311_config_fmt((es_i2s_fmt_t)cfg.i2s_iface.fmt);
  ret_val |= es8311_codec_set_voice_volume(EXAMPLE_VOLUME);
  ret_val |= es8311_codec_ctrl_state(cfg.codec_mode, AUDIO_HAL_CTRL_START);

  if (ESP_OK != ret_val) {
    printf("Failed initialize codec");
  }

  return ret_val;
}

void audio_init() {
  i2s_driver_init();
  i2c_driver_init();
  es7210_init_default();
  es8311_init_default();
}

void audio_deinit() {
#ifdef USE_LEGACY_I2S
  i2s_stop(I2S_NUM);
  i2s_driver_uninstall(I2S_NUM);
#else
  i2s_channel_disable(tx_handle);
  i2s_channel_disable(rx_handle);
  i2s_del_channel(tx_handle);
  i2s_del_channel(rx_handle);
#endif
}

void audio_play_frame(uint8_t *data, uint32_t num_bytes) {
  size_t bytes_written = 0;
  auto err = ESP_OK;
#ifdef USE_LEGACY_I2S
  err = i2s_write(I2S_NUM, data, num_bytes, &bytes_written, portMAX_DELAY);
#else
  err = i2s_channel_write(tx_handle, data, num_bytes, &bytes_written, 1000);
#endif
  if(num_bytes != bytes_written) {
    printf("ERROR to write %ld != written %d", num_bytes, bytes_written);
  }
  if (err != ESP_OK) {
    printf("ERROR writing i2s channel: %d, '%s'\n", err, esp_err_to_name(err));
  }
}
