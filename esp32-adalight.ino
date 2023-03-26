/*
 *  Project     ESP32 Adalight
 *  @author     Ivan Kholmogorov
 *  @link       github.com/kholmogorov27/esp32-adalight
 *  @license    MIT License, copyright (c) 2023 Ivan Kholmogorov
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated documentation files (the "Software"), to deal in the Software without restriction, including without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 * The above copyright notice and this permission notice shall be included in all copies or substantial portions of the Software.
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 *  Inspired by: github.com/dmadison/Adalight-FastLED
 */

/*
 * HELPFUL INFORMATION:
 *  - WS2812B framerate limits: 
 *    https://www.partsnotincluded.com/calculating-adalight-framerate-limits/
 *    For 132 LEDs on 921600 bps the framerate limit is ~120 fps
 */

// Adalight -> WS2812B driver based on ESP32

// General Settings
#define LEDS_AMOUNT  132   // strip length (max 256)
#define	BRIGHTNESS   255  // maximum brightness
// ---

// FastLED Setings
#define COLOR_ORDER    NEO_GRB     // color order for bitbang (NEO_GRB/NEO_RGB/NEO_RGBW)
#define LED_BITSTREAM  NEO_KHZ800  // led bitstream rate (NEO_KHZ800/NEO_KHZ400)
#define DATA_PIN       15          // led data output pin
#define IS_INVERTED    false       // is strip inverted
// ---

// Serial Settings
#define SERIAL_SPEED    115200   // serial port speed
#define SERIAL_TIMEOUT  30       // time before LEDs are shut off if no data (in seconds), 0 to disable
// ---

// Optional Settings
#define SERIAL_FLUSH            // Serial buffer cleared on LED latch
#define LEDS_CHECK_ON_START     // Check LEDs on start
#define START_ANIMATION         // Play the animation on start
#define ANIMATION_DURATION   2  // Animation duration (in seconds)
// ---

// --------------------------------------------------------------------

#include <Adafruit_NeoPixel.h>

// NeoPixel strip object
// Argument 1 = Number of pixels in NeoPixel strip
// Argument 2 = Arduino pin number (most are valid)
// Argument 3 = Pixel type flags, add together as needed:
//   NEO_KHZ800  800 KHz bitstream (most NeoPixel products w/WS2812 LEDs)
//   NEO_KHZ400  400 KHz (classic 'v1' (not v2) FLORA pixels, WS2811 drivers)
//   NEO_GRB     Pixels are wired for GRB bitstream (most NeoPixel products)
//   NEO_RGB     Pixels are wired for RGB bitstream (v1 FLORA pixels, not v2)
//   NEO_RGBW    Pixels are wired for RGBW bitstream (NeoPixel RGBW products)
Adafruit_NeoPixel strip(LEDS_AMOUNT, DATA_PIN, COLOR_ORDER + LED_BITSTREAM);

// NEOPIXEL BEST PRACTICES for most reliable operation:
// - Add 1000 uF CAPACITOR between NeoPixel strip's + and - connections.
// - MINIMIZE WIRING LENGTH between microcontroller board and first pixel.
// - NeoPixel strip's DATA-IN should pass through a 300-500 OHM RESISTOR.
// - AVOID connecting NeoPixels on a LIVE CIRCUIT. If you must, ALWAYS
//   connect GROUND (-) first, then +, then data.
// - When using a 3.3V microcontroller with a 5V-powered NeoPixel strip,
//   a LOGIC-LEVEL CONVERTER on the data line is STRONGLY RECOMMENDED.
// (Skipping these may work OK on your workbench but can fail in the field)


// A 'magic word' (along with LED count & checksum) precedes each block
// of LED data; this assists the microcontroller in syncing up with the
// host-side software and properly issuing the latch (host I/O is
// likely buffered, making usleep() unreliable for latch). You may see
// an initial glitchy frame or two until the two come into alignment.
// The magic word can be whatever sequence you like, but each character
// should be unique, and frequent pixel values like 0 and 255 are
// avoided -- fewer false positives. The host software will need to
// generate a compatible header: immediately following the magic word
// are three bytes: a 16-bit count of the number of LEDs (high byte
// first) followed by a simple checksum value (high byte XOR low byte
// XOR 0x55). LED data follows, 3 bytes per LED, in order R, G, B,
// where 0 = off and 255 = max brightness.

const uint8_t magic[] = {
	'A','d','a'};
#define MAGICSIZE  sizeof(magic)

// Check values are header byte # - 1, as they are indexed from 0
#define HICHECK    (MAGICSIZE)
#define LOCHECK    (MAGICSIZE + 1)
#define CHECKSUM   (MAGICSIZE + 2)

enum processModes_t {Header, Data} mode = Header;

int16_t c;  // current byte, must support -1 if no data available
uint16_t currentPosition;  // current LED position int the strip
uint8_t currentColor[3];  // current LED color
uint32_t bytesRemaining;  // count of bytes yet received, set by checksum
unsigned long t, lastByteTime, lastAckTime;  // millisecond timestamps

void headerMode();
void dataMode();
void timeouts();

// Macros initialized
#ifdef SERIAL_FLUSH
	#undef SERIAL_FLUSH
	#define SERIAL_FLUSH while(Serial.available() > 0) { Serial.read(); }
#else
	#define SERIAL_FLUSH
#endif

#ifdef START_ANIMATION
  #define ANIMATION_COLOR_RANGE_PIECE 65535 / LEDS_AMOUNT
  #define ANIMATION_TIME_RANGE_PIECE ANIMATION_DURATION * 1000 / LEDS_AMOUNT
#endif

void setup(){
	strip.setBrightness(BRIGHTNESS);
	strip.show();

  #ifdef LEDS_CHECK_ON_START
    // red color
    strip.fill(0x00FF0000);
    strip.show();
    delay(500);

    // green color
    strip.fill(0x0000FF00);
    strip.show();
    delay(500);

    // blue color
    strip.fill(0x000000FF);
    strip.show();
    delay(500);
	#endif

  #ifdef START_ANIMATION
    for (byte i=0; i < LEDS_AMOUNT; i++) {
		  strip.rainbow((i+1) * ANIMATION_COLOR_RANGE_PIECE);
  	  strip.show();
      delay(ANIMATION_TIME_RANGE_PIECE);
    }
	#endif

  Serial.begin(SERIAL_SPEED);
	Serial.print("Ada\n"); // Send ACK string to host
	lastByteTime = lastAckTime = millis(); // Set initial counters
}

void loop(){ 
	t = millis(); // Save current time

	// If there is new serial data
	if((c = Serial.read()) >= 0){
    Serial.println("got data: " + String(c));
		lastByteTime = lastAckTime = t; // Reset timeout counters

    switch(mode) {
			case Header:
				headerMode();
				break;
			case Data:
				dataMode();
				break;
		}
		
	}
	else {
		// No new data
		timeouts();
	}
}

void headerMode(){
	static uint8_t
		headPos,
		hi, lo, chk;

	if(headPos < MAGICSIZE){
		// Check if magic word matches
		if(c == magic[headPos]) {headPos++;}
		else {headPos = 0;}
	}
	else{
		// Magic word matches! Now verify checksum
		switch(headPos){
			case HICHECK:
				hi = c;
				headPos++;
				break;
			case LOCHECK:
				lo = c;
				headPos++;
				break;
			case CHECKSUM:
				chk = c;
				if(chk == (hi ^ lo ^ 0x55)) {
					// Checksum looks valid. Get 16-bit LED count, add 1
					// (# LEDs is always > 0) and multiply by 3 for R,G,B.
					bytesRemaining = 3L * (256L * (long)hi + (long)lo + 1L);
					currentPosition = 0;
					strip.clear();
					mode = Data; // Proceed to latch wait mode
				}
				headPos = 0; // Reset header position regardless of checksum result
				break;
		}
	}
}

void dataMode(){
	// If LED data is not full
	if (currentPosition < LEDS_AMOUNT) {
    strip.setPixelColor(IS_INVERTED ? LEDS_AMOUNT-1 - currentPosition++ : currentPosition++, c, Serial.read(), Serial.read());
	}
	bytesRemaining--;
 
	if(bytesRemaining == 0) {
		// End of data -- issue latch:
		mode = Header; // Begin next header search
		strip.show();
		SERIAL_FLUSH;
	}
}

void timeouts(){
	// No data received. If this persists, send an ACK packet
	// to host once every second to alert it to our presence.
	if((t - lastAckTime) >= 1000) {
		Serial.print("Ada\n"); // Send ACK string to host
		lastAckTime = t; // Reset counter

		// If no data received for an extended time, turn off all LEDs.
		if(SERIAL_TIMEOUT != 0 && (t - lastByteTime) >= (uint32_t) SERIAL_TIMEOUT * 1000) {
			strip.clear();
      strip.show();
			mode = Header;
			lastByteTime = t; // Reset counter
		}
	}
}
