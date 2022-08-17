#include <Arduino.h>
#include <BluetoothSerial.h>
#include <ELMduino.h>
#include <math.h>
#include <U8g2lib.h>
#include <stdio.h>

#define ELM_PORT SerialBT
#define POS_RPM 2
#define POS_ENGINE_TEMP 4
#define POS_IN_AIR_TEMP 6
#define POS_VOLTAGE 8
#define POS_SPEED 26

#define BLUETOOTH_NAME "ESP32-DISPLAY"
#define BLUETOOTH_PIN "1234"
#define ELM_DEBUG false
#define ELM_QUERY_RESPONSE_TIMEOUT_MS 1000
#define ELM_ADAPTER_TIMEOUT_MS 200
#define ELM_PROTOCOL ISO_14230_FAST_INIT
#define ELM_SET_HEADER_CMD "AT SH 81 12 F1"
#define ELM_WAKEUP_MSG_CMD "AT WM 81 12 F1 3E"
#define ELM_INIT_BUS_CMD "81"
#define ELM_READ_DATA_CMD "2110"
#define MAX_RPM 8000
#define DISPLAY_WIDTH 128
#define WAIT_ELM_MS 100

#define RPM_50_KMPH_1 6529.
#define RPM_50_KMPH_2 4155.
#define RPM_50_KMPH_3 3110.
#define RPM_50_KMPH_4 2493.
#define RPM_50_KMPH_5 2089.

uint8_t BLUETOOTH_ADAPTER_ADDRESS[] = {0x00, 0x1D, 0xA5, 0x22, 0x18, 0x32};  // Bluetooth address of ODB2 adapter, here 00:1d:a5:22:18:32. For Windows see https://www.addictivetips.com/windows-tips/find-bluetooth-mac-address-windows-10/


U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/ 16, /* data=*/ 17, /* reset=*/ U8X8_PIN_NONE);  // SSD1306 128x64 OLED display, connect GND to GND, VIN to 3V3, SCL to G16 and SDA to G17
ELM327 elm327;
BluetoothSerial SerialBT;

// ECU data
uint8_t voltage_cod = 0;
float voltage_real = 0.;
uint8_t rpm_cod = 0;
unsigned int rpm_real = 0;
uint8_t engine_temp_cod = 0;
float engine_temp_real = 0;
uint8_t in_air_temp_cod = 0;
float in_air_temp_real = 0;
uint8_t speed_cod = 0;
unsigned int speed_real = 0;
const float gear_ratios[] =  {RPM_50_KMPH_1 / 50., RPM_50_KMPH_2 / 50., RPM_50_KMPH_3 / 50., RPM_50_KMPH_4 / 50., RPM_50_KMPH_5 / 50.};
uint8_t gear = 0;

// variables for control in loop
unsigned long previous_elm_ms = 0;
bool elm_command_sent = false;

void setup(void) {
  Serial.begin(115200);
  
  u8g2.begin();
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB14_tf);
  u8g2.drawStr(0, 39, "Connecting ...");
  u8g2.sendBuffer();

  
  ELM_PORT.begin(BLUETOOTH_NAME, true);
  ELM_PORT.setPin(BLUETOOTH_PIN);
  
  bool connected = ELM_PORT.connect(BLUETOOTH_ADAPTER_ADDRESS);
  if(connected) {
    Serial.println("Connected Succesfully!");
  } else {
    while(!ELM_PORT.connected(10000)) {
      Serial.println("Failed to connect. Make sure remote device is available and in range");
      ESP.restart();
    }
  }
  elm327.begin(ELM_PORT, ELM_DEBUG, ELM_QUERY_RESPONSE_TIMEOUT_MS, ELM_PROTOCOL, ELM_ADAPTER_TIMEOUT_MS);

  // same values used by Motoscan (https://www.motoscan.de)
  elm327.sendCommand_Blocking(ELM_SET_HEADER_CMD);
  elm327.sendCommand_Blocking(ELM_WAKEUP_MSG_CMD);
  elm327.sendCommand_Blocking(ELM_INIT_BUS_CMD);
  elm327.sendCommand(ELM_READ_DATA_CMD);
  elm_command_sent = true;
}

unsigned int get_rpm_bar_length() {
  return rpm_real * DISPLAY_WIDTH / MAX_RPM;
}

void calc_gear() {
  if (speed_real == 0 || rpm_real == 0) {
    gear = 0;
  } else {
    float current_ratio = (float)rpm_real / (float)(speed_real)
    uint8_t best_gear = 1;
    float best_diff = 1000;
    for (size_t i=0; i<sizeof(gear_ratios) / sizeof(gear_ratios[0]); i++) {
      float diff = fabs(current_ratio - gear_ratios[i]);
      if (diff < best_diff) {
        best_gear = i + 1;
        best_diff = diff;
      }
    }
    gear = best_gear;
  }
}

void updated_display() {
  static char voltage_str[30], rpm_str[5], speed_str[5];
  snprintf(voltage_str, 30, "%4.1fV %3.0f° %3.0f°", voltage_real, in_air_temp_real, engine_temp_real);
  snprintf(rpm_str, 5, "%4u", rpm_real);
  snprintf(speed_str, 4, "%3u", speed_real);
  
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB14_tf);
  u8g2.drawUTF8(0, 14, voltage_str);
  u8g2.drawBox(0, 17, get_rpm_bar_length(), 7);
  u8g2.setFont(u8g2_font_logisoso24_tn );
  u8g2.drawStr(0,64, rpm_str);
  u8g2.setFont(u8g2_font_logisoso38_tn );
  u8g2.drawStr(58 ,64, speed_str);
  u8g2.sendBuffer();
}

void parse_payload() {
  // convert hex bytes to integer
  sscanf(elm327.payload + POS_RPM * 2, "%2hhx", &rpm_cod);
  sscanf(elm327.payload + POS_ENGINE_TEMP * 2, "%2hhx", &engine_temp_cod);
  sscanf(elm327.payload + POS_IN_AIR_TEMP * 2, "%2hhx", &in_air_temp_cod);
  sscanf(elm327.payload + POS_VOLTAGE * 2, "%2hhx", &voltage_cod);
  sscanf(elm327.payload + POS_SPEED * 2, "%2hhx", &speed_cod);
  
  // convert with values found empirical
  rpm_real = rpm_cod * 10;
  engine_temp_real = engine_temp_cod * 0.748 - 47.7;
  in_air_temp_real = in_air_temp_cod * 0.748 - 47.7;
  voltage_real = voltage_cod * 0.094;
  speed_real = speed_cod;
  calc_gear();
}

void print_elm_error() {
  static const char* errors[] = {"GENERAL ERROR", "NO RESPONSE", "BUFFER OVERFLOW", "GARBAGE", "UNABLE TO CONNECT", "NO DATA", "STOPPED", "TIMEOUT", "GETTING MSG", "MSG RXD"};
  static const size_t error_str_len = 100;
  static char error_str[error_str_len] = "";
  size_t error_idx = elm327.nb_rx_state + 1;

  Serial.print("ELM327 error: ");
  
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_helvB14_tf);
  u8g2.drawStr(0, 14, "Restarting ...");
  
  if (error_idx < sizeof(errors) / sizeof(errors[0])) {
    snprintf(error_str, error_str_len, "Error %d: %s", elm327.nb_rx_state, errors[error_idx]);
    Serial.println(error_str);
    u8g2.drawStr(0,64, error_str);
  }
  u8g2.sendBuffer();
}

void loop(void) {
  if (elm_command_sent) {
    if (elm327.get_response() == ELM_SUCCESS) {
      parse_payload();
      updated_display();
      elm_command_sent = false;
    } else if (elm327.nb_rx_state != ELM_GETTING_MSG) {
      print_elm_error();
      elm327.sendCommand_Blocking("AT WS");  // restart adapter
      delay(10000);  // show message 10s before restart
      ESP.restart();
    }
  } else if (millis() - previous_elm_ms >= WAIT_ELM_MS) {
    elm327.sendCommand(ELM_READ_DATA_CMD);
    elm_command_sent = true;
    previous_elm_ms = millis();
  }
}
