#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdarg.h>
#include <signal.h>
#include <termios.h>
#include <sys/ioctl.h>
#include "ntp_client.h"

// Global variable declarations
static volatile int keep_running = 1;
static int term_width = 80;
static int term_height = 24;
volatile sig_atomic_t terminal_resized = 0;

// Buffer constants - keep for reference during refactoring
#define MAX_BUFFER_LINES 100
#define MAX_LINE_LENGTH 512

// ANSI escape codes
#define CLEAR_SCREEN "\x1b[2J"
#define CURSOR_HOME "\x1b[H"
#define HIDE_CURSOR "\x1b[?25l"
#define SHOW_CURSOR "\x1b[?25h"

// Default NTP server
#define DEFAULT_NTP_SERVER "pool.ntp.org"

#define CLOCK_HEIGHT 5

// Define a MIN macro for use in size calculations
#define MIN(a, b) ((a) < (b) ? (a) : (b))
// Track last drawn values to optimize partial updates
static int last_second = -1;
static int last_minute = -1;
static int last_hour = -1;
static int last_hundredths = -1;
static bool buffer_initialized = false;

// ANSI escape codes
#define ANSI_CLEAR_SCREEN "\x1b[2J"
#define ANSI_CURSOR_HOME "\x1b[H"
#define ANSI_CURSOR_POSITION "\x1b[%d;%dH"
#define ANSI_SAVE_CURSOR "\x1b[s"
#define ANSI_RESTORE_CURSOR "\x1b[u"

// Forward declarations
void clear_screen(void);
void set_cursor_position(int row, int col);
void draw_clock(time_t now);
void draw_full_clock(time_t now);
void draw_status_bar(time_t current_time, int time_since_sync);
void direct_draw_status_bar(time_t current_time, int time_since_sync);
void direct_clear_screen(void);
void direct_print(int row, int col, const char *format, ...);
void update_hundredths(int row, int col, int hundredths);
void show_message(const char *format, ...);
void draw_hundredths(int row, int col, int hundredths);
void update_terminal_size(void);
void handle_sigint(int sig);
void handle_sigwinch(int sig);
void init_terminal(void);
void restore_terminal(void);
int sync_with_ntp(void);

/**
 * Direct print to terminal at specified position
 */
void direct_print(int row, int col, const char *format, ...) 
{
    va_list args;
    va_start(args, format);
    
    char temp_buffer[MAX_LINE_LENGTH];
    int ret = vsnprintf(temp_buffer, sizeof(temp_buffer), format, args);
    if (ret >= (int)sizeof(temp_buffer)) {
        // Truncation occurred
        temp_buffer[sizeof(temp_buffer) - 1] = '\0';
    }
    
    // Set cursor position
    set_cursor_position(row, col);
    
    // Output directly to terminal
    printf("%s", temp_buffer);
    fflush(stdout);
    
    va_end(args);
}

/**
 * Update the hundredths display without redrawing the entire clock
 */
void update_hundredths(int row, int col, int hundredths)
{
    // Format the hundredths display text with colors
    char hundredths_buffer[40]; // Buffer for the hundredths display
    memset(hundredths_buffer, 0, sizeof(hundredths_buffer));
    
    // Dark gray dot
    strcat(hundredths_buffer, "\x1b[90m");
    strcat(hundredths_buffer, ".");
    strcat(hundredths_buffer, "\x1b[0m");
    
    // Bright red hundredths
    strcat(hundredths_buffer, "\x1b[91m");
    char temp[16];
    snprintf(temp, sizeof(temp), "%01d", hundredths);
    strcat(hundredths_buffer, temp);
    strcat(hundredths_buffer, "\x1b[0m");
    
    // White "UTC" text
    strcat(hundredths_buffer, "\x1b[97m");
    strcat(hundredths_buffer, " UTC");
    strcat(hundredths_buffer, "\x1b[0m");
    
    // Print directly to the screen at the specified position
    direct_print(row, col, "%s", hundredths_buffer);
}

/**
 * Clear screen and position cursor at top
 */
void clear_screen(void) 
{
    printf("%s%s", CLEAR_SCREEN, CURSOR_HOME);
    fflush(stdout);
}

/**
 * Directly clear screen without using buffer
 */
void direct_clear_screen(void) {
    printf("%s%s", CLEAR_SCREEN, CURSOR_HOME);
    fflush(stdout);
}

// Set the cursor position
void set_cursor_position(int row, int col) 
{
    printf(ANSI_CURSOR_POSITION, row, col);
}

/**
 * Show a temporary message directly on screen
 */
void show_message(const char *format, ...) 
{
    va_list args;
    va_start(args, format);
    
    char message[MAX_LINE_LENGTH];
    vsnprintf(message, sizeof(message), format, args);
    
    // Display message at the top of the screen
    direct_print(1, 1, "%s", message);
    
    va_end(args);
}

const char* DIGIT_ART[10][5] =
{
    {
        " ████ ",
        "██  ██",
        "██  ██",
        "██  ██",
        " ████ "
    },
    {
        "  ██  ",
        " ███  ",
        "  ██  ",
        "  ██  ",
        " ████ "
    },
    {
        " ████ ",
        "    ██",
        " ████ ",
        "██    ",
        "██████"
    },
    {
        " ████ ",
        "    ██",
        " ████ ",
        "    ██",
        " ████ "
    },
    {
        "██  ██",
        "██  ██",
        "██████",
        "    ██",
        "    ██"
    },
    {
        "██████",
        "██    ",
        "██████",
        "    ██",
        "██████"
    },
    {
        " ████ ",
        "██    ",
        "██████",
        "██  ██",
        " ████ "
    },
    {
        "██████",
        "    ██",
        "   ██ ",
        "  ██  ",
        " ██   "
    },
    {
        " ████ ",
        "██  ██",
        " ████ ",
        "██  ██",
        " ████ "
    },
    {
        " ████ ",
        "██  ██",
        " █████",
        "    ██",
        " ████ "
    }
};

const char* COLON_ART[5] = 
{
    "  ",
    "██",
    "  ",
    "██",
    "  "
};

/**
 * Handle CTRL+C signal
 */
void handle_sigint(int sig) 
{
    (void)sig; // Mark as used to silence warning
    
    // First restore terminal to normal state
    restore_terminal();
    
    // Clear screen and position cursor at top
    printf("%s%s", CLEAR_SCREEN, CURSOR_HOME);
    fflush(stdout);
    
    // Exit with a clean status
    exit(0);
}

/**
 * Handle terminal resize signal
 */

void handle_sigwinch(int sig) 
{
    terminal_resized = 1;
}

/**
 * Get terminal dimensions
 */
void update_terminal_size() {
    struct winsize w;
    ioctl(STDOUT_FILENO, TIOCGWINSZ, &w);
    term_width = w.ws_col;
    term_height = w.ws_row;
}

/**
 * Draw a centered string at the specified row
 */
void draw_centered_string(int row, const char* str) {
    int len = strlen(str);
    int pos_x = (term_width - len) / 2;
    if (pos_x < 0) pos_x = 0;
    
    direct_print(row, pos_x + 1, "%s", str);
}

/**
 * Draw the hundredths of a second at the specified position
 * This should only be called for the bottom line (line 4)
 */
void draw_hundredths(int row, int col, int hundredths) 
{
    // Check if row is valid
    if (row < 0) return;
    
    // Make sure the column is valid
    if (col < 1) col = 1;
    
    // Format the hundredths display text
    char hundredths_buffer[40]; // Buffer for the hundredths display
    memset(hundredths_buffer, 0, sizeof(hundredths_buffer));
    
    // Format the display with colors using separate strcat calls for each color segment
    // Dark gray dot
    strcat(hundredths_buffer, "\x1b[90m");
    strcat(hundredths_buffer, ".");
    strcat(hundredths_buffer, "\x1b[0m");
    
    // Bright red hundredths
    strcat(hundredths_buffer, "\x1b[91m");
    char temp[16];
    snprintf(temp, sizeof(temp), "%01d", hundredths);
    strcat(hundredths_buffer, temp);
    strcat(hundredths_buffer, "\x1b[0m");
    
    // White "UTC" text
    strcat(hundredths_buffer, "\x1b[97m");
    strcat(hundredths_buffer, " UTC");
    strcat(hundredths_buffer, "\x1b[0m");
    
    // Print directly to screen
    direct_print(row, col, "%s", hundredths_buffer);
}

/**
 * Draw the clock digits at the center of the screen
 */
void draw_full_clock(time_t current_time) 
{
    struct tm* time_info = localtime(&current_time);
    
    // Get the hundredths of a second
    int hundredths = (ntp_getCurrentHundredths() / 10);
    
    char time_str[9];
    sprintf(time_str, "%02d:%02d:%02d", time_info->tm_hour, time_info->tm_min, time_info->tm_sec);

    // Adding 1 space between each element 
    // Including space for ANSI color codes
    int start_row = (term_height - 5) / 2 - 2; // 5 is the height of digits, -2 to add some margin
    if (start_row < 1) start_row = 1;
    
    // Precalculate the full width of the clock display
    // 6 digits (each 6 chars wide) + 2 colons (each 2 chars wide) + 7 separations (each 1 space) 
    // Plus ANSI color codes which don't affect visible width
    int clock_display_width = 6 * 6 + 2 * 2 + 7;
    
    // Width of the hundredths display: ".0 UTC" 6 chars plus ANSI codes
    int hundredths_display_width = 6;
    
    // Full width including hundredths display and some spacing
    int total_display_width = clock_display_width + 3 + hundredths_display_width;
    // Calculate starting column for horizontal centering of the entire display (clock + hundredths)
    int start_col = (term_width - total_display_width) / 2;
    if (start_col < 1) start_col = 1;
    
    // Calculate the starting column for the hundredths display
    int hundredths_col = start_col + clock_display_width + 1; // Adjusted for better alignment
    
    // Draw each line of the digits
    char buffer[512]; // Larger buffer to accommodate color codes and spacing
    for (int line = 0; line < 5; line++) 
    {
        memset(buffer, 0, sizeof(buffer));
        
        // Hours tens digit - bright red
        strcat(buffer, "\x1b[91m");
        strcat(buffer, DIGIT_ART[time_info->tm_hour / 10][line]);
        strcat(buffer, "\x1b[0m "); // Reset + space for separation
        
        // Hours ones digit - bright red
        strcat(buffer, "\x1b[91m");
        strcat(buffer, DIGIT_ART[time_info->tm_hour % 10][line]);
        strcat(buffer, "\x1b[0m "); // Reset + space for separation
        
        // Colon - light black (dark gray)
        strcat(buffer, "\x1b[90m");
        strcat(buffer, COLON_ART[line]);
        strcat(buffer, "\x1b[0m "); // Reset + space for separation
        
        // Minutes tens digit - bright red
        strcat(buffer, "\x1b[91m");
        strcat(buffer, DIGIT_ART[time_info->tm_min / 10][line]);
        strcat(buffer, "\x1b[0m "); // Reset + space for separation
        
        // Minutes ones digit - bright red
        strcat(buffer, "\x1b[91m");
        strcat(buffer, DIGIT_ART[time_info->tm_min % 10][line]);
        strcat(buffer, "\x1b[0m "); // Reset + space for separation
        
        // Colon - light black (dark gray)
        strcat(buffer, "\x1b[90m");
        strcat(buffer, COLON_ART[line]);
        strcat(buffer, "\x1b[0m "); // Reset + space for separation
        
        // Seconds tens digit - bright red
        strcat(buffer, "\x1b[91m");
        strcat(buffer, DIGIT_ART[time_info->tm_sec / 10][line]);
        strcat(buffer, "\x1b[0m "); // Reset + space for separation
        
        // Seconds ones digit - bright red
        strcat(buffer, "\x1b[91m");
        strcat(buffer, DIGIT_ART[time_info->tm_sec % 10][line]);
        strcat(buffer, "\x1b[0m"); // Reset
        
        // Print the line directly to the screen
        direct_print(start_row + line, start_col, "%s", buffer);
    }
    
    // Draw the hundredths part on the last line
    draw_hundredths(start_row + 4, hundredths_col, hundredths);
}

/**
 * Draw the clock by calling draw_full_clock
 * This function maintains backward compatibility with existing code
 */
void draw_clock(time_t current_time)
{
    draw_full_clock(current_time);
}

/**
 * Draw the status bar at the bottom of the screen
 * This function ensures the status bar is always positioned at the bottom
 * of the terminal regardless of resizing
 * Note: This is a legacy version that uses buffer_line
 */
void draw_status_bar(time_t current_time, int time_since_sync) 
{
    struct tm* time_info = localtime(&current_time);
    
    // Determine if the current position indicator should blink
    // Since main loop sleeps for 900ms (just under 1 second), 
    // we need to use a combination of seconds and the expected timing
    // to determine the blink state
    int current_second = time_info->tm_sec;
    // Even seconds show the character, odd seconds hide it
    // Since we update roughly every 10ms, use modulo to create blinking
    int should_show_character = (current_second % 2 == 0);
    
    // Format date and time with hundredths
    int hundredths = ntp_getCurrentHundredths();
    char datetime_str[40];
    snprintf(datetime_str, sizeof(datetime_str), "%04d-%02d-%02d %02d:%02d:%02d.%01d UTC", 
            time_info->tm_year + 1900, time_info->tm_mon + 1, time_info->tm_mday,
            time_info->tm_hour, time_info->tm_min, time_info->tm_sec, hundredths);
    
    // Get NTP server name
    char server_name_buffer[256];
    if (!ntp_getServerName(server_name_buffer, sizeof(server_name_buffer))) {
        strcpy(server_name_buffer, "Not connected");
    }
    
    // Limit server name to a safe size (128 chars) to prevent buffer overflow
    server_name_buffer[128] = '\0';
    // Calculate times since sync and until next sync in H:MM:SS format
    char time_since_str[20] = "Never";
    char time_until_str[20] = "Unknown";
    int hours_since = 0, mins_since = 0, secs_since = 0;
    int hours_until = 0, mins_until = 0, secs_until = 0;
    
    // Time since last sync
    if (time_since_sync < 0) 
    {
        strcpy(time_since_str, "Never");
    } 
    else 
    {
        hours_since = time_since_sync / 3600;
        mins_since = (time_since_sync % 3600) / 60;
        secs_since = time_since_sync % 60;
        snprintf(time_since_str, sizeof(time_since_str), "%d:%02d:%02d", hours_since, mins_since, secs_since);
    }
    
    // Calculate next sync time
    int seconds_to_next_sync = 7200 - (time_since_sync % 7200); // 7200 = 2 hours
    if (time_since_sync < 0) 
    {
        seconds_to_next_sync = 7200;
    }
    
    // Format time until next sync
    hours_until = seconds_to_next_sync / 3600;
    mins_until = (seconds_to_next_sync % 3600) / 60;
    secs_until = seconds_to_next_sync % 60;
    snprintf(time_until_str, sizeof(time_until_str), "%d:%02d:%02d", hours_until, mins_until, secs_until);
    
    // Use global terminal dimensions - don't recompute them here
    // This avoids potential issues with inconsistent dimensions
    // Format the server name
    char server_name_buffer_formatted[64] = "";
    if (strlen(server_name_buffer) > 0) 
    {
        snprintf(server_name_buffer_formatted, sizeof(server_name_buffer_formatted), "%.*s", (int)sizeof(server_name_buffer_formatted) - 1, server_name_buffer);
    } 
    else 
    {
        strcpy(server_name_buffer_formatted, "Not connected");
    }
    
    // Format the left section with date/time and server name
    char left_section[256];
    snprintf(left_section, sizeof(left_section), " %s │ %s ", datetime_str, server_name_buffer_formatted);
    
    // Calculate progress as percentage of time elapsed in the sync cycle
    float progress = 0.0;
    if (time_since_sync >= 0) 
    {
        progress = (float)time_since_sync / (time_since_sync + seconds_to_next_sync);
    }
    
    // Step 1: Position cursor at the bottom line
    int status_line_y = term_height;
    
    // Create a buffer for the status bar line with all spaces
    char status_line_buffer[MAX_LINE_LENGTH];
    memset(status_line_buffer, 0, sizeof(status_line_buffer));
    
    // Fill with spaces for the background - always draw this regardless of content changes
    // This ensures the background is properly drawn on every frame
    strcpy(status_line_buffer, "\x1b[30;47m"); // Black text on grey background
    for (int i = 0; i < term_width && i < MAX_LINE_LENGTH - 20; i++) 
    {
        strcat(status_line_buffer, " ");
    }
    strcat(status_line_buffer, "\x1b[0m");
    
    // Add to buffer
    direct_print(status_line_y, 1, "%s", status_line_buffer);
    // Step 4: Add left section to buffer with black text on gray background
    char formatted_left_section[256];
    snprintf(formatted_left_section, sizeof(formatted_left_section), "\x1b[30;47m%s\x1b[0m", left_section);
    direct_print(status_line_y, 1, "%s", formatted_left_section);
    // Build the progress bar section text WITHOUT formatting to calculate its true length
    char plain_progress_section[512];
    
    // Create the divider, sync label and time
    sprintf(plain_progress_section, "│ Sync: %s [", time_since_str);
    
    // Calculate maximum size for progress section (max 50% of terminal width)
    // Ensure a reasonable minimum for very small terminals
    int min_term_width = 40; // Minimum reasonable terminal width
    int effective_term_width = (term_width < min_term_width) ? min_term_width : term_width;
    int max_progress_width = effective_term_width / 2;
    
    // Calculate size of fixed elements (dividers, times, labels)
    int fixed_elements_width = strlen("│ Sync: ") + strlen(time_since_str) + 
                            strlen(" │ ") + strlen(" │ ") + 
                            strlen(time_until_str) + 1; // +1 for right padding space
    
    // Calculate width available for the actual progress bar
    int bar_width = max_progress_width - fixed_elements_width;
    if (bar_width < 10) bar_width = 10; // Ensure minimum bar width
    
    // Add placeholder characters for the bar
    for (int i = 0; i < bar_width; i++) {
        strcat(plain_progress_section, "X"); // Placeholder character
    }
    
    // Add the divider and time until
    strcat(plain_progress_section, " │ ");
    strcat(plain_progress_section, time_until_str);
    strcat(plain_progress_section, " "); // Right padding space
    
    // Calculate where to position the progress section to be right-justified
    // Ensure progress_section_column is never less than the left section length + minimum spacing
    int left_section_length = strlen(left_section);
    int min_progress_section_column = left_section_length + 2; // +2 for minimum spacing
    int progress_section_column = term_width - strlen(plain_progress_section) + 1;
    
    // If terminal is too small, ensure at least the left section is completely visible
    if (progress_section_column < min_progress_section_column) {
        progress_section_column = min_progress_section_column;
    }
    
    // Build the progress section in a buffer
    char progress_section_buffer[MAX_LINE_LENGTH];
    memset(progress_section_buffer, 0, sizeof(progress_section_buffer));
    
    // Start with the right ANSI color code for the background
    strcpy(progress_section_buffer, "\x1b[30;47m"); // Black text on grey background
    
    // Now add the progress section content
    
    // Add divider and sync label with time
    strcat(progress_section_buffer, "│ Sync: ");
    strcat(progress_section_buffer, time_since_str);
    strcat(progress_section_buffer, " [");
    
    // Calculate filled portion of the bar
    // Calculate filled portion of the bar
    int filled_width = (int)(progress * bar_width);
    if (filled_width > bar_width) filled_width = bar_width;
    
    // Calculate the fractional part of the progress to determine if we should show a half block
    float fractional_part = (progress * bar_width) - filled_width;
    bool show_half_block = (fractional_part >= 0.1) && (filled_width < bar_width);
    
    // Determine the position of the blinking element:
    // - If we have a partial fill (half block): the half block position blinks
    // - If we have no half block: the boundary between filled and unfilled blinks
    //   (either last filled block or first unfilled dot)
    int blink_position = filled_width;
    if (!show_half_block && filled_width > 0) {
        // When no half block and we have filled blocks, blink the last filled block
        blink_position = filled_width - 1;
    }
    
    // Add the filled portion (bright yellow blocks on grey)
    strcat(progress_section_buffer, "\x1b[93;47m"); // Bright yellow on grey
    // Add full blocks for completed sections
    for (int i = 0; i < filled_width; i++) {
        if (i == blink_position && !show_half_block) {
            // This is the blinking element (the last filled block)
            if (should_show_character) {
                strcat(progress_section_buffer, "█"); // Full block for complete fill
            } else {
                strcat(progress_section_buffer, " "); // Space for blinking effect
            }
        } else {
            // Regular filled blocks never blink
            strcat(progress_section_buffer, "█"); // Full block for complete fill
        }
    }
    
    // Handle half block if needed
    if (show_half_block) {
        if (blink_position == filled_width) {
            // This half block is the blinking element
            if (should_show_character) {
                strcat(progress_section_buffer, "▌"); // Half block for partial fill
            } else {
                strcat(progress_section_buffer, " "); // Space for blinking effect
            }
        } else {
            // This half block is not the blinking element
            strcat(progress_section_buffer, "▌"); // Half block for partial fill
        }
        filled_width++; // Increment to account for the half block position
    }
    
    // Add the empty portion (dark grey mid-dots on grey)
    strcat(progress_section_buffer, "\x1b[90;47m"); // Dark grey on grey
    for (int i = filled_width; i < bar_width; i++) {
        if (i == blink_position) {
            // This is the blinking element (the first unfilled dot)
            if (should_show_character) {
                strcat(progress_section_buffer, "·"); // Mid-dot for unfilled portion
            } else {
                strcat(progress_section_buffer, " "); // Space for blinking effect
            }
        } else {
            // Regular unfilled dots never blink
            strcat(progress_section_buffer, "·"); // Mid-dot for unfilled portion
        }
    }
    strcat(progress_section_buffer, "] ");
    strcat(progress_section_buffer, time_until_str);
    strcat(progress_section_buffer, " ");
    
    // Reset terminal colors
    strcat(progress_section_buffer, "\x1b[0m");
    
    // Add progress section to buffer only if there's enough room to display it
    // For very small terminals, we might skip the progress section entirely
    if (term_width >= min_term_width) {
        // Check if we need to truncate the progress section
        if (progress_section_column + strlen(plain_progress_section) > term_width) {
            // Truncate the progress buffer to fit available width
            int available_width = term_width - progress_section_column + 1;
            if (available_width > 0) {
                progress_section_buffer[available_width] = '\0';
                strcat(progress_section_buffer, "\x1b[0m"); // Ensure color is reset
            }
        }
        direct_print(status_line_y, progress_section_column, "%s", progress_section_buffer);
    }
}

/**
 * Direct draw status bar - draws directly to the screen without buffering
 */
void direct_draw_status_bar(time_t current_time, int time_since_sync) 
{
    struct tm* time_info = localtime(&current_time);
    
    // Determine if the current position indicator should blink
    int current_second = time_info->tm_sec;
    // Even seconds show the character, odd seconds hide it
    int should_show_character = (current_second % 2 == 0);
    
    // Format date and time with hundredths
    int hundredths = (ntp_getCurrentHundredths() / 10);
    char datetime_str[40];
    snprintf(datetime_str, sizeof(datetime_str), "%04d-%02d-%02d %02d:%02d:%02d.%01d UTC", 
            time_info->tm_year + 1900, time_info->tm_mon + 1, time_info->tm_mday,
            time_info->tm_hour, time_info->tm_min, time_info->tm_sec, hundredths);
    
    // Get NTP server name
    char server_name_buffer[256];
    if (!ntp_getServerName(server_name_buffer, sizeof(server_name_buffer))) 
    {
        strcpy(server_name_buffer, "Not connected");
    }
    
    // Limit server name to a safe size (128 chars) to prevent buffer overflow
    server_name_buffer[128] = '\0';
    
    // Calculate times since sync and until next sync in H:MM:SS format
    char time_since_str[20] = "Never";
    char time_until_str[20] = "Unknown";
    int hours_since = 0, mins_since = 0, secs_since = 0;
    int hours_until = 0, mins_until = 0, secs_until = 0;
    
    // Time since last sync
    if (time_since_sync < 0) 
    {
        strcpy(time_since_str, "Never");
    } 
    else 
    {
        hours_since = time_since_sync / 3600;
        mins_since = (time_since_sync % 3600) / 60;
        secs_since = time_since_sync % 60;
        snprintf(time_since_str, sizeof(time_since_str), "%d:%02d:%02d", hours_since, mins_since, secs_since);
    }
    
    // Calculate next sync time
    int seconds_to_next_sync = 7200 - (time_since_sync % 7200); // 7200 = 2 hours
    if (time_since_sync < 0) 
    {
        seconds_to_next_sync = 7200;
    }
    
    // Format time until next sync
    hours_until = seconds_to_next_sync / 3600;
    mins_until = (seconds_to_next_sync % 3600) / 60;
    secs_until = seconds_to_next_sync % 60;
    snprintf(time_until_str, sizeof(time_until_str), "%d:%02d:%02d", hours_until, mins_until, secs_until);
    
    // Format the server name
    char server_name_buffer_formatted[64] = "";
    if (strlen(server_name_buffer) > 0) 
    {
        snprintf(server_name_buffer_formatted, sizeof(server_name_buffer_formatted), "%.*s", (int)sizeof(server_name_buffer_formatted) - 1, server_name_buffer);
    } 
    else 
    {
        strcpy(server_name_buffer_formatted, "Not connected");
    }
    
    // Format the left section with date/time and server name
    char left_section[256];
    snprintf(left_section, sizeof(left_section), " %s │ %s ", datetime_str, server_name_buffer_formatted);
    
    // Calculate progress as percentage of time elapsed in the sync cycle
    float progress = 0.0;
    if (time_since_sync >= 0) 
    {
        progress = (float)time_since_sync / (time_since_sync + seconds_to_next_sync);
    }
    
    // Position cursor at the bottom line
    int status_line_y = term_height;
    
    // First, draw the background line
    set_cursor_position(status_line_y, 1);
    printf("\x1b[30;47m"); // Black text on grey background
    
    // Print spaces for the full width of the terminal
    for (int i = 0; i < term_width; i++) {
        printf(" ");
    }
    printf("\x1b[0m"); // Reset colors
    
    // Draw the left section
    set_cursor_position(status_line_y, 1);
    printf("\x1b[30;47m%s\x1b[0m", left_section); // Black text on grey background
    
    // Build the progress bar section text WITHOUT formatting to calculate its true length
    char plain_progress_section[512];
    
    // Create the divider, sync label and time
    sprintf(plain_progress_section, "│ Sync: %s [", time_since_str);
    
    // Calculate maximum size for progress section (max 50% of terminal width)
    // Ensure a reasonable minimum for very small terminals
    int min_term_width = 40; // Minimum reasonable terminal width
    int effective_term_width = (term_width < min_term_width) ? min_term_width : term_width;
    int max_progress_width = effective_term_width / 2;
    
    // Calculate size of fixed elements (dividers, times, labels)
    int fixed_elements_width = strlen("│ Sync: ") + strlen(time_since_str) + 
                            strlen(" [") + strlen(" [") + 
                            strlen(time_until_str) + 1; // +1 for right padding space
    
    // Calculate width available for the actual progress bar
    int bar_width = max_progress_width - fixed_elements_width;
    if (bar_width < 10) bar_width = 10; // Ensure minimum bar width
    
    // Add placeholder characters for the bar
    for (int i = 0; i < bar_width; i++) {
        strcat(plain_progress_section, "X"); // Placeholder character
    }
    
    // Add the divider and time until
    strcat(plain_progress_section, "] ");
    strcat(plain_progress_section, time_until_str);
    strcat(plain_progress_section, " "); // Right padding space
    
    // Calculate where to position the progress section to be right-justified
    // Ensure progress_section_column is never less than the left section length + minimum spacing
    int left_section_length = strlen(left_section);
    int min_progress_section_column = left_section_length + 2; // +2 for minimum spacing
    int progress_section_column = term_width - strlen(plain_progress_section) + 1;
    
    // If terminal is too small, ensure at least the left section is completely visible
    if (progress_section_column < min_progress_section_column) {
        progress_section_column = min_progress_section_column;
    }
    
    // Only proceed with drawing the progress section if there's enough room
    if (term_width >= min_term_width) {
        // Position cursor for the progress section
        set_cursor_position(status_line_y, progress_section_column);
        
        // Start with black text on grey background
        printf("\x1b[30;47m"); // Black text on grey background
        
        // Add divider and sync label with time
        printf("│ Sync: %s [", time_since_str);
        
        // Calculate filled portion of the bar
        int filled_width = (int)(progress * bar_width);
        if (filled_width > bar_width) filled_width = bar_width;
        
        // Calculate the fractional part of the progress to determine if we should show a half block
        float fractional_part = (progress * bar_width) - filled_width;
        bool show_half_block = (fractional_part >= 0.1) && (filled_width < bar_width);
        
        // Determine the position of the blinking element
        int blink_position = filled_width;
        if (!show_half_block && filled_width > 0) {
            // When no half block and we have filled blocks, blink the last filled block
            blink_position = filled_width - 1;
        }
        
        // Add the filled portion (bright yellow blocks on grey)
        printf("\x1b[93;47m"); // Bright yellow on grey
        
        // Add full blocks for completed sections
        for (int i = 0; i < filled_width; i++) {
            if (i == blink_position && !show_half_block) {
                // This is the blinking element (the last filled block)
                if (should_show_character) {
                    printf("█"); // Full block for complete fill
                } else {
                    printf(" "); // Space for blinking effect
                }
            } else {
                // Regular filled blocks never blink
                printf("█"); // Full block for complete fill
            }
        }
        
        // Handle half block if needed
        if (show_half_block) {
            if (blink_position == filled_width) {
                // This half block is the blinking element
                if (should_show_character) {
                    printf("▌"); // Half block for partial fill
                } else {
                    printf(" "); // Space for blinking effect
                }
            } else {
                // This half block is not the blinking element
                printf("▌"); // Half block for partial fill
            }
            filled_width++; // Increment to account for the half block position
        }
        
        // Add the empty portion (dark grey mid-dots on grey)
        printf("\x1b[90;47m"); // Dark grey on grey
        for (int i = filled_width; i < bar_width; i++) {
            if (i == blink_position) {
                // This is the blinking element (the first unfilled dot)
                if (should_show_character) {
                    printf("·"); // Mid-dot for unfilled portion
                } else {
                    printf(" "); // Space for blinking effect
                }
            } else {
                // Regular unfilled dots never blink
                printf("·"); // Mid-dot for unfilled portion
            }
        }
        
        // Add the final part of the progress section
        printf("] \x1b[30;47m%s ", time_until_str);
        
        // Reset terminal colors
        printf("\x1b[0m");
    }
    
    // Flush to ensure immediate display
    fflush(stdout);
}

void init_terminal() 
{
    // Save current terminal settings
    struct termios old_termios, new_termios;
    tcgetattr(0, &old_termios);
    new_termios = old_termios;
    new_termios.c_lflag &= ~ECHO; // Turn off echo
    tcsetattr(0, TCSANOW, &new_termios);
    
    // Hide cursor
    printf("%s", HIDE_CURSOR);
    fflush(stdout);
}


/**
 * Restore terminal settings
 */
void restore_terminal() 
{
    // Restore cursor
    printf("%s", SHOW_CURSOR);
    
    // Clear screen
    printf("%s%s", CLEAR_SCREEN, CURSOR_HOME);
    fflush(stdout);
    
    // Restore terminal settings
    struct termios old_termios;
    tcgetattr(0, &old_termios);
    old_termios.c_lflag |= ECHO; // Turn on echo
    tcsetattr(0, TCSANOW, &old_termios);
}

/**
 * Attempt to sync with NTP server
 */
int sync_with_ntp() 
{
    char server_name_buffer[256];
    if (!ntp_getServerName(server_name_buffer, sizeof(server_name_buffer))) 
    {
        strcpy(server_name_buffer, "Not connected.");
    }
    
    // Display sync message
    printf("Syncing with NTP server: %s", server_name_buffer);
    
    int result = ntp_sync();
    if (result == 0) 
    {
        printf("Sync successful.\n");
        return 1;
    } 
    else 
    {
        printf("Sync failed with error code: %d", result);
        return 0;
    }
}

int supports_ansi() 
{
    if (!isatty(STDOUT_FILENO)) return 0;

    struct termios t;
    tcgetattr(STDIN_FILENO, &t);
    t.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &t);

    printf("\x1B[c");
    fflush(stdout);

    char buf[32] = {0};
    alarm(1);
    int r = read(STDIN_FILENO, buf, sizeof(buf) - 1);
    alarm(0);

    tcsetattr(STDIN_FILENO, TCSANOW, &t);

    return (r > 0 && strstr(buf, "\x1B[") != NULL);
}

int main(int argc, char* argv[]) 
{
    if (!supports_ansi()) 
    {
        printf("No ANSI support.\n");
        exit(1);
    }
    printf("ANSI supported.\n");


    // Register signal handler for CTRL+C
    signal(SIGINT, handle_sigint);
    signal(SIGWINCH, handle_sigwinch);
    
    // Initialize NTP client with configuration
    ntp_config_t config;
    memset(&config, 0, sizeof(config));
    strncpy(config.server_name, DEFAULT_NTP_SERVER, sizeof(config.server_name));
    config.server_port = 123;  // Standard NTP port
    config.timeout_ms = 5000;  // 5 second timeout
    config.retry_count = 3;    // Retry 3 times
    config.sync_interval = 7200; // Sync every 2 hours (7200 seconds)
    
    // Initialize the NTP client with the configuration
    ntp_status_t init_status = ntp_init(&config);
    if (init_status != NTP_OK) 
    {
      printf("Failed to initialize NTP client, error code: %d\n", init_status);
    }
    
    // Set the NTP server (now that the client is properly initialized)
    ntp_setServer(DEFAULT_NTP_SERVER);
    
    // Initialize terminal and clear it
    init_terminal();
    update_terminal_size();
    
    // Clear screen at startup
    direct_clear_screen();
    
    // Force a proper terminal size update at startup
    // This ensures all clock elements are properly displayed
    terminal_resized = 1;
    update_terminal_size();
    
    // Perform initial sync
    int message_line = term_height - 3;
    
    // Perform initial NTP sync
    sync_with_ntp();
    
    // Do an initial full draw of the clock and status bar
    direct_clear_screen();

    while (keep_running) 
    {
      // Check if terminal was resized
      if (terminal_resized) 
      {
        update_terminal_size();
        terminal_resized = 0;
        // Redraw the whole screen
        direct_clear_screen();
      }
    
      // Get current time and time since last sync
      time_t current_time = ntp_getCurrentTime();
      int time_since_sync = ntp_getTimeSinceLastSync();
    
      // Check if it's time to sync again (every 2 hours)
      if (time_since_sync >= 7200 || time_since_sync < 0)  // 7200 seconds = 2 hours
      {
        direct_clear_screen(); 
        sync_with_ntp();
        direct_clear_screen();
      }
    
    // Update terminal size to handle possible window resizing
    update_terminal_size();
    
    // Get the most up-to-date time including hundredths for a smooth display
    current_time = ntp_getCurrentTime();
    time_since_sync = ntp_getTimeSinceLastSync();
    
    // Draw clock components directly to screen
    draw_full_clock(current_time);
    direct_draw_status_bar(current_time, time_since_sync);
    
    // Sleep for a shorter interval to provide smoother hundredths updates
    usleep(100000); // 10ms for smoother hundredths display
    }

    // Cleanup and restore terminal
    restore_terminal();
    direct_print(term_height / 2, (term_width - 26) / 2, "Clock display terminated.");
    
    return 0;
}
