#include <ESP8266WiFi.h>
#include <WiFiClient.h>
#include <ESP8266WebServer.h>
#include <ESP8266mDNS.h>
#include <Wire.h>
#include <EEPROM.h>
#include "Adafruit_HTU21DF.h"
#include "Adafruit_GFX.h"
#include "Adafruit_LEDBackpack.h"

Adafruit_HTU21DF htu = Adafruit_HTU21DF();

#define DISPLAY_ADDRESS   0x70

Adafruit_7segment display = Adafruit_7segment();

const uint8_t num_samples_per_influx_write = 6;
uint16_t update_delay = 5000; // in ms
uint32_t next_update;

uint16_t disp_delay = 2000; // in ms
uint32_t disp_update = 0;
uint8_t show_hum = 0;

const char* sg_ssid = "temp logger ";
const int   sg_wifi_channel = 3;
uint8_t     is_ap = 1;
const uint8_t ssid_len = 32;
const uint8_t passwd_len = 63;
const uint8_t wifi_connect_delay = 120; // in .5 seconds intervals
String macID;

const char *influx_db_host = "10.1.1.2";
const int influx_db_port = 8086;
const char *influx_db_name = "rumpcentral";

// TODO: Allow the user to name the device

ESP8266WebServer server(80);

const char *wifi_form = "<html><body><form action=\"/save\" method=\"POST\"><h1>Connect to Wifi Network</h1>"
                        "SSID:<input type=\"text\" name=\"ssid\" width=\"40\"></input></br>"
                        "password:<input type=\"text\" name=\"password\" width=\"40\"></input></br>"
                        "<input type=\"submit\" value=\"save\"></input>"
                        "</form></body></html>";

void handle_root() 
{    
    if (is_ap)
    {
        server.send(200, "text/html", wifi_form);
    }
    else
    {
        double input = htu.readHumidity();  
        server.send(200, "text/plain", "hum: " + String(input));
    }
}

void handle_save()
{
    Serial.println("in save");
    if (server.method() != HTTP_POST)
    {
        server.send(400, "text/plain", "Must POST to /save");
        return;   
    }  
   
    String passwd = server.arg("password");
    String ssid = server.arg("ssid");
    
    Serial.println("in post!");
    Serial.println(ssid);
    Serial.println(passwd); 
    
    if (passwd.length() > 0 && passwd.length() <= passwd_len &&
        ssid.length() > 0 && ssid.length() <= ssid_len)
    {
        write_wifi_info(ssid, passwd);
        server.send(200, "text/html", "Wifi configuration saved.");
    }
    else
        server.send(200, "text/html", wifi_form);
}

void handle_not_found()
{
    String message = "File Not Found\n\n";
    message += "URI: ";
    message += server.uri();
    message += "\nMethod: ";
    message += (server.method() == HTTP_GET)?"GET":"POST";
    message += "\nArguments: ";
    message += server.args();
    message += "\n";

    for (uint8_t i=0; i<server.args(); i++)
    {
        message += " " + server.argName(i) + ": " + server.arg(i) + "\n";
    }
    server.send(404, "text/plain", message);
}


void send_data_to_influx(double hum, double temp)
{
    WiFiClient client;

    if (!client.connect(influx_db_host, influx_db_port)) 
    {
        Serial.println("write data to influx failed.");
        return;
    }

    String url = String("/write?db=") + String(influx_db_name);
    //
    Serial.println(url);
    
    String body;
   
    body += String("maker,location=maker humidity=") + String(hum) + String(",temperature=") + String(temp);    
    String headers = "POST " + String(url) + " HTTP/1.1\r\n" +
                 "Host: " + String(influx_db_host) + String("\r\n") + 
                 "Content-Type: application/octet-stream\r\n" +
                 "Content-Length: " + String(body.length()) + "\r\n" +
                 "Connection: close\r\n\r\n";
    
    Serial.println(headers + body);
    
    client.print(headers + body);
    delay(50);

    // TODO: Improve error catching, I guess?
    while(client.available())
    {
        String line = client.readStringUntil('\r');
        Serial.print(line);
    }
    Serial.println("data submitted");
}


void ap_setup(void)
{
    IPAddress Ip(192, 168, 1, 1);
    IPAddress NMask(255, 255, 255, 0);

    WiFi.softAPConfig(Ip, Ip, NMask);
    String AP_NameString = String(sg_ssid) + macID;

    char AP_NameChar[AP_NameString.length() + 1];
    memset(AP_NameChar, 0, AP_NameString.length() + 1);

    for (int i=0; i<AP_NameString.length(); i++)
        AP_NameChar[i] = AP_NameString.charAt(i);

    WiFi.softAP(AP_NameChar, "", sg_wifi_channel);
    
    IPAddress myIP = WiFi.softAPIP();
    Serial.println();
    Serial.print("AP IP address: ");
    Serial.println(myIP);
}

void read_eeprom_string(uint16_t offset, String &str, uint8_t max_len)
{
    char tmp[max_len + 1];
    uint8_t i;
    
    for(i = 0; i < max_len; i++)
        tmp[i] = EEPROM.read(i + offset);
    tmp[max_len - 1] = 0;
    
    str = String(tmp);
}

void write_eeprom_string(uint16_t offset, String str)
{
    uint8_t i;
    
    for(i = 0; i < str.length(); i++)
        EEPROM.write(i + offset, str[i]);
        
    // write the null terminator
    EEPROM.write(offset + str.length(), 0);
}

void read_wifi_info(String &ssid, String &passwd)
{
    read_eeprom_string(0, ssid, ssid_len);
    read_eeprom_string(ssid_len, passwd, passwd_len);

    Serial.println("read ssid " + ssid + " passwd " + passwd);    
}

void write_wifi_info(String &ssid, String &passwd)
{
    write_eeprom_string(0, ssid);
    write_eeprom_string(ssid_len, passwd);
    EEPROM.commit();
}

void wifi_setup(void)
{
    String ssid, passwd;
    uint8_t mac[WL_MAC_ADDR_LENGTH];
    
    read_wifi_info(ssid, passwd);
    if (ssid.length() == 0)
    {
        ap_setup();
        return;
    }
    
    WiFi.begin(ssid.c_str(), passwd.c_str());
    
    WiFi.softAPmacAddress(mac);
    macID = String(mac[WL_MAC_ADDR_LENGTH - 2], HEX) +
            String(mac[WL_MAC_ADDR_LENGTH - 1], HEX);
    macID.toUpperCase();

    for(uint8_t i = 0; WiFi.status() != WL_CONNECTED && i < wifi_connect_delay; i++) 
    {
      delay(500);
      Serial.print(".");
    }
    
    if (WiFi.status() != WL_CONNECTED)
    {
        // we didn't connect. blow away the config and go into AP mode
        ap_setup();
        
        String blank("");
        write_wifi_info(blank, blank);
        return;
    }
    
    is_ap = 0;
    Serial.println("");
    Serial.print("Connected to ");
    Serial.println(ssid);
    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());
    
    if (MDNS.begin("supplegreen"))
        Serial.println("MDNS responder started");
}

void server_setup(void)
{
    server.on("/", handle_root);
    server.on("/save", handle_save);
    server.onNotFound(handle_not_found);
    
    server.begin();
    Serial.println("HTTP server started");
}


void setup(void)
{
    pinMode(LED_BUILTIN, OUTPUT);
    
    digitalWrite(LED_BUILTIN, HIGH);
   
    // hardware setup
    Serial.begin(115200);
    display.begin(DISPLAY_ADDRESS);
    EEPROM.begin(512);
    
    if (!htu.begin())
    {
         Serial.println("");
         Serial.println("Couldn't find sensor!");
         while (1)
           ;
    }

    // write dashes to 7segment display
    display.print(10000, DEC);
    display.writeDisplay();

    // wifi setup
    wifi_setup();
    server_setup();
   
    // display update "timers"
    next_update = millis();
    disp_update = millis();
}

void loop(void)
{
    char ch;
    char tmp[16];
    double temp = 0.0, hum = 0.0;
    static uint8_t num_updates = num_samples_per_influx_write;
        
    if (next_update && millis() >= next_update)
    {
        hum = htu.readHumidity();
        temp = htu.readTemperature();
    
        Serial.print("humidity: ");   
        Serial.print(hum);
        Serial.print("% temp: ");
        Serial.print(temp);
        Serial.print("C\n"); 

        if (num_updates == num_samples_per_influx_write)
        {
             send_data_to_influx(hum, temp);
             num_updates = 0;
        }
        next_update += update_delay;
        num_updates++;
    }
            
    if (temp != 0.0 && hum != 0.0 && disp_update && millis() >= disp_update)
    {
        if (show_hum)
            display.print(hum);
        else
            display.print(temp);
        display.writeDisplay();

        show_hum = !show_hum;
        disp_update += disp_delay;
    }

    server.handleClient();
}
