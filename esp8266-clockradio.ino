/*
 * esp8266-clockradio
 * 
 * Turn an ESP8266 featherboard[1] + Music Maker FeatherWing[2] + Quad/Alphanumeric FeatherWing[3] into a clock radio.
 * 
 * TODO: Code still required for music & alarm support
 * TODO: Instead of the Music Maker Featherwing, integrate Si463x[4] for AM/FM radio
 * 
 * [1] https://www.adafruit.com/product/2821
 * [2] https://www.adafruit.com/product/3357
 * [3] https://www.adafruit.com/product/3127
 * [4] https://www.silabs.com/products/audio-and-radio/automotive-tuners/si463x-single-chip-digital-receivers
 * 
 * 
 * Copyright (c) 2019 Adam McDaniel
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 * 
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <ESP8266WiFi.h>
#include <WiFiUdp.h>
#include "ht16k33.h"
#include "asciifont.h"
#include "secret.h"

const char* ssid       = SECRET_SSID;
const char* password   = SECRET_PASS;

#define SLEEP_TIME_HOURS   17
#define SLEEP_TIME_MINUTES 10
#define WAKEUP_TIME_HOURS   9
#define WAKEUP_TIME_MINUTES 0

const unsigned int localPort = 2390;      // local port to listen for UDP packets
const char* ntpServerName = "time.nrc.ca";
IPAddress   ntpServerIP;

const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[ NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets

const long  gmtOffset_sec = -6 * 3600;
const int   daylightOffset_sec = 3600;
unsigned long localSeconds = 0;

WiFiUDP udp;
HT16K33 ht;

void setup() {
    Serial.begin(115200);

    ht.begin(0x01);
    ht.define16segFont((uint16_t *)&fontTable);
    ht.setBrightness(7);

    ht.set16Seg(0, 'N');
    ht.set16Seg(1, 'T');
    ht.set16Seg(2, 'P');
    ht.set16Seg(3, '*');
    ht.sendLed();

    // Initialize Wifi

    Serial.print("Connecting to ");
    Serial.println(ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");

    Serial.println("WiFi connected");
    Serial.println("IP address: ");
    Serial.println(WiFi.localIP());

    Serial.println("Starting UDP");
    udp.begin(localPort);
    Serial.print("Local port: ");
    Serial.println(udp.localPort());

    // Get time from NTP

    WiFi.hostByName(ntpServerName, ntpServerIP);

    sendNTPpacket(ntpServerIP); // send an NTP packet to a time server
    // wait to see if a reply is available
    delay(2000);

    int cb = udp.parsePacket();
    if (!cb) {
        Serial.println("no packet yet");
    } else {
        Serial.print("packet received, length=");
        Serial.println(cb);
        // We've received a packet, read the data from it
        udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

        //the timestamp starts at byte 40 of the received packet and is four bytes,
        // or two words, long. First, esxtract the two words:

        unsigned long highWord = word(packetBuffer[40], packetBuffer[41]);
        unsigned long lowWord = word(packetBuffer[42], packetBuffer[43]);
        // combine the four bytes (two words) into a long integer
        // this is NTP time (seconds since Jan 1 1900):
        unsigned long secsSince1900 = highWord << 16 | lowWord;
        Serial.print("Seconds since Jan 1 1900 = ");
        Serial.println(secsSince1900);

        // now convert NTP time into everyday time:
        Serial.print("Unix time = ");
        // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
        const unsigned long seventyYears = 2208988800UL;
        // subtract seventy years:
        unsigned long epoch = secsSince1900 - seventyYears;
        // print Unix time:
        Serial.println(epoch);

        localSeconds = epoch + gmtOffset_sec + 2;  // extra two seconds to offset the 2000ms delay above
    }


    // XXX: Unfortunately Forcing the wifi to disconnect and then sleep can cause a random watchdog reset 
    // See https://github.com/esp8266/Arduino/issues/6172
    // For skip the disconnect until this bug is fixed.

    Serial.println("Modem to sleep!");
    // WiFi.disconnect();
    WiFi.mode(WIFI_OFF);
    WiFi.forceSleepBegin();

    delay(1);
}

void loop() {
    // Update Clock Display

    uint8_t hours = (localSeconds % 86400L)/ 3600;
    uint8_t minutes = (localSeconds % 3600) / 60;
    uint8_t seconds = localSeconds % 60;

    const char* digits = "0123456789";

    // print the hour, minute and second:
    Serial.print("The time is ");       // UTC is the time at Greenwich Meridian (GMT)
    Serial.print(hours);
    Serial.print(":");
    Serial.print(minutes);
    Serial.print(":");
    Serial.println(seconds);

    // Flash at 5pm... it's time to go home!
    if (hours == 17)
        ht.setBlinkRate(HT16K33_DSP_BLINK1HZ);

    if (hours >= 10)
        ht.set16Seg(0, digits[int(hours / 10)]);
    else
        ht.set16Seg(0, ' ');
    ht.set16Seg(1, digits[hours % 10]);

    // Use the dot as a colon
    // TODO: Could it flash?
    ht.displayRam[3] |= 0b01000000;

    if (minutes >= 10)
        ht.set16Seg(2, digits[int(minutes / 10)]);
    else
        ht.set16Seg(2, '0');
    ht.set16Seg(3, digits[minutes % 10]);

    ht.sendLed();

    uint8_t delaySeconds = 60 - seconds;
    Serial.print("Sleeping for ");
    Serial.print(delaySeconds);
    Serial.println(" seconds.");

    localSeconds += delaySeconds;
#ifdef DEBUG
    while (delaySeconds-- >= 1) {
        Serial.print("Waiting: ");
        Serial.println(delaySeconds);
        delay(1000);
    }
#else
    delay(delaySeconds * 1000);
#endif

    // At SLEEP_TIME, or anytime after, shutdown the display.
    if ((hours == SLEEP_TIME_HOURS && minutes >= SLEEP_TIME_MINUTES) ||
         hours  > SLEEP_TIME_HOURS) {
        int wakeupSeconds = 0;

        // Calculate seconds until 9am tomorrow.
        wakeupSeconds =  (24 - hours) * 3600;
        wakeupSeconds += (60 - minutes) * 60;
        wakeupSeconds += WAKEUP_TIME_HOURS * 3600;
        wakeupSeconds += WAKEUP_TIME_MINUTES * 60;

        Serial.println("Display off");
        ht.displayOff();
        Serial.println("Display Sleep");
        ht.sleep();
        // Wake up again at 9am tomorrow morning
        Serial.print("Deep sleep for ");
        Serial.print(wakeupSeconds);
        Serial.println(" seconds");
        ESP.deepSleep(wakeupSeconds * 1e6);
    }
}

// send an NTP request to the time server at the given address
void sendNTPpacket(IPAddress& address) {
    Serial.println("sending NTP packet...");
    // set all bytes in the buffer to 0
    memset(packetBuffer, 0, NTP_PACKET_SIZE);
    // Initialize values needed to form NTP request
    // (see URL above for details on the packets)
    packetBuffer[0] = 0b11100011;   // LI, Version, Mode
    packetBuffer[1] = 0;     // Stratum, or type of clock
    packetBuffer[2] = 6;     // Polling Interval
    packetBuffer[3] = 0xEC;  // Peer Clock Precision
    // 8 bytes of zero for Root Delay & Root Dispersion
    packetBuffer[12]  = 49;
    packetBuffer[13]  = 0x4E;
    packetBuffer[14]  = 49;
    packetBuffer[15]  = 52;

    // all NTP fields have been given values, now
    // you can send a packet requesting a timestamp:
    udp.beginPacket(address, 123); //NTP requests are to port 123
    udp.write(packetBuffer, NTP_PACKET_SIZE);
    udp.endPacket();
}
