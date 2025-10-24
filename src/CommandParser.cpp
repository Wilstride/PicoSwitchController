#include "CommandParser.h"
#include <cstring>
#include <cstdlib>
#include <cctype>
#include <cstdio>
#include "pico/stdlib.h"
#include "FastLogger.h"

CommandParser::CommandParser(SwitchBluetooth* switch_controller) : _switch(switch_controller) {}

bool CommandParser::parse_and_execute(const char* command_line) {
    // Skip leading whitespace
    const char* ptr = command_line;
    skip_whitespace(ptr);
    
    if (*ptr == '\0' || *ptr == '#') {
        return true; // Empty line or comment
    }
    
    // Fast command identification by first character to avoid string comparisons
    char first_char = toupper(*ptr);
    
    // Extract command
    char command[16]; // Reduced buffer size
    int i = 0;
    while (*ptr && !isspace(*ptr) && i < sizeof(command) - 1) {
        command[i++] = toupper(*ptr++);
    }
    command[i] = '\0';
    
    // Skip whitespace between command and args
    skip_whitespace(ptr);
    
    // Execute command using optimized switch
    switch (first_char) {
        case 'P':
            if (command[1] == 'R') { // "PRESS"
                return parse_press_command(ptr);
            }
            break;
        case 'H':
            if (command[1] == 'O') { // "HOLD" 
                return parse_button_command(ptr, true);
            }
            break;
        case 'R':
            if (command[1] == 'E') { // "RELEASE"
                return parse_button_command(ptr, false);
            }
            break;
        case 'S':
            if (command[1] == 'T') { // "STICK"
                return parse_stick_command(ptr);
            } else if (command[1] == 'L') { // "SLEEP"
                return parse_sleep_command(ptr);
            }
            break;
        default:
            FastLogger::log_fmt("Unknown command: %s", command);
            return false;
    }
    
    FastLogger::log_fmt("Unknown command: %s", command);
    return false;
}

bool CommandParser::parse_button_command(const char* args, bool pressed) {
    const char* ptr = args;
    char button_name[16]; // Reduced buffer size
    
    // Start consolidation to group all buttons in one HID report
    _switch->start_consolidation();
    
    // Parse multiple button names separated by spaces
    while (*ptr) {
        if (!parse_button_name(ptr, button_name, sizeof(button_name))) {
            break; // No more buttons to parse
        }
        
        _switch->set_button(button_name, pressed);
        
        // Skip to next button
        skip_whitespace(ptr);
    }
    
    // End consolidation and transmit as single frame
    _switch->end_consolidation();
    
    return true;
}

bool CommandParser::parse_press_command(const char* args) {
    // PRESS command using consolidation for maximum reliability
    const char* ptr = args;
    char button_name[16];
    
    // Start consolidation for press phase
    _switch->start_consolidation();
    
    // Press all buttons in same frame
    while (*ptr) {
        if (!parse_button_name(ptr, button_name, sizeof(button_name))) {
            break;
        }
        
        _switch->set_button(button_name, true);
        skip_whitespace(ptr);
    }
    
    // End press consolidation and transmit
    _switch->end_consolidation();
    
    // Start consolidation for release phase  
    _switch->start_consolidation();
    
    // Release all buttons in same frame
    ptr = args;  // Reset pointer
    while (*ptr) {
        if (!parse_button_name(ptr, button_name, sizeof(button_name))) {
            break;
        }
        
        _switch->set_button(button_name, false);
        skip_whitespace(ptr);
    }
    
    // End release consolidation and transmit
    _switch->end_consolidation();
    
    return true;
}

bool CommandParser::parse_stick_command(const char* args) {
    const char* ptr = args;
    skip_whitespace(ptr);
    
    // Parse stick name
    char stick_name[32];
    if (!parse_button_name(ptr, stick_name, sizeof(stick_name))) {
        FastLogger::log("Invalid stick name for STICK command");
        return false;
    }
    
    // Parse horizontal value
    float h, v;
    if (!parse_float(ptr, h)) {
        FastLogger::log("Invalid horizontal value for STICK command");
        return false;
    }
    
    // Parse vertical value
    if (!parse_float(ptr, v)) {
        FastLogger::log("Invalid vertical value for STICK command");
        return false;
    }
    
    // Use consolidation for stick commands too
    _switch->start_consolidation();
    _switch->set_stick(stick_name, h, v);
    _switch->end_consolidation();
    
    return true;
}

bool CommandParser::parse_sleep_command(const char* args) {
    const char* ptr = args;
    skip_whitespace(ptr);
    
    float duration;
    if (!parse_float(ptr, duration)) {
        // Use fast logger instead of blocking printf
        return false;
    }
    
    // Convert to milliseconds and set up non-blocking sleep
    uint32_t sleep_duration_ms = (uint32_t)(duration * 1000);
    _sleep_active = true;
    _sleep_end_time = to_ms_since_boot(get_absolute_time()) + sleep_duration_ms;
    
    return true;
}

bool CommandParser::is_sleeping() {
    return _sleep_active;
}

void CommandParser::update_sleep_state() {
    if (_sleep_active && to_ms_since_boot(get_absolute_time()) >= _sleep_end_time) {
        _sleep_active = false;
    }
}

void CommandParser::skip_whitespace(const char*& ptr) {
    while (*ptr && isspace(*ptr)) {
        ptr++;
    }
}

bool CommandParser::parse_float(const char*& ptr, float& value) {
    skip_whitespace(ptr);
    
    char* end_ptr;
    value = strtof(ptr, &end_ptr);
    
    if (ptr == end_ptr) {
        return false; // No digits found
    }
    
    ptr = end_ptr;
    return true;
}

bool CommandParser::parse_button_name(const char*& ptr, char* button_name, size_t max_len) {
    skip_whitespace(ptr);
    
    size_t i = 0;
    while (*ptr && !isspace(*ptr) && i < max_len - 1) {
        button_name[i++] = tolower(*ptr++);
    }
    button_name[i] = '\0';
    
    return i > 0;
}