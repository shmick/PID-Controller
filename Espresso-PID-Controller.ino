/*
  Control an espresso machine boiler using a PID controller

  Code samples used from the following:
  PID Library: https://github.com/br3ttb/Arduino-PID-Library
  PID Lab: https://www.pdx.edu/nanogroup/sites/www.pdx.edu.nanogroup/files/2013_Arduino%20PID%20Lab_0.pdf
  Smoothing: https://www.arduino.cc/en/Tutorial/Smoothing

  Hardware:
  Wemos D1 Mini ( https://www.wemos.cc/en/latest/d1/d1_mini.html )
  128 x 64 OLED Display using Adafruit_SSD1306 library
  AD8495 Thermocouple Amplifier ( https://www.adafruit.com/product/1778 )
  ADS1115 16-Bit ADC 
  Solid State Relay ( Crydom TD1225 )
*/

#include <PID_v1.h>

// Needed for I2C
#include <Wire.h>

// set to 1 if using the OLED display. Disabled by default.
#define OLED_DISPLAY 0

#if OLED_DISPLAY == 1
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Fonts/FreeSerifBold18pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#endif

// Needed for IotWebConf https://github.com/prampec/IotWebConf
#include <DNSServer.h>
#include <IotWebConf.h>

// Needed for ADS1115 ADC - https://github.com/baruch/ADS1115
#include "src/ADS1115/ADS1115.h"

// Needed to debounce the steam switch https://github.com/JChristensen/JC_Button
#include <JC_Button.h>

// *****************************************
// * Config options that you can customize *
// *****************************************

// -- Initial name of the Thing. Used e.g. as SSID of the own Access Point.
const char thingName[] = "espresso";
// -- Initial password to connect to the Thing, when it creates an own Access Point.
const char wifiInitialApPassword[] = "espresso";

#define RelayPin D2
#define SteamPin D4

// Default to being ON
bool operMode = true;

// After powering on, how many minutes until we force the boiler to power down
const int maxRunTime = 180;

// Max time in seconds that the steam switch can be toggled from on to off to reset the operMode state to true
const int steamReset = 3;

// Max number of minutes we can remain in steam mode
const int steamMaxMins = 10;

// Turn the display off after 200 minutes
const int maxDisplayMins = 200;

// Define the PID setpoints
double Setpoint = 105; // This will be the default coffee setpoint

const double CoffeeSetpoint = Setpoint;
const double SteamSetpoint = 125;

// Define the PID tuning Parameters
double Kp = 4.5;
double Ki = 0.125;
double Kd = 0.2;

// PWM Window in milliseconds
const int WindowSize = 5000;

// ***********************************************************
// ***********************************************************
// * There should be no need to tweak many things below here *
// ***********************************************************
// ***********************************************************

const int ADSGAIN = 2;

// The AD8495 board uses a 1.25v voltage regulator for Vref
const double Vref = 1.25;

// PID variables
// Using P_ON_M mode ( http://brettbeauregard.com/blog/2017/06/introducing-proportional-on-measurement/ )
double Input, Output;
PID myPID(&Input, &Output, &Setpoint, Kp, Ki, Kd, P_ON_M, DIRECT);
double PWMOutput;
unsigned long windowStartTime;

// Define the info needed for the temperature averaging
const int numReadings = 8;
int readings[numReadings]; // the readings from the analog input
int readIndex = 0;         // the index of the current reading
int total = 0;             // the running total
int average = 0;           // the average

// Thermocouple variables
float Vout; // The voltage coming from the out pin on the TC amp
float Vtc;  // Placeholder for getting AD8495 value ready to perform the NIST calc

// Corrected temperature readings for a K-type thermocouple
// https://srdata.nist.gov/its90/type_k/kcoefficients_inverse.html
// Coefficient values for 0C - 500C / 0mV - 20.644mV
const double c0 = 0.000000E+00;
const double c1 = 2.508355E+01;
const double c2 = 7.860106E-02;
const double c3 = -2.503131E-01;
const double c4 = 8.315270E-02;
const double c5 = -1.228034E-02;
const double c6 = 9.804036E-04;
const double c7 = -4.413030E-05;
const double c8 = 1.057734E-06;
const double c9 = -1.052755E-08;

// IotWebConf
DNSServer dnsServer;
WebServer server(80);
HTTPUpdateServer httpUpdater;
IotWebConf iotWebConf(thingName, &dnsServer, &server, wifiInitialApPassword);

// Variable to store the HTTP request
String header;

// All timers reference the value of now
unsigned long now = millis(); //This variable is used to keep track of time

// OLED display timer
const int OLEDinterval = 250;           // interval at which to write new data to the OLED
unsigned long previousOLEDMillis = now; // will store last time OLED was updated

// Serial output timer
const int serialPing = 500;      //This determines how often we ping our loop
unsigned long lastMessage = now; //This keeps track of when our loop last spoke to serial

int runTimeMins;
long runTimeSecs;
unsigned long runTimeStart = now;

// Temp read interval
const int TempInterval = 50;
unsigned long currentTempMillis;
unsigned long previousTempMillis = now;

// Server tasks interval
const int serverInterval = 50;
unsigned long currentServerMillis;
unsigned long previousServerMillis = now;

// Reset the operation mode to true by toggling the steam switch for less than 3 secconds
bool steamMode = false;
bool steamTimer = false;
unsigned long steamTimeStart;
unsigned long steamTimeMillis;
Button steamsw(SteamPin, 100); // 100ms debounce for the steam switch

// Setup I2C pins
#define ESP_SCL D5
#define ESP_SDA D6

#if OLED_DISPLAY == 1
// OLED Display setup
#define OLED_RESET 16
#define OLED_I2C 0x3C
Adafruit_SSD1306 display(OLED_RESET);
#if (SSD1306_LCDHEIGHT != 64)
#error("Height incorrect, please fix Adafruit_SSD1306.h");
// You will need to modify the Adafruit_SSD1306.h file
// Step 1: uncomment this line: #define SSD1306_128_64
// Step 2: add a comment to this line: #define SSD1306_128_32
#endif
#endif

// ADS1115 ADC
float adcval;
float ADS_PGA;
ADS1115 adc;

unsigned long prevLoopMillis;
int numLoops = 0;
int currLoops = 0;

void keepTime(void)
{
  //Keep track of time
  now = millis();
  runTimeSecs = (now - runTimeStart) / 1000;
  runTimeMins = (now - runTimeStart) / 60000;
}

int readADC()
{
  static int read_triggered = 0;
  if (!read_triggered)
  {
    if (adc.trigger_sample() == 0)
      read_triggered = 1;
  }
  else
  {
    if (!adc.is_sample_in_progress())
    {
      adcval = adc.read_sample();
      read_triggered = 0;
    }
  }
  return adcval;
}

void readTemps(void)
{
  currentTempMillis = now;
  if (currentTempMillis - previousTempMillis > TempInterval)
  {

    // subtract the last reading:
    total = total - readings[readIndex];
    // Read the temps from the thermocouple
    readings[readIndex] = readADC();
    // add the reading to the total:
    total = total + readings[readIndex];
    // advance to the next position in the array:
    readIndex = readIndex + 1;
    // if we're at the end of the array...
    if (readIndex >= numReadings)
    {
      // ...wrap around to the beginning:
      readIndex = 0;
    }

    // calculate the average:
    average = total / numReadings;

    // This should match the output voltage on the Out pin of the AD8945
    Vout = average * ADS_PGA / 1000;

    // Based on Analog Devices AN-1087
    // Convert the AD8495 output back to millivolts so we can perform the NIST calc
    Vtc = ((Vout * 1000) - (Vref * 1000) - 1.25) / 122.4;

    // Use the NIST corrected temperature readings for a K-type thermocouple in the 0-500°C range
    // https://srdata.nist.gov/its90/type_k/kcoefficients_inverse.html
    Input = c0 +
            c1 * Vtc +
            c2 * pow(Vtc, 2) +
            c3 * pow(Vtc, 3) +
            c4 * pow(Vtc, 4) +
            c5 * pow(Vtc, 5) +
            c6 * pow(Vtc, 6) +
            c7 * pow(Vtc, 7) +
            c8 * pow(Vtc, 8) +
            c9 * pow(Vtc, 9);

    previousTempMillis = currentTempMillis;
  }
}

// unless true is specified, default to false
bool enablePID(bool enable = false)
{
  if (enable == false)
  {
    // de-activate the relay
    digitalWrite(RelayPin, LOW);
    // set PID mode to manual mode
    myPID.SetMode(MANUAL);
    // Force PID output to 0
    Output = 0;
    // set operation mode to false
    operMode = false;
  }
  else if (enable == true)
  {
    myPID.SetMode(AUTOMATIC);
    runTimeStart = now;
    operMode = true;
  }
}

void relayControl(void)
{
  // If we've reached maxRunTime, disable the PID control
  if (runTimeMins >= maxRunTime && operMode)
  {
    enablePID(false);
  }
  // Set the PID back to Automatic mode if operMode is true
  else if (!myPID.GetMode() && operMode)
  {
    enablePID(true);
  }

  if (operMode)
  {
    myPID.Compute();

    // Starts a new PWM cycle every WindowSize milliseconds
    if (now - windowStartTime > WindowSize)
    { //time to shift the Relay Window
      windowStartTime += WindowSize;
    }

    // Calculate the number of milliseconds that have passed in the current PWM cycle.
    // If that is less than the Output value, the relay is turned ON
    // If that is greater than (or equal to) the Output value, the relay is turned OFF.
    PWMOutput = Output * (WindowSize / 100.00);
    if ((PWMOutput > 100) && (PWMOutput > (now - windowStartTime)))
    {
      digitalWrite(RelayPin, HIGH);
    }
    else
    {
      digitalWrite(RelayPin, LOW);
    }
  }
}

void steamSwitch()
{
  steamsw.read(); // read the steam switch

  if (steamsw.isPressed() && !steamMode) // if switch is on, set steamMode to true
  {
    steamMode = true;
  }
  else if (!steamsw.isPressed() && steamMode)
  {
    steamMode = false;
  }

  // set the steamTimer value only when the steamMode changes state
  if (steamMode && !steamTimer)
  {
    steamTimer = true;
    steamTimeStart = now;
  }
  else if (!steamMode && steamTimer)
  {
    steamTimer = false;
  }

  // If steamTimer is true, update steamTimeMillis
  if (steamTimer)
  {
    steamTimeMillis = now - steamTimeStart;
  }

  // if steamMode is now false, check to see if we should set operMode to true
  if (!steamMode && steamTimeMillis > 0)
  {
    if (steamTimeMillis / 1000 <= steamReset)
    {
      steamTimeMillis = 0;
      enablePID(true);
    }
  }

  // This must be the last if statement. It's a safety check to ensure
  // that we set enablePID to false if the steam switch has been on too long
  if (steamTimeMillis / 1000 / 60 >= steamMaxMins)
  {
    enablePID(false);
  }
  else if (steamMode && operMode && Setpoint != SteamSetpoint)
  {
    Setpoint = SteamSetpoint;
  }
  else if (!steamMode && operMode && Setpoint != CoffeeSetpoint)
  {
    Setpoint = CoffeeSetpoint;
  }
}

// Track how many loops per second are executed.
void trackloop()
{
  if (now - prevLoopMillis >= 1000)
  {
    currLoops = numLoops;
    numLoops = 0;
    prevLoopMillis = now;
  }
  numLoops++;
}

#if OLED_DISPLAY == 1
void displayOLED(void)
{
  unsigned long currentOLEDMillis = now;

  if (currentOLEDMillis - previousOLEDMillis > OLEDinterval)
  {
    // save the last time you wrote to the OLED display
    previousOLEDMillis = currentOLEDMillis;

    // have to wipe the buffer before writing anything new
    display.clearDisplay();

    if (operMode)
    {
      // TOP HALF = Temp + Input Temp
      display.setFont(&FreeSans9pt7b);
      display.setCursor(0, 22);
      display.print("Temp");

      display.setFont(&FreeSerifBold18pt7b);
      display.setCursor(48, 26);
      if (Input >= 100)
        display.print(Input, 1);
      else
        display.print(Input);

      // BOTTOM HALF = Output + Output Percent
      display.setFont(&FreeSans9pt7b);
      display.setCursor(0, 56);
      display.print("Output");

      display.setFont(&FreeSerifBold18pt7b);
      display.setCursor(60, 60);
      // Dont add a decimal place for 100 or 0
      if ((Output >= 100.0) || (Output == 0.0))
      {
        display.print(Output, 0);
      }
      else if (Output < 10)
      {
        display.print(Output, 2);
      }
      else
      {
        display.print(Output, 1);
      }
    }
    else if (!operMode)
    {
      // After maxDisplayMins minutes turn off the display
      if (runTimeMins >= maxDisplayMins)
      {
        display.clearDisplay();
      }
      else
      {
        display.setFont(&FreeSerifBold18pt7b);
        display.setCursor(28, 28);
        display.print("OFF");
      }
    }
    // Do the needful!
    display.display();
  }
}
#endif

void displaySerial(void)
{
  // Output some data to serial to see what's happening
  if (now - lastMessage > serialPing)
  {
    if (operMode)
    {
      Serial.print("Time: ");
      Serial.println(runTimeSecs);
    }
    else
    {
      Serial.print("Input: ");
      Serial.print(Input, 1);
      Serial.print(", ");
      Serial.print("Mode: off");
      Serial.print("\n");
    }
    lastMessage = now; //update the time stamp.
  }
}

// ESP8266WebServer handler
void handleRoot()
{
  // -- Let IotWebConf test and handle captive portal requests.
  if (iotWebConf.handleCaptivePortal())
  {
    // -- Captive portal request were already served.
    return;
  }
  String s = "<!DOCTYPE html><html lang=\"en\"><head><meta name=\"viewport\" content=\"width=device-width, initial-scale=1, user-scalable=no\"/>";
  s += "<title>Espresso PID Controller</title></head><body>Espresso PID Controller";
  s += "<br>\n";
  s += "Go to <a href='stats'>stats page</a> for controller stats.";
  s += "<br>\n";
  s += "Go to <a href='config'>configure page</a> to change settings.";
  s += "</body></html>\n";

  server.send(200, "text/html", s);
}

// ESP8266WebServer handler
void handleStats()
{
  String br = "<br>\n";
  String m = "<head> <meta http-equiv=\"refresh\" content=\"2\"> </head>\n";
  m += "<h1>\n";
  m += "Time: " + String(runTimeSecs) + br;
  m += "Setpoint: " + String(Setpoint) + br;
  m += "PID Input:  " + String(Input) + br;
  m += "PID Output: " + String(Output) + br;
  m += "Avg: " + String(average) + br;
  m += "Vout: " + String(Vout) + br;
  m += "Loops: " + String(currLoops) + br;
  m += "OperMode: " + String(operMode) + br;
  m += "SteamMode: " + String(steamMode);
  server.send(200, "text/html", m);
}

// ESP8266WebServer handler
void handleJSON()
{
  // Quick and dirty JSON output without the use of ArduinoJson
  String c = ", ";
  String m = "{ ";
  m += "\"Uptime\":" + String(now / 1000) + c;
  m += "\"Runtime\":" + String(runTimeSecs) + c;
  m += "\"Setpoint\":" + String(Setpoint) + c;
  m += "\"Input\":" + String(Input) + c;
  m += "\"Output\":" + String(Output) + c;
  m += "\"ADC\":" + String(average) + c;
  m += "\"Vout\":" + String(Vout) + c;
  m += "\"Mode\":" + String(operMode) + c;
  m += "\"Loops\":" + String(currLoops) + c;
  m += "\"steamMode\":" + String(steamMode);
  m += " }";
  server.send(200, "application/json", m);
}

// ESP8266WebServer handler
void handleSetvals()
{
  String message;

  String opmode_val = server.arg("opmode");
  String sp_val = server.arg("sp");

  if (opmode_val == "off")
  {
    enablePID(false);
    message += "opermode: " + opmode_val;
    message += "\n";
  }
  else if (opmode_val == "on")
  {
    enablePID(true);
    message += "opermode: " + opmode_val;
    message += "\n";
  }

  if (sp_val != NULL)
  {
    double sp_int = sp_val.toFloat();
    if (sp_int <= 105.11 || sp_int > 0.1)
    {
      Setpoint = sp_int;
      message += "Setpoint: " + sp_val;
      message += "\n";
    }
    else
    {
      message += "sp: " + String(sp_val) + " is invalid\n";
    }
  }

  server.send(200, "text/plain", message);
}

void esp8266Tasks()
{
  currentServerMillis = now;
  if (currentServerMillis - previousServerMillis > serverInterval)
  {
    iotWebConf.doLoop();
    previousServerMillis = currentServerMillis;
  }
}

void configADC(void)
{
  // Setup ADS1115
  adc.begin();
  adc.set_data_rate(ADS1115_DATA_RATE_860_SPS);
  adc.set_mode(ADS1115_MODE_SINGLE_SHOT);
  adc.set_mux(ADS1115_MUX_GND_AIN0);

  //  TWO_THIRDS // 2/3x gain +/- 6.144V  1 bit = 0.1875mV
  //  ONE        // 1x gain   +/- 4.096V  1 bit = 0.125mV
  //  TWO        // 2x gain   +/- 2.048V  1 bit = 0.0625mV
  //  FOUR       // 4x gain   +/- 1.024V  1 bit = 0.03125mV
  //  EIGHT      // 8x gain   +/- 0.512V  1 bit = 0.015625mV
  //  SIXTEEN    // 16x gain  +/- 0.256V  1 bit = 0.0078125mV

  if (ADSGAIN == 23)
  {
    adc.set_pga(ADS1115_PGA_TWO_THIRDS);
    ADS_PGA = 0.1875;
  }
  else if (ADSGAIN == 1)
  {
    adc.set_pga(ADS1115_PGA_ONE);
    ADS_PGA = 0.125;
  }
  else if (ADSGAIN == 2)
  {
    adc.set_pga(ADS1115_PGA_TWO);
    ADS_PGA = 0.0625;
  }
  else if (ADSGAIN == 4)
  {
    adc.set_pga(ADS1115_PGA_FOUR);
    ADS_PGA = 0.03125;
  }
  else if (ADSGAIN == 8)
  {
    adc.set_pga(ADS1115_PGA_EIGHT);
    ADS_PGA = 0.015625;
  }
  else if (ADSGAIN == 16)
  {
    adc.set_pga(ADS1115_PGA_SIXTEEN);
    ADS_PGA = 0.0078125;
  }
}

void iotWebConfSetup(void)
{
  // -- Initializing the configuration.
  iotWebConf.setupUpdateServer(&httpUpdater);
  iotWebConf.init();

  server.on("/", handleRoot);
  server.on("/stats", handleStats); //Reads ADC function is called from out index.html
  server.on("/json", handleJSON);
  server.on("/set", HTTP_POST, handleSetvals);
  server.on("/config", [] { iotWebConf.handleConfig(); });
}

void setup()
{
  Serial.begin(115200); //Start a serial session
  lastMessage = now;    // timestamp

  // Set the Relay to output mode and ensure the relay is off
  pinMode(RelayPin, OUTPUT);
  digitalWrite(RelayPin, LOW);

  steamsw.begin(); // initialize the button object

  // PID settings
  windowStartTime = now;
  myPID.SetOutputLimits(0, 100);
  myPID.SetSampleTime(50);

  // initialize all the readings to 0:
  for (int thisReading = 0; thisReading < numReadings; thisReading++)
  {
    readings[thisReading] = 0;
  }

  // Enable I2C communication
  Wire.setClock(400000L); // ESP8266 Only
  Wire.begin(ESP_SDA, ESP_SCL);

#if OLED_DISPLAY == 1
  // Setup the OLED display
  display.begin(SSD1306_SWITCHCAPVCC, OLED_I2C); // initialize with the I2C addr 0x3C (for the 128x32)
  display.clearDisplay();
  display.setTextColor(WHITE);
  display.setTextSize(1);
  display.display();
#endif

  configADC();
  iotWebConfSetup();
} // end of setup()

void loop()
{
  keepTime();
  readTemps();
  steamSwitch();
  relayControl();
  trackloop();
#if OLED_DISPLAY == 1
  displayOLED();
#endif
  esp8266Tasks();
} // End of loop()
