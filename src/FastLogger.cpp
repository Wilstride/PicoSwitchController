#include "FastLogger.h"
#include <cstring>
#include <cstdio>
#include <cstdarg>

// Static member definitions
char FastLogger::log_buffer[FastLogger::BUFFER_SIZE];
volatile int FastLogger::write_pos = 0;
volatile int FastLogger::read_pos = 0;
volatile bool FastLogger::buffer_full = false;

void FastLogger::init() {
    write_pos = 0;
    read_pos = 0;
    buffer_full = false;
    memset(log_buffer, 0, BUFFER_SIZE);
}

void FastLogger::log(const char* message) {
    // Fast non-blocking logging - just queue the message
    add_message(message);
}

void FastLogger::log_fmt(const char* format, ...) {
    char temp_buffer[MAX_MESSAGE_LEN];
    
    va_list args;
    va_start(args, format);
    vsnprintf(temp_buffer, sizeof(temp_buffer), format, args);
    va_end(args);
    
    add_message(temp_buffer);
}

void FastLogger::add_message(const char* message) {
    int msg_len = strlen(message);
    if (msg_len >= MAX_MESSAGE_LEN - 1) {
        msg_len = MAX_MESSAGE_LEN - 2; // Leave room for newline and null terminator
    }
    
    // Calculate required space (message + newline + null terminator)
    int required_space = msg_len + 2;
    
    // Check if we have enough space
    int available_space;
    if (write_pos >= read_pos) {
        available_space = BUFFER_SIZE - write_pos + read_pos - 1;
    } else {
        available_space = read_pos - write_pos - 1;
    }
    
    if (required_space > available_space) {
        buffer_full = true;
        return; // Buffer full, drop message to avoid blocking
    }
    
    // Copy message to buffer
    for (int i = 0; i < msg_len; i++) {
        log_buffer[write_pos] = message[i];
        write_pos = (write_pos + 1) % BUFFER_SIZE;
    }
    
    // Add newline
    log_buffer[write_pos] = '\n';
    write_pos = (write_pos + 1) % BUFFER_SIZE;
    
    // Add null terminator for current message
    log_buffer[write_pos] = '\0';
    write_pos = (write_pos + 1) % BUFFER_SIZE;
}

bool FastLogger::get_next_message(char* output, int max_len) {
    if (read_pos == write_pos && !buffer_full) {
        return false; // No messages
    }
    
    int msg_pos = 0;
    while (read_pos != write_pos && msg_pos < max_len - 1) {
        char c = log_buffer[read_pos];
        read_pos = (read_pos + 1) % BUFFER_SIZE;
        
        if (c == '\0') {
            break; // End of message
        }
        
        output[msg_pos++] = c;
    }
    
    output[msg_pos] = '\0';
    buffer_full = false; // Clear full flag when we read a message
    
    return msg_pos > 0;
}

void FastLogger::flush_logs() {
    char message[MAX_MESSAGE_LEN];
    while (get_next_message(message, sizeof(message))) {
        printf("%s", message); // Send to USB serial for web terminal
    }
}

bool FastLogger::has_pending_logs() {
    return read_pos != write_pos || buffer_full;
}