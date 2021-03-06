/*
 * SPDX-FileCopyrightText: 2021 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <string.h>
#include <stdio.h>
#include "sdkconfig.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/adc.h"
#include "esp_timer.h"
#include <rom/ets_sys.h>

#include "dac-cosin.h"

#define TIMES              1024 //256
#define GET_UNIT(x)        ((x>>3) & 0x1)

#if CONFIG_IDF_TARGET_ESP32
#define ADC_RESULT_BYTE     2
#define ADC_CONV_LIMIT_EN   1                       //For ESP32, this should always be set to 1
#define ADC_CONV_MODE       ADC_CONV_SINGLE_UNIT_1  //ESP32 only supports ADC1 DMA mode
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE1
#elif CONFIG_IDF_TARGET_ESP32S2
#define ADC_RESULT_BYTE     2
#define ADC_CONV_LIMIT_EN   0
#define ADC_CONV_MODE       ADC_CONV_BOTH_UNIT
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE2
#elif CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32H2
#define ADC_RESULT_BYTE     4
#define ADC_CONV_LIMIT_EN   0
#define ADC_CONV_MODE       ADC_CONV_ALTER_UNIT     //ESP32C3 only supports alter mode
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE2
#elif CONFIG_IDF_TARGET_ESP32S3
#define ADC_RESULT_BYTE     4
#define ADC_CONV_LIMIT_EN   0
#define ADC_CONV_MODE       ADC_CONV_BOTH_UNIT
#define ADC_OUTPUT_TYPE     ADC_DIGI_OUTPUT_FORMAT_TYPE2
#endif

#if CONFIG_IDF_TARGET_ESP32C3 || CONFIG_IDF_TARGET_ESP32S3 || CONFIG_IDF_TARGET_ESP32H2
static uint16_t adc1_chan_mask = BIT(2) | BIT(3);
static uint16_t adc2_chan_mask = BIT(0);
static adc_channel_t channel[3] = {ADC1_CHANNEL_2, ADC1_CHANNEL_3, (ADC2_CHANNEL_0 | 1 << 3)};
#endif
#if CONFIG_IDF_TARGET_ESP32S2
static uint16_t adc1_chan_mask = BIT(2) | BIT(3);
static uint16_t adc2_chan_mask = BIT(0);
static adc_channel_t channel[3] = {ADC1_CHANNEL_2, ADC1_CHANNEL_3, (ADC2_CHANNEL_0 | 1 << 3)};
#endif
#if CONFIG_IDF_TARGET_ESP32
static uint16_t adc1_chan_mask = BIT(7);
static uint16_t adc2_chan_mask = 0;
static adc_channel_t channel[1] = {ADC1_CHANNEL_7};
#endif

void gpio_ini(int pin);

static const char *TAG = "ADC DMA";

uint32_t freq_khz = 2000;

static void continuous_adc_init(uint16_t adc1_chan_mask, uint16_t adc2_chan_mask, adc_channel_t *channel, uint8_t channel_num)
{
    adc_digi_init_config_t adc_dma_config = {
        .max_store_buf_size = 1024,
        .conv_num_each_intr = TIMES,
        .adc1_chan_mask = adc1_chan_mask,
        .adc2_chan_mask = adc2_chan_mask,
    };
    ESP_ERROR_CHECK(adc_digi_initialize(&adc_dma_config));

    adc_digi_configuration_t dig_cfg = {
        .conv_limit_en = ADC_CONV_LIMIT_EN,
        .conv_limit_num = 250,
        // .sample_freq_hz = 2000 * 1000, // 2000000 is maximum value, not more!
        .sample_freq_hz = freq_khz * 1000,
        .conv_mode = ADC_CONV_MODE,
        .format = ADC_OUTPUT_TYPE,
    };

    adc_digi_pattern_config_t adc_pattern[SOC_ADC_PATT_LEN_MAX] = {0};
    dig_cfg.pattern_num = channel_num;
    for (int i = 0; i < channel_num; i++) {
        uint8_t unit = GET_UNIT(channel[i]);
        uint8_t ch = channel[i] & 0x7;
        adc_pattern[i].atten = ADC_ATTEN_DB_0;
        adc_pattern[i].channel = ch;
        adc_pattern[i].unit = unit;
        adc_pattern[i].bit_width = SOC_ADC_DIGI_MAX_BITWIDTH;

        ESP_LOGI(TAG, "adc_pattern[%d].atten is :%x", i, adc_pattern[i].atten);
        ESP_LOGI(TAG, "adc_pattern[%d].channel is :%x", i, adc_pattern[i].channel);
        ESP_LOGI(TAG, "adc_pattern[%d].unit is :%x", i, adc_pattern[i].unit);
    }
    dig_cfg.adc_pattern = adc_pattern;
    ESP_ERROR_CHECK(adc_digi_controller_configure(&dig_cfg));

    printf("TIMES=%d  sample_freq_hz= %d\n", TIMES, dig_cfg.sample_freq_hz);

}


// requirement is nanoseconds
// https://github.com/eerimoq/simba/issues/155
static int sys_port_get_time_into_tick()
{
    // get clock count
    // http://sub.nanona.fi/esp8266/timing-and-ticks.html
    uint32_t ccount;
    __asm__ __volatile__("esync; rsr %0,ccount":"=a" (ccount));


    // get nanos
    // from https://github.com/adafruit/ESP8266-Arduino/blob/50122289a3ecb7cddcba34db9ec9ec8b4d7e7442/hardware/esp8266com/esp8266/cores/esp8266/Esp.h#L129
    // system_get_cpu_freq() returns 80 or 160; unit MHz
    ccount /= ets_get_cpu_frequency(); // (freq_MHz*1000000) Hz = 1/second // Get the real CPU ticks per us to the ets.
    
    return ccount;
}

void IRAM_ATTR app_main_adc()
{
    esp_err_t ret;
    uint32_t ret_num = 0;
    uint8_t result[TIMES] = {0};
    memset(result, 0xcc, TIMES);

    dac_app_main();

    continuous_adc_init(adc1_chan_mask, adc2_chan_mask, channel, sizeof(channel) / sizeof(adc_channel_t));

    int t1 = portGET_RUN_TIME_COUNTER_VALUE();
    int t11 = sys_port_get_time_into_tick();
    uint64_t t_start = esp_timer_get_time();
    ets_delay_us(25); // just for test, not used
    __asm__ __volatile__ ("nop");

    uint64_t t_end = esp_timer_get_time();
    int t22 = sys_port_get_time_into_tick();
    int t2 = portGET_RUN_TIME_COUNTER_VALUE();
    gpio_ini(GPIO_NUM_14);

    adc_digi_start();

    uint64_t start = esp_timer_get_time();
    uint64_t start2 = esp_timer_get_time();
    int t33 = sys_port_get_time_into_tick();
    int t3 = portGET_RUN_TIME_COUNTER_VALUE();

    for(int i=0; i<100; i++){
        GPIO.out_w1ts = (1 << GPIO_NUM_14);
        GPIO.out_w1tc = (1 << GPIO_NUM_14);
    }

    // while(1) 
    {
        ret = adc_digi_read_bytes(result, TIMES, &ret_num, ADC_MAX_DELAY);

        uint64_t end = esp_timer_get_time();
        int t44 = sys_port_get_time_into_tick();
        int t4 = portGET_RUN_TIME_COUNTER_VALUE();

        printf("TIME took %llu microseconds ret number= %d\n", (end - start), ret_num);
        printf("TIME2 took %llu microseconds ret number= %d\n", (end - start2), ret_num);
        printf("us=%llu \n", t_end-t_start);
        printf("tt= %d %d %d %u\n", t22-t11, t44-t33, t4-t3, ets_get_cpu_frequency());

        if (ret == ESP_OK || ret == ESP_ERR_INVALID_STATE) {
            if (ret == ESP_ERR_INVALID_STATE) {
                /**
                 * @note 1
                 * Issue:
                 * As an example, we simply print the result out, which is super slow. Therefore the conversion is too
                 * fast for the task to handle. In this condition, some conversion results lost.
                 *
                 * Reason:
                 * When this error occurs, you will usually see the task watchdog timeout issue also.
                 * Because the conversion is too fast, whereas the task calling `adc_digi_read_bytes` is slow.
                 * So `adc_digi_read_bytes` will hardly block. Therefore Idle Task hardly has chance to run. In this
                 * example, we add a `vTaskDelay(1)` below, to prevent the task watchdog timeout.
                 *
                 * Solution:
                 * Either decrease the conversion speed, or increase the frequency you call `adc_digi_read_bytes`
                 */
            }

            ESP_LOGI("TASK:", "ret is %x, ret_num is %d", ret, ret_num);
            for (int i = 0; i < ret_num; i += ADC_RESULT_BYTE) {
                adc_digi_output_data_t *p = (void*)&result[i];
    #if CONFIG_IDF_TARGET_ESP32

                // ESP_LOGI(TAG, "Unit: %d, Channel: %d, Value: %x", 1, p->type1.channel, p->type1.data);

                // ESP_LOGI(TAG, "i=%d Value_Hex: %x Value_Dec= %d ", i, p->type1.data, p->type1.data);

    #else
                if (ADC_CONV_MODE == ADC_CONV_BOTH_UNIT || ADC_CONV_MODE == ADC_CONV_ALTER_UNIT) {
                    if (check_valid_data(p)) {
                        ESP_LOGI(TAG, "Unit: %d,_Channel: %d, Value: %x", p->type2.unit+1, p->type2.channel, p->type2.data);
                    } else {
                        // abort();
                        ESP_LOGI(TAG, "Invalid data [%d_%d_%x]", p->type2.unit+1, p->type2.channel, p->type2.data);
                    }
                }
    #if CONFIG_IDF_TARGET_ESP32S2
                else if (ADC_CONV_MODE == ADC_CONV_SINGLE_UNIT_2) {
                    ESP_LOGI(TAG, "Unit: %d, Channel: %d, Value: %x", 2, p->type1.channel, p->type1.data);
                } else if (ADC_CONV_MODE == ADC_CONV_SINGLE_UNIT_1) {
                    ESP_LOGI(TAG, "Unit: %d, Channel: %d, Value: %x", 1, p->type1.channel, p->type1.data);
                }
    #endif  //#if CONFIG_IDF_TARGET_ESP32S2
    #endif
            }
            //See `note 1`
            vTaskDelay(1);
        } else if (ret == ESP_ERR_TIMEOUT) {
            /**
             * ``ESP_ERR_TIMEOUT``: If ADC conversion is not finished until Timeout, you'll get this return error.
             * Here we set Timeout ``portMAX_DELAY``, so you'll never reach this branch.
             */
            ESP_LOGW(TAG, "No data, increase timeout or reduce conv_num_each_intr");
            vTaskDelay(1000);
        }

    }

    adc_digi_stop();
    ret = adc_digi_deinitialize();
    assert(ret == ESP_OK);
}

void gpio_ini(int pin){
    //zero-initialize the config sructure.
    gpio_config_t io_conf = {};
    //disable interrupt
    io_conf.intr_type = GPIO_INTR_DISABLE;
    //set as output mode
    io_conf.mode = GPIO_MODE_OUTPUT;
    //bit mask of the pins that you want to set,e.g.GPIO18/19
    io_conf.pin_bit_mask = (1ULL<<GPIO_NUM_14);
    //disable pull-down mode
    io_conf.pull_down_en = 0;
    //disable pull-up mode
    io_conf.pull_up_en = 0;
    //configure GPIO with the given settings
    gpio_config(&io_conf);
}

void gpio_max_speed_test(){
    gpio_ini(GPIO_NUM_14);
    // int cnt = 0;
    // while(1) {
    //     printf("cnt: %d\n", cnt++);
    //     vTaskDelay(1000 / portTICK_RATE_MS);
    //     gpio_set_level(GPIO_NUM_14, cnt % 2);
    // }
    while (1)   
    {
        for(int i=0; i<10; i++){
            GPIO.out_w1ts = (1 << GPIO_NUM_14);
            // __asm__ __volatile__("nop;nop;nop;nop;nop;nop;nop;"); // Bug workaround (I found this snippet somewhere in this forum)

            GPIO.out_w1tc = (1 << GPIO_NUM_14);
            // __asm__ __volatile__("nop;nop;nop;nop;nop;nop;nop;");
        }
        vTaskDelay(1000 / portTICK_RATE_MS);
    }
}

void app_main(){
#ifdef GPIO_MAX_SPEED_TEST
    gpio_max_speed_test();
#else    
    app_main_adc();       
#endif    
}
