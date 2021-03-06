//------------------------------------------------------------------------------
// Makelangelo - firmware for various robot kinematic models
// dan@marginallycelver.com 2013-12-26
// Copyright at end of file.  Please see
// http://www.github.com/MarginallyClever/makelangeloFirmware for more information.
//------------------------------------------------------------------------------


//------------------------------------------------------------------------------
// INCLUDES
//------------------------------------------------------------------------------
#include "configure.h"

#include <SPI.h>  // pkm fix for Arduino 1.5

#include "Vector3.h"
#include "sdcard.h"


//------------------------------------------------------------------------------
// GLOBALS
//------------------------------------------------------------------------------

// robot UID
int robot_uid = 0;

// description of the machine's position
Axis axies[NUM_AXIES];

// length of belt when weights hit limit switch
float calibrateRight  = 101.1;
float calibrateLeft   = 101.1;

// plotter position.
float feed_rate = DEFAULT_FEEDRATE;
float acceleration = DEFAULT_ACCELERATION;
float step_delay;

char absolute_mode = 1; // absolute or incremental programming mode?

// Serial comm reception
char serialBuffer[MAX_BUF + 1]; // Serial buffer
int sofar;                      // Serial buffer progress
long last_cmd_time;             // prevent timeouts
long line_number = 0;           // make sure commands arrive in order

float tool_offset[NUM_TOOLS][NUM_AXIES];
int current_tool = 0;




//------------------------------------------------------------------------------
// METHODS
//------------------------------------------------------------------------------


// returns angle of dy/dx as a value from 0...2PI
float atan3(float dy, float dx) {
  float a = atan2(dy, dx);
  if (a < 0) a = (PI * 2.0) + a;
  return a;
}


/**
 * @return switch state
 */
char readSwitches() {
#ifdef USE_LIMIT_SWITCH
  // get the current switch state
  return ( (digitalRead(LIMIT_SWITCH_PIN_LEFT) == LOW) | (digitalRead(LIMIT_SWITCH_PIN_RIGHT) == LOW) );
#else
  return 0;
#endif  // USE_LIMIT_SWITCH
}


/** 
 * feed rate is given in units/min and converted to cm/s
 */
void setFeedRate(float v1) {
  if ( feed_rate != v1 ) {
    feed_rate = v1;
    if (feed_rate > MAX_FEEDRATE) feed_rate = MAX_FEEDRATE;
    if (feed_rate < MIN_FEEDRATE) feed_rate = MIN_FEEDRATE;
#ifdef VERBOSE
    Serial.print(F("F="));
    Serial.println(feed_rate);
#endif
  }
}


void findStepDelay() {
  step_delay = 1000000.0f / DEFAULT_FEEDRATE;
}


/**
 * @param delay in microseconds
 */
void pause(long us) {
  delay(us / 1000);
  delayMicroseconds(us % 1000);
}


/**
 * print the current feed rate
 */
void printFeedRate() {
  Serial.print(F("F"));
  Serial.print(feed_rate);
  Serial.print(F("steps/s"));
}


/**
 * M101 Annn Tnnn Bnnn
 * Change axis A limits to max T and min B.
 * look for change to dimensions in command, apply and save changes.
 */
void parseLimits() {
  int axisNumber = parseNumber('A',-1);
  if(axisNumber==-1) return;
  if(axisNumber>=NUM_AXIES) return;
  
  float newT = parseNumber('T', axies[axisNumber].limitMax);
  float newB = parseNumber('B', axies[axisNumber].limitMin);
  boolean changed=false;
  
  if(!equalEpsilon(axies[axisNumber].limitMax,newT)) {
    axies[axisNumber].limitMax=newT;
    changed=true;
  }
  if(!equalEpsilon(axies[axisNumber].limitMin,newB)) {
    axies[axisNumber].limitMin=newB;
    changed=true;
  }
  if(changed==true) {
    saveLimits();
  }

  printConfig();
  /*
  float pos[NUM_AXIES];
  int i;
  for(i=0;i<NUM_AXIES;++i) {
    pos[i]=axies[i].pos;
  }
  teleport(pos);*/
}


/**
 * Test that IK(FK(A))=A
 */
void testKinematics() {
  long A[NUM_MOTORS], i, j;
  float axies1[NUM_AXIES];
  float axies2[NUM_AXIES];

  for (i = 0; i < 3000; ++i) {
    for (j = 0; j < NUM_AXIES; ++j) {
      axies1[j] = random(axies[j].limitMin, axies[j].limitMax);
    }

    IK(axies1, A);
    FK(A, axies2);
    
    for (j = 0; j < NUM_AXIES; ++j) {
      Serial.print('\t');
      Serial.print(AxisNames[j]);
      Serial.print(axies1[j]);
    }
    for (j = 0; j < NUM_MOTORS; ++j) {
      Serial.print('\t');
      Serial.print(MotorNames[j]);
      Serial.print(A[j]);
    }
    for (j = 0; j < NUM_AXIES; ++j) {
      Serial.print('\t');
      Serial.print(AxisNames[j]);
      Serial.print('\'');
      Serial.print(axies2[j]);
    }
    for (j = 0; j < NUM_AXIES; ++j) {
      Serial.print(F("\td"));
      Serial.print(AxisNames[j]);
      Serial.print('=');
      Serial.print(axies2[j]-axies1[j]);
    }
    Serial.println();
  }
}


/**
   Translate the XYZ through the IK to get the number of motor steps and move the motors.
   @input pos NUM_AXIES floats describing destination coordinates
   @input new_feed_rate speed to travel along arc
*/
void lineSafeInternal(float *pos, float new_feed_rate) {
  long steps[NUM_MOTORS + NUM_SERVOS];
  IK(pos, steps);

  int i;
  for(i=0;i<NUM_AXIES;++i) {
    axies[i].pos = pos[i];
  }
  
  feed_rate = new_feed_rate;
  motor_line(steps, new_feed_rate);
}


/**
   Move the pen holder in a straight line using bresenham's algorithm
   @input pos NUM_AXIES floats describing destination coordinates
   @input new_feed_rate speed to travel along arc
*/
void lineSafe(float *pos, float new_feed_rate) {
  float destination[NUM_AXIES];
  int i;
  for(i=0;i<NUM_AXIES;++i) {
    destination[i] = pos[i] - tool_offset[current_tool][i];
    // @TODO confirm destination is within max/min limits.
  }

#ifdef SUBDIVIDE_LINES
  // split up long lines to make them straighter
  float delta[NUM_AXIES];
  float start[NUM_AXIES];
  float temp[NUM_AXIES];
  float len=0;  
  for(i=0;i<NUM_AXIES;++i) {
    start[i] = axies[i].pos;
    delta[i] = destination[i] - start[i];
    len += delta[i] * delta[i];
  }

  len = sqrt(len);
  // @TODO what if some axies don't need subdividing?  like z axis on polargraph.
  int pieces = ceil(len * (float)SEGMENT_PER_CM_LINE );
  float a;
  long j;

  // draw everything up to (but not including) the destination.
  for (j = 1; j < pieces; ++j) {
    a = (float)j / (float)pieces;
    for(i=0;i<NUM_AXIES;++i) {
      temp[i] = delta[i] * a + start[i];
    }
    lineSafeInternal(temp, new_feed_rate);
  }
#endif

  // guarantee we stop exactly at the destination (no rounding errors).
  lineSafeInternal(destination, new_feed_rate);
}


/**
   This method assumes the limits have already been checked.
   This method assumes the start and end radius match.
   This method assumes arcs are not >180 degrees (PI radians)
   @input cx center of circle x value
   @input cy center of circle y value
   @input destination point where movement ends
   @input dir - ARC_CW or ARC_CCW to control direction of arc
   @input new_feed_rate speed to travel along arc
*/
void arc(float cx, float cy, float *destination, char clockwise, float new_feed_rate) {
  // get radius
  float dx = axies[0].pos - cx;
  float dy = axies[1].pos - cy;
  float sr = sqrt(dx * dx + dy * dy);

  // find angle of arc (sweep)
  float sa = atan3(dy, dx);
  float ea = atan3(destination[1] - cy, destination[0] - cx);
  float er = sqrt(dx * dx + dy * dy);

  float da = ea - sa;
       if (clockwise != 0 && da < 0) ea += 2 * PI;
  else if (clockwise == 0 && da > 0) sa += 2 * PI;
  da = ea - sa;
  float dr = er - sr;

  // get length of arc
  // float circ=PI*2.0*radius;
  // float len=theta*circ/(PI*2.0);
  // simplifies to
  float len1 = abs(da) * sr;
  float len = sqrt( len1 * len1 + dr * dr );

  int i, segments = ceil( len * SEGMENT_PER_CM_ARC );

  float n[NUM_AXIES], angle3, scale;
  float a, r;
  #if NUM_AXIES>2
  float sz = axies[2].pos;
  float z = destination[2];
  #endif
  
  for (i = 0; i <= segments; ++i) {
    // interpolate around the arc
    scale = ((float)i) / ((float)segments);

    a = ( da * scale ) + sa;
    r = ( dr * scale ) + sr;

    n[0] = cx + cos(a) * r;
    n[1] = cy + sin(a) * r;
    #if NUM_AXIES>2
    n[2] = ( z - sz ) * scale + sz;
    #endif
    // send it to the planner
    lineSafe(n, new_feed_rate);
  }
}


/**
 * Instantly move the virtual plotter position.  Does not check if the move is valid.
 */
void teleport(float *pos) {
  wait_for_empty_segment_buffer();

  int i;
  for(i=0;i<NUM_AXIES;++i) {
    axies[i].pos = pos[i];
  }
  
  long steps[NUM_MOTORS+NUM_SERVOS];
  IK(pos, steps);
  motor_set_step_count(steps);
}


/**
 * M100
 * Print a helpful message to serial.  The first line must never be changed to play nice with the JAVA software.
 */
void help() {
  Serial.print(F("\n\nHELLO WORLD! "));
  sayModelAndUID();
  sayFirmwareVersionNumber();
  Serial.println(F("== http://www.marginallyclever.com/ =="));
  Serial.println(F("M100 - display this message"));
  Serial.println(F("M101 [Tx.xx] [Bx.xx] [Rx.xx] [Lx.xx]"));
  Serial.println(F("       - display/update board dimensions."));
  Serial.println(F("As well as the following G-codes (http://en.wikipedia.org/wiki/G-code):"));
  Serial.println(F("G00,G01,G02,G03,G04,G28,G90,G91,G92,M18,M114"));
}


void sayModelAndUID() {
  Serial.print(F("I AM "));
  Serial.print(MACHINE_STYLE_NAME);
  Serial.print(F(" #"));
  Serial.println(robot_uid);
}


/**
 * D5
 * report current firmware version
 */
void sayFirmwareVersionNumber() {
  char versionNumber = loadVersion();

  Serial.print(F("Firmware v"));
  Serial.println(versionNumber, DEC);
}


/**
 * M114
 * Print the X,Y,Z, feedrate, acceleration, and home position
 */
void where() {
  wait_for_empty_segment_buffer();

  int i;
  for(i=0;i<NUM_AXIES;++i) {
    Serial.print(AxisNames[i]);
    Serial.print(axies[i].pos);
    Serial.print(' ');
  }

  printFeedRate();
  
  Serial.print(F(" A"  ));
  Serial.println(acceleration);
  
  for(i=0;i<NUM_AXIES;++i) {
    Serial.print('H');
    Serial.print(AxisNames[i]);
    Serial.print(axies[i].homePos);
    Serial.print(' ');
  }
  Serial.println();
}


/**
 * M102
 * Print the machine limits to serial.
 */
void printConfig() {
  int i;

  Serial.print(F("("));
  
  for(i=0;i<NUM_AXIES;++i) {
    Serial.print(axies[i].limitMin);
    if(i<NUM_AXIES-1)  Serial.print(',');
  }

  Serial.print(F(") - ("));
  
  for(i=0;i<NUM_AXIES;++i) {
    Serial.print(axies[i].limitMax);
    if(i<NUM_AXIES-1)  Serial.print(',');
  }

  Serial.print(F(")\n"));
}


/**
   Set the relative tool offset
   @input toolID the active tool id
   @input pos the offsets
*/
void set_tool_offset(int toolID, float *pos) {
  int i;

  for(i=0;i<NUM_AXIES;++i) {
    tool_offset[toolID][i] = pos[i];
  }
}


/**
 * @return the position + active tool offset
 */
void get_end_plus_offset(float *results) {
  int i;
  for(i=0;i<NUM_AXIES;++i) {
    results[i] = tool_offset[current_tool][i] + axies[i].pos;
  }
}


/**
 * M6 [Tnnn]
 * Change the currently active tool
 */
void toolChange(int tool_id) {
  if (tool_id < 0) tool_id = 0;
  if (tool_id >= NUM_TOOLS) tool_id = NUM_TOOLS - 1;
  current_tool = tool_id;
#ifdef HAS_SD
  if (sd_printing_now) {
    sd_printing_paused = true;
  }
#endif
}


/**
 * Look for character /code/ in the buffer and read the float that immediately follows it.
 * @return the value found.  If nothing is found, /val/ is returned.
 * @input code the character to look for.
 * @input val the return value if /code/ is not found.
 */
float parseNumber(char code, float val) {
  char *ptr = serialBuffer; // start at the beginning of buffer
  while ((long)ptr > 1 && (*ptr) && (long)ptr < (long)serialBuffer + sofar) { // walk to the end
    if (*ptr == code) { // if you find code on your walk,
      return atof(ptr + 1); // convert the digits that follow into a float and return it
    }
    ptr = strchr(ptr, ' ') + 1; // take a step from here to the letter after the next space
  }
  return val;  // end reached, nothing found, return default val.
}


/**
 * G4 [Snn] [Pnn]
 * Wait S milliseconds and P seconds.
 */
void parseDwell() {
  wait_for_empty_segment_buffer();
  float delayTime = parseNumber('S', 0) + parseNumber('P', 0) * 1000.0f;
  pause(delayTime);
}


/** 
 * G0-G1 [Xnnn] [Ynnn] [Znnn] [Unnn] [Vnnn] [Wnnn] [Ann] [Fnn]
 * straight lines
 */
void parseLine() {
  float offset[NUM_AXIES];
  get_end_plus_offset(offset);
  acceleration = min(max(parseNumber('A', acceleration), MIN_ACCELERATION), MAX_ACCELERATION);
  float f = parseNumber('F', feed_rate);
  
  int i;
  float pos[NUM_AXIES];
  for(i=0;i<NUM_AXIES;++i) {
    pos[i] = parseNumber(AxisNames[i], (absolute_mode ? offset[i] : 0)) + (absolute_mode ? 0 : offset[i]);
  }
  
  lineSafe( pos, f );
}


/** 
 * G2-G3 [Xnnn] [Ynnn] [Ann] [Fnn] [Inn] [Jnn]
 * arcs in the XY plane
 * @param clockwise (G2) 1 for cw, (G3) 0 for ccw
 */
void parseArc(int clockwise) {
  float offset[NUM_AXIES];
  get_end_plus_offset(offset);
  acceleration = min(max(parseNumber('A', acceleration), MIN_ACCELERATION), MAX_ACCELERATION);
  float f = parseNumber('F', feed_rate);
  
  int i;
  float pos[NUM_AXIES];
  for(i=0;i<NUM_AXIES;++i) {
    pos[i] = parseNumber(AxisNames[i], (absolute_mode ? offset[i] : 0)) + (absolute_mode ? 0 : offset[i]);
  }
  
  arc(parseNumber('I', (absolute_mode ? offset[0] : 0)) + (absolute_mode ? 0 : offset[0]),
      parseNumber('J', (absolute_mode ? offset[1] : 0)) + (absolute_mode ? 0 : offset[1]),
      pos,
      clockwise,
      f );
}


/**
 * G92 [Xnnn] [Ynnn] [Znnn] [Unnn] [Vnnn] [Wnnn]
 * Teleport mental position
 */
void parseTeleport() {
  float offset[NUM_AXIES];
  get_end_plus_offset(offset);
  
  int i;
  float pos[NUM_AXIES];
  for(i=0;i<NUM_AXIES;++i) {
    pos[i] = parseNumber(AxisNames[i], (absolute_mode ? offset[i] : 0)) + (absolute_mode ? 0 : offset[i]);
  }
  teleport(pos);
}


/**
 * G54-G59 [Xnnn] [Ynnn] [Znnn] [Unnn] [Vnnn] [Wnnn]
 * Adjust tool offset
 */
void parseToolOffset(int toolID) {
  int i;
  float offset[NUM_AXIES];
  for(i=0;i<NUM_AXIES;++i) {
    offset[i] = parseNumber(AxisNames[i], tool_offset[toolID][i]);
  }
  set_tool_offset(toolID,offset);
}

/**
 * @return 1 if CRC ok or not present, 0 if CRC check fails.
 */
char checkCRCisOK() {
  // is there a line number?
  long cmd = parseNumber('N', -1);
  if (cmd != -1 && serialBuffer[0] == 'N') { // line number must appear first on the line
    if ( cmd != line_number ) {
      // wrong line number error
      Serial.print(F("BADLINENUM "));
      Serial.println(line_number);
      return 0;
    }

    // is there a checksum?
    if (strchr(serialBuffer, '*') != 0) {
      // yes.  is it valid?
      char checksum = 0;
      int c = 0;
      while (serialBuffer[c] != '*' && c < MAX_BUF) checksum ^= serialBuffer[c++];
      c++; // skip *
      int against = strtod(serialBuffer + c, NULL);
      if ( checksum != against ) {
        Serial.print(F("BADCHECKSUM "));
        Serial.println(line_number);
        return 0;
      }
    } else {
      Serial.print(F("NOCHECKSUM "));
      Serial.println(line_number);
      return 0;
    }

    line_number++;
  }
  
  return 1;  // ok!
}

/**
 * M117 [string] 
 * Display string on the LCD panel.  Command is ignored if there is no LCD panel.
 */
void parseMessage() {
#ifdef HAS_LCD
  int i;
  // "M117 " is 5 characters long
  for(i=0;i<5;++i) {
    if(serialBuffer[i]==0) {
      // no message
      lcd_message[0]=0;
      return;  
    }
  }

  // preserve message for display
  for(i=0;i<LCD_MESSAGE_LENGTH;++i) {
    lcd_message[i] = serialBuffer[i+5];
    if(lcd_message[i]==0) break;
  }
#endif
}


/**
 * M226 P[a] S[b] 
 * Wait for pin a to be in state b (1 or 0).  if P or S are missing, wait for user to press click wheel on LCD
 * Command is ignored if there is no LCD panel (and no button to press)
 */
void pauseForUserInput() {
#ifdef HAS_LCD
  int pin = parseNumber('P', BTN_ENC);
  int newState = parseNumber('S', 1);
  newState = (newState==1)?HIGH:LOW;
  
  while(digitalRead(pin)!=newState) {
    SD_check();
    LCD_update();
  }
#endif
}


/**
 * process commands in the serial receive buffer
 */
void processCommand() {
  if (serialBuffer[0] == ';') return;  // blank lines
  if(!checkCRCisOK()) return;  // message garbled

  if (!strncmp(serialBuffer, "UID", 3)) {
    robot_uid = atoi(strchr(serialBuffer, ' ') + 1);
    saveUID();
  }

  long cmd;

  // M codes
  cmd = parseNumber('M', -1);
  switch (cmd) {
    case   6:  toolChange(parseNumber('T', current_tool));  break;
    case  17:  motor_engage();  break;
    case  18:  motor_disengage();  break;
    case 100:  help();  break;
    case 101:  parseLimits();  break;
    case 102:  printConfig();  break;
    case 110:  line_number = parseNumber('N', line_number);  break;
    case 114:  where();  break;
    case 117:  parseMessage();
    case 226:  pauseForUserInput();
    default:   break;
  }

  // G codes
  cmd = parseNumber('G', -1);
  switch (cmd) {
    case  0:
    case  1:  parseLine();  break;
    case  2:  parseArc(1);  break;  // clockwise
    case  3:  parseArc(0);  break;  // counter-clockwise
    case  4:  parseDwell();  break;
    case 28:  robot_findHome();  break;
    #if MACHINE_STYLE == POLARGRAPH
    case 29:  calibrateBelts();  break;
    #endif
    case 54:
    case 55:
    case 56:
    case 57:
    case 58:
    case 59:  parseToolOffset(cmd-54);  break;
    case 90:  absolute_mode = 1;  break; // absolute mode
    case 91:  absolute_mode = 0;  break; // relative mode
    case 92:  parseTeleport();  break;
    default:  break;
  }

  // machine style-specific codes
  cmd = parseNumber('D', -1);
  switch (cmd) {
    case  0:  jogMotors();  break;
//    case  3:  SD_ListFiles();  break;
    case  4:  SD_StartPrintingFile(strchr(serialBuffer, ' ') + 1);  break; // read file
    case  5:  sayFirmwareVersionNumber();  break;
    case  6:  parseSetHome();  break;
    case  7:  setCalibration();  break;
    case  8:  reportCalibration();  break;
    case  9:  saveCalibration();  break;
    case 10:  // get hardware version
              Serial.print(F("D10 V"));
              Serial.println(MACHINE_HARDWARE_VERSION);
              break;
#if MACHINE_STYLE == POLARGRAPH
    case 11:  makelangelo5Setup();  break;
    case 12:  recordHome();
#endif
#ifdef MACHINE_HAS_LIFTABLE_PEN
    case 13:  setPenAngle(parseNumber('Z',axies[2].pos));  break;
#endif
    default:  break;
  }
}


#if MACHINE_STYLE == POLARGRAPH
/**
 * D11 makelangelo 5 specific setup call
 */
void makelangelo5Setup() {
  // if you accidentally upload m3 firmware to an m5 then upload it ONCE with this line uncommented.
  float limits[NUM_AXIES*2];
  limits[0] = 320.5;
  limits[1] = -320.5;
  limits[2] = 500;
  limits[3] = -500;
  limits[4] = PEN_UP_ANGLE;
  limits[5] = PEN_DOWN_ANGLE;
  adjustDimensions(limits);
  
  calibrateLeft=1011;
  calibrateRight=1011;
  saveCalibration();

  float homePos[NUM_AXIES];
  homePos[0] = 0;
  homePos[1] = limits[2]-210.7;
  homePos[2] = 50;
  setHome(homePos);

}
#endif


/**
 * D6 [Xnnn] [Ynnn] [Znnn] [Unnn] [Vnnn] [Wnnn]
 * Set home position for each axis.
 */
void parseSetHome() {
  int i;
  float offset[NUM_AXIES];
  for(i=0;i<NUM_AXIES;++i) {
    offset[i] = parseNumber(AxisNames[i], axies[i].homePos);
  }
  setHome(offset);
}


/**
 * D0 [Lnn] [Rnn] [Unn] [Vnn] [Wnn] [Tnn]
 * Jog each motor nn steps.
 * I don't know why the latter motor names are UVWT.
 */
void jogMotors() {
  int i, j, amount;

  motor_engage();
  
  for (i = 0; i < NUM_MOTORS; ++i) {
    if (MotorNames[i] == 0) continue;
    amount = parseNumber(MotorNames[i], 0);
    if (amount != 0) {
      Serial.print(F("Moving "));
      Serial.print(MotorNames[i]);
      Serial.print(F(" ("));
      Serial.print(i);
      Serial.print(F(") "));
      Serial.print(amount);
      Serial.print(F(" steps. Dir="));
      Serial.print(motors[i].dir_pin);
      Serial.print(F(" Step="));
      Serial.print(motors[i].step_pin);
      Serial.print('\n');

      int x = amount < 0 ? HIGH  : LOW;
      digitalWrite(motors[i].dir_pin, x);

      amount = abs(amount);
      for (j = 0; j < amount; ++j) {
        digitalWrite(motors[i].step_pin, HIGH);
        digitalWrite(motors[i].step_pin, LOW);
        pause(step_delay);
      }
    }
  }
}


/**
 * D7 [Lnnn] [Rnnn]
 * Set calibration length of each belt
 */
void setCalibration() {
  calibrateLeft = parseNumber('L', calibrateLeft);
  calibrateRight = parseNumber('R', calibrateRight);
  reportCalibration();
}


/**
 * D8
 * Report calibration values for left and right belts
 */
void reportCalibration() {
  Serial.print(F("D8 L"));
  Serial.print(calibrateLeft);
  Serial.print(F(" R"));
  Serial.println(calibrateRight);
}


/**
 * Compare two floats to the first decimal place.
 * return true when abs(a-b)<0.1
 */
boolean equalEpsilon(float a, float b) {
  int aa = floor(a * 10);
  int bb = floor(b * 10);
  //Serial.print("aa=");        Serial.print(aa);
  //Serial.print("\tbb=");      Serial.print(bb);
  //Serial.print("\taa==bb ");  Serial.println(aa==bb?"yes":"no");

  return aa == bb;
}


void setHome(float *pos) {
  boolean changed=false;
  
  int i;
  for(i=0;i<NUM_AXIES;++i) {
    if(!equalEpsilon(axies[i].homePos,pos[i])) changed=true;
  }
  if(changed==true) {
    for(i=0;i<NUM_AXIES;++i) {
      axies[i].homePos = pos[i];
    }
    saveHome();
  }
}


/**
 * prepares the input buffer to receive a new message and tells the serial connected device it is ready for more.
 */
void parser_ready() {
  sofar = 0; // clear input buffer
  Serial.print(F("\n> "));  // signal ready to receive input
  last_cmd_time = millis();
}


/**
 * reset all tool offsets
 */
void tools_setup() {
  for (int i = 0; i < NUM_TOOLS; ++i) {
    for (int j = 0; j < NUM_AXIES; ++j) {
      tool_offset[i][j]=0;
    }
  }
}


/**
 * runs once on machine start
 */
void setup() {
  // start communications
  Serial.begin(BAUD);

  loadConfig();

  motor_setup();
  motor_engage();
  tools_setup();
  findStepDelay();

  //easyPWM_init();
  SD_init();
  LCD_init();

  // initialize the plotter position.
  float pos[NUM_AXIES];
  for(int i=0;i<NUM_AXIES;++i) {
    pos[i]=0;
  }
  if(NUM_AXIES>=3) pos[2]=PEN_UP_ANGLE;
  teleport(pos);
#ifdef MACHINE_HAS_LIFTABLE_PEN
  setPenAngle(PEN_UP_ANGLE);
#endif
  setFeedRate(DEFAULT_FEEDRATE);

  robot_setup();
  
  // display the help at startup.
  help();

  parser_ready();
}


/**
 * See: http://www.marginallyclever.com/2011/10/controlling-your-arduino-through-the-serial-monitor/
 */
void Serial_listen() {
  // listen for serial commands
  while (Serial.available() > 0) {
    char c = Serial.read();
    if (c == '\r') continue;
    if (sofar < MAX_BUF) serialBuffer[sofar++] = c;
    if (c == '\n') {
      serialBuffer[sofar] = 0;

      // echo confirmation
      //      Serial.println(F(serialBuffer));

      // do something with the command
      processCommand();
      parser_ready();
    }
  }
}


/**
 * main loop
 */
void loop() {
  Serial_listen();
  SD_check();
#ifdef HAS_LCD
  LCD_update();
#endif

  // The PC will wait forever for the ready signal.
  // if Arduino hasn't received a new instruction in a while, send ready() again
  // just in case USB garbled ready and each half is waiting on the other.
  if ( !segment_buffer_full() && (millis() - last_cmd_time) > TIMEOUT_OK ) {
    parser_ready();
  }

#if MACHINE_STYLE == ARM6
/*
  static int switchState=LOW;
  if(digitalRead(MOTOR_5_LIMIT_SWITCH_PIN)!=switchState) {
    switchState = !switchState;
    Serial.println(switchState?"ON":"OFF");
  }
  */
  /*
  Serial.print( digitalRead(MOTOR_0_LIMIT_SWITCH_PIN)==HIGH ? "1 " : "0 ");
  Serial.print( digitalRead(MOTOR_1_LIMIT_SWITCH_PIN)==HIGH ? "1 " : "0 ");
  Serial.print( digitalRead(MOTOR_2_LIMIT_SWITCH_PIN)==HIGH ? "1 " : "0 ");
  Serial.print( digitalRead(MOTOR_3_LIMIT_SWITCH_PIN)==HIGH ? "1 " : "0 ");
  Serial.print( digitalRead(MOTOR_4_LIMIT_SWITCH_PIN)==HIGH ? "1 " : "0 ");
  Serial.print( digitalRead(MOTOR_5_LIMIT_SWITCH_PIN)==HIGH ? "1 " : "0 ");
  Serial.println();*/
  
#endif
}


/**
   This file is part of makelangelo-firmware.

   makelangelo-firmware is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   makelangelo-firmware is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with DrawbotGUI.  If not, see <http://www.gnu.org/licenses/>.
*/
