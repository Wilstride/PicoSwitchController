#ifndef FastLogger_h
#define FastLogger_h

#include <stdint.h>

class FastLogger {
public:
    static void init();
    static void log(const char* message);
    static void log_fmt(const char* format, ...);
    static void flush_logs();
    static bool has_pending_logs();
    
private:
    static constexpr int BUFFER_SIZE = 2048;
    static constexpr int MAX_MESSAGE_LEN = 128;
    
    static char log_buffer[BUFFER_SIZE];
    static volatile int write_pos;
    static volatile int read_pos;
    static volatile bool buffer_full;
    
    static void add_message(const char* message);
    static bool get_next_message(char* output, int max_len);
};

#endif