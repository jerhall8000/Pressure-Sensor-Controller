/**
   Code to control a pressure sensor and
   take samples, had an automated sleep function
   to allow long deployment times.
   Allows writing to Datalogger modem for storage
   and allows direct talk through to modem and
   pressure sensor via hardware serial

   Author: Jeremy Halligan and Christopher Liu
   Date: 1/19/2016
*/

// THINGS TO ADD
//-Fix Time Scale
//-Code in MAX3223 controls
//-Add in Aux

#include <SoftwareSerial.h>
#include <LowPower.h>
#include <SPI.h>
#include <RTC_DS3234.h>
#include <SD.h>
#include <EEPROM.h>

//_______________________SETTINGS______________________//


//----SET DATE AND TIME HERE----//
String DATESTAMP;
char DATE[11];
char TIME[] = "12:00:00";    //  24hr Format


//---------PIN SETUP-----------//
//  Comm Serial Port Pins (Hardware Serial)
#define  commTx 0
#define  commRx 1

//  Pin 2 for interrupt (0)
#define  intPin 2

// Aux Serial Port Pins (Software Serial)
#define  auxTx  3
#define  auxRx  4
SoftwareSerial auxSerial(auxTx, auxRx);

//  Modem Serial Port Pins (Software Serial)
//Works but dont know why. DO NOT CHANGE PINS2
#define  modemTx 6
#define  modemRx 5
SoftwareSerial modemSerial(modemTx, modemRx);

//  Pressure Sensor Serial Port Pins (Software Serial)
#define  pressTx 8
#define  pressRx 7
SoftwareSerial pressSerial(pressTx, pressRx);   //Sets up Software Serial for Pressure Sensor

#define  SD_CS  9     //CS pin for sdCard
#define  RTC_CS 10    //CS for RTC

// Switches
#define pressSwitch A0
#define SDSwitch    A1
#define MAX1Switch  A2    //******** NEED TO CODE IN COMM Switch
#define MAX2Switch  A3

//Battery Voltage
#define battV       A4

//Analog Device: BURNWIRE
#define analogSwitch A5


//---DEFAULT SAMPLING CONFIGURATIONS---//

unsigned int numSamples = 10;    //  Dictates how many samples will be taken each interval
unsigned int offCycles = 2;     //  # of cycles board is off for (1 cycle = 8 seconds)
unsigned int secondsBetweenSamples = 10; //seconds between samples
unsigned int waitTimeBoot = 10; //amount of seconds to wait before beginning sampling
const int menuTimeOut60 = 60; //seconds before returning to main menu in echo mode
const int menuTimeOut30 = 30; //time out in seconds for menu

//   Volatile Variables   //
volatile boolean allowInterrupts = 1; //device status flag, 1 allows interrupts, 0 does not allow
volatile boolean menuFlag = 0;        //flag to return to menu from the inside of wakeUpHandler
volatile boolean broke = 0;           //used to break out of functions
volatile boolean interruptFlag = 0;   //flag to show that device was interrupted, 0 for true
volatile int device_status = 0;       //0 for sleep, 1 for sampling, 2 for permanent sleep
volatile int seconds = 0;             //global counter, increments every second
volatile int intTime = 0;             //interrupt timer to prevent multiple interrupts
volatile int prevIntTime = 0;         //secondary interrupt timer

//    Variables    //
bool SDinit = 0;    //Tracks is SD card is initialized
int serialData = 0; //data that comes in on the Hardware serial port
int sampleNumber = 0; //  Keeps count of the samples
int sampleStamp; //index for sample number in set
char buffer1[7]; //holds incoming data from pressure sensor
String timeStamp; //holds timestring from RTC
String dataString; //holds the entire string to be printed to modem
int jIndex; //index for for loops
int index; // another index for loops
char commandString[12]; //string for date/time
char nowDate[12]; //string for setting date
char nowTime[9]; //string for setting time
long burntime = 900; //time for burnwire timeout

// EEPROM Addresses //
uint32_t timeT;
uint8_t time0;
uint8_t time1;
uint8_t time2;
uint8_t time3;

/**
   ISR for timer 1, increments in seconds.
*/
ISR(TIMER1_COMPA_vect) {
  seconds ++;
  if (seconds == 1000) {
    seconds = 0;
  }
}

// Create an RTC instance, using the chip select pin
RTC_DS3234 RTC(RTC_CS);


void setup() {
  Serial.begin(9600);      //serial comm with computer interface
  pressSerial.begin(9600); //serial comm with Pressure Sensor
  modemSerial.begin(9600); //serial comm with modem
  auxSerial.begin(9600);   //serial comm with Auxilary port

  SPI.begin();             //Initialize SPI and RTC for the RTC
  RTC.begin();

  pinMode(pressSwitch, OUTPUT);       //Set up control pins
  pinMode(MAX1Switch, OUTPUT);
  pinMode(MAX2Switch, OUTPUT);
  pinMode(SDSwitch, OUTPUT);
  pinMode(battV, INPUT);              //analog input for battery voltage
  pinMode(commRx, INPUT);             //Hardware Serial Setup
  pinMode(commTx, OUTPUT);
  pinMode(analogSwitch, OUTPUT);    //Analog Switch Pin: Burnwire

  digitalWrite(pressSwitch, LOW);
  digitalWrite(MAX1Switch, LOW);
  digitalWrite(MAX2Switch, LOW);
  digitalWrite(SDSwitch, LOW);
  digitalWrite(analogSwitch, LOW);

  timeCheck();
  Serial.println(unixRead());
  /**Initialize SD Card**/
  if (!SD.begin(SD_CS)) {
    Serial.println(F("Card failed"));
  }
  else {
    writeToSDCard(F("Initialized: "));                     // Prints Date and Time set
    timeStamp = getTime();
    writeToSDCard(timeStamp);
  }




  /** Timer 1 Start-up**/
  //   Timer1 initialization for awake time
  cli();
  TCCR1A = 0; //Set Timer 1 registers to 0
  TCCR1B = 0;
  // setCTC compare register
  OCR1A = 7632; //this value is approx. 1 second worth of clock cycles
  // CTC Mode for Timer 1
  TCCR1B |= (1 << WGM12);
  //Prescale the timer to 1024 prescaler
  TCCR1B |= (1 << CS10);
  TCCR1B |= (1 << CS12);
  //Enable timer compare interrupt
  TIMSK1 |= (1 << OCIE1A); //Starts timer 1 ********
  //turn back on global interrupts.
  sei();



  attachInterrupt(0, intPIN2, CHANGE); //attach interrupt to pin 2
}

void loop() {
  MainMenu(); //load into device menu
}



/*____________INTERRUPTS_________*/

/**
   ISR to detect interrupt from computer. Jump pin 3 to pin 0 on Arduino
*/
void intPIN2() {
  if (allowInterrupts) {
    //ISR
    intTime = millis();    //filter out interrupts triggered by noise from the first interrupt
    if (intTime - prevIntTime > 1000) {
      //Serial.println("INTERRUPT SUCESSFUL");     /////For debugging
      prevIntTime = intTime;
      interruptFlag = 1;
    }
  }
}

/**
   Returns true, sets interrupt flag to 0, and clears serial port if interrupt is detected
*/
boolean checkInterrupt() {
  if (interruptFlag) {
    //Serial.println("Interrupt Detected!");     /////For debugging
    interruptFlag = 0;
    serialData = 0; //erase serial port
    return true;
  }
  return false;
}


/*______INTERRUPT HANDLER______*/

/**
   Handles the waking up of the Arduino.

   Device Status Codes:
   0 - Sample Sleep
   1 - Sampling
   2 - Permanent Sleep, only wakes on interrupts

   Returns:
   True - go to main menu
   False - continue on in the code
*/

boolean wakeUpHandler() {
  while (Serial.available()) {
    Serial.read();
  }

  allowInterrupts = 0;       //Disable interrupts
  Serial.flush();
  serialData = 0;
  delay(10);

  if (device_status == 0 || device_status == 2) {    //Device was sleeping
    if (device_status == 0) {
      Serial.println(F("State=SAMPSLP"));
    }
    else {
      Serial.println(F("State=PERMSLP"));
    }
    Serial.println(F("Wake?"));
    seconds = 0;
    serialData = 0;
    while (!Serial.available() && seconds < 10) {} // Wait for characters on serial port and check time out
    delay(10);
    if (seconds >= 10) {   //For time out
      delay(5);
      Serial.println(F("Timed Out-Sleeping"));
      delay(10);
      return false;
    }
    serialData = Serial.read();

    //N or n: puts device back to sleep until interrupt
    if (serialData == 78 || serialData == 110) {     //  78 -> N, 110 -> n in ASCII
      Serial.println(F("Sleeping"));
      if (device_status == 2) {
        sleep(0);
      }
      return false;
    }

    //Y or y input: continue thru main loop
    else if (serialData == 89 || serialData == 121) {      //  89 -> Y, 121 -> y in ASCII
      Serial.println(F("Going to Menu"));
      menuFlag = 1;
      return true;
    }

    //Invalid Input: Prints invalid input and goes to sleep
    else if (serialData != 89 || serialData != 121 || serialData != 78 || serialData != 110) {
      Serial.println(F("Bad Input-Sleeping"));
      return false;
    }

  }

  else if (device_status == 1) {         //Device currently sampling
    Serial.println(F("State=SAMP"));
    Serial.println(F("Exit to Menu?"));

    seconds = 0;
    while (!Serial.available() && seconds < 10) {} // Wait for characters on serial port and check time out
    delay(10);
    if (seconds >= 10) {   //For time out
      //Serial.println(F("Timed Out-Sleeping"));
      return false;
    }
    serialData = Serial.read();

    //N or n: puts device back to sleep until interrupt
    if (serialData == 78 || serialData == 110) {     //  78 -> N, 110 -> n in ASCII
      Serial.println(F("Sampling"));
      return false;
    }

    //Y or y input: continue thru main loop
    else if (serialData == 89 || serialData == 121) {      //  89 -> Y, 121 -> y in ASCII
      digitalWrite(pressSwitch, LOW);   // turn off the pressure sensor
      Serial.println(F("Going to Menu"));
      menuFlag = 1;
      return true;
    }

    //Invalid Input: Prints invalid input and returns sampling
    else if (serialData != 89 || serialData != 121 || serialData != 78 || serialData != 110) {
      Serial.println(F("Bad Input-Sampling"));
      return false;
    }

  }

  else {
    Serial.println(F("State=??"));
    Serial.println(F("Going to Menu"));
    menuFlag = 1;
    return true;
  }

}



/*___________FUNCTIONS___________*/

/*__________ Enters into the main menu___________  */
void MainMenu() {
  while (true) {
    Serial.println();
    Serial.println(F("MAIN MENU"));
    Serial.println(F("1. Echo to Modem"));
    Serial.println(F("2. Echo to Press Sensor"));
    Serial.println(F("3. Start Sampling"));
    Serial.println(F("4. Board Status"));
    Serial.println(F("5. Sleep"));
    Serial.println(F("6. Test burn wire"));
    Serial.println(F("7. Settings\n"));
    delay(100);

    seconds = 0;      //reset seconds timer
    while (Serial.available()) {
      Serial.read();
    }

    allowInterrupts = 0;       //Disable interrupts
    Serial.flush();
    serialData = 0;
    delay(10);


    while (!Serial.available() && seconds < menuTimeOut30) {} // Wait for characters on serial port and check time out
    delay(10);
    if (seconds >= menuTimeOut30) {
      Serial.println(F("Timed out - Sampling"));
      sample();
      break;
    }
    serialData = Serial.read();
    switch (serialData) {
      case 49:       //1 Echo input to modem
        Serial.println(F("Opening Communications..."));
        echoModem();
        break;

      case 50:        //2 Echo input to Pressure Sensor
        echoPressure();
        break;

      case 51:        //3 Start taking samples
        Serial.println(F("Sampling"));
        delay(10);
        sample();
        break;

      case 52:
        delay(5);
        Serial.print(F("Battery Voltage: "));
        Serial.print(getBattVolt());
        Serial.println(" V");
        Serial.print(F("Voltage to ATMEGA328: "));
        Serial.print(readVcc());
        Serial.println(" V");
        Serial.print(F("Int. Temp on ATMEGA328: "));
        //        Serial.print(getIntTemp());
        Serial.println(" C");
        break;



      case 53:        //5 Go to permanent sleep or run if timed out
        delay(5);
        Serial.println(F("Sleeping"));
        delay(10);
        sleep(0);
        broke = 0;
        break;

      case 54:
        delay(5);
        seconds = 0;
        Serial.println(F("Burn Wire Test"));
        Serial.println(F("Press '^' to return to menu\n"));
        Serial.println(F("Press any key to trigger switch"));
        while (seconds < (burntime * 1000)) {
          serialData = 0;
          if (Serial.available()) {
            seconds = 0;
            serialData = Serial.read();
            if (serialData == 94) {
              break;
            }
            Serial.println(F("Switch turned HIGH"));
            digitalWrite(analogSwitch, HIGH);
            delay(5);
          }
        }
        Serial.println(F("Turning switch off"));
        digitalWrite(analogSwitch, LOW);
        delay(5);
        break;

      case 55:        //7 Go to settings menu
        settingsMenu();
        broke = 0;
        break;

      default:
        Serial.println(F("Bad Input-Sleeping"));
        sleep(0);
        broke = 0;
        break;
    }
    if (!menuFlag) {
      break;
    }
  }

}

void sample() {
  allowInterrupts = 1;  //Re-enable interrupts
  delay(10);

  while (true) {
    digitalWrite(MAX1Switch, HIGH);     //Turn on comm with pressure pin
    digitalWrite(pressSwitch, HIGH);   // turn on the pressure sensor
    delay(5);
    sampleNumber++;                 // increment the sample set number

    waitSeconds(waitTimeBoot);      //wait for pressure sensor voltage to stabilize
    //check for interrupt
    if (broke) {
      broke = 0;
      return;
    }

    allowInterrupts = 1;  //Re-enable interrupts

    //Takes N (numSamples) samples from Pressure Sensor, time stamps each one and stores into sampleData
    for (index = 0; index < numSamples; index++) {

      pressSerial.listen(); //enable pressure sensor serial port

      device_status = 1; //set device status to sampling

      //check for interrupts
      if (checkInterrupt()) {
        if (wakeUpHandler()) {
          return;
        }
      }
      allowInterrupts = 1;  //Re-enable interrupts

      pressSerial.flush();     //Flush the serial
      /**
         Send command to pressSerial port, which is 8(RX), 9 (TX)
         The pressure sensor command 'g' followed by a carriage return
         will trigger a new reading
      */
      pressSerial.print('*');
      pressSerial.print('G');
      pressSerial.print('\r');
      // Serial.println(F("Sample Requested."));  //For Debugging
      //clear the buffer
      for (jIndex = 0; jIndex < 6; jIndex++) {
        buffer1[jIndex] = ' ';
      }

      //get time from RTC
      dataString = getTime();

      seconds = 0;
      while (!pressSerial.available() && !checkInterrupt() && seconds < menuTimeOut30) {} // Wait for characters on serial port

      if (seconds >= 20) {
        writeToSDCard(F("No response from sensor."));
        delay(10);
        return;
      }

      if (checkInterrupt()) {
        if (wakeUpHandler()) {
          return;
        }
      }
      delay(100);

      //  Read in first 6 characters to buffer (ie. pressure values)
      for (jIndex = 0; jIndex < 6; jIndex++) {
        buffer1[jIndex] = pressSerial.read();
      }
      //Serial.println(F("Sample Taken."));  //For Debugging
      //   Concatenate into YY/MM/DD HH:MM:SS; PRESSURE; SAMPLESET#SAMPLE#
      //   Ex: "15/08/16 12:00:00; 102156; 11"
      dataString.concat(" ");
      delay(50);
      dataString.concat(buffer1);        //Pressure
      delay(150);
      //      dataString.concat(sampleNumber);   //Sample Set Number
      //      dataString.concat(sampleStamp);    //Individual Sample Number (per set)
      //      delay(150);

      //Writing time stamped data to the SD card
      writeToSDCard(dataString);

      //Writing data to the modem
      modemSerial.listen(); //enable modem serial port
      modemSerial.println(dataString); //print data to modem
      delay(100);

      waitSeconds(secondsBetweenSamples);  //Wait for 10 seconds until next sample

      //check for interrupt
      if (broke) {
        broke = 0;
        return;
      }
      allowInterrupts = 1;  //Re-enable interrupts

      pressSerial.flush();   //Flush serial for any misc. data

    };  //End of SAMPLE LOOP

    digitalWrite(pressSwitch, LOW);    //Turn off pressure pin
    digitalWrite(MAX1Switch, LOW);     //Turn off comm with pressure pin
    delay(5);

    unixWrite();

    if (checkInterrupt()) {         //check for an interrupt
      if (wakeUpHandler())
        return;
    }
    allowInterrupts = 1;  //Re-enable interrupts

    //Serial.print("Go to sleep for ");    /////For debugging
    //Serial.println((offCycles * 8) + 4);
    sleep(offCycles);              //Puts Arduino to Sleep for offCycles

    //check for interrupt
    if (broke) {
      broke = 0;
      return;
    }
    allowInterrupts = 1;  //Re-enable interrupts
  }
}


/**
   Puts arduino into Sleep Function.

   If cycles = 0, set device to permanent sleep, requires interrupt to wake up
   Else, set arduino to sleep for set number of 8 second cycles
*/
void sleep(int cycles) {
  delay(10);
  allowInterrupts = 1;
  delay(5);
  if (cycles == 0) {
    while (!broke) {
      device_status = 2;
      delay(2000);
      LowPower.powerDown(SLEEP_FOREVER, ADC_OFF, BOD_OFF);
      delay(2000);
      if (checkInterrupt()) {
        if (wakeUpHandler()) {
          broke = 1;
          delay(10);
          return;
        }
        allowInterrupts = 1;
      }
    }
  }
  else {
    device_status = 0;
    for (jIndex = 0; jIndex < cycles; jIndex++) {
      delay(2000);
      LowPower.powerDown(SLEEP_8S, ADC_OFF, BOD_OFF);
      delay(2000);
      if (checkInterrupt()) {
        if (wakeUpHandler()) {
          broke = 1;
          return;
          delay(10);
        }
        allowInterrupts = 1;
      }
    }
  }
  delay(10);
}

/** Retrieves time from RTC and returns it in YY/MM/DD HH:MM:SS format **/
String getTime() {
  DateTime now = RTC.now();     //gets time
  const int len = 32;
  static char buf[len];
  return now.toStringYMD(buf, len); //returns in format mentioned above
}

/** Retrieves date from RTC and returns it in YYMMDD format **/
String getDate() {
  DateTime now = RTC.now();     //gets time
  const int len = 32;
  static char buf[len];
  return now.toStringDate(buf, len); //returns in format mentioned above
}

/*  Delay the board a set number of seconds using timer 1  */
void waitSeconds(int timeSec) {
  allowInterrupts = 1;
  //  Serial.print(F("Delay for "));   ////For Debugging
  //  Serial.println(timeSec);      ////For Debugging
  device_status = 1;
  seconds = 0;
  int comp = timeSec;

  while (seconds < comp) {
    delay(5); //don't touch, works but don't know why
    if (checkInterrupt()) {
      if (wakeUpHandler()) {
        broke = 1;
        return;
      }
      allowInterrupts = 1;
    }
  }
}

//Enable talk through to modem
void echoModem() {

  digitalWrite(MAX1Switch, HIGH); //Turns on MAX3223 For communications
  delay(10);
  seconds = 0;

  Serial.println(F("Connected to MODEM"));
  Serial.println(F("Press '^' to return to Menu."));
  modemSerial.listen();     // Listen to the modem serial port

  Serial.flush();
  char dataIn = 'a';
  while (seconds < menuTimeOut60) {
    dataIn = 0;
    if (Serial.available()) {
      seconds = 0;
      dataIn = Serial.read();
      if (dataIn == 94) {
        break;
      }
      modemSerial.print(dataIn);
      Serial.print(dataIn);
    }

    if (modemSerial.available()) {
      seconds = 0;
      Serial.print((char)modemSerial.read());
    }
  }
  digitalWrite(MAX1Switch, LOW);  //Shuts off MAX3223
}

//Enable talk through to modem
void echoPressure() {

  digitalWrite(MAX1Switch, HIGH);  //Turns on MAX3223 For communications
  delay(10);

  seconds = 0; //reset the seconds counter

  Serial.println(F("Powering Sensor"));
  digitalWrite(pressSwitch, HIGH);   // turn on the pressure sensor
  delay(10);

  while (!digitalRead(pressSwitch)); //wait for pressure pin to go high

  Serial.println(F("Connected to PRESS SENSOR"));
  Serial.println(F("Press '^' to return to Menu."));
  delay(50);

  pressSerial.listen();

  Serial.flush();
  char dataIn = 'a';
  //continue as long as there is input
  while (seconds < menuTimeOut60) {
    dataIn = 0;
    if (Serial.available()) {
      seconds = 0;
      dataIn = Serial.read();
      if (dataIn == 94) {
        break;
      }
      pressSerial.print(dataIn);
      Serial.print(dataIn);
    }

    if (pressSerial.available()) {
      seconds = 0;
      Serial.print((char)pressSerial.read());
    }
  }
  digitalWrite(pressSwitch, LOW);   // turn off the pressure sensor
  digitalWrite(MAX1Switch, LOW);    // turn off MAX3223
}

/**
   Menu that allows changing the length of sleep/awake timers
*/
void settingsMenu() {
  boolean startOver = 1;
  char dataIn = 'a';

  while (startOver) {

    seconds = 0;      //reset seconds timer
    while (Serial.available()) {
      Serial.read();
    }
    Serial.flush(); //wait for Serial line to open
    serialData = 0; //clear serial data
    delay(10); //wait

    Serial.println(F("Settings:"));
    Serial.println(F("1. Initial Sampling Delay"));
    Serial.println(F("2. Samples per set"));
    Serial.println(F("3. Seconds between each sample"));
    Serial.println(F("4. Sleep cycles between sample sets"));
    Serial.println(F("5. Adjust Clock"));
    Serial.println(F("6. Initialize SD Card"));
    Serial.println(F("\n---- Press '^' to exit ----"));
    delay(100);

    while (!Serial.available() && seconds < menuTimeOut30); //wait for user input

    delay(10);
    if (seconds >= menuTimeOut30) {
      delay(5);
      Serial.println(F("Timed out-Sleeping"));
      delay(10);
      sleep(0);
      broke = 0;
      break;
    }

    serialData = Serial.read();
    switch (serialData) {
      case 49:       //edit waitTimeBoot
        Serial.print(F("Current Sampling Delay: "));
        Serial.println(waitTimeBoot);
        Serial.print(F("Enter new Sampling Delay: "));
        delay(100);

        while (!Serial.available());
        waitTimeBoot = Serial.parseInt();
        Serial.println();
        Serial.print(F("Updated Sampling Delay: "));
        Serial.println(waitTimeBoot);
        delay(100);
        break;

      case 50:        //adjust number of samples
        Serial.print(F("Current Samples/Set: "));
        Serial.println(numSamples);
        Serial.print(F("Enter new Samples/Set: "));
        delay(100);

        while (!Serial.available());
        numSamples = Serial.parseInt();
        Serial.println();
        Serial.print(F("Updated Samples/Set: "));
        Serial.println(numSamples);
        delay(100);
        break;

      case 51:        //edit secondsBetweenSamples
        Serial.print(F("Current seconds between samples: "));
        Serial.println(secondsBetweenSamples);
        Serial.print(F("Enter new time: "));
        delay(100);

        while (!Serial.available());
        secondsBetweenSamples = Serial.parseInt();
        Serial.println();
        Serial.print(F("Updated seconds between samples: "));
        Serial.println(secondsBetweenSamples);
        delay(100);
        break;

      case 52:   //edit Sleep Time
        Serial.print(F("Current number of 8 second cycles: "));
        Serial.println(offCycles);
        Serial.print(F("Enter new amount of cycles: "));
        delay(100);

        while (!Serial.available());
        offCycles = Serial.parseInt();
        Serial.println();
        Serial.print(F("Updated amount of cycles: "));
        Serial.println(offCycles);
        Serial.print(offCycles * 8);
        Serial.println(F(" seconds between each sample set"));
        delay(100);
        break;

      case 53:   //Adjust RTC
        timeStamp = getTime();
        Serial.print(F("Current Time is: "));
        Serial.println(timeStamp);
        Serial.println(F("Change current time? [Y/N]"));
        seconds = 0;
        while (!Serial.available() && seconds < 30) {} // Wait for characters on serial port and check time out
        if (seconds >= 30) {
          delay(5);
          Serial.println(F("Timed out"));
          break;
        }
        serialData = Serial.read();

        //N or n: Go back to Settings Menu
        if (serialData == 78 || serialData == 110) {     //  78 -> N, 110 -> n in ASCII
          Serial.println(F("Going to Settings Menu"));
          break;
        }

        //Y or y input: Change current Time
        else if (serialData == 89 || serialData == 121) {      //  89 -> Y, 121 -> y in ASCII
          Serial.println(F("Input date: Mon DD YYYY "));
          delay(5);
          parseSerial();
          for (index = 0; index < 11; index++) {
            nowDate[index] = commandString[index];
          }

          Serial.println(F("\nInput time: hh:mm:ss "));
          delay(5);
          parseSerial();
          for (index = 0; index < 8; index++) {
            nowTime[index] = commandString[index];
          }

          RTC.adjust(DateTime(nowDate, nowTime));   //sets the RTC to the date & time file was compiled
          Serial.print(F("Time : "));
          delay(10);
          Serial.println(getTime() + "\n");
          delay(10);
          unixWrite();
          break;
        }

        //Invalid Input: Go back to Settings Menu
        else if (serialData != 89 || serialData != 121 || serialData != 78 || serialData != 110) {
          Serial.println(F("\nBad Input"));
          delay(500);
          break;
        }

      case 54:
        cardInitialize();
        break;

      case 94:
        startOver = 0;
        break;

      default:
        Serial.println(F("Bad Input"));
        break;
    }
  }
}

void parseSerial() {
  for (index = 0; index < 12; commandString[index++] = 0x00);
  index = 0;
  Serial.flush();
  char dataIn = 'a';
  while (dataIn != 13 && index < 12) {
    while (!Serial.available());
    dataIn = Serial.read();
    commandString[index++] = dataIn;
    Serial.print(dataIn);

    if (index == 13) {
      Serial.println("Command cannot be more than 12 chars.");
      break;
    }

    if (dataIn == 13) {
      Serial.println();//new line
      commandString[index] = 0x00;//append null charater to end of string
      delay(100);
      break;
    }
  }
}

void writeToSDCard(String input) {
  DATESTAMP = getDate();
  DATESTAMP.concat(".txt");
  File dataFile = SD.open(DATESTAMP, FILE_WRITE);
  if (dataFile) {
    dataFile.println(input);
    Serial.println(input);
    dataFile.close();
  } else {
    Serial.println(F("Error printing to SD Card"));
    dataFile.close();
  }
}

//long getIntTemp () {
//  unsigned int wADC;
//  long t;
//  short c = 321.4;
//
//  // Set the internal reference and mux.
//  ADMUX = (_BV(REFS1) | _BV(REFS0) | _BV(MUX3));
//  ADCSRA |= _BV(ADEN);  // enable the ADC
//  delay(20);            // wait for voltages to become stable.
//  ADCSRA |= _BV(ADSC);  // Start the ADC
//
//  // Detect end-of-conversion
//  while (bit_is_set(ADCSRA, ADSC));
//  wADC = ADCW;
//  t = (wADC - c) / 1.22;  //Temp in C
//  return t;
//}

double getBattVolt () {
  long Vo;
  double Vin;
  Vo = analogRead(battV);
  Vin = ((Vo * (3.3 / 1023)) * 11); // Voltage divider btwn 1MOhm and 100kohm -> 11
  return Vin;
}

void cardInitialize() {
  /**Initialize SD Card**/
  if (!SD.begin(SD_CS)) {
    Serial.println(F("Card failed"));
  }
  else {
    writeToSDCard(F("Initialized: "));                     // Prints Date and Time set
    delay(10);
    writeToSDCard(F(__DATE__));
    writeToSDCard(F(__TIME__));

  }
}

double readVcc() {
  long result;
  // Read 1.1V reference against AVcc
  ADMUX = _BV(REFS0) | _BV(MUX3) | _BV(MUX2) | _BV(MUX1);
  delay(20); // Wait for Vref to settle
  ADCSRA |= _BV(ADSC); // Convert
  while (bit_is_set(ADCSRA, ADSC));
  result = ADCL;
  result |= ADCH << 8;
  result = 1126400L / result; // Back-calculate AVcc in mV
  double val = (result * 1.1) / 1100;
  return val;
}

template <class T> int EEPROM_writeAnything(int ee, const T& value)
{
  const byte* p = (const byte*)(const void*)&value;
  unsigned int i;
  for (i = 0; i < sizeof(value); i++)
    EEPROM.write(ee++, *p++);
  return i;
}

template <class T> int EEPROM_readAnything(int ee, T& value)
{
  byte* p = (byte*)(void*)&value;
  unsigned int i;
  for (i = 0; i < sizeof(value); i++)
    *p++ = EEPROM.read(ee++);
  return i;
}

void unixWrite() {
  DateTime now = RTC.now();
  timeT = now.unixtime();
  EEPROM_writeAnything(0, timeT);
}
//
uint32_t unixRead() {
  EEPROM_readAnything(0, timeT);
  return timeT;
}

void timeCheck() {
  Serial.print(F("Current Time: "));
  Serial.println(getTime());
  DateTime now = RTC.now();
  timeT = now.unixtime();
  uint8_t  timeT8 = ((timeT >> 24) & 0xFF);
  Serial.println(timeT8);
  uint32_t readClock = unixRead();
  uint8_t readClock8 = ((readClock >> 24) & 0xFF);
  Serial.println(readClock8);
  if (timeT8 < readClock8) {
    RTC.adjust(DateTime(readClock));
    Serial.println("Adjusted Time : " + getTime());
  }
}
