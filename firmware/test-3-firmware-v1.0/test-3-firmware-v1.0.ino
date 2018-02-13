 /*
 * Test 3
 * Firmware version 1.0
 *
 * Test 3 is a voltage and current meter for Eurorack modular synthesisers.
 * Uses the custom board definition 'ATmega328P-A (8 Mhz Internal Clock)'.
 *
 * Joran van Gremberghe, Joranalogue Audio Design, http://joranalogue.com/
 * 2017-08-17, GPLv3
 */

#include <EEPROM.h> // Official Arduino EEPROM library by Christopher Andrews
#include <TimerOne.h> // TimerOne library by Paul Stoffregen
#include <SevenSeg.h> // SevenSeg library by Sigvald Marholm

// These values together define the firmware version number, shown on the display during startup. For example, major 1 and minor 0 together form '1.0'. Values above 9 are not possible.
#define versionMajor 1
#define versionMinor 0

// Minimum time needed to register a button 'hold' event, in ms. Default: 1000.
#define buttonHoldTime 1000

// Display polarity; 0 = common anode, 1 = common cathode. Default: 0.
#define dispPolarity 0

// I/O pins.
#define railButtonPin 0
#define modeButtonPin 1

#define dispSegmentAPin 2
#define dispSegmentBPin 3
#define dispSegmentCPin 4
#define dispSegmentDPin 5
#define dispSegmentEPin 6
#define dispSegmentFPin 7
#define dispSegmentGPin 8
#define dispSegmentDPPin 9

#define dispDigit1Pin 10
#define dispDigit2Pin 11
#define dispDigit3Pin 12
#define dispDigit4Pin 13
#define dispLEDPin 14 // PB6, used as external oscillator pin on Arduino Uno board.

// ADC pins. mA measurements translate directly, V measurements are 20 mV/bit.
#define adcPlus5V A0
#define adcPlus5mA A1
#define adcPlus12V A2
#define adcPlus12mA A3
#define adcMinus12V A4
#define adcMinus12mA A5

// Selected power rail, changed using rail button.
// 0 = +5
// 1 = +12
// 2 = -12
volatile byte rail;

// Selected measurement mode, changed using mode button.
// 0 = real-time voltage (V) measurement.
// 1 = real-time current (mA) measurement.
// 2 = stored peak current (mA) measurement.
volatile byte mode, previousMode;

volatile byte peakUpdate;

// Measured values for each rail are stored in an array. Index corresponds to mode.
// (SevenSeg library requires signed variables.)
int plus5Data[3];
int plus12Data[3];
int minus12Data[3];

// Display segment and digit pins.
SevenSeg disp(dispSegmentAPin, dispSegmentBPin, dispSegmentCPin, dispSegmentDPin, dispSegmentEPin, dispSegmentFPin, dispSegmentGPin);
const int digitPins[5] = {dispLEDPin, dispDigit1Pin, dispDigit2Pin, dispDigit3Pin, dispDigit4Pin};

// Varioubles: various variables.
volatile bool updateRailMemory, updateModeMemory, lastRailButtonState, lastModeButtonState, modeButtonHeld, peakResetCanceled, displayBlinking, displayBlinkState;
volatile unsigned long modeButtonPushTime;
volatile byte dispDigitCounter, buttonPollCounter;

void setup()
{
  // Avoid false button triggering on startup.
  lastRailButtonState = lastModeButtonState = HIGH;

  // Set I/O pins.
  pinMode(railButtonPin, INPUT);
  pinMode(modeButtonPin, INPUT);

  pinMode(adcPlus5V, INPUT);
  pinMode(adcPlus5mA, INPUT);
  pinMode(adcPlus12V, INPUT);
  pinMode(adcPlus12mA, INPUT);
  pinMode(adcMinus12V, INPUT);
  pinMode(adcMinus12mA, INPUT);

  disp.setDPPin(dispSegmentDPPin);
  disp.setDigitPins(5, digitPins);

  // Set display polarity (common anode by default).
  if (dispPolarity)
  {
    disp.setCommonCathode();
  }

  // Get the last used rail and mode from EEPROM.
  rail = EEPROM.read(0);
  mode = EEPROM.read(1);

  // Default to +12 in V mode when no stored configuration is found.
  if ((rail > 2) || (mode > 2))
  {
    rail = 1;
    mode = 0;
  }

  // Light up all LEDs for 500 ms.
  for(unsigned int i = 0; i < 50; i++)
  {
    for(int digit = 0; digit < 5; digit++)
    {
      disp.changeDigit(digit);
      disp.writeDigit(8);
      disp.setDP();

      delay(2);
    }
  }

  // Show version number and LED animation for 1 second.
  for(unsigned int i = 0; i < 100; i++)
  {
    disp.writeDigit(' ');
    disp.changeDigit(0);

    // LED animation.
    if (i > 10)
    {
      digitalWrite(dispSegmentAPin, dispPolarity);
    }

    if (i > 20)
    {
      digitalWrite(dispSegmentBPin, dispPolarity);
    }

    if (i > 30)
    {
      digitalWrite(dispSegmentCPin, dispPolarity);
    }

    if (i > 40)
    {
      digitalWrite(dispSegmentDPin, dispPolarity);
    }

    if (i > 50)
    {
      digitalWrite(dispSegmentEPin, dispPolarity);
    }

    if (i > 60)
    {
      digitalWrite(dispSegmentFPin, dispPolarity);
    }

    delay(2);

    // Version number.
    disp.changeDigit(1);
    disp.writeDigit('v');
    delay(2);

    disp.changeDigit(2);
    disp.writeDigit(versionMajor);
    disp.setDP();
    delay(2);

    disp.changeDigit(3);
    disp.writeDigit(versionMinor);
    delay(2);

    disp.changeDigit(4);
    disp.writeDigit(' ');
    delay(2);
  }

  // After startup, the display is driven by Timer1. 2 ms between digit updates and 5 digits (LEDs + 4 display digits) = total display refresh rate of 100 Hz.
  Timer1.initialize(2000);
  Timer1.attachInterrupt(timer1ISR);
}

// MAIN LOOP
// Perform measurements and update the EEPROM.
void loop()
{
  unsigned int plus5CurrentTopAverage;
  unsigned int plus5VoltTopAverage;
  unsigned int plus12CurrentTopAverage;
  unsigned int plus12VoltTopAverage;
  unsigned int minus12CurrentTopAverage;
  unsigned int minus12VoltTopAverage;

  // 1. PERFORM MEASUREMENTS
  for(unsigned int i = 0; i < 10; i++)
  {
    unsigned int plus5CurrentBottomAverage;
    unsigned int plus5VoltBottomAverage;
    unsigned int plus12CurrentBottomAverage;
    unsigned int plus12VoltBottomAverage;
    unsigned int minus12CurrentBottomAverage;
    unsigned int minus12VoltBottomAverage;

    // Read ADCs to bottom average variables.
    for(unsigned int j = 0; j < 10; j++)
    {
      plus5VoltBottomAverage += analogRead(adcPlus5V);
      plus5CurrentBottomAverage += analogRead(adcPlus5mA);
      plus12VoltBottomAverage += analogRead(adcPlus12V);
      plus12CurrentBottomAverage += analogRead(adcPlus12mA);
      minus12VoltBottomAverage += analogRead(adcMinus12V);
      minus12CurrentBottomAverage += 1023 - analogRead(adcMinus12mA); // -12 mA measurement is inverted: 0 is maximum.

      // Wait a bit.
      delay(1);
    }

    // Scale bottom average variables.
    plus5VoltBottomAverage /= 10;
    plus12VoltBottomAverage /= 10;
    minus12VoltBottomAverage /= 10;

    plus5CurrentBottomAverage /= 10.23;
    plus12CurrentBottomAverage /= 10.23;
    minus12CurrentBottomAverage /= 10.23;

    // Update +5 peak.
    if (plus5CurrentBottomAverage > plus5Data[2])
    {
      plus5Data[2] = plus5CurrentBottomAverage;
      peakUpdate = 10;
    }

    // Update +12 peak.
    if (plus12CurrentBottomAverage > plus12Data[2])
    {
      plus12Data[2] = plus12CurrentBottomAverage;
      peakUpdate = 10;
    }

    // Update -12 peak.
    if (minus12CurrentBottomAverage > minus12Data[2])
    {
      minus12Data[2] = minus12CurrentBottomAverage;
      peakUpdate = 10;
    }

    // Increment top voltage average variables.
    plus5VoltTopAverage += plus5VoltBottomAverage;
    plus5VoltBottomAverage = 0;
    plus12VoltTopAverage += plus12VoltBottomAverage;
    plus12VoltBottomAverage = 0;
    minus12VoltTopAverage += minus12VoltBottomAverage;
    minus12VoltBottomAverage = 0;

    // Increment top current average variables.
    plus5CurrentTopAverage += plus5CurrentBottomAverage;
    plus12CurrentTopAverage += plus12CurrentBottomAverage;
    minus12CurrentTopAverage += minus12CurrentBottomAverage;
    plus5CurrentBottomAverage = 0;
    plus12CurrentBottomAverage = 0;
    minus12CurrentBottomAverage = 0;
  }

  // Scale top average variables and store data in measurement array to be displayed.
  plus5VoltTopAverage /= 10;
  plus5Data[0] = plus5VoltTopAverage << 1;
  plus5VoltTopAverage = 0;

  plus12VoltTopAverage /= 10;
  plus12Data[0] = plus12VoltTopAverage << 1;
  plus12VoltTopAverage = 0;

  minus12VoltTopAverage /= 10;
  minus12Data[0] = minus12VoltTopAverage << 1;
  minus12VoltTopAverage = 0;

  plus5CurrentTopAverage /= 10;
  plus5Data[1] = plus5CurrentTopAverage;
  plus5CurrentTopAverage = 0;

  plus12CurrentTopAverage /= 10;
  plus12Data[1] = plus12CurrentTopAverage;
  plus12CurrentTopAverage = 0;

  minus12CurrentTopAverage /= 10;
  minus12Data[1] = minus12CurrentTopAverage;
  minus12CurrentTopAverage = 0;

  // 2. UPDATE THE EEPROM
  // Write the selected rail to the EEPROM if it has changed.
  if (updateRailMemory)
  {
    EEPROM.write(0, rail);
    updateRailMemory = LOW;
  }

  // Write the selected mode to the EEPROM if it has changed.
  if (updateModeMemory)
  {
    EEPROM.write(1, mode);
    updateModeMemory = LOW;
  }

  // Wait a bit.
  delay(1);

  // Change the display blinking state. Blink timing is controlled by the total measurement time (100 ms).
  displayBlinkState = !displayBlinkState;
}

// TIMER1 INTERRUPT SERVICE ROUTINE
// Update the display and poll the buttons.
void timer1ISR()
{
  // 1. UPDATE THE DISPLAY
  int dispValue;

  // Get the measurement data for the selected rail and mode.
  switch (rail)
  {
    case 0:
      dispValue = plus5Data[mode];
      break;

    case 1:
      dispValue = plus12Data[mode];
      break;

    case 2:
      dispValue = minus12Data[mode];
      break;
  }

  // Blink the display if the real-time measurement is overloaded (V >= 20.47 V, mA >= 1000).
  if (((mode == 0) && (dispValue >= 2046)) || ((mode == 1) && (dispValue >= 1000)))
  {
    displayBlinkState = LOW;
    displayBlinking = HIGH;
  }
  else if (mode < 2)
  {
    displayBlinking = LOW;
  }

  // Clear the display to avoid ghosting on the rail and mode LEDs.
  disp.writeDigit(' ');

  // Update the current digit.
  disp.changeDigit(dispDigitCounter);

  switch (dispDigitCounter)
  {
    // Digit 0 = rail and mode LEDs.
    case 0:
      switch (rail)
      {
        case 0:
          digitalWrite(dispSegmentAPin, dispPolarity);
          break;

        case 1:
          digitalWrite(dispSegmentBPin, dispPolarity);
          break;

        case 2:
          digitalWrite(dispSegmentCPin, dispPolarity);
          break;
      }

      switch (mode)
      {
        case 0:
          digitalWrite(dispSegmentDPin, dispPolarity);
          break;

        case 1:
          digitalWrite(dispSegmentEPin, dispPolarity);
          break;

        case 2:
          digitalWrite(dispSegmentEPin, dispPolarity);
          digitalWrite(dispSegmentFPin, dispPolarity);
          break;
      }

      if (peakUpdate > 0)
      {
        digitalWrite(dispSegmentFPin, dispPolarity);
        peakUpdate--;
      }
      break;

    // Digits 1-4: seven segment display. Extract the separate digits from the displayed measurement.
    case 1:
      if (!displayBlinking || displayBlinkState)
      {
        // Only show the digit if the display value is large enough...
        if (dispValue >= 1000)
        {
          dispValue /= 1000;
        }
        // ... otherwise, blank the digit (leading zero suppression).
        else
        {
          dispValue = ' ';
        }
      }
      else
      {
        dispValue = ' ';
      }

      disp.writeDigit(dispValue);
      break;

    case 2:
      if (!displayBlinking || displayBlinkState)
      {
        // Only show the digit if the display value is large enough...
        if (dispValue >= 100)
        {
          dispValue = (dispValue / 100) % 10;
        }
        // ... otherwise, blank the digit (leading zero suppression).
        else
        {
          if (mode == 0)
          {
            dispValue = 0;
          }
          else
          {
            dispValue = ' ';
          }
        }
      }
      else
      {
        dispValue = ' ';
      }

      disp.writeDigit(dispValue);
      break;

    case 3:
      if (!displayBlinking || displayBlinkState)
      {
        // Only show the digit if the display value is large enough...
        if (dispValue >= 10)
        {
          dispValue = (dispValue / 10) % 10;
        }
        // ... otherwise, blank the digit (leading zero suppression).
        else
        {
          if (mode == 0)
          {
            dispValue = 0;
          }
          else
          {
            dispValue = ' ';
          }
        }
      }
      else
      {
        dispValue = ' ';
      }

      disp.writeDigit(dispValue);
      break;

    case 4:
      if (!displayBlinking || displayBlinkState)
      {
        dispValue %= 10;
      }
      else
      {
        dispValue = ' ';
      }

      disp.writeDigit(dispValue);
      break;
  }

  // Show the decimal point on digit 2 in voltage measurement mode.
  if ((mode == 0) && (dispDigitCounter == 2) && (!displayBlinking || displayBlinkState))
  {
    disp.setDP();
  }
  else
  {
    disp.clearDP();
  }

  // Prepare the next digit.
  if (dispDigitCounter < 4)
  {
    dispDigitCounter++;
  }
  else
  {
    dispDigitCounter = 0;
  }

  // 2. POLL THE BUTTONS
  // Poll once every 10 Timer2 interrupt cycles (= 10 Hz), for debouncing.
  if (buttonPollCounter < 9)
  {
    buttonPollCounter++;
  }
  else
  {
    buttonPollCounter = 0;

    // Poll the rail button.
    if (digitalRead(railButtonPin) && (lastRailButtonState == LOW))
    {
      // Pushing and releasing the rail button while the mode button is held, cancels the peak reset...
      if (modeButtonHeld)
      {
        mode = previousMode;
        displayBlinking = LOW;
        peakResetCanceled = HIGH;
      }
      else
      // ...otherwise, cycle through the rails.
      {
        if (rail < 2)
        {
          rail++;
        }
        else
        {
          rail = 0;
        }

        updateRailMemory = HIGH;
      }

      lastRailButtonState = HIGH;
    }
    else if (!digitalRead(railButtonPin) && (lastRailButtonState == HIGH))
    {
      lastRailButtonState = LOW;
    }

    // Poll the mode button.
    if (digitalRead(modeButtonPin) && (lastModeButtonState == LOW))
    {
      // Pushing and releasing the mode button cycles through the measurement modes...
      if (!modeButtonHeld)
      {
        if (mode < 2)
        {
          mode++;
        }
        else
        {
          mode = 0;
        }

        updateModeMemory = HIGH;
      }
      /// ...holding it down and releasing resets the mA peak measurement.
      else if (!peakResetCanceled)
      {
        plus5Data[2] = 0;
        plus12Data[2] = 0;
        minus12Data[2] = 0;

        mode = previousMode;
        displayBlinking = LOW;
      }

      modeButtonHeld = LOW;
      lastModeButtonState = HIGH;
    }
    else if (!digitalRead(modeButtonPin) && (lastModeButtonState == HIGH))
    {
      // Store the time the mode button is pushed down, to detect 'hold' events.
      modeButtonHeld = LOW;
      lastModeButtonState = LOW;
      modeButtonPushTime = millis();
    }
    else if (!digitalRead(modeButtonPin) && ((millis() - modeButtonPushTime) >= buttonHoldTime) && !modeButtonHeld)
    {
      // Mode button is held down; waiting for the user to either release and reset the mA peak measurement or cancel by pressing the rail button.
      peakResetCanceled = LOW;
      previousMode = mode;
      mode = 2;

      modeButtonHeld = HIGH;
      displayBlinkState = LOW;
      displayBlinking = HIGH;
    }
  }
}

// This is just a test
