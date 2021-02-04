/*
 * Source code for Aquarius
 * Author: Stephen Co (stephen.co@northern-circuits.ca)
 */

#include "FastLED.h" // Include the fantastic FastLED library for APA102C/SK9822 addressable LED strip support (SPI)

#define NUM_LEDS 60 // Aquarius uses 60 LEDs, 30 on each of the left and right inner frames
#define PUMP_PIN 2 // Use pin 2 for PWM-ing the water pump, this is not synchronized with anything in particular, only used to minimize audible pump noise, should not be changed
#define MAG_PIN 10 // Use pin 10 (OC1A) for PWM-ing the electromagnet, this pin runs off of timer1, should not be changed

// We use an Arduino Nano clone which uses the Atmel ATMega328P microcontroller
// To use hardware SPI for this microcontroller, the data and clock pins are respectively 11 and 13
// Note that pin 11 (OCR2A) is a PWM pin which runs off timer2 (same with pin 3, OCR2B)
//  This means we must not set the timers for timer1 otherwise it will mess up the LED timing
#define DATA_PIN 11
#define CLOCK_PIN 13

// Global variables for tracking states, modes, and more
CRGB leds[NUM_LEDS]; // Define the array of leds to be addressed
static int previousMode = 50; // Track the previous mode
static uint8_t powered_on = 3; // Track the state of the pump and magnet, 0 = OFF, 1 - PUMP ON, 2 - PUMP AND MAG ON, 3 - INITIAL
static uint8_t mag_tick = 0; // Tick counter for magnet ramp up
static uint8_t mag_duty = 88; // * USER SETTING * From 0-225, duty cycle is this value divided by 225, default of 50, higher values make wider wave patterns but increases likelihood that it will spill outside of the funnel
static bool initial_power_on = 0; // Flag to track first time powering on

unsigned long previousSeconds = 0; // Time tracker for previous amount of minutes seen
float shutdown_timer = 3; // * USER SETTING * How many HOURS Aquarius runs for before shutting water off (if it was on in the first place), can be fractions of an hour too
bool shutdown_timer_expired = 0; // Flag for disabling the water pump once the timer expires

/*******************************
 * Initial setup of everything 
 *******************************/
void setup() {  
  LEDS.addLeds<APA102, DATA_PIN, CLOCK_PIN, BGR>(leds, NUM_LEDS); // Setup the LEDs, I might not need to specify the DATA and CLOCK pins here since its defaulted to hardware SPI

  pinMode(PUMP_PIN, OUTPUT); // Set the PUMP_PIN as an output, this will be ramped up and down with analogWrites
  pinMode(MAG_PIN, OUTPUT); // Set the MAG_PIN as an output with varying PWM frequency, this will be controlled through the timer1 registers
  LEDS.setBrightness(255); // Set brightness to 0 at start
  analogWrite(PUMP_PIN, 0); // Keep pump off at start

  // Setup the Atmel 328P timer1 registers
  // Set WGM13:0 = 4'b1110 to put timer1 into Fast PWM mode with ICR1 acting as the TOP counter value for compare
  // Set CS12:0 = 3'b101 to use a 1024 prescale factor to get 15.625 KHz base frequency
  // Set COM1A1:0 = 2'b10
  TCCR1A = _BV(COM1A0) | _BV(COM1B1) | _BV(WGM11) | _BV(WGM10); 
  TCCR1B =  _BV(WGM13) | _BV(WGM12)  |  _BV(CS12) | _BV(CS10);

  OCR1A = 225; // ~80 Hz
  OCR1B = 0; // Off for now
}

/********************
 * Shut down pump and magnet
 ********************/
void turn_off() {
  // If we are not powered off
  if (powered_on != 0) {
    while (OCR1B != 0) {
      delay(1);
      OCR1B--; // Step the magnet down by 1 tick
    }
    analogWrite(PUMP_PIN, 0); // Shut the pump off
    powered_on = 0; // Advance the state machine from any state to OFF

    TCCR1A = 0; // Set Atmel timer registers to 0
    TCCR1B = 0; // Set Atmel timer registers to 0
  }
}

/********************
 * Ramp up water pump
 ********************/
void turn_on() {
  for (int i = 0; i < 255; i++) {
          delay(1);
          analogWrite(PUMP_PIN, i); // Kickstart the motor first slowly
  }
  delay(250);
  for (int i = 0; i < 80; i++) {
    delay(3);
    analogWrite(PUMP_PIN, 255 - i); // Slowly ramp motor back down to 40% duty cycle
  }
  TCCR1A = _BV(COM1A0) | _BV(COM1B1) | _BV(WGM11) | _BV(WGM10); 
  TCCR1B =  _BV(WGM13) | _BV(WGM12)  |  _BV(CS12) | _BV(CS10);
  OCR1A = 225; // ~80 Hz
  OCR1B = 0;
}

/********************
 * The default map() function in the Arduino library is inaccurate
 *  when going from larger to smaller ranges
 * The version here fixes that and is referenced as an open
 *  issue on Github here: https://github.com/arduino/Arduino/issues/2466
 ********************/
long fixed_map(long x, long in_min, long in_max, long out_min, long out_max) {
  if ((in_max - in_min) > (out_max - out_min)) {
    return (x - in_min) * (out_max - out_min+1) / (in_max - in_min+1) + out_min;
  }
  else
  {
    return (x - in_min) * (out_max - out_min) / (in_max - in_min) + out_min;
  }
}

/**********************************************************************************************************************************************************
 * This is the main program loop
 * There is only 1 DIAL which controls which mode you are in, it cycles through the modes in an incremental manner
 **********************************************************************************************************************************************************/

// Mode specific variables, I could have easily merged all these into only a few sets, but I divided it up this way to make
//   it easier for folks to add new modes, and/or remove/edit existing modes

// * USER SETTING * Mode 1 (Ambient) variables
static uint8_t m1_tick = 0;
static uint8_t m1_hue = 0;
static uint8_t m1_colorTime = 25; // * USER SETTING * How long before we increment the hue on the HSV color wheel, higher means slower color changes

// * USER SETTING * Mode 2 (Helix) variables
static uint8_t m2_tick = 0;
static uint8_t m2_sine = 0;
static uint8_t m2_oscillate = 6; // * USER SETTING * The speed between stream oscillations, higher is slower, lower is faster
static CRGB m2_rgb = CRGB::White; // * USER SETTING * The RGB color of the single stream

// * USER SETTING * Mode 3 (Ghost) variables
static uint8_t m3_tick = 0;
static uint8_t m3_state = 0;
static uint8_t m3_ghostCycleTime = 7; // * USER SETTING * The amount of time to wait between ghost fades
static uint8_t m3_value1 = 255;
static uint8_t m3_value2 = 255;
static uint8_t m3_hue1 = 255; // * USER SETTING * The hue of stream 1, refer to the HSV color wheel for corresponding color
static uint8_t m3_hue2 = 255; // * USER SETTING * The hue of stream 2, refer to the HSV color wheel for corresponding color
static uint8_t m3_saturation1 = 0; // * USER SETTING * The saturation of stream 1, setting of 0 means it loses all its color and becomes white
static uint8_t m3_saturation2 = 0; // * USER SETTING * The saturation of stream 2, setting of 0 means it loses all its color and becomes white

// * USER SETTING * Mode 4 (Spectrum) variables
static uint8_t m4_tick = 0;
static uint8_t m4_sine = 0;
static uint8_t m4_oscillate = 2; // * USER SETTING * The speed between stream oscillations, higher is slower, lower is faster
static uint8_t m4_startHue1 = 0; // * USER SETTING * This is the starting color on the HSV color wheel for stream 1
static uint8_t m4_startHue2 = 85; // * USER SETTING * This is the starting color on the HSV color wheel for stream 2, note the 85 offset
static uint8_t m4_startHue3 = 170; // * USER SETTING * This is the starting color on the HSV color wheel for stream 3, note the 170 offset
static uint8_t m4_hueDelta = 0;

// * USER SETTING * Mode 5 (Volcanic Lightning) variables
static uint8_t m5_tick = 0;
static bool m5_flash = 0;
static uint8_t m5_flashChance = 100; // * USER SETTING * A random event with a 99% chance that there will be no lighting strike, higher means less likely, lower means more likely
static uint8_t m5_flashTime = 5; // * USER SETTING * How long the lightning flash stays lit for
static CRGB m5_leftRgb = CRGB::Red; // * USER SETTING * One LED strip is filled with RED, can be any other color
static CRGB m5_rightRgb = CRGB::Blue; // * USER SETTING * Other LED strip is filled with BLUE, can be any other color
static CRGB m5_flashRgb = CRGB::White; // * USER SETTING * Other LED strip is filled with BLUE, can be any other color

// * USER SETTING * Mode 6 (Chrome) variables
static uint8_t m6_tick = 0;
static uint8_t m6_cycleTime = 170; // * USER SETTING * Time between quadrant color changes
static uint8_t m6_blendSpeed = 20; // * USER SETTING * Speed of a quadrant color change
static uint8_t m6_randomQuadrant = 0;
static uint8_t m6_randomRgb = 0;
static CRGB m6_rgb1 = CRGB::Yellow; // * USER SETTING * A quadrant color
static CRGB m6_rgb2 = CRGB::Blue; // * USER SETTING * A quadrant color
static CRGB m6_rgb3 = CRGB::Green; // * USER SETTING * A quadrant color
static CRGB m6_rgb4 = CRGB::Red; // * USER SETTING * A quadrant color
static CRGB m6_blend[4];
static CRGB m6_targetRgb;

// * USER SETTING * Mode 7 (Vaporwave Aesthetics AKA NeoTokyo) variables
static uint8_t m7_tick = 0;
static uint8_t m7_cycleTime = 200; // * USER SETTING * Time between a color change
static uint8_t m7_delta = 0;
static uint8_t m7_change = 0;
static CRGB m7_rgb1 = CRGB(0, 0, 255); // * USER SETTING * Stream 1 color
static CRGB m7_rgb2 = CRGB(0, 255, 255); // * USER SETTING * Stream 2 color
static CRGB m7_rgb3 = CRGB(102, 0, 255); // * USER SETTING * Stream 3 color
static CRGB m7_rgb4 = CRGB(153, 0, 255); // * USER SETTING * Stream 4 color
static CRGB m7_rgb5 = CRGB(204, 0, 255); // * USER SETTING * Stream 5 color
static CRGB m7_targetRgb[5];

// * USER SETTING * Mode 8 (Unicorn) variables
static uint8_t m8_tick = 0;
static uint8_t m8_sine = 0;
static uint8_t m8_oscillate = 4; // * USER SETTING * The speed between stream oscillations, higher is slower, lower is faster

// * USER SETTING * Mode 9 (Northern Lights) variables
static uint8_t m9_tick = 0;
static uint8_t m9_sine = 0;
static uint8_t m9_oscillate = 10; // * USER SETTING * The speed between stream oscillations, higher is slower, lower is faster

// * USER SETTING * Mode 10 (Tropical) variables
static uint8_t m10_tick = 0;
static uint8_t m10_sine = 0;
static uint8_t m10_oscillate = 4; // * USER SETTING * The speed between stream oscillations, higher is slower, lower is faster
static uint8_t m10_hue = 0;

// * USER SETTING * Mode 11 (CUSTOM Vaporwave Aesthetics AKA NeoTokyo) variables
static uint8_t m11_tick = 0;
static uint8_t m11_cycleTime = 200; // * USER SETTING * Time between a color change
static uint8_t m11_delta = 0;
static uint8_t m11_change = 0;
static CRGB m11_rgb1 = CRGB(255, 0, 0); // * USER SETTING * Stream 1 color
static CRGB m11_rgb2 = CRGB(0, 255, 0); // * USER SETTING * Stream 2 color
static CRGB m11_rgb3 = CRGB(0, 0, 255); // * USER SETTING * Stream 3 color
static CRGB m11_rgb4 = CRGB(255, 255, 255); // * USER SETTING * Stream 4 color
static CRGB m11_rgb5 = CRGB::Yellow; // * USER SETTING * Stream 5 color
static CRGB m11_targetRgb[5];

// * USER SETTING * Mode 12 (Flux) variables
static unsigned long m12_previousSeconds = 0;
static unsigned long m12_currentSeconds = 0;
static unsigned long m12_cycleTime = 20; // * USER SETTING * The time between a mode change, higher means longer times
static int m12_mode = 0;
static uint8_t m12_numModes = 9; // * USER SETTING * The number of modes to cycle through, it will not cycle through the OFF, AMBIENT, and FLUX modes (it is zero indexed to Mode 2)
static bool m12_random = 0;

void loop() {

  int period; // General variable that holds the PERIOD of an LED strobe
  int on_time; // General variable that holds the amount of time LED stays ON
  int off_time; // General variable that holds the amount of time LED stays OFF
  unsigned long currentSeconds = millis()/1000; // The amount of seconds that has elapsed since Aquarius was turned on

  int mode = fixed_map(analogRead(A0), 0, 1023, 0, 12); // * USER SETTING * Number of modes, default of 12 (0-11)
  
  FastLED.setBrightness(255); // Set max brightness always
 
  // If water pump has been running for a certain time
  if ((unsigned long)((currentSeconds - previousSeconds) / 3600) >= shutdown_timer) {
    shutdown_timer_expired = 1; // Disable water pump and electromagnet because its been running too long
    turn_off();
    // Set track time for next event
    previousSeconds = currentSeconds;
  }
  // Reset the shutdown timer if the mode changes
  if (previousMode != mode) {
    shutdown_timer_expired = 0;
    previousMode = mode;
  }

  // This initial delay is required for some unknown reason, otherwise the electromagnet will abrubtly turn on
  if (initial_power_on == 0) {
    delay(3500); // * USER SETTING * Initial power on delay, default 3.5 seconds, probably should keep this intact
    initial_power_on = 1;
  }

  // If we aren't in Mode 0 or 1, then attempt to ramp up the pump and/or magnet if they are OFF
  if (mode != 0 && mode != 1 && shutdown_timer_expired == 0) {
    // If we are currently in OFF state
    if (powered_on == 0 || powered_on == 3) {
      turn_on(); // Ramp up the water pump
      powered_on = 1; // Advance the state machine from OFF to PUMP ON
    }
    // Else if we are currently in PUMP ON state
    else if (powered_on == 1) {
      // Step up the electromagnet every 4 counter ticks
      if (mag_tick++ >= 4) {
        OCR1B++;
        mag_tick = 0;
      }
      // If the duty cycle of the electromagnet exceeds magnet duty variable
      if (OCR1B >= mag_duty) {
        powered_on = 2; // Advance the state machine from PUMP ON to PUMP AND MAG ON
      }
    }
  }
  else {
    previousSeconds = currentSeconds; // In Modes 0 and 1, the pump is never engaged so don't track the shutdown timer
  }

  // This applies only for the RANDOM mode, if we aren't in mode 12, then do NOT randomize
  if (mode != 12) {
    m12_random = 0;
  }
  else {
    m12_random = 1;
  }

  /****************************************************************************************************
   * The code below can be modified by the user to achieve desired patterns and effects
   * It's highly recommended to not change Mode 0 (and Mode 1) as they are the only modes here
   *   that turn the pump and magnet OFF.
   * If you really want to change them, take caution of that fact that the water will evaporate faster
   *   and also lose itself due to micro splashing faster.  You don't want to inadvertently drain the
   *   water so fast that the pump will dry run.  If the pump dry runs too long, it may cause the motor
   *   to seize up which would render Aquarius inoperable.
   ****************************************************************************************************/

  /****************************************************************************************************
  * Mode 0: Everything is OFF
  *****************************************************************************************************/
  if (mode == 0) {
    turn_off();
    FastLED.clear(); // Clear all the LEDs
    FastLED.show();
  }

  /****************************************************************************************************
  * Mode 1: Ambience, Aquarius slowly dims in and out and acts as an ambient night light or accent/mood lighting.
  *****************************************************************************************************/  
  else if (mode == 1) {
    FastLED.setBrightness(130);
    turn_off();
    if (m1_tick++ >= m1_colorTime) {
      m1_hue++;
      m1_tick = 0;
    }
    fill_solid(leds, NUM_LEDS, CHSV(m1_hue, 255, 255)); // * USER SETTING * You can change the m1_hue value to a solid color of your choice
    FastLED.show();
  }
  
  /****************************************************************************************************
  * Mode 2: Helix
  *   This is a clean single white stream that oscillates up and down with the help of a sine function
  *   to modulate the periodicity of the LED strobing.
  *****************************************************************************************************/
  else if (mode == 2 || (m12_random == 1 && m12_mode == 0)) {
    if (m2_tick++ >= m2_oscillate) { // * USER SETTING * This value controls how fast the stream oscillates, higher is longer period
      m2_sine++;
      m2_tick = 0;
    }
    
    period = 12300 + 2 * sin8(m2_sine); // The sine function is what makes the stream oscillate up and down, was 12445
    
    // Note that for the following 2 user settings, the sum of the ON time and OFF time must be 100% or else you'll get issues
    //  Increasing the ON time (and subsequently, decreasing the OFF time) makes the stream WIDER but more "fuzzy"
    //  Increasing it also increases the power consumption, its recommended to NOT exceed 20% ON time or the voltage regulator
    //    may get hot.  The math is that each LED can consume upwards of 60 mA, so with 60 LEDs that means 3600 mA assuming
    //    100% ON time which far exceeds the power adapter and the voltage regulator (2A). 20% ON time brings this down to
    //    720 mA max current consumption on the LEDs alone which is right below the cusp of the total system limits.
    
    on_time = period * 0.05; // * USER SETTING * The 0.05 means that 5% of the whole period is devoted to the LED being ON
    off_time = period * 0.95; // * USER SETTING * The 0.95 means that 95% of the whole period is devoted to the LED being OFF

    fill_solid(leds, NUM_LEDS, m2_rgb); // * USER SETTING * Can change the color of the stream here
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time);
  }

  /****************************************************************************************************
  * Mode 3: Ghost
  *  This is a multi-stream pattern that demonstrates periodic fading in and fading out.  It uses a 
  *  rudimentary state machine to track the 5 different states of action:
  *  1. Both streams ON (starting state)
  *  2. Stream 1 fades out, Stream 2 ON
  *  3. Stream 2 fades out, Stream 1 ON
  *  4. Stream 1 fades in, Stream 2 ON
  *  5. Stream 2 fades in, Stream 1 ON
  *****************************************************************************************************/
  else if (mode == 3 || (m12_random == 1 && m12_mode == 1)) {
    period = 11000; // was 11150
    on_time = period/2 * 0.05;
    off_time = (period * 0.95)/2;

    fill_solid(leds, NUM_LEDS, CHSV(m3_hue1, m3_saturation1, m3_value1));
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time);
    fill_solid(leds, NUM_LEDS, CHSV(m3_hue2, m3_saturation2, m3_value2));
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time);
    if (m3_tick++ >= m3_ghostCycleTime) {
      if (m3_state == 0) { // Stream 1 fading out
        m3_value1--;
        if (m3_value1 == 0) { // Stream 1 completes fade out, next state is fade back in
          m3_state = 1;
          m3_tick = 0;
        }
      }
      else if (m3_state == 1) { // Stream 1 fading in
        m3_value1++;
        if (m3_value1 == 255) { // Stream 1 completes fade in, next state is stream 2 fades out
          m3_state = 2;
          m3_tick = 0;
        }
      }
      else if (m3_state == 2) { // Stream 2 fading out
        m3_value2--;
        if (m3_value2 == 0) { // Stream 2 completes fade out, next state is fade back in
          m3_state = 3;
          m3_tick = 0;
        }
      }
      else if (m3_state == 3) { // Stream 2 fading in
        m3_value2++;
        if (m3_value2 == 255) { // Stream 2 completes fade in, next state is stream 1 fades out
          m3_state = 0;
          m3_tick = 0;
        }
      }
    }
  }

  /****************************************************************************************************
  * Mode 4: Spectrum
  *   This is a triple stream display with a gradient fill running from the bottom to the top of each
  *   side of the LED strips.  Each of the 3 streams has a different starting color offset and
  *   will slowly shift over time to cover the entire HSV spectrum.  The gradient fill enables
  *   each stream to be multi-colored instead of a solid color (if you follow each stream from the bottom
  *   to the top, you'll notice its not 1 solid color).
  *****************************************************************************************************/
  else if (mode == 4 || (m12_random == 1 && m12_mode == 2)) {
    if (m4_tick++ >= m4_oscillate) {
      m4_sine++;
      m4_hueDelta++;
      m4_tick = 0;
    }
    period = 7730 + sin8(m4_sine); // was 8375
    on_time = period/3 * 0.05; // Divide the period by 3 because we want 3 streams
    off_time = (period * 0.95)/3; // Divide the period by 3 because we want 3 streams

    // We use LONGEST_HUES so that the gradient fill picks the longest path around the HSV color wheel (for maximum color dispersion)
    fill_gradient(leds, 0, CHSV(m4_startHue1 + m4_hueDelta, 255, 255), NUM_LEDS/2 - 1, CHSV(m4_hueDelta, 255, 255), LONGEST_HUES);
    fill_gradient(leds, NUM_LEDS - 1, CHSV(m4_startHue1 + m4_hueDelta, 255, 255), NUM_LEDS/2, CHSV(m4_hueDelta, 255, 255), LONGEST_HUES);
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time);
    fill_gradient(leds, 0, CHSV(m4_startHue2 + m4_hueDelta, 255, 255), NUM_LEDS/2 - 1, CHSV(m4_hueDelta, 255, 255), LONGEST_HUES);
    fill_gradient(leds, NUM_LEDS - 1, CHSV(m4_startHue2 + m4_hueDelta, 255, 255), NUM_LEDS/2, CHSV(m4_hueDelta, 255, 255), LONGEST_HUES);
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time);
    fill_gradient(leds, 0, CHSV(m4_startHue3 + m4_hueDelta, 255, 255), NUM_LEDS/2 - 1, CHSV(m4_hueDelta, 255, 255), LONGEST_HUES);
    fill_gradient(leds, NUM_LEDS - 1, CHSV(m4_startHue3 + m4_hueDelta, 255, 255), NUM_LEDS/2, CHSV(m4_hueDelta, 255, 255), LONGEST_HUES);
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time);
  }

  /****************************************************************************************************
  * Mode 5: Volcanic Lightning
  *   This is a single stream pattern where each LED strip is shown in a different solid color (RED/BLUE) and
  *   momentary flashes of white illuminate a second stream to simulate lightning strikes.  The different 
  *   left/right colors gives the single stream a unique appearance of taking on both hues at the same time.
  *****************************************************************************************************/
  else if (mode == 5 || (m12_random == 1 && m12_mode == 3)) {
    if (random8(0, m5_flashChance) >= 1 && m5_flash == 0) { // A pseudo-random chance of a lightning strike
      period = 12500; // was 12750
      on_time = period * 0.05;
      off_time = period * 0.95;

      fill_solid(leds, NUM_LEDS/2, m5_leftRgb); 
      fill_solid(leds + NUM_LEDS/2, NUM_LEDS/2, m5_rightRgb);
      FastLED.show();
      delayMicroseconds(on_time);
      FastLED.clear();
      FastLED.show();
      delayMicroseconds(off_time);
    }
    else { // The random lightning strike triggered
      if (m5_flash == 0) {
        m5_tick = 0;
      }
      period = 11400; // was 11500
      on_time = period/2 * 0.05;
      off_time = (period * 0.95)/2;
      fill_solid(leds, NUM_LEDS/2, m5_leftRgb);
      fill_solid(leds + NUM_LEDS/2, NUM_LEDS/2, m5_rightRgb);
      FastLED.show();
      delayMicroseconds(on_time);
      FastLED.clear();
      FastLED.show();
      delayMicroseconds(off_time);
      fill_solid(leds, NUM_LEDS, m5_flashRgb);
      FastLED.show();
      delayMicroseconds(on_time);
      FastLED.clear();
      FastLED.show();
      delayMicroseconds(off_time);
      m5_flash = 1;
      if (m5_tick++ >= m5_flashTime) { // How long the lightning strike stays lit for
        m5_flash = 0;
      }
    }
  }

  /****************************************************************************************************
  * Mode 6: Chrome
  *   This pattern enables 4 quadrants of colors that periodically change colors.  The LED ON time has
  *   also been increased to 18% to make it "fuzzy" looking.  The fuzziness of the stream combined with
  *   the 4 quadrants of color results in a chrome-like shimmer effect.
  *****************************************************************************************************/
  else if (mode == 6 || (m12_random == 1 && m12_mode == 4)) {
    if (m6_tick++ >= m6_cycleTime) {
      m6_tick = 0;
      m6_randomQuadrant = random8(0, 4);
      m6_randomRgb = random8(0, 4);
    }

    if (m6_randomRgb == 0) {
      m6_targetRgb = CRGB::Red;
    }
    else if (m6_randomRgb == 1) {
      m6_targetRgb = CRGB::Green;
    }
    else if (m6_randomRgb == 2) {
      m6_targetRgb = CRGB::Blue;
    }
    else if (m6_randomRgb == 3) {
      m6_targetRgb = CRGB::Yellow;
    }
    
    if (m6_randomQuadrant == 0) {
      m6_blend[0] = blend(m6_rgb1, m6_targetRgb, m6_blendSpeed);
      m6_rgb1 = m6_blend[0];
    }
    else if (m6_randomQuadrant == 1) {
      m6_blend[1] = blend(m6_rgb2, m6_targetRgb, m6_blendSpeed);
      m6_rgb2 = m6_blend[1];
    }
    else if (m6_randomQuadrant == 2) {
      m6_blend[2] = blend(m6_rgb3, m6_targetRgb, m6_blendSpeed);
      m6_rgb3 = m6_blend[2];
    }
    else if (m6_randomQuadrant == 3) {
      m6_blend[3] = blend(m6_rgb4, m6_targetRgb, m6_blendSpeed);
      m6_rgb4 = m6_blend[3];
    }
    
    period = 12500; // was 12600
    on_time = period * 0.18;
    off_time = period * 0.82;
    fill_solid(leds, NUM_LEDS/4, m6_rgb1);
    fill_solid(leds + 15, NUM_LEDS/4, m6_rgb2);
    fill_solid(leds + 30, NUM_LEDS/4, m6_rgb3);
    fill_solid(leds + 45, NUM_LEDS/4, m6_rgb4);
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time);
  }

  /****************************************************************************************************
  * Mode 7: Vaporwave Aesthetics AKA NeoTokyo Glitch
  *   Multi-stream back-to-back pattern that changes colors within a Vaporwave/NeoTokyo palette.
  *****************************************************************************************************/
  else if (mode == 7 || (m12_random == 1 && m12_mode == 5)) {
    if (m7_tick++ >= m7_cycleTime) {
      m7_change = random8(0, 6);
      m7_delta = random8(0, 6);
      m7_tick = 0;
    }
    
    if (m7_change == 0) {
      m7_targetRgb[0] = m7_rgb1;
      m7_targetRgb[1] = m7_rgb2;
      m7_targetRgb[2] = m7_rgb3;
      m7_targetRgb[3] = m7_rgb4;
      m7_targetRgb[4] = m7_rgb5;
      period = 8150; // was 8450
    }
    else if (m7_change == 1) {
      m7_targetRgb[0] = m7_rgb2;
      m7_targetRgb[1] = m7_rgb3;
      m7_targetRgb[2] = m7_rgb4;
      m7_targetRgb[3] = m7_rgb5;
      m7_targetRgb[4] = m7_rgb1;
      period = 8100; // was 8300
    }
    else if (m7_change == 2) {
      m7_targetRgb[0] = m7_rgb3;
      m7_targetRgb[1] = m7_rgb4;
      m7_targetRgb[2] = m7_rgb5;
      m7_targetRgb[3] = m7_rgb1;
      m7_targetRgb[4] = m7_rgb2;
      period = 8300; // was 8600
    }
    else if (m7_change == 3) {
      m7_targetRgb[0] = m7_rgb4;
      m7_targetRgb[1] = m7_rgb5;
      m7_targetRgb[2] = m7_rgb1;
      m7_targetRgb[3] = m7_rgb2;
      m7_targetRgb[4] = m7_rgb3;
      period = 8700; // was 8950
    }
    else if (m7_change == 4) {
      m7_targetRgb[0] = m7_rgb5;
      m7_targetRgb[1] = m7_rgb1;
      m7_targetRgb[2] = m7_rgb2;
      m7_targetRgb[3] = m7_rgb3;
      m7_targetRgb[4] = m7_rgb4;
      period = 7700; // was 7950
    }

    on_time = period/5 * 0.18;
    off_time = (period * 0.88)/5;

    if (m7_change == 5) {
      FastLED.clear();
      FastLED.show();
      delayMicroseconds(off_time/2 * m7_delta);
      fill_solid(leds, NUM_LEDS, m7_targetRgb[0]);
      FastLED.show();
      delayMicroseconds(on_time);
      fill_solid(leds, NUM_LEDS, m7_targetRgb[1]);
      FastLED.show();
      delayMicroseconds(on_time);
      fill_solid(leds, NUM_LEDS, m7_targetRgb[2]);
      FastLED.show();
      delayMicroseconds(on_time);
      fill_solid(leds, NUM_LEDS, m7_targetRgb[3]);
      FastLED.show();
      delayMicroseconds(on_time);
      fill_solid(leds, NUM_LEDS, m7_targetRgb[4]);
      FastLED.show();
      delayMicroseconds(on_time);
      FastLED.clear();
      FastLED.show();
      delayMicroseconds(off_time/2 * (5 - m7_delta));
    }
    else {
      FastLED.clear();
      FastLED.show();
      delayMicroseconds(off_time * m7_delta);
      fill_solid(leds, NUM_LEDS, m7_targetRgb[0]);
      FastLED.show();
      delayMicroseconds(on_time);
      fill_solid(leds, NUM_LEDS, m7_targetRgb[1]);
      FastLED.show();
      delayMicroseconds(on_time);
      fill_solid(leds, NUM_LEDS, m7_targetRgb[2]);
      FastLED.show();
      delayMicroseconds(on_time);
      fill_solid(leds, NUM_LEDS, m7_targetRgb[3]);
      FastLED.show();
      delayMicroseconds(on_time);
      fill_solid(leds, NUM_LEDS, m7_targetRgb[4]);
      FastLED.show();
      delayMicroseconds(on_time);
      FastLED.clear();
      FastLED.show();
      delayMicroseconds(off_time * (5 - m7_delta));
    }
  }

  /****************************************************************************************************
  * Mode 8: Unicorn
  *   A multi-stream pattern of back-to-back streams (no gaps).  This gives an effect where the stream
  *   is like a road with stripes painted across it.  Each of the stripes is clearly defined and visible
  *   but the multi-stream looks like 1 very large stream.  The colors are ROYGBIV (Red-Orange-Yellow-
  *   GREEN-BLUE-INDIGO-VIOLET).
  *****************************************************************************************************/
  else if (mode == 8 || (m12_random == 1 && m12_mode == 6)) {
    if (m8_tick++ >= m8_oscillate) {
      m8_sine++;
      m8_tick = 0;
    }
    period = 7300 + sin8(m8_sine); // was 7700
    on_time = period/7 * 0.05;
    off_time = (period * 0.95)/7;

    fill_solid(leds, NUM_LEDS, CRGB::Red);
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB::Orange);
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB::Yellow);
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB::Green);
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB::Blue);
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB::Indigo);
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB::Violet);
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time * 7);
  }

  /****************************************************************************************************
  * Mode 9: Northern Lights
  *   This is the same pattern as the UNICORN mode except it uses a different color palette.  This
  *   uses colors that simulate the Aurora Borealis/Northern Lights.
  *****************************************************************************************************/
  else if (mode == 9 || (m12_random == 1 && m12_mode == 7)) {
    if (m9_tick++ >= m9_oscillate) {
      m9_sine++;
      m9_tick = 0;
    }
    period = 6900 + sin8(m9_sine); // was 7200
    on_time = period/8 * 0.05;
    off_time = (period * 0.95)/8;

    fill_solid(leds, NUM_LEDS, CRGB(20, 232, 30));
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB(20, 232, 30));
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB(0, 255, 210));
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB(0, 234, 141));
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB(1, 126, 213));
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB(1, 126, 213));
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB(181, 61, 255));
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, CRGB(141, 0, 196));
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time*8);
  }

  /****************************************************************************************************
  * Mode 10: Tropical
  *   A 7-stream pattern where each stream cycles through the entire HSV color spectrum.  Each stream
  *   is also spaced apart from the other stream slightly creating a small gap.  This demonstrates
  *   the programmability of fine tuned individual streams.
  *****************************************************************************************************/
  else if (mode == 10 || (m12_random == 1 && m12_mode == 8)) {
    if (m10_tick++ >= m10_oscillate) {
      m10_sine++;
      m10_tick = 0;
      m10_hue++;
    }
    period = 2600 + sin8(m10_sine); // was 4050
    on_time = period/7 * 0.05;
    off_time = (period * 0.95)/7;

    fill_solid(leds, NUM_LEDS, CHSV(m10_hue, 255, 255));
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time * 0.3);
    fill_solid(leds, NUM_LEDS, CHSV(m10_hue + 36, 255, 255));
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time * 0.3);
    fill_solid(leds, NUM_LEDS, CHSV(m10_hue + 72, 255, 255));
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time * 0.3);
    fill_solid(leds, NUM_LEDS, CHSV(m10_hue + 108, 255, 255));
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time * 0.3);
    fill_solid(leds, NUM_LEDS, CHSV(m10_hue + 144, 255, 255));
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time * 0.3);
    fill_solid(leds, NUM_LEDS, CHSV(m10_hue + 180, 255, 255));
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time * 0.3);
    fill_solid(leds, NUM_LEDS, CHSV(m10_hue + 216, 255, 255));
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time * 5.2);
  }

  /****************************************************************************************************
  * Mode 11: CUSTOM Vaporwave Aesthetics AKA NeoTokyo Glitch
  *   Multi-stream back-to-back pattern that changes colors within a Vaporwave/NeoTokyo palette.
  *****************************************************************************************************/
  else if (mode == 11 || (m12_random == 1 && m12_mode == 9)) {
    if (m11_tick++ >= m11_cycleTime) {
      m11_change = random8(0, 6);
      m11_delta = random8(0, 6);
      m11_tick = 0;
    }
    
    if (m11_change == 0) {
      m11_targetRgb[0] = m11_rgb1;
      m11_targetRgb[1] = m11_rgb2;
      m11_targetRgb[2] = m11_rgb3;
      m11_targetRgb[3] = m11_rgb4;
      m11_targetRgb[4] = m11_rgb5;
      period = 8150; // was 8450
    }
    else if (m11_change == 1) {
      m11_targetRgb[0] = m11_rgb2;
      m11_targetRgb[1] = m11_rgb3;
      m11_targetRgb[2] = m11_rgb4;
      m11_targetRgb[3] = m11_rgb5;
      m11_targetRgb[4] = m11_rgb1;
      period = 8130; // was 8300
    }
    else if (m11_change == 2) {
      m11_targetRgb[0] = m11_rgb3;
      m11_targetRgb[1] = m11_rgb4;
      m11_targetRgb[2] = m11_rgb5;
      m11_targetRgb[3] = m11_rgb1;
      m11_targetRgb[4] = m11_rgb2;
      period = 8320; // was 8600
    }
    else if (m11_change == 3) {
      m11_targetRgb[0] = m11_rgb4;
      m11_targetRgb[1] = m11_rgb5;
      m11_targetRgb[2] = m11_rgb1;
      m11_targetRgb[3] = m11_rgb2;
      m11_targetRgb[4] = m11_rgb3;
      period = 8770; // was 8950
    }
    else if (m11_change == 4) {
      m11_targetRgb[0] = m11_rgb5;
      m11_targetRgb[1] = m11_rgb1;
      m11_targetRgb[2] = m11_rgb2;
      m11_targetRgb[3] = m11_rgb3;
      m11_targetRgb[4] = m11_rgb4;
      period = 7650; // was 7950
    }

    on_time = period/5 * 0.18;
    off_time = (period * 0.88)/5;

    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time * m11_delta);
    fill_solid(leds, NUM_LEDS, m11_targetRgb[0]);
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, m11_targetRgb[1]);
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, m11_targetRgb[2]);
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, m11_targetRgb[3]);
    FastLED.show();
    delayMicroseconds(on_time);
    fill_solid(leds, NUM_LEDS, m11_targetRgb[4]);
    FastLED.show();
    delayMicroseconds(on_time);
    FastLED.clear();
    FastLED.show();
    delayMicroseconds(off_time * (5 - m11_delta));
  }

  /****************************************************************************************************
  * Mode 12: Flux
  *   This mode cycles through all the other modes every 20 seconds or so.  It is meant to demo/showcase
  *   all the modes without any user interaction.  Adding or removing other modes/patterns requires that
  *   the numModes variable be updated too otherwise it might not cycle properly.
  *****************************************************************************************************/
  if (mode == 12) {
    m12_currentSeconds = millis()/1000;
    if ((unsigned long)(m12_currentSeconds - m12_previousSeconds) >= m12_cycleTime) {
      m12_mode++;
      if (m12_mode == m12_numModes) {
        m12_mode = 0;
      }
      m12_previousSeconds = m12_currentSeconds;
    }
  }
}


