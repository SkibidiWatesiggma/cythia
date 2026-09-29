#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#define NODRAWTEXT

/*
 * Prevent Windows headers from colliding with raylib names.
 */
#define Rectangle WinGDIRectangle
#define CloseWindow WinCloseWindow
#define ShowCursor WinShowCursor
#define LoadImageA WinLoadImageA
#define DrawTextA WinDrawTextA
#define DrawTextExA WinDrawTextExA

#include <windows.h>
#include <commdlg.h>

#undef Rectangle
#undef CloseWindow
#undef ShowCursor
#undef LoadImageA
#undef DrawTextA
#undef DrawTextExA

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <float.h>
#include <limits.h>
#include <io.h>

#include "raylib.h"
#include "config.h"


#define CYTHIA_VERSION "Cythia v1.0.3"
#define RMAP_MAGIC "RMAP"

#define GRID_MIN 0.0f
#define GRID_MAX 3.0f
#define GRID_CENTER 1.5f

#define NOTE_SIZE 0.72f
#define CURSOR_SIZE 0.52f

#define CYTHIA_ENABLE_AUDIO 1

#define CYTHIA_DEBUG_HITS 1
#define CYTHIA_DEBUG_AUDIO 0

/*
 * Extra world-space hit forgiveness.
 *
 * 0.000 = exact rendered billboard
 * 0.035 = 3.5 cm expansion on each side
 */
#define HITBOX_PADDING 0.035f

#define AUDIO_CLOCK_EPSILON 0.000001
#define RENDER_TIME_PADDING 0.05


typedef struct {
    uint32_t time;
    float x;
    float y;
    Color color;
    int state;
} Note;


typedef struct {
    char *map_name;
    char *song_name;

    Note *notes;
    uint32_t note_count;

    unsigned char *audio;
    uint64_t audio_length;
} Chart;


typedef struct {
    float multiplier;
    int nofail;
} GameOptions;


/* ============================================================
 * Forward declarations
 * ============================================================ */

static void free_chart(Chart *chart);


/* ============================================================
 * Safe memory readers
 * ============================================================ */

static int read_u16_mem(
    const unsigned char **p,
    const unsigned char *end,
    uint16_t *v
)
{
    if (p == NULL || *p == NULL || end == NULL || v == NULL)
        return 0;

    if (*p > end)
        return 0;

    if ((size_t)(end - *p) < 2)
        return 0;

    *v =
        (uint16_t)(*p)[0] |
        ((uint16_t)(*p)[1] << 8);

    *p += 2;

    return 1;
}


static int read_u32_mem(
    const unsigned char **p,
    const unsigned char *end,
    uint32_t *v
)
{
    if (p == NULL || *p == NULL || end == NULL || v == NULL)
        return 0;

    if (*p > end)
        return 0;

    if ((size_t)(end - *p) < 4)
        return 0;

    *v =
        (uint32_t)(*p)[0] |
        ((uint32_t)(*p)[1] << 8) |
        ((uint32_t)(*p)[2] << 16) |
        ((uint32_t)(*p)[3] << 24);

    *p += 4;

    return 1;
}


static int read_u64_mem(
    const unsigned char **p,
    const unsigned char *end,
    uint64_t *v
)
{
    if (p == NULL || *p == NULL || end == NULL || v == NULL)
        return 0;

    if (*p > end)
        return 0;

    if ((size_t)(end - *p) < 8)
        return 0;

    *v =
        (uint64_t)(*p)[0] |
        ((uint64_t)(*p)[1] << 8) |
        ((uint64_t)(*p)[2] << 16) |
        ((uint64_t)(*p)[3] << 24) |
        ((uint64_t)(*p)[4] << 32) |
        ((uint64_t)(*p)[5] << 40) |
        ((uint64_t)(*p)[6] << 48) |
        ((uint64_t)(*p)[7] << 56);

    *p += 8;

    return 1;
}


static int read_string_mem(
    const unsigned char **p,
    const unsigned char *end,
    char **out
)
{
    uint16_t length;
    char *string;

    if (out == NULL)
        return 0;

    *out = NULL;

    if (!read_u16_mem(
        p,
        end,
        &length
    ))
        return 0;

    if (*p > end)
        return 0;

    if ((size_t)(end - *p) < length)
        return 0;

    string =
        malloc((size_t)length + 1);

    if (string == NULL)
        return 0;

    memcpy(
        string,
        *p,
        length
    );

    string[length] = '\0';

    *p += length;

    *out = string;

    return 1;
}


/* ============================================================
 * Chart
 * ============================================================ */

static void free_chart(
    Chart *chart
)
{
    if (chart == NULL)
        return;

    free(chart->map_name);
    free(chart->song_name);
    free(chart->notes);
    free(chart->audio);

    memset(
        chart,
        0,
        sizeof(*chart)
    );
}


static int compare_notes_by_time(
    const void *a,
    const void *b
)
{
    const Note *na =
        (const Note *)a;

    const Note *nb =
        (const Note *)b;

    if (na->time < nb->time)
        return -1;

    if (na->time > nb->time)
        return 1;

    return 0;
}


static void sort_chart_notes(
    Chart *chart
)
{
    if (
        chart == NULL ||
        chart->notes == NULL ||
        chart->note_count < 2
    )
        return;

    qsort(
        chart->notes,
        chart->note_count,
        sizeof(chart->notes[0]),
        compare_notes_by_time
    );
}


static int read_rmap(
    const unsigned char *data,
    size_t size,
    Chart *chart
)
{
    const unsigned char *p;
    const unsigned char *end;

    uint32_t i;

    if (chart == NULL)
        return 0;

    memset(
        chart,
        0,
        sizeof(*chart)
    );

    if (data == NULL)
        return 0;

    if (size < 4)
        return 0;

    p = data;
    end = data + size;

    if (memcmp(
        p,
        RMAP_MAGIC,
        4
    ) != 0)
        return 0;

    p += 4;

    if (!read_string_mem(
        &p,
        end,
        &chart->map_name
    ))
        goto fail;

    if (!read_string_mem(
        &p,
        end,
        &chart->song_name
    ))
        goto fail;

    if (!read_u32_mem(
        &p,
        end,
        &chart->note_count
    ))
        goto fail;

    if (
        (uint64_t)chart->note_count * 12ULL >
        (uint64_t)(end - p)
    )
        goto fail;

    if (chart->note_count > 10000000U)
        goto fail;

    if (chart->note_count > 0) {
        chart->notes =
            calloc(
                chart->note_count,
                sizeof(*chart->notes)
            );

        if (chart->notes == NULL)
            goto fail;

        for (
            i = 0;
            i < chart->note_count;
            i++
        ) {
            uint32_t time;
            uint32_t xb;
            uint32_t yb;

            float x;
            float y;

            if (!read_u32_mem(
                &p,
                end,
                &time
            ))
                goto fail;

            if (!read_u32_mem(
                &p,
                end,
                &xb
            ))
                goto fail;

            if (!read_u32_mem(
                &p,
                end,
                &yb
            ))
                goto fail;

            memcpy(
                &x,
                &xb,
                sizeof(float)
            );

            memcpy(
                &y,
                &yb,
                sizeof(float)
            );

            if (
                !isfinite(x) ||
                !isfinite(y)
            )
                goto fail;

            if (x < GRID_MIN)
                x = GRID_MIN;

            if (x > GRID_MAX)
                x = GRID_MAX;

            if (y < GRID_MIN)
                y = GRID_MIN;

            if (y > GRID_MAX)
                y = GRID_MAX;

            chart->notes[i].time = time;
            chart->notes[i].x = x;
            chart->notes[i].y = y;
            chart->notes[i].state = 0;
            chart->notes[i].color = WHITE;
        }
    }

    sort_chart_notes(chart);

    if (!read_u64_mem(
        &p,
        end,
        &chart->audio_length
    ))
        goto fail;

    if (
        chart->audio_length >
        (uint64_t)(end - p)
    )
        goto fail;

    if (
        chart->audio_length >
        (uint64_t)SIZE_MAX
    )
        goto fail;

    if (chart->audio_length > 0) {
        chart->audio =
            malloc(
                (size_t)chart->audio_length
            );

        if (chart->audio == NULL)
            goto fail;

        memcpy(
            chart->audio,
            p,
            (size_t)chart->audio_length
        );
    }

    return 1;

fail:

    free_chart(chart);

    return 0;
}


/* ============================================================
 * Windows helpers
 * ============================================================ */

static int get_exe_directory(
    char *out,
    size_t size
)
{
    DWORD length;

    if (
        out == NULL ||
        size == 0 ||
        size > (size_t)UINT_MAX
    )
        return 0;

    length =
        GetModuleFileNameA(
            NULL,
            out,
            (DWORD)size
        );

    if (length == 0)
        return 0;

    if ((size_t)length >= size)
        return 0;

    while (length > 0) {
        char c =
            out[length - 1];

        if (
            c == '\\' ||
            c == '/'
        ) {
            out[length] = '\0';

            return 1;
        }

        length--;
    }

    out[0] = '\0';

    return 1;
}


static int choose_map(
    char *out,
    size_t size
)
{
    OPENFILENAMEA ofn;

    if (
        out == NULL ||
        size == 0 ||
        size > (size_t)UINT_MAX
    )
        return 0;

    memset(
        &ofn,
        0,
        sizeof(ofn)
    );

    out[0] = '\0';

    ofn.lStructSize =
        sizeof(ofn);

    ofn.lpstrFilter =
        "SSPM maps (*.sspm)\0"
        "*.sspm\0"
        "All files (*.*)\0"
        "*.*\0";

    ofn.lpstrFile =
        out;

    ofn.nMaxFile =
        (DWORD)size;

    ofn.Flags =
        OFN_FILEMUSTEXIST |
        OFN_PATHMUSTEXIST |
        OFN_HIDEREADONLY;

    ofn.lpstrTitle =
        "Choose an SSPM map";

    return
        GetOpenFileNameA(
            &ofn
        ) != 0;
}


/*
 * Quote one Windows command-line argument.
 *
 * This handles spaces and embedded quotes/backslashes properly.
 */
static int quote_windows_arg(
    char *out,
    size_t size,
    const char *arg
)
{
    size_t used = 0;
    const char *p;

    if (
        out == NULL ||
        size == 0 ||
        arg == NULL
    )
        return 0;

#define APPEND_CHAR(ch)                         \
    do {                                        \
        if (used + 1 >= size)                  \
            return 0;                           \
        out[used++] = (char)(ch);              \
    } while (0)

    APPEND_CHAR('"');

    p = arg;

    while (*p != '\0') {
        size_t backslashes = 0;

        while (*p == '\\') {
            backslashes++;
            p++;
        }

        if (*p == '"') {
            size_t i;

            for (i = 0; i < backslashes * 2 + 1; i++)
                APPEND_CHAR('\\');

            APPEND_CHAR('"');

            p++;
        } else {
            size_t i;

            for (i = 0; i < backslashes; i++)
                APPEND_CHAR('\\');

            if (*p != '\0') {
                APPEND_CHAR(*p);
                p++;
            }
        }
    }

    /*
     * Backslashes immediately before the closing quote
     * must be doubled.
     */
    {
        size_t i;

        /*
         * Count trailing backslashes already written.
         */
        size_t trailing = 0;

        while (
            used > trailing &&
            out[used - 1 - trailing] == '\\'
        )
            trailing++;

        for (i = 0; i < trailing; i++)
            APPEND_CHAR('\\');
    }

    APPEND_CHAR('"');

    if (used >= size)
        return 0;

    out[used] = '\0';

#undef APPEND_CHAR

    return 1;
}


static int run_parser(
    const char *map_path,
    unsigned char **output,
    size_t *output_size
)
{
    char directory[MAX_PATH];
    char parser_path[MAX_PATH];

    char quoted_parser[MAX_PATH * 2];
    char quoted_map[32768 * 2];

    char command_line[32768];

    SECURITY_ATTRIBUTES sa;
    STARTUPINFOA si;
    PROCESS_INFORMATION pi;

    HANDLE read_pipe = NULL;
    HANDLE write_pipe = NULL;

    unsigned char *buffer = NULL;

    size_t capacity = 0;
    size_t length = 0;

    DWORD bytes_read;

    if (
        output == NULL ||
        output_size == NULL ||
        map_path == NULL
    )
        return 0;

    *output = NULL;
    *output_size = 0;

    if (
        !get_exe_directory(
            directory,
            sizeof(directory)
        )
    )
        return 0;

    {
        int result =
            snprintf(
                parser_path,
                sizeof(parser_path),
                "%sparser.exe",
                directory
            );

        if (
            result < 0 ||
            (size_t)result >= sizeof(parser_path)
        )
            return 0;
    }

    if (
        !quote_windows_arg(
            quoted_parser,
            sizeof(quoted_parser),
            parser_path
        )
    )
        return 0;

    if (
        !quote_windows_arg(
            quoted_map,
            sizeof(quoted_map),
            map_path
        )
    )
        return 0;

    {
        int result =
            snprintf(
                command_line,
                sizeof(command_line),
                "%s %s",
                quoted_parser,
                quoted_map
            );

        if (
            result < 0 ||
            (size_t)result >= sizeof(command_line)
        )
            return 0;
    }

    memset(
        &sa,
        0,
        sizeof(sa)
    );

    sa.nLength =
        sizeof(sa);

    sa.bInheritHandle =
        TRUE;

    if (
        !CreatePipe(
            &read_pipe,
            &write_pipe,
            &sa,
            0
        )
    )
        return 0;

    if (
        !SetHandleInformation(
            read_pipe,
            HANDLE_FLAG_INHERIT,
            0
        )
    ) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);

        return 0;
    }

    memset(
        &si,
        0,
        sizeof(si)
    );

    memset(
        &pi,
        0,
        sizeof(pi)
    );

    si.cb =
        sizeof(si);

    si.dwFlags =
        STARTF_USESTDHANDLES;

    si.hStdInput =
        GetStdHandle(
            STD_INPUT_HANDLE
        );

    si.hStdOutput =
        write_pipe;

    si.hStdError =
        GetStdHandle(
            STD_ERROR_HANDLE
        );

    if (
        !CreateProcessA(
            parser_path,
            command_line,
            NULL,
            NULL,
            TRUE,
            CREATE_NO_WINDOW,
            NULL,
            directory,
            &si,
            &pi
        )
    ) {
        CloseHandle(read_pipe);
        CloseHandle(write_pipe);

        return 0;
    }

    CloseHandle(write_pipe);
    write_pipe = NULL;

    for (;;) {
        unsigned char temp[65536];

        if (
            !ReadFile(
                read_pipe,
                temp,
                sizeof(temp),
                &bytes_read,
                NULL
            )
        )
            break;

        if (bytes_read == 0)
            break;

        if (
            length >
            SIZE_MAX -
            (size_t)bytes_read
        ) {
            free(buffer);

            CloseHandle(read_pipe);

            WaitForSingleObject(
                pi.hProcess,
                INFINITE
            );

            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);

            return 0;
        }

        if (
            length +
            (size_t)bytes_read >
            capacity
        ) {
            size_t new_capacity;

            if (capacity == 0) {
                new_capacity = 131072;
            } else {
                if (
                    capacity >
                    SIZE_MAX / 2
                ) {
                    free(buffer);

                    CloseHandle(read_pipe);

                    WaitForSingleObject(
                        pi.hProcess,
                        INFINITE
                    );

                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);

                    return 0;
                }

                new_capacity =
                    capacity * 2;
            }

            while (
                new_capacity <
                length +
                (size_t)bytes_read
            ) {
                if (
                    new_capacity >
                    SIZE_MAX / 2
                ) {
                    free(buffer);

                    CloseHandle(read_pipe);

                    WaitForSingleObject(
                        pi.hProcess,
                        INFINITE
                    );

                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);

                    return 0;
                }

                new_capacity *= 2;
            }

            {
                unsigned char *new_buffer =
                    realloc(
                        buffer,
                        new_capacity
                    );

                if (new_buffer == NULL) {
                    free(buffer);

                    CloseHandle(read_pipe);

                    WaitForSingleObject(
                        pi.hProcess,
                        INFINITE
                    );

                    CloseHandle(pi.hThread);
                    CloseHandle(pi.hProcess);

                    return 0;
                }

                buffer =
                    new_buffer;

                capacity =
                    new_capacity;
            }
        }

        memcpy(
            buffer + length,
            temp,
            bytes_read
        );

        length +=
            bytes_read;
    }

    CloseHandle(read_pipe);

    WaitForSingleObject(
        pi.hProcess,
        INFINITE
    );

    {
        DWORD exit_code = 1;

        GetExitCodeProcess(
            pi.hProcess,
            &exit_code
        );

        CloseHandle(
            pi.hThread
        );

        CloseHandle(
            pi.hProcess
        );

        if (exit_code != 0) {
            free(buffer);

            return 0;
        }
    }

    *output =
        buffer;

    *output_size =
        length;

    return 1;
}


/* ============================================================
 * Options
 * ============================================================ */

static float parse_speed(
    const char *s
)
{
    if (s == NULL)
        return 0.0f;

    if (strcmp(s, "---") == 0)
        return 1.0f / 1.35f;

    if (strcmp(s, "--") == 0)
        return 1.0f / 1.25f;

    if (strcmp(s, "-") == 0)
        return 1.0f / 1.15f;

    if (strcmp(s, "normal") == 0)
        return 1.0f;

    if (strcmp(s, "+") == 0)
        return 1.15f;

    if (strcmp(s, "++") == 0)
        return 1.25f;

    if (strcmp(s, "+++") == 0)
        return 1.35f;

    if (strcmp(s, "++++") == 0)
        return 1.45f;

    {
        size_t length =
            strlen(s);

        if (
            length > 1 &&
            s[length - 1] == '%'
        ) {
            char *end;
            double value;

            value =
                strtod(
                    s,
                    &end
                );

            if (
                end ==
                    s + length - 1 &&
                value > 0.0 &&
                value <= 10000.0
            )
                return
                    (float)(
                        value / 100.0
                    );
        }
    }

    return 0.0f;
}


static int parse_options(
    int argc,
    char **argv,
    GameOptions *options
)
{
    int i;

    if (options == NULL)
        return 0;

    options->multiplier =
        1.0f;

    options->nofail =
        0;

    if (argc > 3)
        return 0;

    if (argc >= 2) {
        options->multiplier =
            parse_speed(
                argv[1]
            );

        if (
            options->multiplier <=
            0.0f
        )
            return 0;
    }

    for (
        i = 2;
        i < argc;
        i++
    ) {
        if (
            _stricmp(
                argv[i],
                "nofail"
            ) == 0
        ) {
            options->nofail = 1;
        } else {
            return 0;
        }
    }

    return 1;
}


/* ============================================================
 * Colorset
 * ============================================================ */

static void trim_string(
    char *s
)
{
    char *start;
    char *end;

    if (s == NULL)
        return;

    start = s;

    while (
        *start == ' ' ||
        *start == '\t' ||
        *start == '\r' ||
        *start == '\n'
    )
        start++;

    if (start != s) {
        memmove(
            s,
            start,
            strlen(start) + 1
        );
    }

    end =
        s + strlen(s);

    while (
        end > s &&
        (
            end[-1] == ' ' ||
            end[-1] == '\t' ||
            end[-1] == '\r' ||
            end[-1] == '\n'
        )
    )
        end--;

    *end = '\0';
}


static int parse_hex_color(
    const char *s,
    Color *color
)
{
    char buffer[64];
    const char *p;

    size_t length;

    unsigned long value;

    char *end;

    if (
        s == NULL ||
        color == NULL
    )
        return 0;

    strncpy(
        buffer,
        s,
        sizeof(buffer) - 1
    );

    buffer[
        sizeof(buffer) - 1
    ] = '\0';

    trim_string(buffer);

    p = buffer;

    if (*p == '#')
        p++;

    if (
        p[0] == '0' &&
        (
            p[1] == 'x' ||
            p[1] == 'X'
        )
    )
        p += 2;

    length =
        strlen(p);

    if (
        length != 6 &&
        length != 8
    )
        return 0;

    {
        size_t i;

        for (
            i = 0;
            i < length;
            i++
        ) {
            if (
                !(
                    (
                        p[i] >= '0' &&
                        p[i] <= '9'
                    ) ||
                    (
                        p[i] >= 'a' &&
                        p[i] <= 'f'
                    ) ||
                    (
                        p[i] >= 'A' &&
                        p[i] <= 'F'
                    )
                )
            )
                return 0;
        }
    }

    value =
        strtoul(
            p,
            &end,
            16
        );

    if (
        end == p ||
        *end != '\0'
    )
        return 0;

    if (length == 6) {
        color->r =
            (unsigned char)(
                (value >> 16) &
                0xff
            );

        color->g =
            (unsigned char)(
                (value >> 8) &
                0xff
            );

        color->b =
            (unsigned char)(
                value &
                0xff
            );

        color->a =
            255;
    } else {
        color->r =
            (unsigned char)(
                (value >> 24) &
                0xff
            );

        color->g =
            (unsigned char)(
                (value >> 16) &
                0xff
            );

        color->b =
            (unsigned char)(
                (value >> 8) &
                0xff
            );

        color->a =
            (unsigned char)(
                value &
                0xff
            );
    }

    return 1;
}


static int extract_color_from_line(
    char *line,
    Color *color
)
{
    char *p;
    char *equals;
    char *comma;

    if (
        line == NULL ||
        color == NULL
    )
        return 0;

    trim_string(line);

    if (
        line[0] == '\0' ||
        line[0] == ';'
    )
        return 0;

    if (line[0] != '#') {
        char *comment =
            strchr(
                line,
                '#'
            );

        if (comment != NULL)
            *comment = '\0';
    }

    trim_string(line);

    if (line[0] == '\0')
        return 0;

    equals =
        strchr(
            line,
            '='
        );

    if (equals != NULL) {
        p =
            equals + 1;

        trim_string(p);

        if (
            parse_hex_color(
                p,
                color
            )
        )
            return 1;
    }

    comma =
        strchr(
            line,
            ','
        );

    if (comma != NULL) {
        p =
            comma + 1;

        trim_string(p);

        if (
            parse_hex_color(
                p,
                color
            )
        )
            return 1;
    }

    return
        parse_hex_color(
            line,
            color
        );
}


static int build_exe_relative_path(
    char *out,
    size_t size,
    const char *filename
)
{
    char directory[MAX_PATH];

    int result;

    if (
        out == NULL ||
        size == 0 ||
        filename == NULL
    )
        return 0;

    if (
        !get_exe_directory(
            directory,
            sizeof(directory)
        )
    )
        return 0;

    result =
        snprintf(
            out,
            size,
            "%s%s",
            directory,
            filename
        );

    if (
        result < 0 ||
        (size_t)result >= size
    )
        return 0;

    return 1;
}


static int is_absolute_windows_path(
    const char *path
)
{
    size_t length;

    if (path == NULL)
        return 0;

    length =
        strlen(path);

    if (
        length >= 3 &&
        path[1] == ':' &&
        (
            path[2] == '\\' ||
            path[2] == '/'
        )
    )
        return 1;

    if (
        length >= 2 &&
        path[0] == '\\' &&
        path[1] == '\\'
    )
        return 1;

    if (
        length > 0 &&
        path[0] == '/'
    )
        return 1;

    return 0;
}


static int load_colorset(
    Chart *chart
)
{
    FILE *f;

    char line[1024];

    char config_path[MAX_PATH];
    char colorset_path[MAX_PATH];

    Color *colors = NULL;

    size_t color_count = 0;
    size_t color_capacity = 0;

    uint32_t i;

    if (chart == NULL)
        return 0;

    for (
        i = 0;
        i < chart->note_count;
        i++
    ) {
        chart->notes[i].color =
            WHITE;
    }

    if (
        !build_exe_relative_path(
            config_path,
            sizeof(config_path),
            "config.txt"
        )
    ) {
        fprintf(
            stderr,
            "[COLORSET] Could not build config path.\n"
        );

        return 0;
    }

    fprintf(
        stderr,
        "[COLORSET] Config: %s\n",
        config_path
    );

    f =
        fopen(
            config_path,
            "r"
        );

    if (f == NULL) {
        fprintf(
            stderr,
            "[COLORSET] config.txt not found.\n"
        );

        return 0;
    }

    colorset_path[0] =
        '\0';

    while (
        fgets(
            line,
            sizeof(line),
            f
        )
    ) {
        char *p;

        const char *key =
            "COLORSET_PATH";

        size_t key_length =
            strlen(key);

        trim_string(line);

        if (
            line[0] == '\0' ||
            line[0] == '#' ||
            line[0] == ';'
        )
            continue;

        if (
            _strnicmp(
                line,
                key,
                key_length
            ) != 0
        )
            continue;

        p =
            line + key_length;

        while (
            *p == ' ' ||
            *p == '\t'
        )
            p++;

        if (*p != '=')
            continue;

        p++;

        while (
            *p == ' ' ||
            *p == '\t'
        )
            p++;

        trim_string(p);

        {
            size_t len =
                strlen(p);

            if (
                len >= 2 &&
                (
                    (
                        p[0] == '"' &&
                        p[len - 1] == '"'
                    ) ||
                    (
                        p[0] == '\'' &&
                        p[len - 1] == '\''
                    )
                )
            ) {
                p[len - 1] = '\0';
                p++;
            }
        }

        trim_string(p);

        if (*p != '\0') {
            strncpy(
                colorset_path,
                p,
                sizeof(colorset_path) - 1
            );

            colorset_path[
                sizeof(colorset_path) - 1
            ] = '\0';
        }

        break;
    }

    fclose(f);

    trim_string(
        colorset_path
    );

    if (
        colorset_path[0] == '\0'
    ) {
        fprintf(
            stderr,
            "[COLORSET] COLORSET_PATH missing.\n"
        );

        return 0;
    }

    if (
        !is_absolute_windows_path(
            colorset_path
        )
    ) {
        char resolved[MAX_PATH];

        if (
            !build_exe_relative_path(
                resolved,
                sizeof(resolved),
                colorset_path
            )
        ) {
            fprintf(
                stderr,
                "[COLORSET] Could not resolve relative path.\n"
            );

            return 0;
        }

        strncpy(
            colorset_path,
            resolved,
            sizeof(colorset_path) - 1
        );

        colorset_path[
            sizeof(colorset_path) - 1
        ] = '\0';
    }

    fprintf(
        stderr,
        "[COLORSET] File: %s\n",
        colorset_path
    );

    f =
        fopen(
            colorset_path,
            "r"
        );

    if (f == NULL) {
        fprintf(
            stderr,
            "[COLORSET] Could not open colorset file.\n"
        );

        return 0;
    }

    while (
        fgets(
            line,
            sizeof(line),
            f
        )
    ) {
        Color color;

        if (
            !extract_color_from_line(
                line,
                &color
            )
        )
            continue;

        if (
            color_count >=
            color_capacity
        ) {
            size_t new_capacity;

            if (color_capacity == 0)
                new_capacity = 8;
            else {
                if (
                    color_capacity >
                    SIZE_MAX / 2
                ) {
                    free(colors);
                    fclose(f);
                    return 0;
                }

                new_capacity =
                    color_capacity * 2;
            }

            if (
                new_capacity >
                SIZE_MAX / sizeof(Color)
            ) {
                free(colors);
                fclose(f);
                return 0;
            }

            {
                Color *new_colors =
                    realloc(
                        colors,
                        new_capacity *
                        sizeof(Color)
                    );

                if (new_colors == NULL) {
                    free(colors);
                    fclose(f);
                    return 0;
                }

                colors =
                    new_colors;

                color_capacity =
                    new_capacity;
            }
        }

        colors[
            color_count
        ] = color;

        color_count++;
    }

    fclose(f);

    if (color_count == 0) {
        fprintf(
            stderr,
            "[COLORSET] WARNING: colorset contained no readable colors.\n"
        );

        free(colors);

        return 0;
    }

    for (
        i = 0;
        i < chart->note_count;
        i++
    ) {
        chart->notes[i].color =
            colors[
                (size_t)i %
                color_count
            ];
    }

    fprintf(
        stderr,
        "[COLORSET] Loaded %zu color(s).\n",
        color_count
    );

    fprintf(
        stderr,
        "[COLORSET] Applied colors to %u notes.\n",
        chart->note_count
    );

    fprintf(
        stderr,
        "[COLORSET] First color: #%02X%02X%02X%02X\n",
        colors[0].r,
        colors[0].g,
        colors[0].b,
        colors[0].a
    );

    free(colors);

    return 1;
}


/* ============================================================
 * Gameplay math
 * ============================================================ */

static Vector3 note_world_position(
    const Note *note,
    double song_time
)
{
    double note_time;
    double distance;

    if (note == NULL) {
        return (Vector3){
            0.0f,
            0.0f,
            0.0f
        };
    }

    note_time =
        (double)note->time /
        1000.0;

    distance =
        (
            note_time -
            song_time
        ) *
        APPROACH_RATE_M_S;

    return (Vector3){
        note->x -
            GRID_CENTER,

        -(note->y -
            GRID_CENTER),

        (float)distance
    };
}


static uint32_t lower_bound_note_time(
    const Chart *chart,
    double target_time
)
{
    uint32_t left;
    uint32_t right;

    if (
        chart == NULL ||
        chart->note_count == 0
    )
        return 0;

    left = 0;
    right = chart->note_count;

    while (left < right) {
        uint32_t middle =
            left +
            (right - left) / 2;

        double note_time =
            (double)chart->notes[middle].time /
            1000.0;

        if (
            note_time <
            target_time
        )
            left =
                middle + 1;
        else
            right =
                middle;
    }

    return left;
}


static void get_render_range(
    const Chart *chart,
    double song_time,
    uint32_t *first,
    uint32_t *last
)
{
    double spawn_seconds;

    double first_time;
    double last_time;

    if (
        first == NULL ||
        last == NULL
    )
        return;

    *first = 0;
    *last = 0;

    if (
        chart == NULL ||
        chart->note_count == 0
    )
        return;

    if (
        APPROACH_RATE_M_S >
        0.000001f
    ) {
        spawn_seconds =
            (double)SPAWN_DISTANCE_M /
            (double)APPROACH_RATE_M_S;
    } else {
        spawn_seconds = 0.0;
    }

    first_time =
        song_time -
        (
            (double)HIT_WINDOW_MS /
            1000.0
        ) -
        RENDER_TIME_PADDING;

    last_time =
        song_time +
        spawn_seconds +
        RENDER_TIME_PADDING;

    *first =
        lower_bound_note_time(
            chart,
            first_time
        );

    *last =
        lower_bound_note_time(
            chart,
            last_time
        );
}




/* ============================================================
 * Hit detection
 *
 * A note becomes hittable at its scheduled hit time and remains
 * hittable for HIT_WINDOW_MS afterward.
 *
 * The hitbox is centered on the note's position at its hit time.
 * Its size is NOTE_SIZE * HITBOX_SIZE.
 *
 * No raycasting is used for note collision.
 * ============================================================ */
static int find_hit_note(
    Chart *chart,
    Vector3 cursor,
    double song_time
)
{
    uint32_t i;

    double hit_window_seconds =
        (double)HIT_WINDOW_MS / 1000.0;

    float hitbox_size =
        NOTE_SIZE * HITBOX_SIZE;

    float half_hitbox =
        hitbox_size * 0.5f;

    if (
        chart == NULL ||
        chart->notes == NULL
    )
        return -1;

    for (
        i = 0;
        i < chart->note_count;
        i++
    ) {
        Note *note =
            &chart->notes[i];

        double note_time;

        float note_x;
        float note_y;

        if (note->state != 0)
            continue;

        note_time =
            (double)note->time / 1000.0;

        /*
         * Only allow hits from the note's scheduled
         * time through HIT_WINDOW_MS afterward.
         */
        if (
            song_time < note_time ||
            song_time > note_time + hit_window_seconds
        )
            continue;

        /*
         * Note position on the hit plane.
         */
        note_x =
            note->x - GRID_CENTER;

        note_y =
            -(note->y - GRID_CENTER);

        /*
         * Invisible hitbox.
         */
        if (
            cursor.x >= note_x - half_hitbox &&
            cursor.x <= note_x + half_hitbox &&
            cursor.y >= note_y - half_hitbox &&
            cursor.y <= note_y + half_hitbox
        ) {
            return (int)i;
        }
    }

    return -1;
}

/* ============================================================
 * Miss processing
 * ============================================================ */

static void update_misses(
    Chart *chart,
    double song_time,
    int *misses,
    int *combo,
    uint32_t *next_miss_index,
    int nofail
)
{
    double hit_window_seconds;

    if (
        chart == NULL ||
        misses == NULL ||
        combo == NULL ||
        next_miss_index == NULL
    )
        return;

    hit_window_seconds =
        (double)HIT_WINDOW_MS /
        1000.0;

    while (
        *next_miss_index <
        chart->note_count
    ) {
        uint32_t index =
            *next_miss_index;

        Note *note =
            &chart->notes[index];

        double note_time;

        note_time =
            (double)note->time /
            1000.0;

        /*
         * Nothing can be missed yet.
         */
        if (
            song_time <=
            note_time +
            hit_window_seconds
        )
            break;

        /*
         * Already hit.
         */
        if (
            note->state == 1
        ) {
            (*next_miss_index)++;
            continue;
        }

        /*
         * Timing window expired.
         */
        if (
            note->state == 0
        ) {
            note->state = 2;

            if (!nofail) {
                (*misses)++;
                *combo = 0;
            }

#if CYTHIA_DEBUG_HITS
            fprintf(
                stderr,
                "[MISS] note=%u target=%.3f current=%.3f\n",
                index,
                note_time,
                song_time
            );
#endif
        }

        (*next_miss_index)++;
    }
}


/* ============================================================
 * Rendering
 * ============================================================ */

static void draw_grid(
    float parallax_x,
    float parallax_y
)
{
    int i;

    const float min = -1.5f;
    const float max = 1.5f;

    Color grid_color =
        (Color){
            70,
            70,
            70,
            255
        };

    Color edge_color =
        (Color){
            120,
            120,
            120,
            255
        };

    for (
        i = 0;
        i <= 3;
        i++
    ) {
        float p =
            min +
            (float)i;

        DrawLine3D(
            (Vector3){
                p + parallax_x,
                min + parallax_y,
                0.0f
            },
            (Vector3){
                p + parallax_x,
                max + parallax_y,
                0.0f
            },
            grid_color
        );

        DrawLine3D(
            (Vector3){
                min + parallax_x,
                p + parallax_y,
                0.0f
            },
            (Vector3){
                max + parallax_x,
                p + parallax_y,
                0.0f
            },
            grid_color
        );
    }

    DrawLine3D(
        (Vector3){
            min + parallax_x,
            min + parallax_y,
            0.0f
        },
        (Vector3){
            max + parallax_x,
            min + parallax_y,
            0.0f
        },
        edge_color
    );

    DrawLine3D(
        (Vector3){
            max + parallax_x,
            min + parallax_y,
            0.0f
        },
        (Vector3){
            max + parallax_x,
            max + parallax_y,
            0.0f
        },
        edge_color
    );

    DrawLine3D(
        (Vector3){
            max + parallax_x,
            max + parallax_y,
            0.0f
        },
        (Vector3){
            min + parallax_x,
            max + parallax_y,
            0.0f
        },
        edge_color
    );

    DrawLine3D(
        (Vector3){
            min + parallax_x,
            max + parallax_y,
            0.0f
        },
        (Vector3){
            min + parallax_x,
            min + parallax_y,
            0.0f
        },
        edge_color
    );
}


static void draw_hud(
    const Chart *chart,
    int score,
    int hits,
    int misses,
    double song_time
)
{
    int total;

    float accuracy;

    char fps_text[64];
    char score_text[64];
    char accuracy_text[64];
    char time_text[64];

    (void)chart;

    total =
        hits +
        misses;

    accuracy =
        total > 0
            ? (
                (float)hits /
                (float)total
            ) * 100.0f
            : 100.0f;

    snprintf(
        fps_text,
        sizeof(fps_text),
        "FPS: %d",
        GetFPS()
    );

    snprintf(
        score_text,
        sizeof(score_text),
        "Score: %d",
        score
    );

    snprintf(
        accuracy_text,
        sizeof(accuracy_text),
        "Accuracy: %.2f%%",
        accuracy
    );

    snprintf(
        time_text,
        sizeof(time_text),
        "Time: %.3f",
        song_time
    );

    DrawText(
        CYTHIA_VERSION,
        20,
        20,
        24,
        WHITE
    );

    DrawText(
        fps_text,
        20,
        48,
        20,
        WHITE
    );

    DrawText(
        score_text,
        20,
        76,
        20,
        WHITE
    );

    DrawText(
        accuracy_text,
        20,
        102,
        20,
        WHITE
    );

#if CYTHIA_DEBUG_HITS
    DrawText(
        time_text,
        20,
        128,
        20,
        WHITE
    );
#endif
}


static int build_asset_path(
    char *out,
    size_t size,
    const char *filename
)
{
    char directory[MAX_PATH];

    int result;

    if (
        out == NULL ||
        size == 0 ||
        filename == NULL
    )
        return 0;

    if (
        !get_exe_directory(
            directory,
            sizeof(directory)
        )
    )
        return 0;

    result =
        snprintf(
            out,
            size,
            "%sassets\\%s",
            directory,
            filename
        );

    if (
        result < 0 ||
        (size_t)result >= size
    )
        return 0;

    return 1;
}


/* ============================================================
 * Audio helpers
 * ============================================================ */

static const char *detect_audio_extension(
    const unsigned char *data,
    uint64_t size
)
{
    if (
        data != NULL &&
        size >= 4 &&
        memcmp(
            data,
            "OggS",
            4
        ) == 0
    )
        return ".ogg";

    return ".mp3";
}


static double get_song_time(
    double raw_audio_time,
    double fallback_start_time,
    float multiplier
)
{
    if (
        isfinite(raw_audio_time) &&
        raw_audio_time >=
            AUDIO_CLOCK_EPSILON
    ) {
        return
            raw_audio_time;
    }

    return
        (
            GetTime() -
            fallback_start_time
        ) *
        (double)multiplier;
}


/* ============================================================
 * Main
 * ============================================================ */

int main(
    int argc,
    char **argv
)
{
    GameOptions options;
    Chart chart;

    unsigned char *rmap_data =
        NULL;

    size_t rmap_size =
        0;

    Music music =
        {0};

    Texture2D note_texture =
        {0};

    Texture2D border_texture =
        {0};

    Texture2D cursor_texture =
        {0};

    Camera3D camera =
        {0};

    int score =
        0;

    int hits =
        0;

    int misses =
        0;

    int combo =
        0;

    uint32_t next_miss_index =
        0;

    double game_start_time =
        0.0;

    double song_time =
        0.0;

    Vector3 cursor =
        {
            0.0f,
            0.0f,
            0.0f
        };

    char map_path[32768];

    char note_path[MAX_PATH];
    char border_path[MAX_PATH];
    char cursor_path[MAX_PATH];

    memset(
        &chart,
        0,
        sizeof(chart)
    );

    if (
        !parse_options(
            argc,
            argv,
            &options
        )
    ) {
        fprintf(
            stderr,
            "usage: %s [speed] [nofail]\n",
            argv[0]
        );

        fprintf(
            stderr,
            "speed: ---, --, -, normal, +, ++, +++, ++++ or percentage\n"
        );

        return 1;
    }

    fprintf(
        stderr,
        "[CYTHIA] Starting...\n"
    );

    fprintf(
        stderr,
        "[CYTHIA] Version: %s\n",
        CYTHIA_VERSION
    );

    fprintf(
        stderr,
        "[CYTHIA] Speed multiplier: %.3f\n",
        options.multiplier
    );

    if (
        !choose_map(
            map_path,
            sizeof(map_path)
        )
    )
        return 0;

    fprintf(
        stderr,
        "[CYTHIA] Selected map: %s\n",
        map_path
    );

    if (
        !run_parser(
            map_path,
            &rmap_data,
            &rmap_size
        )
    ) {
        fprintf(
            stderr,
            "parser.exe failed\n"
        );

        return 2;
    }

    fprintf(
        stderr,
        "[CYTHIA] Parser output: %zu bytes\n",
        rmap_size
    );

    if (
        !read_rmap(
            rmap_data,
            rmap_size,
            &chart
        )
    ) {
        fprintf(
            stderr,
            "invalid parser output\n"
        );

        free(rmap_data);

        return 3;
    }

    free(rmap_data);
    rmap_data = NULL;

    fprintf(
        stderr,
        "[CYTHIA] Map: %s\n",
        chart.map_name != NULL
            ? chart.map_name
            : "(unnamed)"
    );

    fprintf(
        stderr,
        "[CYTHIA] Song: %s\n",
        chart.song_name != NULL
            ? chart.song_name
            : "(unnamed)"
    );

    fprintf(
        stderr,
        "[CYTHIA] Notes: %u\n",
        chart.note_count
    );

    fprintf(
        stderr,
        "[CYTHIA] Audio: %llu bytes\n",
        (unsigned long long)
            chart.audio_length
    );


    /* --------------------------------------------------------
     * Colorset
     * -------------------------------------------------------- */

    if (
        !load_colorset(
            &chart
        )
    ) {
        fprintf(
            stderr,
            "[COLORSET] Continuing with default WHITE notes.\n"
        );
    }


    /* --------------------------------------------------------
     * Window
     * -------------------------------------------------------- */

    fprintf(
        stderr,
        "[CYTHIA] Initializing fullscreen window...\n"
    );

    {
        int monitor =
            GetCurrentMonitor();

        int width =
            GetMonitorWidth(
                monitor
            );

        int height =
            GetMonitorHeight(
                monitor
            );

        SetConfigFlags(
            FLAG_FULLSCREEN_MODE
        );

        InitWindow(
            width,
            height,
            "Cythia"
        );
    }

    if (
        !IsWindowReady()
    ) {
        fprintf(
            stderr,
            "failed to initialize window\n"
        );

        free_chart(
            &chart
        );

        return 6;
    }

    fprintf(
        stderr,
        "[CYTHIA] Fullscreen window initialized.\n"
    );

    SetTargetFPS(
        FPS_LIMIT
    );


    /* --------------------------------------------------------
     * Audio
     * -------------------------------------------------------- */

#if CYTHIA_ENABLE_AUDIO

    fprintf(
        stderr,
        "[AUDIO] Initializing audio device...\n"
    );

    InitAudioDevice();

    if (
        !IsAudioDeviceReady()
    ) {
        fprintf(
            stderr,
            "[AUDIO] ERROR: audio device failed to initialize.\n"
        );

        CloseWindow();
        free_chart(&chart);

        return 7;
    }

    fprintf(
        stderr,
        "[AUDIO] Audio device ready.\n"
    );

#endif


#if CYTHIA_ENABLE_AUDIO

    if (
        chart.audio_length == 0
    ) {
        fprintf(
            stderr,
            "[AUDIO] ERROR: map has no audio.\n"
        );

        CloseAudioDevice();
        CloseWindow();
        free_chart(&chart);

        return 4;
    }

    {
        const char *file_type =
            detect_audio_extension(
                chart.audio,
                chart.audio_length
            );

        fprintf(
            stderr,
            "[AUDIO] Loading embedded audio as %s...\n",
            file_type
        );

        if (
            chart.audio_length >
            (uint64_t)INT_MAX
        ) {
            fprintf(
                stderr,
                "[AUDIO] ERROR: audio is too large for raylib memory loader.\n"
            );

            CloseAudioDevice();
            CloseWindow();
            free_chart(&chart);

            return 5;
        }

        music =
            LoadMusicStreamFromMemory(
                file_type,
                chart.audio,
                (int)chart.audio_length
            );
    }

    if (
        !IsMusicValid(music)
    ) {
        fprintf(
            stderr,
            "[AUDIO] ERROR: failed to load embedded audio.\n"
        );

        CloseAudioDevice();
        CloseWindow();
        free_chart(&chart);

        return 5;
    }

    SetMusicPitch(
        music,
        options.multiplier
    );

    SetMusicVolume(
        music,
        1.0f
    );

    fprintf(
        stderr,
        "[AUDIO] Music stream loaded successfully.\n"
    );

    fprintf(
        stderr,
        "[AUDIO] Length: %.3f seconds\n",
        GetMusicTimeLength(music)
    );

#endif


    /* --------------------------------------------------------
     * Camera
     * -------------------------------------------------------- */

    camera.position =
        (Vector3){
            0.0f,
            0.0f,
            -5.0f
        };

    camera.target =
        (Vector3){
            0.0f,
            0.0f,
            0.0f
        };

    camera.up =
        (Vector3){
            0.0f,
            1.0f,
            0.0f
        };

    camera.fovy =
        FOV;

    camera.projection =
        CAMERA_PERSPECTIVE;


    /* --------------------------------------------------------
     * Asset paths
     * -------------------------------------------------------- */

    if (
        !build_asset_path(
            note_path,
            sizeof(note_path),
            "note.png"
        ) ||
        !build_asset_path(
            border_path,
            sizeof(border_path),
            "border.png"
        ) ||
        !build_asset_path(
            cursor_path,
            sizeof(cursor_path),
            "cursor.png"
        )
    ) {
        fprintf(
            stderr,
            "failed to build asset paths\n"
        );

#if CYTHIA_ENABLE_AUDIO
        UnloadMusicStream(music);
        CloseAudioDevice();
#endif

        free_chart(
            &chart
        );

        CloseWindow();

        return 6;
    }


    /* --------------------------------------------------------
     * Textures
     * -------------------------------------------------------- */

    note_texture =
        LoadTexture(
            note_path
        );

    border_texture =
        LoadTexture(
            border_path
        );

    cursor_texture =
        LoadTexture(
            cursor_path
        );

    if (
        !IsTextureValid(
            note_texture
        ) ||
        !IsTextureValid(
            border_texture
        ) ||
        !IsTextureValid(
            cursor_texture
        )
    ) {
        fprintf(
            stderr,
            "failed to load gameplay textures\n"
        );

        if (
            IsTextureValid(
                note_texture
            )
        )
            UnloadTexture(
                note_texture
            );

        if (
            IsTextureValid(
                border_texture
            )
        )
            UnloadTexture(
                border_texture
            );

        if (
            IsTextureValid(
                cursor_texture
            )
        )
            UnloadTexture(
                cursor_texture
            );

#if CYTHIA_ENABLE_AUDIO
        UnloadMusicStream(music);
        CloseAudioDevice();
#endif

        free_chart(
            &chart
        );

        CloseWindow();

        return 6;
    }

    fprintf(
        stderr,
        "[CYTHIA] All gameplay textures loaded.\n"
    );


    /* --------------------------------------------------------
     * Start audio
     * -------------------------------------------------------- */

#if CYTHIA_ENABLE_AUDIO

    fprintf(
        stderr,
        "[AUDIO] Starting music...\n"
    );

    PlayMusicStream(
        music
    );

    UpdateMusicStream(
        music
    );

    game_start_time =
        GetTime();

    fprintf(
        stderr,
        "[AUDIO] Music started.\n"
    );

#endif

#if !CYTHIA_ENABLE_AUDIO

    game_start_time =
        GetTime();

#endif

    song_time =
        0.0;

    HideCursor();

    fprintf(
        stderr,
        "[CYTHIA] Entering game loop.\n"
    );


    /* --------------------------------------------------------
     * Game loop
     * -------------------------------------------------------- */

    while (
        !WindowShouldClose()
    ) {
        Vector2 mouse;
        int hit_index;

#if CYTHIA_ENABLE_AUDIO

        double raw_audio_time;

        /*
         * Update streaming audio exactly once per frame.
         */
        UpdateMusicStream(
            music
        );

        /*
         * Read audio clock exactly once.
         */
        raw_audio_time =
            (double)GetMusicTimePlayed(
                music
            );

        song_time =
            get_song_time(
                raw_audio_time,
                game_start_time,
                options.multiplier
            );

#else

        song_time =
            (
                GetTime() -
                game_start_time
            ) *
            (double)options.multiplier;

#endif

        if (
            !isfinite(song_time) ||
            song_time < 0.0
        )
            song_time =
                0.0;


#if CYTHIA_DEBUG_AUDIO

#if CYTHIA_ENABLE_AUDIO
        {
            static double last_audio_debug =
                -100.0;

            if (
                song_time -
                last_audio_debug >=
                1.0
            ) {
                fprintf(
                    stderr,
                    "[AUDIO] song_time=%.3f played=%.3f playing=%d\n",
                    song_time,
                    raw_audio_time,
                    IsMusicStreamPlaying(music)
                );

                last_audio_debug =
                    song_time;
            }
        }
#endif

#endif


        /* ----------------------------------------------------
         * Input
         * ---------------------------------------------------- */

        mouse =
            GetMousePosition();


        /* ----------------------------------------------------
         * Cursor world position
         * ---------------------------------------------------- */

        {
            Ray ray =
                GetMouseRay(
                    mouse,
                    camera
                );

            cursor =
                (Vector3){
                    0.0f,
                    0.0f,
                    0.0f
                };

            if (
                fabsf(
                    ray.direction.z
                ) > 0.00001f
            ) {
                float t =
                    -ray.position.z /
                    ray.direction.z;

                if (
                    t >= 0.0f
                ) {
                    cursor.x =
                        ray.position.x +
                        ray.direction.x *
                        t;

                    cursor.y =
                        ray.position.y +
                        ray.direction.y *
                        t;

                    cursor.z =
                        0.0f;
                }
            }

            if (
                ABSOLUTE_MODE
            ) {
                if (
                    cursor.x < -1.5f
                )
                    cursor.x =
                        -1.5f;

                if (
                    cursor.x > 1.5f
                )
                    cursor.x =
                        1.5f;

                if (
                    cursor.y < -1.5f
                )
                    cursor.y =
                        -1.5f;

                if (
                    cursor.y > 1.5f
                )
                    cursor.y =
                        1.5f;
            }
        }


        /* ----------------------------------------------------
         * Camera
         * ---------------------------------------------------- */

        {
            float cam_x =
                0.0f;

            float cam_y =
                0.0f;

            if (
                CAM_PARALLAX != 0
            ) {
                cam_x =
                    cursor.x *
                    (float)CAM_PARALLAX *
                    0.025f;

                cam_y =
                    cursor.y *
                    (float)CAM_PARALLAX *
                    0.025f;
            }

            camera.position.x =
                cam_x;

            camera.position.y =
                cam_y;

            camera.position.z =
                -5.0f;

            if (
                CAMERA_UNLOCK
            ) {
                Vector2 delta =
                    GetMouseDelta();

                static float yaw =
                    0.0f;

                static float pitch =
                    0.0f;

                yaw -=
                    delta.x *
                    0.0025f;

                pitch -=
                    delta.y *
                    0.0025f;

                if (
                    pitch < -1.55f
                )
                    pitch =
                        -1.55f;

                if (
                    pitch > 1.55f
                )
                    pitch =
                        1.55f;

                camera.target.x =
                    camera.position.x +
                    sinf(yaw) *
                    cosf(pitch);

                camera.target.y =
                    camera.position.y +
                    sinf(pitch);

                camera.target.z =
                    camera.position.z +
                    cosf(yaw) *
                    cosf(pitch);
            } else {
                camera.target.x =
                    cam_x;

                camera.target.y =
                    cam_y;

                camera.target.z =
                    0.0f;
            }
        }


        /* ----------------------------------------------------
         * Hit detection
         * ---------------------------------------------------- */

        if (
            IsMouseButtonPressed(
                MOUSE_BUTTON_LEFT
            )
        ) {
            hit_index =
                find_hit_note(
                     &chart,
                     cursor,
                     song_time
                );

#if CYTHIA_DEBUG_HITS

            fprintf(
                stderr,
                "[CLICK] mouse=(%.1f, %.1f) time=%.3f index=%d\n",
                mouse.x,
                mouse.y,
                song_time,
                hit_index
            );

#endif

            if (
                hit_index >= 0
            ) {
                double error;
                int points;

                error =
                    fabs(
                        song_time -
                        (
                            (double)
                            chart.notes[
                                hit_index
                            ].time /
                            1000.0
                        )
                    );

                if (
                    error <= 0.010
                )
                    points = 1000;
                else if (
                    error <= 0.025
                )
                    points = 900;
                else if (
                    error <= 0.050
                )
                    points = 800;
                else
                    points = 700;

                combo++;

                score +=
                    points *
                    (
                        combo >= 10
                            ? 2
                            : 1
                    );

                hits++;

                chart.notes[
                    hit_index
                ].state =
                    1;

#if CYTHIA_DEBUG_HITS

                fprintf(
                    stderr,
                    "[HIT] SUCCESS note=%d error=%.2fms score=%d combo=%d\n",
                    hit_index,
                    error * 1000.0,
                    score,
                    combo
                );

#endif
            } else {

#if CYTHIA_DEBUG_HITS

                fprintf(
                    stderr,
                    "[CLICK] No hittable note at cursor/time.\n"
                );

#endif

            }
        }


        /* ----------------------------------------------------
         * Misses
         * ---------------------------------------------------- */

        update_misses(
            &chart,
            song_time,
            &misses,
            &combo,
            &next_miss_index,
            options.nofail
        );


        /* ----------------------------------------------------
         * Drawing
         * ---------------------------------------------------- */

        BeginDrawing();

        ClearBackground(
            (Color){
                8,
                8,
                12,
                255
            }
        );

        BeginMode3D(
            camera
        );


        /* ----------------------------------------------------
         * Grid
         * ---------------------------------------------------- */

        {
            float grid_x =
                0.0f;

            float grid_y =
                0.0f;

            if (
                GRID_PARALLAX != 0
            ) {
                grid_x =
                    cursor.x *
                    (float)GRID_PARALLAX *
                    0.025f;

                grid_y =
                    cursor.y *
                    (float)GRID_PARALLAX *
                    0.025f;
            }

            draw_grid(
                grid_x,
                grid_y
            );
        }


        /* ----------------------------------------------------
         * Notes
         * ---------------------------------------------------- */

        {
            uint32_t first;
            uint32_t last;
            uint32_t i;

            float border_cell_w =
                (float)
                border_texture.width /
                3.0f;

            float border_cell_h =
                (float)
                border_texture.height /
                3.0f;

            get_render_range(
                &chart,
                song_time,
                &first,
                &last
            );

            for (
                i = first;
                i < last;
                i++
            ) {
                Note *note =
                    &chart.notes[i];

                Vector3 position;

                float distance;

                Rectangle source;

                if (
                    note->state != 0
                )
                    continue;

                position =
                    note_world_position(
                        note,
                        song_time
                    );

                if (
                    !isfinite(
                        position.x
                    ) ||
                    !isfinite(
                        position.y
                    ) ||
                    !isfinite(
                        position.z
                    )
                )
                    continue;

                distance =
                    position.z;

                /*
                 * Don't draw before spawn.
                 */
                if (
                    distance >
                    SPAWN_DISTANCE_M +
                    (
                        float)(
                            RENDER_TIME_PADDING *
                            APPROACH_RATE_M_S
                        )
                )
                    continue;

                /*
                 * Don't draw long after the hit window.
                 */
                if (
                    distance < 0.0f &&
                    song_time >
                    (
                        (double)
                        note->time /
                        1000.0
                    ) +
                    (
                        (double)
                        HIT_WINDOW_MS /
                        1000.0
                    )
                )
                    continue;


                /* ------------------------------------------------
                 * Border spritesheet
                 * ------------------------------------------------ */

                if (
                    fabsf(
                        note->x -
                        roundf(note->x)
                    ) < 0.001f &&
                    fabsf(
                        note->y -
                        roundf(note->y)
                    ) < 0.001f &&
                    note->x >= 0.0f &&
                    note->x <= 2.0f &&
                    note->y >= 0.0f &&
                    note->y <= 2.0f
                ) {
                    source =
                        (Rectangle){
                            note->x *
                                border_cell_w,

                            note->y *
                                border_cell_h,

                            border_cell_w,
                            border_cell_h
                        };
                } else {
                    source =
                        (Rectangle){
                            border_cell_w,
                            border_cell_h,
                            border_cell_w,
                            border_cell_h
                        };
                }


                /* ------------------------------------------------
                 * Border
                 * ------------------------------------------------ */

                DrawBillboardRec(
                    camera,
                    border_texture,
                    source,
                    position,
                    (Vector2){
                        NOTE_SIZE,
                        NOTE_SIZE
                    },
                    WHITE
                );


                /* ------------------------------------------------
                 * Colored note
                 *
                 * DrawBillboard() uses NOTE_SIZE as width and
                 * automatically preserves the texture aspect
                 * ratio for height.
                 * ------------------------------------------------ */

                DrawBillboard(
                    camera,
                    note_texture,
                    position,
                    NOTE_SIZE,
                    note->color
                );
            }
        }


        /* ----------------------------------------------------
         * Cursor
         * ---------------------------------------------------- */

        DrawBillboard(
            camera,
            cursor_texture,
            cursor,
            CURSOR_SIZE,
            WHITE
        );


        EndMode3D();


        /* ----------------------------------------------------
         * HUD
         * ---------------------------------------------------- */

        draw_hud(
            &chart,
            score,
            hits,
            misses,
            song_time
        );

        EndDrawing();
    }


    /* --------------------------------------------------------
     * Shutdown
     * -------------------------------------------------------- */

    fprintf(
        stderr,
        "[CYTHIA] Shutting down...\n"
    );

#if CYTHIA_ENABLE_AUDIO

    if (
        IsMusicValid(music)
    ) {
        StopMusicStream(
            music
        );

        UnloadMusicStream(
            music
        );
    }

    if (
        IsAudioDeviceReady()
    ) {
        CloseAudioDevice();
    }

#endif

    if (
        IsTextureValid(
            note_texture
        )
    )
        UnloadTexture(
            note_texture
        );

    if (
        IsTextureValid(
            border_texture
        )
    )
        UnloadTexture(
            border_texture
        );

    if (
        IsTextureValid(
            cursor_texture
        )
    )
        UnloadTexture(
            cursor_texture
        );

    CloseWindow();

    free_chart(
        &chart
    );

    fprintf(
        stderr,
        "[CYTHIA] Clean shutdown.\n"
    );

    return 0;
}