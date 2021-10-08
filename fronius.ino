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
#include <WiFi.h>
#include <HTTPClient.h>
#include <Arduino_JSON.h>
#include "sun.h"
#include "house.h"
#include "power.h"
#include "credentials.h"


#define BATT_PIN            36
#define SHORT_SLEEP_MS      60000
#define LONG_SLEEP_MS       1800000
#define INITIAL_VREF        1100

int vref = INITIAL_VREF;

static RTC_DATA_ATTR struct {
  byte mac [ 6 ];
  byte mode;
  byte chl;
  uint32_t ip;
  uint32_t gw;
  uint32_t msk;
  uint32_t dns;
  uint32_t seq;
  uint32_t chk;
} cfgbuf;

static RTC_DATA_ATTR struct {
  bool sun_arrow;
  bool grid_arrow;
  uint8_t percentage;
  uint32_t tstamp;
  bool is_meta_rendered;
  bool is_arrows_rendered; 
} cfgmeta;

void setup()
{

  uint32_t startms = millis();
  if (vref == INITIAL_VREF) {
    // Correct the ADC reference voltage
    esp_adc_cal_characteristics_t adc_chars;
    esp_adc_cal_value_t val_type = esp_adc_cal_characterize(ADC_UNIT_1, ADC_ATTEN_DB_11, ADC_WIDTH_BIT_12, 1100, &adc_chars);
    if (val_type == ESP_ADC_CAL_VAL_EFUSE_VREF) {
      vref = adc_chars.vref;
    }
  }
  initMetaCfg();
  Serial.begin(115200);
  uint32_t initms = millis();
  epd_init();
  uint32_t epdinitms = millis();
  draw_meta();
  uint32_t drawmeta_ms = millis();
  wifi_connect();
  uint32_t wifi_ms = millis();

  Serial.printf("start %dms init %dms epd %dms meta %dms wifi %dms\n", startms, initms, epdinitms, drawmeta_ms, wifi_ms);

}



void loop()
{
  uint32_t startms = millis();
  // When reading the battery voltage, POWER_EN must be turned on
  epd_poweron();
  uint16_t v = analogRead(BATT_PIN);
  epd_poweroff();
  float battery_voltage = ((float)v / 4095.0) * 2.0 * 3.3 * (vref / 1000.0);
  battery(EPD_WIDTH - 200, 50, battery_voltage);
  uint32_t batteryms = millis();

  Rect_t t_area = {
    .x = EPD_WIDTH - 100,
    .y = 0,
    .width = 100,
    .height = 50,
  };

  String jsonbuf = fetch_solar();
  uint32_t fetchms = millis();
  JSONVar response = JSON.parse(jsonbuf);
  uint32_t parsems = millis();

  if (JSON.typeof(response) == "undefined") {
    return;
  }

  int t_x = EPD_WIDTH - 100;
  int t_y = 50;
  String _tstamp = JSON.stringify(response["Head"]["Timestamp"]).substring(12, 17);
  uint32_t minutes = _tstamp.charAt(3) + _tstamp.charAt(4);
  if (minutes != cfgmeta.tstamp) {
    cfgmeta.tstamp = minutes;
    epd_poweron();
    epd_clear_area(t_area);
    writeln((GFXfont *)&apple16, _tstamp.c_str(), &t_x, &t_y, NULL);
    epd_poweroff();
  }
  uint32_t tstampms = millis();

  JSONVar site = response["Body"]["Data"]["Site"];

  // power text
  int offsetx = 100;
  int solarx = EPD_WIDTH / 4 - offsetx - 50;
  int loadx = EPD_WIDTH / 2 - offsetx + 20;
  int gridx = EPD_WIDTH * 3 / 4 - offsetx + 60;
  int texty = EPD_HEIGHT * 3 / 4;

  int width = EPD_WIDTH;
  int height = 70;

  Rect_t area = {
    .x = 0,
    .y = texty - height,
    .width = width,
    .height = height,
  };

  int bufw = (width / 2) + (width % 2);
  uint8_t *buf = (uint8_t *)ps_calloc(sizeof(uint8_t), bufw * height);
  if (!buf) {
    return delay(SHORT_SLEEP_MS);
  }
  memset(buf, 0xFF, bufw * height);

  bool _sun_arrow = write_power_text(site["P_PV"], solarx, height, buf);
  write_power_text(site["P_Load"], loadx, height, buf);
  bool _grid_arrow = !write_power_text(site["P_Grid"], gridx, height, buf);

  epd_poweron();
  epd_clear_area(area);
  epd_draw_image(area, buf, BLACK_ON_WHITE);
  uint32_t powerms = millis();

  // arrows
  if (_sun_arrow != cfgmeta.sun_arrow || !cfgmeta.is_arrows_rendered) {
    cfgmeta.sun_arrow = _sun_arrow;
    write_arrow(_sun_arrow, solarx + 210, texty - 200);
  }
  if (_grid_arrow != cfgmeta.grid_arrow || !cfgmeta.is_arrows_rendered) {
    cfgmeta.grid_arrow = _grid_arrow;
    write_arrow(_grid_arrow, gridx - 60, texty - 200);
  }
  free(buf);

  cfgmeta.is_arrows_rendered = true;
  uint32_t arrowms = millis();

  Serial.printf("start %dms bat %dms http %dms json %dms time %dms pow %dms arr %dms\n",
      startms, batteryms, fetchms, parsems, tstampms, powerms, arrowms);
  // It will turn off the power of the entire
  // POWER_EN control and also turn off the blue LED light
  epd_poweroff_all();

  // deeper sleep when no sun is out.
  int delay_ms = SHORT_SLEEP_MS; // 10s
  if (isnan((double) site["P_PV"])) {
    delay_ms = LONG_SLEEP_MS; // 10min
  }
  return deep_sleep(delay_ms);
}


bool write_power_text(JSONVar text, int x, int y, uint8_t *buf) {
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

  writeln((GFXfont *)&applelarge, (char *)power.c_str(), &x, &y, buf);
  int unitx = x + 20;
  int unity = y;
  writeln((GFXfont *)&apple16, (char *)units.c_str(), &unitx, &unity, buf);
  return num >= 0;
}


void wifi_connect() {

  checkCfg();

  if (WiFi.getMode() != WIFI_OFF) {
    printf("Wifi wasn't off!\n");
    WiFi.persistent(true);
    WiFi.mode(WIFI_OFF);
  }

    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    int m = cfgbuf.mode;
    bool ok;
    switch (cfgbuf.mode) {
    case 0:
        Serial.printf("wifi mode 0\n");
        ok = WiFi.begin(WIFI_SSID, WIFI_PASS);
        break;
    default:
        Serial.printf("wifi mode 1\n");

        ok = WiFi.config(cfgbuf.ip, cfgbuf.gw, cfgbuf.msk, cfgbuf.dns);
        ok = WiFi.begin(WIFI_SSID, WIFI_PASS, cfgbuf.chl, cfgbuf.mac);

        cfgbuf.mode = 0;
        break;
    }
    if (!ok) {
        deep_sleep(SHORT_SLEEP_MS);
    }
    while (WiFi.status() != WL_CONNECTED) delay(1);

    cfgbuf.seq++;
    cfgbuf.mode++;
    writecfg();
}


bool checkCfg() {
  uint32_t x = 0;
  uint32_t *p = (uint32_t *)cfgbuf.mac;
  for (uint32_t i = 0; i < sizeof(cfgbuf) / 4; i++) x += p[i];
  if (x == 0 && cfgbuf.ip != 0) return true;

  // bad checksum, init data
  for (uint32_t i = 0; i < 6; i++) cfgbuf.mac[i] = 0xff;
  cfgbuf.mode = 0; // chk err, reconfig
  cfgbuf.chl = 0;
  cfgbuf.ip = IPAddress(0, 0, 0, 0);
  cfgbuf.gw = IPAddress(0, 0, 0, 0);
  cfgbuf.msk = IPAddress(255, 255, 255, 0);
  cfgbuf.dns = IPAddress(0, 0, 0, 0);
  cfgbuf.seq = 100;
  return false;
}

void writecfg(void) {
  // save new info
  uint8_t *bssid = WiFi.BSSID();
  for (int i = 0; i < sizeof(cfgbuf.mac); i++) cfgbuf.mac[i] = bssid[i];
  cfgbuf.chl = WiFi.channel();
  cfgbuf.ip = WiFi.localIP();
  cfgbuf.gw = WiFi.gatewayIP();
  cfgbuf.msk = WiFi.subnetMask();
  cfgbuf.dns = WiFi.dnsIP();
  // recalculate checksum
  uint32_t x = 0;
  uint32_t *p = (uint32_t *)cfgbuf.mac;
  for (uint32_t i = 0; i < sizeof(cfgbuf) / 4 - 1; i++) x += p[i];
  cfgbuf.chk = -x;
}

bool initMetaCfg() {
  if (cfgmeta.percentage != 0) {
    return true;
  }

  cfgmeta.sun_arrow = true;
  cfgmeta.grid_arrow = true;
  cfgmeta.percentage = 80;
  cfgmeta.tstamp = 0xFFFF;
  cfgmeta.is_meta_rendered = false;
  cfgmeta.is_arrows_rendered = false;
  return false; 
}

void deep_sleep(uint32_t delayms) {
    esp_sleep_enable_timer_wakeup(delayms * 1000); // Deep-Sleep time in microseconds
    esp_deep_sleep_start();
}

void draw_meta() {
  if (!cfgmeta.is_meta_rendered) {

    int width = EPD_WIDTH;
    int height = sun_height + 150;

    uint8_t *header = (uint8_t *)ps_calloc(sizeof(uint8_t), width * height);
    if (!header) {
      return;
    }

    int bufw = (width / 2) + (width % 2);
    memset(header, 0xFF, bufw * height);

    Rect_t area = {
      .x = 0,
      .y = 0,
      .width = width,
      .height = height,
    };


    epd_draw_hline(0, 70, width, 0x00, header);
    epd_draw_hline(0, 71, width, 0x00, header);
    epd_draw_hline(0, 72, width, 0xF0, header);

    draw_icon(
      sun_data, EPD_WIDTH / 4 - sun_width / 2 - 50,
      EPD_HEIGHT / 2 - sun_height - 50,
      sun_width,
      sun_height,
      header
    );
    draw_icon(
      house_data, EPD_WIDTH / 2 - house_width / 2,
      EPD_HEIGHT / 2 - house_height - 60,
      house_width,
      house_height,
      header
    );
    draw_icon(
      power_data,
      EPD_WIDTH * 3 / 4 - power_width / 2 + 50,
      EPD_HEIGHT / 2 - power_height - 50,
      power_width,
      power_height,
      header
    );

    int cursor_x = 10;
    int cursor_y = 50;

    char *wifi = NAME;

    epd_poweron();
    epd_clear();
    writeln((GFXfont *)&apple16, wifi, &cursor_x, &cursor_y, header);
    epd_draw_image(area, header, BLACK_ON_WHITE);
    epd_poweroff();
    free(header);
    cfgmeta.is_meta_rendered = true;
  }
}

void draw_icon(const uint8_t * data, uint32_t x, uint32_t y, uint32_t width, uint32_t height, uint8_t *buf) {
  Rect_t area = {
    .x = x,
    .y = y,
    .width = width,
    .height = height,
  };
  epd_copy_to_framebuffer(area, (uint8_t *)data, buf);
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


String fetch_solar() {
  HTTPClient http;
  http.begin("http://" + String(INVERTER_IP) + "/solar_api/v1/GetPowerFlowRealtimeData.fcgi");
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
  uint8_t _percentage = 100;
  if (voltage > 1 ) { // Only display if there is a valid reading
    if (voltage >= 4.20) {
      _percentage = 100;
    } else if (voltage >= 3.75) {
      _percentage = 90;
    } else if (voltage >= 3.65) {
      _percentage = 80;
    } else if (voltage >= 3.60) {
      _percentage = 70;
    } else if (voltage >= 3.55) {
      _percentage = 60;
    } else if (voltage >= 3.50) {
      _percentage = 50;
    } else if (voltage >= 3.45) {
      _percentage = 40;
    } else {
      _percentage = 0;
    }

    if (_percentage == cfgmeta.percentage) {
      return;
    }
    cfgmeta.percentage = _percentage;

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
    String text = String(cfgmeta.percentage) + "%";
    writeln((GFXfont *)&apple16, text.c_str(), &x, &y, NULL);
    epd_poweroff();
    free(buf);
  }
}
