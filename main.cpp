#include "LCD_DISCO_F429ZI.h"
#include "mbed.h"
#include <time.h>
#include <cstring>
#include <cstdio>

// I2C and EEPROM related definitions
#define SDA_PIN       PC_9            // I2C data pin
#define SCL_PIN       PA_8            // I2C clock pin
#define EEPROM_ADDR   0xA0            // EEPROM device I2C address
#define TIME_STR_SIZE 20              // Size of time string "YYYY/MM/DD HH:MM:SS" (19 characters + NULL terminator)
#define LOG1_ADDR     0               // EEPROM starting address for the latest log record
#define LOG2_ADDR     32              // EEPROM starting address for the previous log record
constexpr int DEBOUNCE_TIME_MS = 200;

// Global objects
LCD_DISCO_F429ZI LCD;               // LCD display object
I2C i2c(SDA_PIN, SCL_PIN);            // I2C object for communication with EEPROM
Timeout debounce_user_button;
Timeout debounce_replayButton;
Timeout debounce_setTimeButton;
Timeout debounce_incrementButton;

// Button definitions using interrupts for asynchronous input
InterruptIn userButton(BUTTON1);       // Button for logging current time
InterruptIn replayButton(PE_6, PullUp);  // Button to switch between log display modes (or decrement in SET_TIME mode)
InterruptIn setTimeButton(PE_4, PullUp); // Button to enter time-setting mode or move to the next editable digit
InterruptIn incrementButton(PE_2, PullUp); // Button to increment the currently selected digit in time-setting mode

// Application state machine to manage different modes
enum AppState {
    IDLE,        // Idle state: display current time
    LOG_TIME,    // Log time state: save current time to EEPROM
    DISPLAY_LOG, // Display log state: show stored log records on LCD
    SET_TIME     // Time-setting state: allow user to adjust the system time
};
volatile AppState state = IDLE;   // Initialize to IDLE state

// Function declarations for EEPROM read/write
// Writes data to EEPROM. "address" is the I2C device address,
// "ep_address" is the internal EEPROM address, "data" is the buffer to write,
// and "size" is the number of bytes to write.
void WriteEEPROM(int address, unsigned int ep_address, char *data, int size);

// Reads data from EEPROM. "address" is the I2C device address,
// "ep_address" is the internal EEPROM address, "data" is the buffer to store read data,
// and "size" is the number of bytes to read.
void ReadEEPROM(int address, unsigned int ep_address, char *data, int size);

// Global variables for time setting and button events
volatile bool incrementPressed = false;      // Flag to indicate increment button pressed in SET_TIME state
volatile bool nextPositionPressed = false;     // Flag to indicate a request to move to the next editable digit
char editBuffer[TIME_STR_SIZE] = {0};            // Buffer holding the time string for editing (format: "YYYY/MM/DD HH:MM:SS")
int currentEditPos = 0;                         // Current index position in editBuffer that is being edited
volatile bool timeSetRequested = false;         // Flag indicating a request to enter time setting mode
volatile bool decrementPressed = false;          // Flag to indicate the decrement operation in SET_TIME mode

volatile bool user_button_debouncing = false;
volatile bool replayButton_debouncing = false;
volatile bool setTimeButton_debouncing = false;
volatile bool incrementButton_debouncing = false;

// Function declarations for various functionalities
void storeCurrentTime();           // Save the current system time to EEPROM
void displayLogs();                // Read and display stored log records from EEPROM on LCD
void updateDisplay();              // Update the LCD with the current system time and date
void updateSetTimeDisplay();       // Update the LCD with the time-setting interface
bool isEditablePosition(int pos);  // Check if a given index in the time string is editable (i.e., not a separator)
bool parseEditBufferToTm(const char *buffer, struct tm *timeinfo); // Convert the editBuffer string to a struct tm
const char* getCurrentFieldName(int pos); // Get the name of the time field (Year, Month, etc.) based on the current edit position
int getMaxDay(int month, int year);        // Get the maximum number of days in the given month (Note: February fixed at 28 days)
int my_strcmp(const char *s1, const char *s2); // Custom string comparison function, similar to strcmp
void adjustField(struct tm *timeinfo, int currentEditPos, int delta); // Adjust the corresponding field in tm by delta

void debounce_user_button_callback(){
    user_button_debouncing = false;
}

void debounce_replayButton_callback(){
    replayButton_debouncing = false;
}

void debounce_setTimeButton_callback(){
    setTimeButton_debouncing = false;
}

void debounce_incrementButton_callback(){
    incrementButton_debouncing = false;
}

// Return the maximum number of days in a given month (ignores leap year for February)
int getMaxDay(int month, int year) {
    switch(month) {
        case 1: case 3: case 5: case 7: case 8: case 10: case 12:
            return 31;
        case 4: case 6: case 9: case 11:
            return 30;
        case 2:
            return 28; // Fixed 28 days for February
        default:
            return 31;
    }
}

// Custom string comparison function similar to the standard strcmp.
// Returns 0 if strings are equal, otherwise returns the difference between the first differing characters.
int my_strcmp(const char *s1, const char *s2) {
    while (*s1 && *s2 && (*s1 == *s2)) {
        s1++;
        s2++;
    }
    return ((unsigned char)*s1 - (unsigned char)*s2);
}

// Adjusts the appropriate field (Year, Month, Day, Hour, Minute, or Second) in the tm structure based on the current edit position.
// "delta" indicates how much to change (positive to increase, negative to decrease).
void adjustField(struct tm *timeinfo, int currentEditPos, int delta) {
    // Retrieve the field name based on the current edit position.
    const char* field = getCurrentFieldName(currentEditPos);
    if (my_strcmp(field, "Year") == 0) {
        // tm_year stores the number of years since 1900.
        timeinfo->tm_year += delta;
    } else if (my_strcmp(field, "Month") == 0) {
        timeinfo->tm_mon += delta;
        // Ensure month cycles within the valid range (0-11)
        if (timeinfo->tm_mon > 11) timeinfo->tm_mon = 0;
        else if (timeinfo->tm_mon < 0) timeinfo->tm_mon = 11;
        // Adjust the day field if it exceeds the maximum days for the new month.
        int max_day = getMaxDay(timeinfo->tm_mon + 1, timeinfo->tm_year + 1900);
        if (timeinfo->tm_mday > max_day) {
            timeinfo->tm_mday = max_day;
        }
    } else if (my_strcmp(field, "Day") == 0) {
        int max_day = getMaxDay(timeinfo->tm_mon + 1, timeinfo->tm_year + 1900);
        timeinfo->tm_mday += delta;
        // Cycle the day within valid limits.
        if (timeinfo->tm_mday > max_day) timeinfo->tm_mday = 1;
        else if (timeinfo->tm_mday < 1) timeinfo->tm_mday = max_day;
    } else if (my_strcmp(field, "Hour") == 0) {
        timeinfo->tm_hour += delta;
        if (timeinfo->tm_hour > 23) timeinfo->tm_hour = 0;
        else if (timeinfo->tm_hour < 0) timeinfo->tm_hour = 23;
    } else if (my_strcmp(field, "Minute") == 0) {
        timeinfo->tm_min += delta;
        if (timeinfo->tm_min > 59) timeinfo->tm_min = 0;
        else if (timeinfo->tm_min < 0) timeinfo->tm_min = 59;
    } else if (my_strcmp(field, "Second") == 0) {
        timeinfo->tm_sec += delta;
        if (timeinfo->tm_sec > 59) timeinfo->tm_sec = 0;
        else if (timeinfo->tm_sec < 0) timeinfo->tm_sec = 59;
    }
}

// Interrupt handler for the userButton (BUTTON1)
// When triggered, if in IDLE state, switch to LOG_TIME state to log the current time.
void onUserButtonPressed() {
    if (user_button_debouncing){
        return;
    }
    user_button_debouncing = true;
    debounce_user_button.attach(&debounce_user_button_callback, std::chrono::milliseconds(DEBOUNCE_TIME_MS));

    if (state == IDLE) {
        state = LOG_TIME;
    }
}

// Interrupt handler for the replayButton (PE_6)
// In SET_TIME mode, this button acts as the "decrement" action;
// in other modes, it toggles between displaying logs and the normal IDLE display.
void onReplayButtonPressed() {
    if (replayButton_debouncing){
        return;
    }
    replayButton_debouncing = true;
    debounce_replayButton.attach(&debounce_replayButton_callback, std::chrono::milliseconds(DEBOUNCE_TIME_MS));

    if (state == SET_TIME) {
        decrementPressed = true;
    } else {
        if (state == IDLE) {
            state = DISPLAY_LOG;
        } else if (state == DISPLAY_LOG) {
            state = IDLE;
        }
    }
}

// Interrupt handler for the setTimeButton (PE_4)
// In IDLE state, it requests entry into the time setting mode;
// in SET_TIME mode, it signals to move to the next editable digit.
void onSetTimeButtonPressed() {
    if (setTimeButton_debouncing){
        return;
    }
    setTimeButton_debouncing = true;
    debounce_setTimeButton.attach(&debounce_setTimeButton_callback, std::chrono::milliseconds(DEBOUNCE_TIME_MS));

    if (state == IDLE) {
        timeSetRequested = true;
    } else if (state == SET_TIME) {
        nextPositionPressed = true;
    }
}

// Interrupt handler for the incrementButton (PE_2)
// This button increments the currently selected time digit while in SET_TIME mode.
void onIncrementButtonPressed() {
    if (incrementButton_debouncing){
        return;
    }
    incrementButton_debouncing = true;
    debounce_incrementButton.attach(&debounce_incrementButton_callback, std::chrono::milliseconds(DEBOUNCE_TIME_MS));

    if (state == SET_TIME) {
        incrementPressed = true;
    }
}

// Updates the LCD with the current system time and date.
// Retrieves the system time, formats it, and displays it on the LCD.
void updateDisplay() {
    time_t rawtime;
    time(&rawtime);                              // Get current time in seconds since epoch
    struct tm *timeinfo = localtime(&rawtime);     // Convert to local time representation

    // Extract individual time components
    int hour = timeinfo->tm_hour;
    int minute = timeinfo->tm_min;
    int second = timeinfo->tm_sec;
    int year = timeinfo->tm_year + 1900;           // tm_year stores years since 1900
    int month = timeinfo->tm_mon + 1;              // tm_mon is 0-indexed (0-11)
    int day = timeinfo->tm_mday;
    
    // Format the time string (hours, minutes, seconds)
    char formattedTime[30];
    sprintf(formattedTime, "%02d:%02d:%02d(H,M,S)", hour, minute, second);
    
    // Format the date string (year, month, day)
    char formattedDate[30];
    sprintf(formattedDate, "%04d/%02d/%02d(Y,M,D)", year, month, day);
    
    // Clear LCD and set properties before displaying
    LCD.Clear(LCD_COLOR_WHITE);
    LCD.SetFont(&Font20);
    //LCD.SetBackColor(LCD_COLOR_ORANGE);
    LCD.SetTextColor(LCD_COLOR_BLACK);
    
    // Display time at vertical position 80
    LCD.DisplayStringAt(0, 80, (uint8_t*)formattedTime, CENTER_MODE);
    // Display date at vertical position 110
    LCD.DisplayStringAt(0, 110, (uint8_t*)formattedDate, CENTER_MODE);
    
    //thread_sleep_for(200); // Delay for 0.1 second to update the display once per second (can use 1s if want)
}

// Saves the current system time to the EEPROM.
// It first backs up the previous log (LOG1) to LOG2, then writes the new log to LOG1.
void storeCurrentTime() {
    char newLog[TIME_STR_SIZE] = {0};
    char oldLog[TIME_STR_SIZE] = {0};
    // Read the current latest log from EEPROM (LOG1)
    ReadEEPROM(EEPROM_ADDR, LOG1_ADDR, oldLog, TIME_STR_SIZE);
    oldLog[TIME_STR_SIZE - 1] = '\0';
    if (oldLog[0] != '\0') {
        // If LOG1 is not empty, back it up to LOG2 (previous record)
        WriteEEPROM(EEPROM_ADDR, LOG2_ADDR, oldLog, TIME_STR_SIZE);
        //thread_sleep_for(10);
    }
    // Get the current system time and format it into a string
    time_t rawtime;
    time(&rawtime);
    strftime(newLog, TIME_STR_SIZE, "%Y/%m/%d %H:%M:%S", localtime(&rawtime));
    newLog[TIME_STR_SIZE - 1] = '\0';
    // Write the new log record to EEPROM at LOG1 address
    WriteEEPROM(EEPROM_ADDR, LOG1_ADDR, newLog, TIME_STR_SIZE);
    //thread_sleep_for(20);
}

// Reads two log records from the EEPROM and displays them on the LCD.
// The function tries to parse the logs; if parsing fails, the original string is used.
void displayLogs() {
    char log1[TIME_STR_SIZE] = {0};
    char log2[TIME_STR_SIZE] = {0};
    
    // Read two log records from EEPROM: LOG1 and LOG2
    ReadEEPROM(EEPROM_ADDR, LOG1_ADDR, log1, TIME_STR_SIZE);
    ReadEEPROM(EEPROM_ADDR, LOG2_ADDR, log2, TIME_STR_SIZE);
    log1[TIME_STR_SIZE - 1] = '\0';
    log2[TIME_STR_SIZE - 1] = '\0';

    char formattedLog1[TIME_STR_SIZE] = {0};
    char formattedLog2[TIME_STR_SIZE] = {0};
    int year, month, day, hour, minute, second;

    // Attempt to parse log1 string format using sscanf
    if (sscanf(log1, "%d/%d/%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
        sprintf(formattedLog1, "%04d/%02d/%02d %02d:%02d:%02d", 
                year, month, day, hour, minute, second);
    } else {
        // If parsing fails, copy the original log string
        for (int i = 0; i < TIME_STR_SIZE - 1 && log1[i] != '\0'; i++) {
            formattedLog1[i] = log1[i];
        }
        formattedLog1[TIME_STR_SIZE - 1] = '\0';
    }

    // Attempt to parse log2 string format
    if (sscanf(log2, "%d/%d/%d %d:%d:%d", &year, &month, &day, &hour, &minute, &second) == 6) {
        sprintf(formattedLog2, "%04d/%02d/%02d %02d:%02d:%02d", 
                year, month, day, hour, minute, second);
    } else {
        for (int i = 0; i < TIME_STR_SIZE - 1 && log2[i] != '\0'; i++) {
            formattedLog2[i] = log2[i];
        }
        formattedLog2[TIME_STR_SIZE - 1] = '\0';
    }
    
    // Clear LCD and set font for log display
    LCD.Clear(LCD_COLOR_WHITE);
    LCD.SetFont(&Font16);
    LCD.DisplayStringAt(0, LINE(2), (uint8_t *)"Time in:H,M,S", CENTER_MODE);
    LCD.DisplayStringAt(0, LINE(3), (uint8_t *)"Date in Y,M,D", CENTER_MODE);
    LCD.DisplayStringAt(0, LINE(5), (uint8_t *)"Latest:", CENTER_MODE);
    LCD.DisplayStringAt(0, LINE(7), (uint8_t *)formattedLog1, CENTER_MODE);
    LCD.DisplayStringAt(0, LINE(9), (uint8_t *)"Previous:", CENTER_MODE);
    LCD.DisplayStringAt(0, LINE(11), (uint8_t *)formattedLog2, CENTER_MODE);
    
    //thread_sleep_for(1000); // Display logs for 1 second
}

// Updates the LCD to show the time-setting interface.
// This includes the current editable time string and an underline indicating the current editable digit.
void updateSetTimeDisplay() {
    LCD.Clear(LCD_COLOR_WHITE);
    LCD.SetFont(&Font16);
    LCD.DisplayStringAt(0, LINE(1), (uint8_t *)"Set Time:", CENTER_MODE);

    // Try to parse the time data from editBuffer (expected format: "YYYY/MM/DD HH:MM:SS")
    int year, month, day, hour, minute, second;
    char formatted[TIME_STR_SIZE] = {0};
    if (sscanf(editBuffer, "%d/%d/%d %d:%d:%d", 
               &year, &month, &day, &hour, &minute, &second) == 6) {
        // Reformat into a fixed format ensuring leading zeros where necessary
        sprintf(formatted, "%04d/%02d/%02d %02d:%02d:%02d", 
                year, month, day, hour, minute, second);
    } else {
        // If parsing fails, simply copy the editBuffer to formatted
        for (int i = 0; i < TIME_STR_SIZE - 1 && editBuffer[i] != '\0'; i++) {
            formatted[i] = editBuffer[i];
        }
        formatted[TIME_STR_SIZE - 1] = '\0';
    }
    
    // Create a display buffer to show on the LCD, allowing us to mark the editable position
    char displayBuffer[TIME_STR_SIZE];
    for (int i = 0; i < TIME_STR_SIZE - 1 && formatted[i] != '\0'; i++) {
        displayBuffer[i] = formatted[i];
    }
    displayBuffer[TIME_STR_SIZE - 1] = '\0';
    
    // Mark the current editable digit with an underscore, if it is an editable position
    if (currentEditPos >= 0 && currentEditPos < TIME_STR_SIZE - 1) {
        displayBuffer[currentEditPos] = '_';
    }
    
    // Display the time string with the current edit indicator
    LCD.DisplayStringAt(0, LINE(3), (uint8_t *)displayBuffer, CENTER_MODE);

    // Display the name of the current field being edited (e.g., "Year", "Month")
    char hint[30];
    snprintf(hint, sizeof(hint), "Edit: %s", getCurrentFieldName(currentEditPos));
    LCD.DisplayStringAt(0, LINE(5), (uint8_t *)hint, CENTER_MODE);
}

// Checks whether a given position in the time string is editable (i.e., a digit rather than a separator).
bool isEditablePosition(int pos) {
    // The valid editable range is from index 0 to TIME_STR_SIZE-2 (since last index is '\0')
    if (pos < 0 || pos >= TIME_STR_SIZE - 1) return false;

    // Define positions where the character is a fixed separator ('/', space, or ':')
    int separators[] = {4, 7, 10, 13, 16};
    for (int i = 0; i < 5; i++) {
        if (pos == separators[i]) return false;
    }
    return true;
}

// Converts the time string stored in editBuffer to a struct tm.
// Returns true if the parsing is successful, false otherwise.
bool parseEditBufferToTm(const char *buffer, struct tm *timeinfo) {
    memset(timeinfo, 0, sizeof(struct tm)); // Clear the structure
    int year, mon, day, hour, min, sec;
    // Expecting the format "YYYY/MM/DD HH:MM:SS"
    if (sscanf(buffer, "%4d/%2d/%2d %2d:%2d:%2d", &year, &mon, &day, &hour, &min, &sec) != 6) {
        return false;
    }
    // Convert the values to the tm structure (note: tm_year is years since 1900 and tm_mon is 0-based)
    timeinfo->tm_year = year - 1900;
    timeinfo->tm_mon = mon - 1;
    timeinfo->tm_mday = day;
    timeinfo->tm_hour = hour;
    timeinfo->tm_min = min;
    timeinfo->tm_sec = sec;
    return true;
}

// Returns the name of the field corresponding to the current edit position.
// For example, positions 0-3 are for the "Year", 5-6 for the "Month", etc.
const char* getCurrentFieldName(int pos) {
    if (pos >= 0 && pos <= 3) return "Year";      // Characters 0-3 correspond to the year
    if (pos >= 5 && pos <= 6) return "Month";       // Characters 5-6 for month
    if (pos >= 8 && pos <= 9) return "Day";         // Characters 8-9 for day
    if (pos >= 11 && pos <= 12) return "Hour";      // Characters 11-12 for hour
    if (pos >= 14 && pos <= 15) return "Minute";    // Characters 14-15 for minute
    if (pos >= 17 && pos <= 18) return "Second";    // Characters 17-18 for second
    return "Unknown";
}

// EEPROM Write Function:
// Combines a 2-byte internal EEPROM address with the data to be written,
// then writes the full buffer to the EEPROM over I2C.
void WriteEEPROM(int address, unsigned int ep_address, char *data, int size) {
    // Optionally lock the I2C bus for atomic operations (commented out here)
    //i2c.lock();
    // Prepare a buffer that includes the 2-byte internal address followed by the data
    char i2cBuffer[size + 2];
    i2cBuffer[0] = (unsigned char)(ep_address >> 8);   // Most significant byte of internal address
    i2cBuffer[1] = (unsigned char)(ep_address & 0xFF);   // Least significant byte
    for (int i = 0; i < size; i++) {
        i2cBuffer[i + 2] = data[i];
    }
    int result = i2c.write(address, i2cBuffer, size + 2, false);
    thread_sleep_for(6);
    //i2c.unlock();
}

// EEPROM Read Function:
// First writes the 2-byte internal EEPROM address, then reads data from that location.
void ReadEEPROM(int address, unsigned int ep_address, char *data, int size) {
    //i2c.lock();
    char i2cBuffer[2];
    i2cBuffer[0] = (unsigned char)(ep_address >> 8);   // Internal address MSB
    i2cBuffer[1] = (unsigned char)(ep_address & 0xFF);   // Internal address LSB
    int result = i2c.write(address, i2cBuffer, 2, false);
    thread_sleep_for(6);
    i2c.read(address, data, size);
    thread_sleep_for(6);
    //i2c.unlock();
} 

// Main entry point of the program
int main() {
    // Bind button interrupts to their respective handler functions
    userButton.fall(&onUserButtonPressed);        // Trigger logging of current time on falling edge
    replayButton.fall(&onReplayButtonPressed);      // Trigger log display toggle or decrement action
    setTimeButton.fall(&onSetTimeButtonPressed);    // Enter time setting mode or move to next editable digit
    incrementButton.fall(&onIncrementButtonPressed);// Increment the current digit in SET_TIME mode

    // Initialize LCD display with initial settings
    LCD.Clear(LCD_COLOR_WHITE);
    LCD.SetFont(&Font20);
    //LCD.SetBackColor(LCD_COLOR_ORANGE);
    LCD.SetTextColor(LCD_COLOR_BLACK);

    // Set initial system time to January 1, 2025.
    tm t = {0};
    t.tm_year = 125; // 2025 (years since 1900)
    t.tm_mon = 0;    // January (months are 0-indexed)
    t.tm_mday = 1;
    set_time(mktime(&t));  // Convert tm to time_t and set the system time

    // Main application loop
    while(1) {
        // Check if time setting is requested while in IDLE mode.
        // If requested, initialize the edit buffer with the current time and switch to SET_TIME state.
        if (timeSetRequested && state == IDLE) {
            timeSetRequested = false;
            state = SET_TIME;
            time_t rawtime;
            time(&rawtime);
            struct tm *timeinfo = localtime(&rawtime);
            // Format the current system time into the editBuffer (ensuring proper format)
            strftime(editBuffer, TIME_STR_SIZE, "%Y/%m/%d %H:%M:%S", timeinfo);
            currentEditPos = 0;
            // Find the first editable digit by skipping non-editable separator positions
            while (currentEditPos < TIME_STR_SIZE - 1 && !isEditablePosition(currentEditPos)) {
                currentEditPos++;
            }
            updateSetTimeDisplay();
        }

        // Execute state-specific operations
        if (state == LOG_TIME) {
            storeCurrentTime();  // Save the current time into EEPROM
            state = IDLE;        // Return to IDLE state after logging
        } 
        if (state == DISPLAY_LOG) {
            displayLogs();       // Show the stored log records on the LCD
        } 
        if (state == SET_TIME) {
            // Handle increment operation: if the increment button was pressed
            if (incrementPressed) {
                incrementPressed = false;
                struct tm currentTime;
                if (parseEditBufferToTm(editBuffer, &currentTime)) {
                    // Increase the currently selected field by 1
                    adjustField(&currentTime, currentEditPos, 1);
                    // Reformat the new time into the editBuffer
                    strftime(editBuffer, TIME_STR_SIZE, "%Y/%m/%d %H:%M:%S", &currentTime);
                }
                updateSetTimeDisplay();
            }
            
            // Handle decrement operation: if the decrement flag was set (triggered by replayButton in SET_TIME)
            if (decrementPressed) {
                decrementPressed = false;
                struct tm currentTime;
                if (parseEditBufferToTm(editBuffer, &currentTime)) {
                    // Decrease the currently selected field by 1
                    adjustField(&currentTime, currentEditPos, -1);
                    strftime(editBuffer, TIME_STR_SIZE, "%Y/%m/%d %H:%M:%S", &currentTime);
                }
                updateSetTimeDisplay();
            }
            
            // Handle moving to the next editable digit position:
            // If the nextPosition flag is set, either save the new time or move to the next digit.
            if (nextPositionPressed) {
                nextPositionPressed = false;
                
                // If we are at the last editable position (the second digit of seconds)
                if (currentEditPos == 18) {
                    // Parse the edited time, set the system time, and exit SET_TIME mode
                    struct tm newTime;
                    if (parseEditBufferToTm(editBuffer, &newTime)) {
                        time_t newTimeT = mktime(&newTime);
                        set_time(newTimeT);
                    }
                    state = IDLE;
                } else {
                    // Otherwise, cycle to the next editable digit (skip over non-editable separator positions)
                    int startPos = currentEditPos;
                    do {
                        currentEditPos = (currentEditPos + 1) % (TIME_STR_SIZE - 1);
                        if (currentEditPos == startPos) break; // Prevent infinite loop if no editable position found
                    } while (!isEditablePosition(currentEditPos));
                }
                updateSetTimeDisplay();
            }
        }
        // In IDLE state, continuously update the display with the current time
        if (state == IDLE) {
            updateDisplay();
        }
        thread_sleep_for(50);  // Delay 50 ms in the main loop to reduce CPU load
    }
    return 0;
}
