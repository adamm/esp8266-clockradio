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
#include <time.h>
#include "ht16k33.h"
#include "asciifont.h"
#include "secret.h"

const char* ssid       = SECRET_SSID;
const char* password   = SECRET_PASS;

#define SLEEP_TIME_HOURS   17
#define SLEEP_TIME_MINUTES 10
#define WAKEUP_TIME_HOURS   8
#define WAKEUP_TIME_MINUTES 0

// ESP8266's reports its maximum deep-sleep length is longer than actually allowed, forcing
// the device to sleep until manual reset. Reduce returned value by 5% to ensure validity.
#define MAX_SLEEP_SECONDS   (uint32_t)(ESP.deepSleepMax()*0.95/1e6)
#define NTP_REFRESH_SECONDS MAX_SLEEP_SECONDS/2


const unsigned int localPort = 2390;      // local port to listen for UDP packets
const char* ntpServerName = "ca.pool.ntp.org";
IPAddress   ntpServerIP;

const int NTP_PACKET_SIZE = 48; // NTP time stamp is in the first 48 bytes of the message
byte packetBuffer[NTP_PACKET_SIZE]; //buffer to hold incoming and outgoing packets

uint8_t hours = 0;
uint8_t minutes = 0;
uint8_t seconds = 0;

const time_t dstEnabledOnEpoch  = 1615716000; // March 14 2021 10am UTC
const time_t dstEnabledSeconds  = 10886400;   // DST Enabled for 5 months 7 days
const time_t dstDisabledSeconds = 20563200;   // DST Disabled for 8 months 27 days

// Mountain Standard Time and Mountain Daylight Time offsets from GMT by seconds.
const int32_t mstOffset_sec = -7 * 3600;
const int32_t mdtOffset_sec = -6 * 3600;

struct rtcData {
  uint32_t crc32;
  time_t now;
  time_t wakeupAt;
  time_t refreshNTP;
} rtcData;

WiFiUDP udp;
HT16K33 ht;

// An arbitrary function at NULL pointer forces the hardware to be reset.
void (*reset)(void) = 0;


void setup() {
    Serial.begin(74880);

    ht.begin(0x01);
    ht.define16segFont((uint16_t *)&fontTable);
    ht.setBrightness(7);

    uint8_t goodNTP = 0;

    if (loadMemory()) {
        // If our memory is good, and we're under the refresh NTP time,
        // mark NTP as good.
        if (0 < rtcData.now && rtcData.now < rtcData.refreshNTP)
            goodNTP = 1;
    }
    else {
        clearMemory();
        ht.set16Seg(0, 'N');
        ht.set16Seg(1, 'T');
        ht.set16Seg(2, 'P');
        ht.set16Seg(3, '*');
        ht.sendLed();
    }

    Serial.printf("The assumed time is %s\n", getTimeStr(rtcData.now));

    if (!goodNTP) {
        time_t rtcDataAssumed = rtcData.now;
        time_t rtcDrift = 0;

        getTimeFromNTP();  // sets rtcData.now and rtcData.refreshNTP
        Serial.printf("The actual time is %s\n", getTimeStr(rtcData.now));

        rtcDrift = rtcDataAssumed - rtcData.now;
        Serial.printf("Drift of %ld seconds\n", rtcDrift);
    }

    Serial.printf("The next refresh at %s\n", getTimeStr(rtcData.refreshNTP));

    if (0 < rtcData.wakeupAt && rtcData.wakeupAt > rtcData.now) {
        Serial.printf("The next wakeup at  %s\n", getTimeStr(rtcData.wakeupAt));

        Serial.println("Woke up too soon! Back to bed.");
        longSleep(); // Woke up too soon.  Go back to sleep!
                     // Refreshes rtcData.wakeupAt if needed, too.
    }

    // The RTC memory is good, NTP is good, and we're supposed to be awake! Update the display in loop();
}


void loop() {
    hours = (rtcData.now % 86400L) / 3600;
    minutes = (rtcData.now % 3600) / 60;
    seconds = rtcData.now % 60;

    updateDisplay();

    // At SLEEP_TIME, or anytime after, shutdown the display.
    if ((hours == SLEEP_TIME_HOURS && minutes >= SLEEP_TIME_MINUTES) ||
         hours  > SLEEP_TIME_HOURS)  {
        Serial.println("It's after working hours. Put the display to sleep.");
        longSleep();
    }
    else {
        shortSleep();
    }
}


time_t getTimeFromNTP(){
    // Initialize Wifi

    Serial.print("Connecting to ");
    Serial.print(ssid);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, password);

    while (WiFi.status() != WL_CONNECTED) {
        delay(500);
        Serial.print(".");
    }
    Serial.println("");

    Serial.print("IP address: ");
    Serial.println(WiFi.localIP());

    udp.begin(localPort);

    // Get time from NTP

    WiFi.hostByName(ntpServerName, ntpServerIP);

    sendNTPpacket(ntpServerIP); // send an NTP packet to a time server
    // wait to see if a reply is available
    delay(2000);

    int cb = udp.parsePacket();
    if (!cb) {
        Serial.println("no packet yet");
        // Unable to connect, force reset everything and try again.
        ht.set16Seg(0, 'R');
        ht.set16Seg(1, 't');
        ht.set16Seg(2, 'r');
        ht.set16Seg(3, 'y');
        ht.sendLed();
        clearMemory();
        reset();
    } else {
        Serial.print("packet received, length=");
        Serial.println(cb);
        // We've received a packet, read the data from it
        udp.read(packetBuffer, NTP_PACKET_SIZE); // read the packet into the buffer

        //the timestamp starts at byte 40 of the received packet and is four bytes,
        // or two words, long. First, esxtract the two words:

        uint32_t highWord = word(packetBuffer[40], packetBuffer[41]);
        uint32_t lowWord = word(packetBuffer[42], packetBuffer[43]);
        // combine the four bytes (two words) into a long integer
        // this is NTP time (seconds since Jan 1 1900):
        uint32_t secsSince1900 = highWord << 16 | lowWord;
        secsSince1900 += 2;  // extra two seconds to offset the 2000ms delay accessing NTP

        Serial.print("Seconds since Jan 1 1900 UTC = ");
        Serial.println(secsSince1900);

        // now convert NTP time into everyday time:
        // Unix time starts on Jan 1 1970. In seconds, that's 2208988800:
        const uint32_t seventyYears = 2208988800UL;
        // subtract seventy years:
        time_t epoch = secsSince1900 - seventyYears;

        Serial.print("Seconds since Jan 1 1970 UTC = ");
        Serial.println(epoch);

        // Attempt to calculate whether Daylight Savings Time is enabled using deltas
        uint8_t dstEnabled = 0;
        time_t checkDstEpoch = dstEnabledOnEpoch;

        while (epoch > checkDstEpoch) {
          if (!dstEnabled) {
            checkDstEpoch += dstEnabledSeconds;
            dstEnabled = 1;
          }
          else {
            checkDstEpoch += dstDisabledSeconds;
            dstEnabled = 0;
          }
        }

        Serial.print("DST is ");
        Serial.print(dstEnabled ? "enabled" : "disabled");
        Serial.print(". Adjusting epoch by ");
        Serial.println(dstEnabled ? mdtOffset_sec : mstOffset_sec);
        Serial.print("Seconds since Jan 1 1970 ");
        Serial.print(dstEnabled ? "MDT" : "MST");

        rtcData.now = epoch + (dstEnabled ? mdtOffset_sec : mstOffset_sec);
        Serial.print(" = ");
        Serial.println(rtcData.now);

        rtcData.refreshNTP = rtcData.now + NTP_REFRESH_SECONDS; // Force an NTP refresh in an hour
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


void updateDisplay() {
    // Update Clock Display
    const char* digits = "0123456789";

    // Flash at 5pm... it's time to go home!
    if (hours == SLEEP_TIME_HOURS)
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
}


void shortSleep() {
    uint8_t delaySeconds = 60 - seconds;

    rtcData.wakeupAt = 0;
    rtcData.now += delaySeconds;
    saveMemory();

    Serial.print("Deep sleep with screen on for ");
    Serial.print(delaySeconds);
    Serial.println(" seconds");
    ESP.deepSleep(delaySeconds * 1e6);
}


void longSleep() {
    uint32_t delaySeconds = 0;
    uint32_t maxSeconds = MAX_SLEEP_SECONDS;

    // If we're being asked to sleep and no wakeupAt time set...
    if (rtcData.wakeupAt == 0) {
        // ...calculate seconds until WAKEUP_TIME tomorrow.
        delaySeconds =  (24 - hours) * 3600;
        delaySeconds += (60 - minutes) * 60;
        delaySeconds += (60 - seconds);
        delaySeconds += (WAKEUP_TIME_HOURS-1) * 3600;
        delaySeconds += (WAKEUP_TIME_MINUTES-1) * 60;

        // Force NTP to refresh on longSleep() wakeup.
        // If we woke-up too soon (now < wakeupAt) then go back to sleep during setup().
        rtcData.wakeupAt = rtcData.now + delaySeconds;
    }
    else {
        delaySeconds = rtcData.wakeupAt - rtcData.now;
    }

    Serial.printf("Try to stay asleep for %lu seconds ", delaySeconds);
    Serial.printf("...which is %s\n", getTimeStr(rtcData.wakeupAt));

    if (delaySeconds > maxSeconds) {
        Serial.printf("Maximum sleep time is  %lu seconds ", maxSeconds);
        delaySeconds = maxSeconds;
        Serial.printf("...which is %s\n", getTimeStr(rtcData.now+delaySeconds));
    }

    rtcData.now += delaySeconds;
    saveMemory();

    ht.clearAll();
    ht.displayOff();
    ht.sleep();

    Serial.printf("Deep sleep for %lu seconds.\n", delaySeconds);
    ESP.deepSleep(delaySeconds * 1e6);
}


char* getTimeStr(const time_t t) {
    char* timeStr = new char[21];
    struct tm* lt = localtime(&t);

    strftime(timeStr, 21, "%Y-%m-%d %H:%M:%S", lt);

    return timeStr;
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


uint32_t calculateCRC32(const uint8_t *data, size_t length) {
    uint32_t crc = 0xffffffff;

    while (length--) {
        uint8_t c = *data++;
        for (uint32_t i = 0x80; i > 0; i >>= 1) {
            bool bit = crc & 0x80000000;
            if (c & i) {
                bit = !bit;
            }
            crc <<= 1;
            if (bit) {
                crc ^= 0x04c11db7;
            }
        }
    }
    return crc;
}


void saveMemory() {
    // Generate new data set for the struct
    memset(&rtcData, sizeof(rtcData), '\0');

    Serial.printf("SAVING  rtcData.now = %lu, refreshNTP = %lu, wakeupAt = %lu\n", rtcData.now, rtcData.refreshNTP, rtcData.wakeupAt);

    // Update CRC32 of data
    rtcData.crc32 = calculateCRC32((uint8_t*) &rtcData.now, sizeof(rtcData)-sizeof(rtcData.crc32));
    // Write struct to RTC memory
    if (ESP.rtcUserMemoryWrite(0, (uint32_t*) &rtcData, sizeof(rtcData))) {
//        Serial.print("RTC Write: ");
//        printMemory();
    }
}


uint8_t loadMemory() {
    // Read struct from RTC memory
    if (ESP.rtcUserMemoryRead(0, (uint32_t*) &rtcData, sizeof(rtcData))) {
        Serial.print("RTC Read: ");
        printMemory();
        uint32_t crcOfData = calculateCRC32((uint8_t*) &rtcData.now, sizeof(rtcData)-sizeof(rtcData.crc32));
//        Serial.print("CRC32 of data: ");
//        Serial.println(crcOfData, HEX);
//        Serial.print("CRC32 read from RTC: ");
//        Serial.println(rtcData.crc32, HEX);
        if (crcOfData != rtcData.crc32) {
            Serial.println("CRC32 in RTC memory doesn't match CRC32 of data. Data is probably invalid!");
        } else {
            Serial.println("CRC32 check ok, data is probably valid.");
            Serial.printf("LOADING rtcData.now = %lu, refreshNTP = %lu, wakeupAt = %lu\n", rtcData.now, rtcData.refreshNTP, rtcData.wakeupAt);
        }
        return (crcOfData == rtcData.crc32);
    }
    return 0;
}


void clearMemory() {
    Serial.println("Clear RTC memory");

    memset(&rtcData, 0, sizeof(struct rtcData));

    ESP.rtcUserMemoryWrite(0, (uint32_t*) &rtcData, sizeof(rtcData));
}


//prints all rtcData, including the leading crc32
void printMemory() {
    char buf[3];
    uint8_t *ptr = (uint8_t *)&rtcData;

    for (size_t i = 0; i < sizeof(rtcData); i++) {
        sprintf(buf, "%02X", ptr[i]);
        Serial.print(buf);
        if ((i + 1) % 32 == 0) {
            Serial.println();
        } else {
            Serial.print(" ");
        }
    }
    Serial.println();
}
