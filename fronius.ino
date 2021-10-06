#ifndef BOARD_HAS_PSRAM
#error "Please enable PSRAM !!!"
#endif

#include <Arduino.h>
#include <math.h>
#include <esp_task_wdt.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "epd_driver.h"
#include "apple16.h"
#include "applelarge.h"
#include "esp_adc_cal.h"
#include <WiFiMulti.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include "sun.h"
#include "house.h"
#include "power.h"
#include "credentials.h"


WiFiMulti WiFiMulti;


#define BATT_PIN            36
int vref = 1100;

void setup()
{

    Serial.begin(115200);



    // Correct the ADC reference voltage
    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
        vref = adc_chars.vref;
    }

    epd_init();
    
    int width = EPD_WIDTH;
    int height = 3;
    
    uint8_t *header = (uint8_t *)ps_calloc(sizeof(uint8_t), width * height);
    if (!header) {
        Serial.println("alloc memory failed !!!");
        while (1);
    }
    int bufw = (width / 2) + (width % 2);
    memset(header, 0xFF, bufw * height);

    Rect_t area = {
        .x = 0,
        .y = 70,
        .width = width,
        .height = height,
    };


    epd_draw_hline(0, 0, width, 0x00, header);
    epd_draw_hline(0, 1, width, 0x00, header);
    epd_draw_hline(0, 2, width, 0xF0, header);

    epd_poweron();
    epd_clear();
    epd_poweroff();

    
    draw_icon(sun_data, EPD_WIDTH / 4 - sun_width / 2 - 50, EPD_HEIGHT / 2 - sun_height - 50, sun_width, sun_height);
    draw_icon(house_data, EPD_WIDTH / 2 - house_width / 2, EPD_HEIGHT / 2 - house_height - 60, house_width, house_height);
    draw_icon(power_data, EPD_WIDTH * 3 / 4 - power_width / 2 + 50, EPD_HEIGHT / 2 - power_height - 50, power_width, power_height);

    epd_poweron();
    epd_draw_image(area, header, BLACK_ON_WHITE);
    epd_poweroff();

    WiFiMulti.addAP(WIFI_SSID, WIFI_PASS);
    while(WiFiMulti.run() != WL_CONNECTED) {
      delay(5000);
    };

    int cursor_x = 10;
    int cursor_y = 50;

    char *wifi = NAME;

    epd_poweron();
    writeln((GFXfont *)&apple16, wifi, &cursor_x, &cursor_y, NULL);    
    epd_poweroff();

}

void draw_icon(const uint8_t * data, uint32_t x, uint32_t y, uint32_t width, uint32_t height) {
  
    uint8_t *buf = (uint8_t *)ps_calloc(sizeof(uint8_t), width * height / 2);
    if (!buf) {
      return;
    }
    memset(buf, 0xFF, width * height / 2);

    Rect_t area = {
      .x = x,
      .y = y,
      .width = width,
      .height = height,
    };
    epd_poweron();
    epd_draw_grayscale_image(area, (uint8_t *)data);
    epd_poweroff();
    free(buf);
}

void loop()
{
    // When reading the battery voltage, POWER_EN must be turned on
    epd_poweron();
    
    uint16_t v = analogRead(BATT_PIN);
    float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
    battery(EPD_WIDTH - 200, 50, battery_voltage);


    
    if(WiFiMulti.run() != WL_CONNECTED) {
      delay(5000);
      return;
    };

    Rect_t t_area = {
      .x = EPD_WIDTH - 100,
      .y = 0,
      .width = 100,
      .height = 50,
    };

    String jsonbuf = fetch_solar();
    JSONVar response = JSON.parse(jsonbuf);

    if (JSON.typeof(response) == "undefined") {
      return;
    }

    int t_x = EPD_WIDTH - 100;
    int t_y = 50;
    String tstamp = JSON.stringify(response["Head"]["Timestamp"]).substring(12,17);
    epd_poweron();
    epd_clear_area(t_area);
    writeln((GFXfont *)&apple16, tstamp.c_str(), &t_x, &t_y, NULL);
    epd_poweroff();

    JSONVar site = response["Body"]["Data"]["Site"];

    int offsetx = 100;
    int solarx = EPD_WIDTH / 4 - offsetx - 50;
    int loadx = EPD_WIDTH / 2 - offsetx + 20;
    int gridx = EPD_WIDTH * 3 / 4 - offsetx + 60;
    int texty = EPD_HEIGHT * 3 / 4;

    epd_poweron();
    bool rarrow = write_power_text(site["P_PV"], solarx, texty);
    write_arrow(rarrow, solarx + 210, texty - 200);
    write_power_text(site["P_Load"], loadx, texty);
    bool garrow = !write_power_text(site["P_Grid"], gridx, texty);
    write_arrow(garrow, gridx - 50, texty - 200);
    epd_poweroff();

    // It will turn off the power of the entire
    // POWER_EN control and also turn off the blue LED light
    epd_poweroff_all();

    // deeper sleep when no sun is out.
    int delay_ms = 10000; // 10s
    if (isnan((double) site["P_PV"])) {
      delay_ms = 600000; // 10min
    }
    delay(delay_ms);
}

void write_arrow(bool is_right, int x, int y) {
  String arrow;
  if (is_right) {
    arrow = "➡";
  } else {
    arrow = "⬅";
  }
  Rect_t area = {
      .x = x,
      .y = y - 70,
      .width = 80,
      .height = 70,
    };
  epd_clear_area(area);
  writeln((GFXfont *)&applelarge, arrow.c_str(), &x, &y, NULL);
}

bool write_power_text(JSONVar text, int x, int y) {
    double num = safe_number(text);
    String power;
    String units;
    if (num > 1000 || num < -1000) {
      power = String(abs(num) / 1000, 2);
      units = "kW";
    } else {
      power = String(abs(num), 0);
      units = "W";
    }

    Rect_t area = {
      .x = x,
      .y = y - 70,
      .width = 240,
      .height = 70,
    };
    epd_clear_area(area);
    writeln((GFXfont *)&applelarge, (char *)power.c_str(), &x, &y, NULL);
    int unitx = x + 20;
    int unity = y;
    writeln((GFXfont *)&apple16, (char *)units.c_str(), &unitx, &unity, NULL);
    return num >= 0;
}

String fetch_solar() {
    HTTPClient http;
    http.begin("http://" + INVERTER_IP + "/solar_api/v1/GetPowerFlowRealtimeData.fcgi");
    int code = http.GET();
    String payload = "{}";
    if (code > 0) {
      payload = http.getString();
    }
    http.end();
    return payload;
}

double safe_number(JSONVar num) {
    double maybe_nan = (double) num;
    if (isnan(maybe_nan)) {
      return 0.0;
    } else {
      return maybe_nan;
    }
}

void battery(int x, int y, float voltage) {
  uint8_t percentage = 100;
  if (voltage > 1 ) { // Only display if there is a valid reading
    if (voltage >= 4.20) {
      percentage = 100;
    } else if (voltage >= 3.75) {
      percentage = 90;
    } else if (voltage >= 3.65) {
      percentage = 80;
    } else if (voltage >= 3.60) {
      percentage = 70;
    } else if (voltage >= 3.55) {
      percentage = 60;
    } else if (voltage >= 3.50) {
      percentage = 50;
    } else if (voltage >= 3.45) {
      percentage = 40;
    } else {
      percentage = 0;
    }
    int width = 100;
    int height = 30;
    Rect_t area = {
      .x = x,
      .y = y - 30,
      .width = width,
      .height = height
    };
    uint8_t *buf = (uint8_t *)ps_calloc(sizeof(uint8_t), width * height / 2);
    if (!buf) {
      return;
    }
    memset(buf, 0xFF, width * height / 2);
    epd_poweron();
    epd_clear_area(area);
    epd_draw_image(area, buf, BLACK_ON_WHITE);
    String text = String(percentage) + "%";
    writeln((GFXfont *)&apple16, text.c_str(), &x, &y, NULL);    
    epd_poweroff();
    free(buf);
  }
}
