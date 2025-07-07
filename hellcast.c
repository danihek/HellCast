#include <playerctl/playerctl.h>
#include <sys/ioctl.h>
#include <ncurses.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <signal.h>
#include <locale.h>
#include <glib.h>
#include <math.h>
#include <sys/wait.h>

#include <curl/curl.h>

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#include "stb_image_resize2.h"

#define REFRESH_INTERVAL 100000 // microseconds (0.1s)
volatile sig_atomic_t resized = 0;

// Unicode visuals
const char *BAR_LEFT = "";
const char *BAR_RIGHT = "";
const char *BAR_FULL = "█";
const char *BLOCK_EMPTY = "░";

const int COLOR_LABEL = 1;
const int COLOR_TITLE = 2;
const int COLOR_BAR_FILLED = 3;
const int COLOR_BAR_EMPTY = 4;
const int COLOR_FOOTER = 5;
const int COLOR_ALBUM = 6;

// Previous UI state tracking
char *prev_title = NULL;
char *prev_artist = NULL;
char *prev_album = NULL;
char *prev_arturl = NULL;
char *prev_status = NULL;
int64_t prev_position = -1;
int64_t prev_duration = -1;
int prev_width = -1;
int prev_height = -1;
gint64 position = 0;
int show_status_bar = 0;

static char cached_url[1024] = "";
static int cached_pixel_width = 0, cached_pixel_height = 0;
static const char *orig_file = "/tmp/art_original.png";
static const char *scaled_file = "/tmp/art_scaled.png";

struct MemoryBuffer {
    unsigned char *data;
    size_t size;
};

// functionsss
void handle_winch(int sig)
{
    resized = 1;
}

void cleanup_ncurses()
{
    printf("\033[?1003l\n"); // Disable mouse tracking
    fflush(stdout);
    endwin();
}

static size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    struct MemoryBuffer *mem = (struct MemoryBuffer *)userp;

    unsigned char *ptr = realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0;

    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = 0;

    return realsize;
}

int download_image(const char *url, struct MemoryBuffer *img_data) {
    CURL *curl = curl_easy_init();
    if (!curl) return 0;

    img_data->data = NULL;
    img_data->size = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, (void *)img_data);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl/7.87.0");

    CURLcode res = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    return (res == CURLE_OK);
}

int save_scaled_image_png(const unsigned char *input, size_t input_size,
                          int max_width, int max_height, const char *output_path)
{
    int w, h, channels;
    unsigned char *img = stbi_load_from_memory(input, input_size, &w, &h, &channels, 3);
    if (!img) return 0;

    int square_size = w < h ? w : h;
    int offset_x = (w - square_size) / 2;
    int offset_y = (h - square_size) / 2;

    unsigned char *cropped = malloc(square_size * square_size * 3);
    if (!cropped) {
        stbi_image_free(img);
        return 0;
    }

    for (int y = 0; y < square_size; y++) {
        for (int x = 0; x < square_size; x++) {
            int src_idx = ((offset_y + y) * w + (offset_x + x)) * 3;
            int dst_idx = (y * square_size + x) * 3;
            cropped[dst_idx + 0] = img[src_idx + 0];
            cropped[dst_idx + 1] = img[src_idx + 1];
            cropped[dst_idx + 2] = img[src_idx + 2];
        }
    }

    int target_size = max_width < max_height ? max_width : max_height;
    unsigned char *resized = malloc(target_size * target_size * 3);
    if (!resized) {
        free(cropped);
        stbi_image_free(img);
        return 0;
    }

    stbir_resize_uint8_linear(cropped, square_size, square_size, 0,
                              resized, target_size, target_size, 0, 3);

    FILE *fp = fopen(output_path, "wb");
    if (!fp) {
        free(resized);
        free(cropped);
        stbi_image_free(img);
        return 0;
    }

    stbi_write_png(output_path, target_size, target_size, 3, resized, target_size * 3);
    fclose(fp);

    free(resized);
    free(cropped);
    stbi_image_free(img);
    return 1;
}

void render_sixel_image_native_cached(const char *url, int x, int y, int max_chars_w, int max_chars_h, int cell_pixel_w, int cell_pixel_h)
{
    // Calculate target pixel dimensions based on character cell size
    int target_pixel_width = max_chars_w * cell_pixel_w;
    int target_pixel_height = max_chars_h * cell_pixel_h;

    // If URL changed, download new image
    int need_download = strcmp(cached_url, url) != 0;
    // Check if dimensions changed in terms of *pixels*
    int need_resize = need_download || cached_pixel_width != target_pixel_width || cached_pixel_height != target_pixel_height;

    if (need_download) {
        struct MemoryBuffer img_data = {0};

        if (!download_image(url, &img_data)) {
            fprintf(stderr, "Image download failed: %s\n", url);
            return;
        }

        // Save original image for reuse
        FILE *fp = fopen(orig_file, "wb");
        if (fp) {
            fwrite(img_data.data, 1, img_data.size, fp);
            fclose(fp);
        }

        strncpy(cached_url, url, sizeof(cached_url) - 1);
        cached_url[sizeof(cached_url) - 1] = '\0';
        free(img_data.data);
    }

    // Resize only if needed
    if (need_resize) {
        // Read original image from disk
        FILE *fp = fopen(orig_file, "rb");
        if (!fp) {
            fprintf(stderr, "Failed to open cached original image.\n");
            return;
        }

        fseek(fp, 0, SEEK_END);
        long size = ftell(fp);
        rewind(fp);

        unsigned char *buf = malloc(size);
        if (!buf) {
            fclose(fp);
            fprintf(stderr, "Memory allocation failed for original image buffer.\n");
            return;
        }
        fread(buf, 1, size, fp);
        fclose(fp);

        if (!save_scaled_image_png(buf, size, target_pixel_width, target_pixel_height, scaled_file)) {
            fprintf(stderr, "Scaling failed.\n");
            free(buf);
            return;
        }

        cached_pixel_width = target_pixel_width;
        cached_pixel_height = target_pixel_height;
        free(buf);
    }

    printf("\033[%d;%dH", y, x);
    fflush(stdout);

    char cmd[512];
    snprintf(cmd, sizeof(cmd), "img2sixel %s", scaled_file);
    system(cmd);
}

gchar *update_artist(PlayerctlPlayer *player)
{
    GError *error = NULL;
    gchar *artist = playerctl_player_get_artist(player, &error);
    if (error != NULL)
    {
        g_printerr("Failed to get artist: %s\n", error->message);
        g_error_free(error);
        artist = NULL;
    }
    return artist;
}

gchar *update_title(PlayerctlPlayer *player)
{
    GError *error = NULL;
    gchar *title = playerctl_player_get_title(player, &error);
    if (error != NULL)
    {
        g_printerr("Failed to get title: %s\n", error->message);
        g_error_free(error);
        title = NULL;
    }
    return title;
}

gchar *update_album(PlayerctlPlayer *player)
{
    GError *error = NULL;
    gchar *album = playerctl_player_get_album(player, &error);
    if (error != NULL)
    {
        g_printerr("Failed to get album: %s\n", error->message);
        g_error_free(error);
        album = NULL;
    }
    return album;
}

gchar *update_arturl(PlayerctlPlayer *player)
{
    GError *error = NULL;
    gchar *metadata = playerctl_player_print_metadata_prop(player, "mpris:artUrl", &error);
    if (error != NULL)
    {
        g_printerr("Failed to get metadata: %s\n", error->message);
        g_error_free(error);
        metadata = NULL;
    }
    return metadata;
}

int64_t update_position(PlayerctlPlayer *player)
{
    GError *error = NULL;
    gint64 pos = playerctl_player_get_position(player, &error);
    if (error != NULL)
    {
        g_printerr("Failed to get position: %s\n", error->message);
        g_error_free(error);
        pos = 0;
    }
    return pos;
}

int64_t update_duration(PlayerctlPlayer *player)
{
    GError *error = NULL;
    char *duration_char = playerctl_player_print_metadata_prop(player, "mpris:length", &error);
    int64_t duration = strtoll(duration_char ? duration_char : "0", NULL, 10);
    g_free(duration_char);

    return duration;
}

gchar *update_status(PlayerctlPlayer *player)
{
    gchar *state = NULL;
    g_object_get(player, "status", &state, NULL);
    return state;
}

void draw_centered(int row, const char *text, int width, int color_pair) 
{
    int len = strlen(text);
    int x = (width - len) / 2;
    attron(COLOR_PAIR(color_pair));
    mvprintw(row, x, "%s", text);
    attroff(COLOR_PAIR(color_pair));
}

void update_state(char **target, const char *src) {
    if (*target) free(*target);
    *target = strdup(src ? src : "");
}

int intersect(int ax1, int ay1, int ax2, int ay2,
        int bx1, int by1, int bx2, int by2)
{
    if (ax2 <= bx1 || bx2 <= ax1)
        return 0;
    if (ay2 <= by1 || by2 <= ay1)
        return 0;

    // intersect
    return 1;
}
void draw_ui(const char *title, const char *artist, const char *album, const char *arturl,
             int64_t position, int64_t duration, const char *status)
{
    curs_set(0);

    int width = COLS;
    int height = LINES;

    int pos_sec = position / 1000000;
    int dur_sec = duration / 1000000;

    float progress = (duration > 0) ? (float)position / duration : 0.0f;
    int bar_width = width / 2;
    if (bar_width < 10) bar_width = 10;
    int filled = (int)(progress * bar_width);

    // Only redraw if something visible actually changed
    if (prev_title && prev_artist && prev_album && prev_status &&
        strcmp(prev_title, title) == 0 &&
        strcmp(prev_artist, artist) == 0 &&
        strcmp(prev_album, album) == 0 &&
        strcmp(prev_arturl, arturl) == 0 &&
        strcmp(prev_status, status) == 0 &&
        prev_position == position &&
        prev_duration == duration &&
        prev_width == width &&
        prev_height == height) {
        return;
    }

    // Save state
    update_state(&prev_title, title);
    update_state(&prev_artist, artist);
    update_state(&prev_album, album);
    update_state(&prev_arturl, arturl);
    update_state(&prev_status, status);
    prev_position = position;
    prev_duration = duration;
    prev_width = width;
    prev_height = height;

    start_color();
    use_default_colors();
    init_pair(COLOR_LABEL, -1, -1);
    init_pair(COLOR_TITLE, COLOR_RED, -1);
    init_pair(COLOR_BAR_FILLED, COLOR_GREEN, -1);
    init_pair(COLOR_BAR_EMPTY, -1, -1);
    init_pair(COLOR_FOOTER, COLOR_YELLOW, -1);
    init_pair(COLOR_ALBUM, COLOR_CYAN, -1);

    // erase instead of clear to reduce flicker
    erase();

    // -- UI Layout --

    // ALBUM
    if (album && strlen(album) > 0) {
        attron(COLOR_PAIR(COLOR_ALBUM));
        mvprintw(0, 2, " %.*s", width - 4, album);
        attroff(COLOR_PAIR(COLOR_ALBUM));
    }

    // ARTIST
    if (artist && strlen(artist) > 0) {
        draw_centered(2, artist, width, COLOR_LABEL);
    }

    // TITLE
    draw_centered(3, title, width, COLOR_TITLE);

    // TIME
    char time_text[64];
    snprintf(time_text, sizeof(time_text), "%d:%02d / %d:%02d",
             pos_sec / 60, pos_sec % 60, dur_sec / 60, dur_sec % 60);

    int bar_y = (height / 10)*9;
    int time_y = bar_y - 1;
    draw_centered(time_y, time_text, width, COLOR_TITLE);

    // PROGRESS BAR
    int bar_start_x = (width - bar_width) / 2;
    mvprintw(bar_y, bar_start_x - (int)strlen(BAR_LEFT), "%s", BAR_LEFT);

    for (int i = 0; i < bar_width; i++) {
        int pair = i < filled ? COLOR_BAR_FILLED : COLOR_BAR_EMPTY;
        attron(COLOR_PAIR(pair));
        mvprintw(bar_y, bar_start_x + i - 1, "%s", i < filled ? BAR_FULL : BLOCK_EMPTY);
        attroff(COLOR_PAIR(pair));
    }

    mvprintw(bar_y, bar_start_x + bar_width, "%s", BAR_RIGHT);

    // win
    struct winsize sz;
    ioctl(0, TIOCGWINSZ, &sz);
    //ws_row;     // rows, in characters
    //ws_col;     // columns, in characters
    //ws_xpixel;  // horizontal size, pixels
    //ws_ypixel;  // vertical size, pixels
    int cell_w = sz.ws_xpixel / sz.ws_col;
    int cell_h = sz.ws_ypixel / sz.ws_row;

    // BOX (optional: draw only once unless dimensions change)
    int box_size = bar_width / 1.5;

    // BOX dimensions (these define the area the sixel image will occupy)
    // Use 'box_width' and 'box_height' to represent the character dimensions
    // for the SIXEL image, so the border goes *around* this area.
    int box_width_chars = box_size; // This is the width in characters for the sixel image
    int box_height_chars = (box_size / 2); // This is the height in characters for the sixel image
    int box_start_x_chars = (width - box_width_chars) / 2;
    int box_start_y_chars = 7;

    render_sixel_image_native_cached(arturl, box_start_x_chars, box_start_y_chars, box_width_chars, box_height_chars, cell_w, cell_h);

    // FOOTER
    if (show_status_bar)
    {
        attron(COLOR_PAIR(COLOR_FOOTER));
        //mvprintw(height - 3, 2, "ArtURL ⏵  %s", arturl);
        mvprintw(height - 2, 2, " ⏵  Status: %s |    %dx%d ", status, width, height);
        attroff(COLOR_PAIR(COLOR_FOOTER));
    }

    // Use "optimized" refresh (xd) to prevent flicker
    wnoutrefresh(stdscr);
    doupdate();
}

void fetch_iteration(PlayerctlPlayer *player)
{
    if (resized)
    {
        endwin();
        refresh();
        clear();
        resized = 0;
    }

    gchar *artist = update_artist(player);
    gchar *title = update_title(player);
    gchar *album = update_album(player);
    gchar *arturl = update_arturl(player);

    gchar *status = update_status(player);
    if (!strcmp(status, "Playing"))
        position = update_position(player);

    int64_t duration = update_duration(player);
    if (duration <= 0)
        duration = 1; // Prevent division by zero

    // UI content
    draw_ui(title, artist, album, arturl, position, duration, status);

    g_free(status);
    g_free(artist);
    g_free(title);
    g_free(album);
}

int main(void)
{
    // Unicode characters
    setlocale(LC_ALL, "");

    // Handle terminal resize
    struct sigaction sa;
    sa.sa_handler = handle_winch;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGWINCH, &sa, NULL);

    // Handle Ctrl+C gracefully
    signal(SIGINT, (void *)cleanup_ncurses);

    // Init ncurses
    initscr();
    noecho();
    cbreak();
    nodelay(stdscr, TRUE);
    keypad(stdscr, TRUE);
    mousemask(ALL_MOUSE_EVENTS | REPORT_MOUSE_POSITION, NULL);
    printf("\033[?1003h\n"); // Enable mouse tracking
    fflush(stdout);

    // Main loop
    while (1)
    {
        GError *error = NULL;
        PlayerctlPlayer *player = playerctl_player_new(NULL, &error);
        if (error != NULL)
        {
            g_printerr("Failed to initialize player: %s\n", error->message);
            g_error_free(error);
            cleanup_ncurses();
            return 0;
        }

        int ch = getch();
        MEVENT event;

        if (ch == KEY_MOUSE)
        {
            if (getmouse(&event) == OK)
            {
                int bar_y = (LINES / 10) * 9;

                if (event.y == bar_y) {
                    int bar_width = COLS / 2;
                    int bar_start_x = (COLS - bar_width) / 2;

                    // Previous button click area (1 char left of bar)
                    if (event.x >= bar_start_x - 3 && event.x < bar_start_x - 2) {
                        playerctl_player_previous(player, &error);
                    }

                    // Next button click area (1 char right of bar)
                    else if (event.x >= bar_start_x + bar_width && event.x < bar_start_x + bar_width + 1) {
                        playerctl_player_next(player, &error);
                    }

                    // Inside bar click → seek
                    else if (event.x >= bar_start_x && event.x < (bar_start_x + bar_width)) {
                        float clicked_fraction = (float)(event.x - bar_start_x) / bar_width;
                        gint64 duration = update_duration(player);
                        gint64 new_position = (gint64)(clicked_fraction * duration);
                        playerctl_player_set_position(player, new_position, &error);
                    }
                }
            }
        }

        // Keybinds
        if (ch == 'q') break;

        if (ch == ' ')
        {
            playerctl_player_play_pause(player, &error);
        }
        if (ch == 'h' || ch == KEY_LEFT)
        {
            playerctl_player_seek(player, -5000000, &error); // 5 sec back
        }
        if (ch == 'l' || ch == KEY_RIGHT)
        {
            playerctl_player_seek(player, 5000000, &error); // 5 sec forward
        }
        if (ch == 'j' || ch == KEY_DOWN)
        {
            playerctl_player_previous(player, &error);
        }
        if (ch == 'k' || ch == KEY_UP)
        {
            playerctl_player_next(player, &error);
        }
        if (ch == 'i')
        {
            if (show_status_bar == 1)
                show_status_bar = 0;
            else
                show_status_bar = 1;

            resized = 1;
        }

        if (error != NULL)
        {
            g_printerr("Playerctl error: %s\n", error->message);
            g_error_free(error);
        }

        fetch_iteration(player);
        if (error == NULL)
            g_object_unref(player);
        napms(100);
        usleep(REFRESH_INTERVAL);
    }

    cleanup_ncurses();
    return 0;
}
