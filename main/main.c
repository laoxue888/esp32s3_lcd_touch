/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <stdio.h>
#include<stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_vendor.h"
#include "esp_lcd_panel_ops.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/i2c.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "lv_conf.h"
#include "esp_check.h"
#include "esp_lcd_touch_cst816s.h"
#include "lvgl.h"
#include "esp_lcd_ili9341.h"
#include "esp_lcd_touch_cst816s.h"
#include "gui_guider.h"
#include "custom.h"
#include "wifi_demo.h"
// #include "led.h"
#include "pwm.h"
#include "events_init.h"

/* LCD pins */
#define EXAMPLE_LCD_GPIO_BL (GPIO_NUM_15)
#define EXAMPLE_LCD_GPIO_SCLK (GPIO_NUM_6)
#define EXAMPLE_LCD_GPIO_MOSI (GPIO_NUM_7)
#define EXAMPLE_LCD_GPIO_DC (GPIO_NUM_4)
#define EXAMPLE_LCD_GPIO_CS (GPIO_NUM_5)
#define EXAMPLE_LCD_GPIO_RST (GPIO_NUM_8)

/* LCD size */
#define EXAMPLE_LCD_H_RES (240)
#define EXAMPLE_LCD_V_RES (280)

/* LCD settings */
#define EXAMPLE_LCD_DRAW_BUFF_HEIGHT (50)
#define EXAMPLE_LCD_SPI_NUM (SPI2_HOST)
#define EXAMPLE_LCD_PIXEL_CLK_HZ (40 * 1000 * 1000)
#define EXAMPLE_LCD_CMD_BITS (8)
#define EXAMPLE_LCD_PARAM_BITS (8)
#define EXAMPLE_LCD_COLOR_SPACE (ESP_LCD_COLOR_SPACE_RGB)
#define EXAMPLE_LCD_BITS_PER_PIXEL (16)
#define EXAMPLE_LCD_BL_ON_LEVEL (1)
#define EXAMPLE_LCD_DRAW_BUFF_DOUBLE (1)
#define EXAMPLE_LCD_BIT_PER_PIXEL       (16)            /* LCD bit per pixel */

#define EXAMPLE_USE_TOUCH 1

#if EXAMPLE_USE_TOUCH
#define TOUCH_HOST I2C_NUM_0
#define EXAMPLE_PIN_NUM_TOUCH_SCL (GPIO_NUM_10)
#define EXAMPLE_PIN_NUM_TOUCH_SDA (GPIO_NUM_11)
#define EXAMPLE_PIN_NUM_TOUCH_RST (GPIO_NUM_13)
#define EXAMPLE_PIN_NUM_TOUCH_INT (GPIO_NUM_14)
esp_lcd_touch_handle_t tp = NULL;
#endif

/* LVGL_TASK 任务 配置
 * 包括: 任务句柄 任务优先级 堆栈大小 创建任务
 */
#define EXAMPLE_LVGL_TASK_MAX_DELAY_MS  500
#define EXAMPLE_LVGL_TASK_MIN_DELAY_MS  1
#define EXAMPLE_LVGL_TASK_STACK_SIZE    (5 * 1024)
#define EXAMPLE_LVGL_TASK_PRIORITY      2
void lvgl_port_task(void *arg);

/* LED_TASK 任务 配置
 * 包括: 任务句柄 任务优先级 堆栈大小 创建任务
 */
#define LED_TASK_PRIO           11          /* 任务优先级 */
#define LED_STK_SIZE            2048        /* 任务堆栈大小 */
TaskHandle_t LEDTask_Handler;               /* 任务句柄 */
void led_task(void *pvParameters);          /* 任务函数 */

static portMUX_TYPE my_spinlock = portMUX_INITIALIZER_UNLOCKED;

static char *TAG            = "main";

static SemaphoreHandle_t    lvgl_mux    = NULL;

/* LCD IO and panel */
static esp_lcd_panel_io_handle_t lcd_io = NULL;
static esp_lcd_panel_handle_t lcd_panel = NULL;

/* LVGL display and touch */
static lv_display_t *lvgl_disp = NULL;

lv_ui guider_ui;

#if EXAMPLE_USE_TOUCH
static void example_lvgl_touch_cb(lv_indev_drv_t *drv, lv_indev_data_t *data)
{
    esp_lcd_touch_handle_t tp = (esp_lcd_touch_handle_t)drv->user_data;
    assert(tp);

    uint16_t tp_x;
    uint16_t tp_y;
    uint8_t tp_cnt = 0;
    /* Read data from touch controller into memory */
    esp_lcd_touch_read_data(tp);
    /* Read data from touch controller */
    bool tp_pressed = esp_lcd_touch_get_coordinates(tp, &tp_x, &tp_y, NULL, &tp_cnt, 1);
    if (tp_pressed && tp_cnt > 0)
    {
        data->point.x = tp_x;
        data->point.y = tp_y;
        data->state = LV_INDEV_STATE_PRESSED;
        ESP_LOGD(TAG, "Touch position: %d,%d", tp_x, tp_y);
    }
    else
    {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}
#endif

static bool lvgl_lock(int timeout_ms)
{
    assert(lvgl_mux && "bsp_display_start must be called first");

    const TickType_t timeout_ticks = (timeout_ms == -1) ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(lvgl_mux, timeout_ticks) == pdTRUE;
}

static void lvgl_unlock(void)
{
    assert(lvgl_mux && "bsp_display_start must be called first");
    xSemaphoreGive(lvgl_mux);
}

static esp_err_t lcd_init(void)
{
    esp_err_t ret = ESP_OK;
    /* LCD backlight */
    gpio_config_t bk_gpio_config = {
        .mode = GPIO_MODE_OUTPUT,
        .pin_bit_mask = 1ULL << EXAMPLE_LCD_GPIO_BL};
    ESP_ERROR_CHECK(gpio_config(&bk_gpio_config));

    /* LCD initialization */
    ESP_LOGD(TAG, "Initialize SPI bus");
    const spi_bus_config_t buscfg = {
        .sclk_io_num = EXAMPLE_LCD_GPIO_SCLK,
        .mosi_io_num = EXAMPLE_LCD_GPIO_MOSI,
        .miso_io_num = GPIO_NUM_NC,
        .quadwp_io_num = GPIO_NUM_NC,
        .quadhd_io_num = GPIO_NUM_NC,
        .max_transfer_sz = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_DRAW_BUFF_HEIGHT * sizeof(uint16_t),
    };
    ESP_RETURN_ON_ERROR(spi_bus_initialize(EXAMPLE_LCD_SPI_NUM, &buscfg, SPI_DMA_CH_AUTO), TAG, "SPI init failed");

    ESP_LOGD(TAG, "Install panel IO");
    const esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = EXAMPLE_LCD_GPIO_DC,
        .cs_gpio_num = EXAMPLE_LCD_GPIO_CS,
        .pclk_hz = EXAMPLE_LCD_PIXEL_CLK_HZ,
        .lcd_cmd_bits = EXAMPLE_LCD_CMD_BITS,
        .lcd_param_bits = EXAMPLE_LCD_PARAM_BITS,
        .spi_mode = 0,
        .trans_queue_depth = 10,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)EXAMPLE_LCD_SPI_NUM, &io_config, &lcd_io), err, TAG, "New panel IO failed");

    ESP_LOGD(TAG, "Install LCD driver");
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_LCD_GPIO_RST,
        .color_space = EXAMPLE_LCD_COLOR_SPACE,
        .bits_per_pixel = EXAMPLE_LCD_BITS_PER_PIXEL,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_st7789(lcd_io, &panel_config, &lcd_panel), err, TAG, "New panel failed");

    esp_lcd_panel_reset(lcd_panel);
    esp_lcd_panel_init(lcd_panel);
    esp_lcd_panel_mirror(lcd_panel, true, true);
    esp_lcd_panel_disp_on_off(lcd_panel, true);

    /* LCD backlight on */
    ESP_ERROR_CHECK(gpio_set_level(EXAMPLE_LCD_GPIO_BL, EXAMPLE_LCD_BL_ON_LEVEL));

    esp_lcd_panel_set_gap(lcd_panel, 0, 20);
    esp_lcd_panel_invert_color(lcd_panel, true);

    return ret;
err:
    if (lcd_panel)
    {
        esp_lcd_panel_del(lcd_panel);
    }
    if (lcd_io)
    {
        esp_lcd_panel_io_del(lcd_io);
    }
    spi_bus_free(EXAMPLE_LCD_SPI_NUM);
    return ret;
}

static void lcd_touch_init(void)
{
#if EXAMPLE_USE_TOUCH
    ESP_LOGI(TAG, "Initialize I2C bus");
    esp_log_level_set("lcd_panel.io.i2c", ESP_LOG_NONE);
    esp_log_level_set("CST816S", ESP_LOG_NONE);
    const i2c_config_t i2c_conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = EXAMPLE_PIN_NUM_TOUCH_SDA,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_io_num = EXAMPLE_PIN_NUM_TOUCH_SCL,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = 100 * 1000,
    };
    i2c_param_config(TOUCH_HOST, &i2c_conf);

    i2c_driver_install(TOUCH_HOST, i2c_conf.mode, 0, 0, 0);

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    const esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST816S_CONFIG();
    // Attach the TOUCH to the I2C bus
    esp_lcd_new_panel_io_i2c((esp_lcd_i2c_bus_handle_t)TOUCH_HOST, &tp_io_config, &tp_io_handle);

    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = EXAMPLE_PIN_NUM_TOUCH_RST,
        .int_gpio_num = EXAMPLE_PIN_NUM_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };

    ESP_LOGI(TAG, "Initialize touch controller");
    esp_lcd_touch_new_i2c_cst816s(tp_io_handle, &tp_cfg, &tp);
#endif
}

static esp_err_t lcd_lvgl_init(void)
{
    /* Initialize LVGL */
    const lvgl_port_cfg_t lvgl_cfg = {
        .task_priority = 4,       /* LVGL task priority */
        .task_stack = 4096,       /* LVGL task stack size */
        .task_affinity = -1,      /* LVGL task pinned to core (-1 is no affinity) */
        .task_max_sleep_ms = 500, /* Maximum sleep in LVGL task */
        .timer_period_ms = 5      /* LVGL timer tick period in ms */
    };
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port initialization failed");

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = lcd_io,
        .panel_handle = lcd_panel,
        .buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_DRAW_BUFF_HEIGHT * sizeof(uint16_t),
        .double_buffer = EXAMPLE_LCD_DRAW_BUFF_DOUBLE,
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
        .monochrome = false,
        /* Rotation values must be same as used in esp_lcd for initial settings of the screen */
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = true,
        }};
    lvgl_disp = lvgl_port_add_disp(&disp_cfg);

#if EXAMPLE_USE_TOUCH
    static lv_indev_drv_t indev_drv; // Input device driver (Touch)
    lv_indev_drv_init(&indev_drv);
    indev_drv.type = LV_INDEV_TYPE_POINTER;
    indev_drv.disp = lvgl_disp;
    indev_drv.read_cb = example_lvgl_touch_cb;
    indev_drv.user_data = tp;
    lv_indev_drv_register(&indev_drv);
#endif

    return ESP_OK;
}

void app_main(void)
{
    lcd_init();
    lcd_touch_init();
    lcd_lvgl_init();

    /* =============== Create LVGL Task =============== */
    ESP_LOGI(TAG, "Run LVGL Task");

    taskENTER_CRITICAL(&my_spinlock);

    /* lvgl任务 */
    xTaskCreate(lvgl_port_task, "LVGL", EXAMPLE_LVGL_TASK_STACK_SIZE, NULL, EXAMPLE_LVGL_TASK_PRIORITY, NULL);
    
    /* key任务 */
    xTaskCreate((TaskFunction_t )led_task,
                (const char *   )"led_task",
                (uint16_t       )LED_STK_SIZE,
                (void *         )NULL,
                (UBaseType_t    )LED_TASK_PRIO,
                (TaskHandle_t * )&LEDTask_Handler);
    taskEXIT_CRITICAL(&my_spinlock);
}

void lvgl_port_task(void *arg)
{
    lvgl_mux = xSemaphoreCreateMutex();
    assert(lvgl_mux);

    ESP_LOGI(TAG, "Starting LVGL task");
    ESP_LOGI(TAG, "Display LVGL UI");

    // ui_init();
    //NVS初始化（WIFI底层驱动有用到NVS，所以这里要初始化）
    nvs_flash_init();
    //wifi STA工作模式初始化
    wifi_sta_init();

    setup_ui(&guider_ui);

    float f = 3.14;

    uint32_t task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
    while (1) {
        /* Lock the mutex due to the LVGL APIs are not thread-safe */
        if (lvgl_lock(-1)) {
            task_delay_ms = lv_timer_handler();
            /* Release the mutex */
            lvgl_unlock();
        }
        if (task_delay_ms > EXAMPLE_LVGL_TASK_MAX_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MAX_DELAY_MS;
        } else if (task_delay_ms < EXAMPLE_LVGL_TASK_MIN_DELAY_MS) {
            task_delay_ms = EXAMPLE_LVGL_TASK_MIN_DELAY_MS;
        }

        f += 0.1;
        if (f >= 100)
        {
            f = 0.0;
        }

        //显示浮点数，屏幕刷新只能放到这个线程
        char show_text[12];
        sprintf(show_text, "%.2f", f);
        lv_label_set_text(guider_ui.screen_label_x, show_text);

        vTaskDelay(pdMS_TO_TICKS(task_delay_ms));
    }
}


void led_task(void *pvParameters)
{
    pvParameters = pvParameters;
    while (1)
    {
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}
