#define TFT_DISPLAY
//#define DRY_RUN

#include <Arduino.h>
#include <BluetoothSerial.h>
#include <ELMduino.h>

#include <math.h>
#include <stdio.h>

#ifdef TFT_DISPLAY
  #include <SPI.h>
  #include <TFT_eSPI.h>
  #include "fonts/PTSansNarrowBold40.h"
  #include "fonts/PTSansNarrowNumbers60.h"
  #include "fonts/PTSansNarrowNumbersBold85.h"
  #define AA_FONT_SMALL PTSansNarrowBold40
  #define AA_FONT_MEDIUM PTSansNarrowNumbers60
  #define AA_FONT_LARGE PTSansNarrowNumbersBold85
#else
  #include <U8g2lib.h>
#endif

#define ELM_PORT SerialBT

// positions in the the byte string returned by ECU containing information
#define POS_RPM 3
#define POS_ecu_engine_temp 4
#define POS_IN_AIR_TEMP 6
#define POS_VOLTAGE 8
#define POS_SPEED 26

#define BLUETOOTH_NAME "ESP32-DISPLAY"  // can be anything
#define BLUETOOTH_PIN "1234"
#define ELM_DEBUG false  // prints all data received from ECU on serial console
#define ELM_QUERY_RESPONSE_TIMEOUT_MS 1000  // time to wait for ELM327 adapter to response to ESP32
#define ELM_ADAPTER_TIMEOUT_MS 200  // time for the ECU to response to ELM327 chip
#define ELM_PROTOCOL ISO_14230_FAST_INIT
#define ELM_SET_HEADER_CMD "AT SH 81 12 F1"  // ISO 14230 Part 2: 81 means physical addressing with length 1 and no additional length byte, the length part will automatically be overwritten, 12 is the adress of the ECU for BMW F/G650, F1 is the address of the ELM adapter
#define ELM_WAKEUP_MSG_CMD "AT WM 81 12 F1 3E"  // unkonwn why header + 3E, maybe BMW specific?
#define ELM_START_COMMUNICATION_CMD "81"  // ISO 14230 Part 2: StartCommunication has the hex value of 81
#define ELM_READ_DATA_CMD "2110"  // ISO 14230 Part 3: "21" is hex value of readDataByLocalIdentifier, "10" is the BMW F/G650 number to return all ECU live data

#define DISPLAY_WIDTH 320
#define DISPLAY_HEIGHT 172

#define LOOP_WAIT_TIME_MS 100  // interval to update data

#define LIGHT_SENSOR_PIN 13  // pin to measure brightness
#define LED_OUTPUT_PIN 14
#define LED_FREQ 5000
#define LED_CHANNEL  0
#define LED_RESOLUTION 8

#define MAX_RPM 8000
#define RPM_50_KMPH_1 6529.  // RPM in 1st gear at 50 km/h to calculate gear 
#define RPM_50_KMPH_2 4155.
#define RPM_50_KMPH_3 3110.
#define RPM_50_KMPH_4 2493.
#define RPM_50_KMPH_5 2089.

uint8_t BLUETOOTH_ADAPTER_ADDRESS[] = {0x00, 0x1D, 0xA5, 0x22, 0x18, 0x32};  // Bluetooth address of ODB2 adapter, here 00:1d:a5:22:18:32. For Windows see https://www.addictivetips.com/windows-tips/find-bluetooth-mac-address-windows-10/


#ifdef TFT_DISPLAY
  #define COLOUR_BACKGROUND TFT_BLACK
  #define COLOUR_MAIN TFT_WHITE
  #define COLOUR_GREEN 0xaff6
  #define COLOUR_YELLOW 0xf7f7
  #define COLOUR_BLUE 0xbfff
  #define COLOUR_RED 0xfdd7

  TFT_eSPI tft = TFT_eSPI(); // Connect SCK to D18 and set TFT_SCLK in User_setup.h to 18, connect SDA to D23 and set TFT_MOSI to 23, connect RES to RES and set TFT_RST to -1, connect RS to D2 and set TFT_DC to D2, connect CS to D21 and set TFT_CS to 21, Connect LDEA to 3V3 
#else
  U8G2_SSD1306_128X64_NONAME_F_SW_I2C u8g2(U8G2_R0, /* clock=*/ 16, /* data=*/ 17, /* reset=*/ U8X8_PIN_NONE);  // SSD1306 128x64 OLED display, connect GND to GND, VIN to 3V3, SCL to G16 and SDA to G17
#endif

ELM327 elm327;
BluetoothSerial SerialBT;

// ECU data
float ecu_voltage = 0.;
uint16_t ecu_rpm = 0;
float ecu_engine_temp = 0;
float ecu_intake_air_temp = 0;
uint8_t ecu_speed = 0;

// variables for control in loop
unsigned long previous_cmd_sent_ms = 0;
bool elm_command_sent = false;




void setup(void) {
   Serial.begin(115200);

  #ifdef TFT_DISPLAY
    // set up dimming of LCD background light 
    ledcSetup(LED_CHANNEL, LED_FREQ, LED_RESOLUTION);
    ledcAttachPin(LED_OUTPUT_PIN, LED_CHANNEL);
    ledcWrite(LED_CHANNEL, 255);  // at start, use full brightness
    
    tft.begin();
    tft.setRotation(3);
    tft.loadFont(AA_FONT_SMALL);
    tft.fillScreen(COLOUR_BACKGROUND);
    tft.setTextColor(COLOUR_MAIN, COLOUR_BACKGROUND, true);
    tft.setCursor(5, 80);
    tft.println("Connecting ...");
    tft.unloadFont();
  #else
    u8g2.begin();
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvB14_tf);
    u8g2.drawStr(0, 30, "Connecting ...");
    u8g2.sendBuffer();
  #endif

  #ifdef DRY_RUN
    delay(3000);
  #else
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
    elm327.sendCommand_Blocking(ELM_SET_HEADER_CMD);
    elm327.sendCommand_Blocking(ELM_WAKEUP_MSG_CMD);
    elm327.sendCommand_Blocking(ELM_START_COMMUNICATION_CMD);  // return "BUS INIT: OK<CR>83F112C1E98FBF<CR><CR>>"  83 F1 12 C1 E9 8F BF
    elm327.sendCommand(ELM_READ_DATA_CMD);
    elm_command_sent = true;
    previous_cmd_sent_ms = millis();
  #endif
  #ifdef TFT_DISPLAY
    tft.fillScreen(COLOUR_BACKGROUND);
  #endif
}

#ifdef TFT_DISPLAY
  void adjust_brightness() {
    static const uint8_t dimmed_lcd_value = 10;  // 0==dark, 255==full brighness
    static const int low_light_threshold  = 50;
    static const int bright_light_threshold  = 400;
    static const uint32_t wait_time_ms = 2000;  // after the light value moves behind the threshold, we also wait to prevent flickering when driving under street lights
    static bool display_in_bright_mode = true;
    static int8_t should_change_if_persistent = 0;  // -1 to change later to low light, 0 for no change, +1 to change to full light
    static uint32_t change_status_set_ms = 0;
    int light_sensor = analogRead(LIGHT_SENSOR_PIN);
    
    if (display_in_bright_mode && light_sensor < low_light_threshold) {
      if (should_change_if_persistent == 1) {
        if((millis() - change_status_set_ms) > wait_time_ms) {
          should_change_if_persistent = 0;
          display_in_bright_mode = false;
          ledcWrite(LED_CHANNEL, dimmed_lcd_value);
        }
      } else {
        should_change_if_persistent = 1;
        change_status_set_ms = millis();
      }
    } else if (!display_in_bright_mode && light_sensor > bright_light_threshold) {
      if (should_change_if_persistent == -1) {
        if((millis() - change_status_set_ms) > wait_time_ms) {
          should_change_if_persistent = 0;
          display_in_bright_mode = true;
          ledcWrite(LED_CHANNEL, 255);
        }
      } else {
        should_change_if_persistent = -1;
        change_status_set_ms = millis();
      }
    }
  }
#endif

uint8_t get_gear() {
  static const float gear_ratios[] =  {RPM_50_KMPH_1 / 50., RPM_50_KMPH_2 / 50., RPM_50_KMPH_3 / 50., RPM_50_KMPH_4 / 50., RPM_50_KMPH_5 / 50.};
  
  if (ecu_speed == 0 || ecu_rpm == 0) {
    return 0;
  }
  float current_ratio = (float)ecu_rpm / (float)(ecu_speed);
  uint8_t best_gear = 1;
  float best_diff = 1000.;
  for (size_t i=0; i<sizeof(gear_ratios) / sizeof(gear_ratios[0]); i++) {
    float diff = fabs(current_ratio - gear_ratios[i]);
    if (diff < best_diff) {
      best_gear = i + 1;
      best_diff = diff;
    }
  }
  return best_gear;
}


void update_display() {
  //ecu_rpm = analogRead(LIGHT_SENSOR_PIN);  // debug
  unsigned int rpm_bar_length = ecu_rpm * DISPLAY_HEIGHT / MAX_RPM;
  static char gear_str[2];
  uint8_t gear = get_gear();
  if (gear == 0) {
    gear_str[0] = '\n';
  } else {
    snprintf(gear_str, 2, "%u", gear);
  }

  #ifdef TFT_DISPLAY
    static const uint8_t rpm_bar_padding = 0;
    static const uint8_t first_line_y = 0;
    static const uint8_t second_line_y = 113;
    static const uint8_t rpm_line_y = 27;
    static const uint8_t speed_line_y = DISPLAY_HEIGHT - 93;
    static const uint8_t rpm_bar_thickness = 40;  // must be even
    static const float delta_rpm_bar = -255.0 / DISPLAY_HEIGHT;
    static const uint16_t colour1_rpm_bar = COLOUR_BLUE;
    static const uint16_t colour2_rpm_bar = TFT_RED;
    static const uint8_t sep_width = DISPLAY_HEIGHT / ceil(MAX_RPM / 2000.);

    // RPM bar
    float alpha_rpm_bar = 255.;
    uint16_t colour = colour1_rpm_bar;
    static const uint8_t radius = rpm_bar_thickness / 2;
    uint8_t current_width;
    uint8_t current_x;
    for (size_t i=0; i<rpm_bar_length; i++) {
      if (current_width < rpm_bar_thickness) {
        current_width = round(sqrt(radius * radius - (radius - i) * (radius - i)) * 2);
        current_x = (rpm_bar_thickness - current_width) / 2 + rpm_bar_padding;
      } else {
        current_width = rpm_bar_thickness;
        current_x = rpm_bar_padding;
      }
      uint16_t y = DISPLAY_HEIGHT - i;
      tft.drawFastHLine(current_x, y, current_width, colour);
      uint8_t at_sep_pos = i % (sep_width - 1);  // seperator lines
      alpha_rpm_bar += delta_rpm_bar;
      if (i > 1 && (at_sep_pos == 0 || at_sep_pos == 1)) {
        colour = COLOUR_BACKGROUND;
      } else {
        colour = tft.alphaBlend((uint8_t)alpha_rpm_bar, colour1_rpm_bar, colour2_rpm_bar);
      }
    }
    tft.fillRect(rpm_bar_padding, 0, rpm_bar_thickness, DISPLAY_HEIGHT - rpm_bar_length, COLOUR_BACKGROUND);
    
    tft.loadFont(AA_FONT_SMALL);

    // voltage
    if (ecu_voltage > 14.5 || ecu_voltage < 12) {
      tft.setTextColor(COLOUR_RED, COLOUR_BACKGROUND, true);
    } else if (ecu_voltage < 13) {
      tft.setTextColor(COLOUR_YELLOW, COLOUR_BACKGROUND, true);
    } else {
      tft.setTextColor(COLOUR_GREEN, COLOUR_BACKGROUND, true);
    }
    tft.setTextPadding(tft.textWidth("12.0"));
    tft.setTextDatum(TR_DATUM);
    static const uint16_t distance_from_bar = 3;
    tft.drawFloat(ecu_voltage, 1, 68 + rpm_bar_padding + rpm_bar_thickness + distance_from_bar, first_line_y);
    tft.setTextPadding(0);

    tft.setTextDatum(TL_DATUM);
    tft.drawString("V", 68 + rpm_bar_padding + rpm_bar_thickness + distance_from_bar, first_line_y);
    
    // intake temp
    if (ecu_intake_air_temp < 6) {
      tft.setTextColor(COLOUR_BLUE, COLOUR_BACKGROUND, true);
    } else {
      tft.setTextColor(COLOUR_YELLOW, COLOUR_BACKGROUND, true);
    }
    tft.setTextPadding(tft.textWidth("105"));
    tft.setTextDatum(TR_DATUM);
    tft.drawFloat(ecu_intake_air_temp, 0, 142 + rpm_bar_padding + rpm_bar_thickness + distance_from_bar, first_line_y);
    tft.setTextPadding(0);
    
    tft.setTextDatum(TR_DATUM);
    tft.drawString("°C", 174  + rpm_bar_padding + rpm_bar_thickness + distance_from_bar, first_line_y);

    // engine temp
    if (ecu_engine_temp < 75) {
      tft.setTextColor(COLOUR_BLUE, COLOUR_BACKGROUND, true);
    } else if (ecu_engine_temp < 105) {
      tft.setTextColor(COLOUR_GREEN, COLOUR_BACKGROUND, true);
    } else {
      tft.setTextColor(COLOUR_RED, COLOUR_BACKGROUND, true);
    }
    tft.setTextPadding(tft.textWidth("105"));
    tft.setTextDatum(TR_DATUM);
    tft.drawFloat(ecu_engine_temp, 0, 231 + rpm_bar_padding + rpm_bar_thickness + distance_from_bar, first_line_y);
    tft.setTextPadding(0);
    
    tft.setTextDatum(TR_DATUM);
    tft.drawString("°C", DISPLAY_WIDTH - 7, first_line_y);

    // rpm
    tft.unloadFont();
    tft.loadFont(AA_FONT_MEDIUM);
    
    tft.setTextColor(COLOUR_MAIN, COLOUR_BACKGROUND, false);
    tft.setTextDatum(TR_DATUM);
    tft.setTextPadding(tft.textWidth("4444"));
//    tft.fillRect(rpm_bar_padding + rpm_bar_thickness + distance_from_bar, rpm_line_y+7, DISPLAY_WIDTH, 75, TFT_NAVY);
    tft.drawNumber(ecu_rpm, 120 + rpm_bar_padding + rpm_bar_thickness + distance_from_bar, rpm_line_y, 172);
    
    tft.setTextColor(COLOUR_BLUE, COLOUR_BACKGROUND, false);

    // fixes problem that old figures are not overwritten because "1" is less height than 0
//    tft.drawFastHLine(0, 119, DISPLAY_WIDTH, COLOUR_BACKGROUND);
    
    // speed
    tft.unloadFont();
    tft.loadFont(AA_FONT_LARGE);
    tft.setTextDatum(TR_DATUM);
    tft.setTextPadding(tft.textWidth("100"));
    tft.drawNumber(ecu_speed, 285, speed_line_y);

    // gear
    tft.setTextColor(COLOUR_MAIN, COLOUR_BACKGROUND, false);
    tft.setTextDatum(TR_DATUM);
    tft.setTextPadding(tft.textWidth("4"));
    tft.drawString(gear_str, DISPLAY_WIDTH, rpm_line_y);
    
    tft.unloadFont();

  #else
    static char voltage_str[30], rpm_str[5], speed_str[5];
    snprintf(voltage_str, 30, "%4.1fV %3.0f° %3.0f°", ecu_voltage, ecu_intake_air_temp, ecu_engine_temp);
    snprintf(speed_str, 4, "%3u", ecu_speed);
    
    snprintf(rpm_str, 5, "%3.1f", ecu_rpm/1000.);
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvB14_tf);
    u8g2.drawUTF8(0, 14, voltage_str);
    u8g2.drawBox(0, 17, rpm_bar_thickness, 6);
  
    u8g2.setDrawColor(0);
    for (size_t i=31; i<rpm_bar_length; i+=32) {
      u8g2.drawBox(i, 17, 2, 6);
    }
    u8g2.setDrawColor(1);
  
    u8g2.setFont(u8g2_font_logisoso34_tn );
    u8g2.drawStr(0, 64, gear_str);
  
    u8g2.setFont(u8g2_font_logisoso22_tn );
    u8g2.drawStr(24, 58, rpm_str);
  
    u8g2.setFont(u8g2_font_logisoso38_tn );
    u8g2.drawStr(58 ,64, speed_str);
    u8g2.sendBuffer();
  #endif
}

void parse_payload() {
  uint8_t voltage_cod = 0;
  uint8_t rpm_cod = 0;
  uint8_t ecu_engine_temp_cod = 0;
  uint8_t in_air_temp_cod = 0;
  uint8_t speed_cod = 0;

  // convert hex bytes to integer
  sscanf(elm327.payload + POS_RPM * 2, "%2hhx", &rpm_cod);
  sscanf(elm327.payload + POS_ecu_engine_temp * 2, "%2hhx", &ecu_engine_temp_cod);
  sscanf(elm327.payload + POS_IN_AIR_TEMP * 2, "%2hhx", &in_air_temp_cod);
  sscanf(elm327.payload + POS_VOLTAGE * 2, "%2hhx", &voltage_cod);
  sscanf(elm327.payload + POS_SPEED * 2, "%2hhx", &speed_cod);
  
  // convert with values found empirical
  ecu_rpm = rpm_cod * 50;
  ecu_engine_temp = ecu_engine_temp_cod * 0.748 - 47.7;
  ecu_intake_air_temp = in_air_temp_cod * 0.748 - 47.7;
  ecu_voltage = voltage_cod * 0.091;
  ecu_speed = speed_cod * 1.04;  // ECU is calibrated for Xcountry 130/80 17 instead Xchallenge 140/80 18
}

void print_elm_error() {
  static const char* errors[] = {"GENERAL ERROR", "NO RESPONSE", "BUFFER OVERFLOW", "GARBAGE", "UNABLE TO CONNECT", "NO DATA", "STOPPED", "TIMEOUT", "GETTING MSG", "MSG RXD"};
  static const size_t error_str_len = 100;
  static char error_str[error_str_len] = "";
  static const char restarting_str[] = "Restarting ...";
  size_t error_idx = elm327.nb_rx_state + 1;

  Serial.print("ELM327 error: ");
  if (error_idx < sizeof(errors) / sizeof(errors[0])) {
    snprintf(error_str, error_str_len, "Error %d: %s", elm327.nb_rx_state, errors[error_idx]);
    Serial.println(error_str);
  } else {
    error_str[0] = '\n';
  }
  #ifdef TFT_DISPLAY
    tft.fillScreen(COLOUR_BACKGROUND);
    tft.setCursor(0, 15);
    tft.loadFont(AA_FONT_SMALL);
    tft.setTextColor(TFT_WHITE, COLOUR_BACKGROUND);
    tft.print(restarting_str);
//    tft.setTextColor(TFT_RED, COLOUR_BACKGROUND);
//    tft.setCursor(0, 60);
//    tft.print(error_str);
  #else
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_helvB14_tf);
    u8g2.drawStr(0, 14, restarting_str);
//    u8g2.drawStr(0,64, error_str);
    u8g2.sendBuffer();
  #endif
}

void loop(void) {


































































  
  #ifdef TFT_DISPLAY
    adjust_brightness();
  #endif
  #ifdef DRY_RUN
    if (millis() - previous_cmd_sent_ms >= LOOP_WAIT_TIME_MS) {
      if (ecu_rpm + 200 < MAX_RPM) {
        ecu_rpm += 189;
      } else{
        ecu_rpm = 1956;
      }
      if (ecu_engine_temp + 1 < 121) {
        ecu_engine_temp += 1;
      } else{
        ecu_engine_temp = -20;
      }
      if (ecu_intake_air_temp + 1 < 45) {
        ecu_intake_air_temp += 1;
      } else{
        ecu_intake_air_temp = -20;
      }
      if (ecu_voltage < 15) {
        ecu_voltage += 0.1;
      } else{
        ecu_voltage = 10.;
      }
      if (ecu_speed < 180) {
        ecu_speed += 1;
      } else{
        ecu_speed = 0;
      }
      update_display();
      previous_cmd_sent_ms = millis();
    }
  #else
    if (elm_command_sent) {
      if (elm327.get_response() == ELM_SUCCESS) {
        parse_payload();
        update_display();
        elm_command_sent = false;
      } else if (elm327.nb_rx_state != ELM_GETTING_MSG) {
        print_elm_error();
        elm327.sendCommand_Blocking("AT WS");  // restart adapter
        ESP.restart();
      }
    } else if (millis() - previous_cmd_sent_ms >= LOOP_WAIT_TIME_MS) {
      elm327.sendCommand(ELM_READ_DATA_CMD);
      elm_command_sent = true;
      previous_cmd_sent_ms = millis();
    }
  #endif
}
