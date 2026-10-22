/*
 * Vendored from Varnish.pak (include/varnish.h). Varnish ships this header for
 * paks to copy; update it from there rather than editing it here.
 *
 * varnish.h — Header-only client library for Varnish overlay system.
 *
 * Include this header in your Pak to show on-screen pill notifications
 * via the Varnish daemon.  No linking required — just include and call.
 *
 * Requires: <fcntl.h>, <stdio.h>, <stdlib.h>, <string.h>, <unistd.h>
 *
 * Example:
 *   #include <varnish.h>
 *
 *   // Show a pill at bottom-center for 5 seconds
 *   varnish_pill("mypak", "bottom-center", 5, "Download complete!");
 *
 *   // Show an indefinite pill (stays until hidden)
 *   varnish_pill("mypak", "top-right", 0, "Syncing...");
 *
 *   // Hide it
 *   varnish_hide("mypak");
 *
 * Positions: top-left, top-center, top-right,
 *            bottom-left, bottom-center, bottom-right, center
 *
 * If the Varnish daemon is not running, calls fail silently (return -1).
 * Invalid client IDs / positions also return -1. Newlines in text are
 * normalized to spaces before the command is written to the FIFO.
 */

#ifndef VARNISH_CLIENT_H
#define VARNISH_CLIENT_H

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>

#ifndef VARNISH_FIFO
#define VARNISH_FIFO "/tmp/varnish.fifo"
#endif

static inline int varnish_valid_client_id(const char *client_id) {
    const unsigned char *p = (const unsigned char *)client_id;

    if (!client_id || !client_id[0])
        return 0;

    while (*p) {
        if (!isalnum(*p) && *p != '_' && *p != '-' && *p != '.')
            return 0;
        p++;
    }

    return 1;
}

static inline int varnish_valid_position(const char *position) {
    return position &&
           (strcmp(position, "top-left") == 0 ||
            strcmp(position, "top-center") == 0 ||
            strcmp(position, "top-right") == 0 ||
            strcmp(position, "bottom-left") == 0 ||
            strcmp(position, "bottom-center") == 0 ||
            strcmp(position, "bottom-right") == 0 ||
            strcmp(position, "center") == 0);
}

static inline void varnish_sanitize_text(const char *src, char *dst, size_t dst_size) {
    size_t i = 0;

    if (!dst || dst_size == 0)
        return;

    if (!src) {
        dst[0] = '\0';
        return;
    }

    while (*src && i + 1 < dst_size) {
        char ch = *src++;
        if (ch == '\n' || ch == '\r')
            ch = ' ';
        dst[i++] = ch;
    }

    dst[i] = '\0';
}

static inline int varnish_send_line(const char *line) {
    int fd;
    size_t len;
    ssize_t wrote;

    if (!line || !line[0])
        return -1;

    fd = open(VARNISH_FIFO, O_WRONLY | O_NONBLOCK);
    if (fd < 0)
        return -1;

    len = strlen(line);
    do {
        wrote = write(fd, line, len);
    } while (wrote < 0 && errno == EINTR);
    close(fd);
    return wrote == (ssize_t)len ? 0 : -1;
}

/*
 * Show a pill notification.
 *
 * client_id:      Short identifier for your Pak (e.g., "menulody").
 *                 Each client_id gets one slot — calling again with the
 *                 same client_id replaces the previous pill.
 * position:       One of: "top-left", "top-center", "top-right",
 *                         "bottom-left", "bottom-center", "bottom-right",
 *                         "center"
 * duration_secs:  How long to show (seconds). 0 = indefinite (until varnish_hide).
 * text:           The text to display in the pill.
 *
 * Returns 0 on success, -1 if daemon is not running.
 */
static inline int varnish_pill(const char *client_id, const char *position,
                               int duration_secs, const char *text) {
    char sanitized_text[256];
    char line[384];
    int line_len;

    if (!varnish_valid_client_id(client_id) ||
        !varnish_valid_position(position) ||
        duration_secs < 0 || !text) {
        return -1;
    }

    varnish_sanitize_text(text, sanitized_text, sizeof(sanitized_text));
    line_len = snprintf(line, sizeof(line), "PILL %s %s %d %s\n",
                        client_id, position, duration_secs, sanitized_text);
    if (line_len < 0 || line_len >= (int)sizeof(line))
        return -1;

    return varnish_send_line(line);
}

static inline int varnish_valid_sheet(const char *sheet) {
    const char *first;

    if (!sheet || sheet[0] != '/') return 0;
    first = strchr(sheet, '%');
    if (!first || first[1] != 'd') return 0;
    return strchr(first + 2, '%') == NULL;
}

/*
 * Show one icon from a NextUI-style asset sheet, in a themed pill.
 *
 * sheet_path:     Absolute path with a single %d for the device scale, like
 *                 NextUI's assets@%dx.png. Ship one pixel-exact sheet per
 *                 scale; Varnish never resamples.
 * x, y, w, h:     The icon's rectangle on the sheet in 1x units (Varnish
 *                 multiplies by the scale, as NextUI's SCALE4 does).
 *
 * The icon is recoloured with the theme's hint colour (NextUI's color6), the
 * same as NextUI's own status icons, so draw it white on transparent. Show
 * state changes as different icons on the sheet. Same client_id, position and
 * duration rules as varnish_pill.
 *
 * Returns 0 on success, -1 if daemon is not running or arguments are invalid.
 */
static inline int varnish_icon(const char *client_id, const char *position,
                               int duration_secs, int x, int y, int w, int h,
                               const char *sheet_path) {
    char sanitized_path[256];
    char line[384];
    int line_len;

    if (!varnish_valid_client_id(client_id) ||
        !varnish_valid_position(position) ||
        duration_secs < 0 || x < 0 || y < 0 || w <= 0 || h <= 0 ||
        !varnish_valid_sheet(sheet_path)) {
        return -1;
    }

    varnish_sanitize_text(sheet_path, sanitized_path, sizeof(sanitized_path));
    line_len = snprintf(line, sizeof(line), "ICON %s %s %d %d %d %d %d %s\n",
                        client_id, position, duration_secs, x, y, w, h, sanitized_path);
    if (line_len < 0 || line_len >= (int)sizeof(line))
        return -1;

    return varnish_send_line(line);
}

#ifndef VARNISH_MENU_MAX_ITEMS
#define VARNISH_MENU_MAX_ITEMS 6
#endif
#ifndef VARNISH_MENU_MAX_HINTS
#define VARNISH_MENU_MAX_HINTS 2
#endif

static inline int varnish_valid_menu_hints(const char *hints) {
    int count = 0;

    int total = 0, split = 0;

    if (!hints || !hints[0]) return 0;
    if (strcmp(hints, "-") == 0) return 1;

    /* [LEFT;]RIGHT: one optional ';' between the corners, either may be
     * empty, each up to VARNISH_MENU_MAX_HINTS pairs. */
    while (*hints) {
        int button_len = 0, label_len = 0;

        if (*hints == ';') {
            if (split++) return 0;
            count = 0;
            hints++;
            continue;
        }
        while (isupper((unsigned char)*hints) || isdigit((unsigned char)*hints)) {
            hints++;
            button_len++;
        }
        if (button_len < 1 || button_len > 6 || *hints != '=') return 0;
        hints++;
        while (isalnum((unsigned char)*hints) || *hints == '_' ||
               *hints == '-' || *hints == '.') {
            hints++;
            label_len++;
        }
        if (label_len < 1 || label_len > 16) return 0;
        if (++count > VARNISH_MENU_MAX_HINTS) return 0;
        total++;
        if (*hints == ',' && hints[1] && hints[1] != ';') hints++;
        else if (*hints && *hints != ';') return 0;
    }
    return total > 0;
}

/*
 * Show a menu: a panel in the middle of the screen with the title in a header
 * band and the items left-aligned below it, the selected one in the theme's
 * selection colours. The bar along the bottom where NextUI shows its button
 * hints is covered, and this menu's hints are drawn in its corners.
 *
 * selected:   Index of the highlighted item, or -1.
 * hints:      "BUTTON=LABEL" pairs joined by commas, up to 2 per corner, e.g.
 *             "B=BACK,A=SELECT". They go bottom right; "LEFT;RIGHT" puts the
 *             part before ';' bottom left. "-" for none. BUTTON is what goes
 *             in the button (A, START); '_' in a label is drawn as a space.
 * title:      Title text, or NULL / "" for none.
 * items:      1 to VARNISH_MENU_MAX_ITEMS item texts ('|' becomes '/').
 *
 * Send it again to move the selection or change the items; hide it with
 * varnish_hide(client_id). Varnish only draws: read the buttons yourself and
 * hold varnish_grab_input() so the screen underneath does not move.
 *
 * Returns 0 on success, -1 if daemon is not running or arguments are invalid.
 */
static inline int varnish_menu(const char *client_id, int selected, const char *hints,
                               const char *title, const char *const *items, int item_count) {
    char body[256];
    char line[384];
    size_t used = 0;
    int line_len;

    if (!varnish_valid_client_id(client_id) || !varnish_valid_menu_hints(hints) ||
        !items || item_count < 1 || item_count > VARNISH_MENU_MAX_ITEMS ||
        selected < -1 || selected >= item_count)
        return -1;

    for (int i = -1; i < item_count; i++) {
        const char *part = i < 0 ? (title ? title : "") : items[i];
        char clean[128];

        if (!part || (i >= 0 && !part[0])) return -1;
        varnish_sanitize_text(part, clean, sizeof(clean));
        for (char *c = clean; *c; c++)
            if (*c == '|') *c = '/';
        line_len = snprintf(body + used, sizeof(body) - used, "%s%s",
                            i < 0 ? "" : "|", clean);
        if (line_len < 0 || (size_t)line_len >= sizeof(body) - used)
            return -1;
        used += (size_t)line_len;
    }

    line_len = snprintf(line, sizeof(line), "MENU %s %d %s %s\n",
                        client_id, selected, hints, body);
    if (line_len < 0 || line_len >= (int)sizeof(line))
        return -1;

    return varnish_send_line(line);
}

/*
 * Hide the pill, icon or menu for the given client.
 * Returns 0 on success, -1 if daemon is not running.
 */
static inline int varnish_hide(const char *client_id) {
    char line[96];
    int line_len;

    if (!varnish_valid_client_id(client_id))
        return -1;

    line_len = snprintf(line, sizeof(line), "HIDE %s\n", client_id);
    if (line_len < 0 || line_len >= (int)sizeof(line))
        return -1;

    return varnish_send_line(line);
}

/*
 * Clear all overlay pills from all clients.
 * Returns 0 on success, -1 if daemon is not running.
 */
static inline int varnish_clear(void) {
    return varnish_send_line("CLEAR\n");
}

#ifndef VARNISH_GRAB
#define VARNISH_GRAB "/tmp/varnish.grab"
#endif
#ifndef VARNISH_GRAB_RELEASE_WAIT_MS
#define VARNISH_GRAB_RELEASE_WAIT_MS 30000
#endif

/*
 * Grab input: while held, the host app (NextUI) stops seeing button presses,
 * so an overlay menu can be driven without moving the screen underneath.
 * Read the buttons yourself from /dev/input. Presses the host saw begin still
 * reach it when released. Power and volume always pass.
 *
 * The grab names the calling process and lapses if it exits, so a crash
 * cannot leave the host deaf. Requires a Varnish preload with input grab.
 * Returns 0 on success, -1 on error.
 */
static inline int varnish_grab_input(void) {
    char tmp[sizeof(VARNISH_GRAB) + 24];
    char pid[24];
    int fd, len, ok;

    snprintf(tmp, sizeof(tmp), "%s.%d", VARNISH_GRAB, (int)getpid());
    len = snprintf(pid, sizeof(pid), "%d\n", (int)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0)
        return -1;
    ok = write(fd, pid, (size_t)len) == len;
    close(fd);
    if (!ok || rename(tmp, VARNISH_GRAB) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

static inline int varnish_grab_owned(void) {
    char buf[32];
    ssize_t n;
    int fd = open(VARNISH_GRAB, O_RDONLY | O_CLOEXEC);

    if (fd < 0)
        return 0;
    n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0)
        return 0;
    buf[n] = '\0';
    return atoi(buf) == (int)getpid();
}

/*
 * Release a grab taken by this process, and wait until the host has it back.
 *
 * The host can read input well after it happened (NextUI stalls while WiFi is
 * reconfigured), so dropping the grab outright could hand it a press that was
 * meant for your menu. Instead this asks, and the preload lets go once the
 * host has caught up and drained its input. Blocks until then, or until
 * VARNISH_GRAB_RELEASE_WAIT_MS, after which it lets go regardless.
 *
 * Returns 0 on success (or if this process held no grab), -1 on error.
 */
static inline int varnish_release_input(void) {
    char tmp[sizeof(VARNISH_GRAB) + 24];
    char line[40];
    int fd, len, ok;

    if (!varnish_grab_owned())
        return 0;

    snprintf(tmp, sizeof(tmp), "%s.%d", VARNISH_GRAB, (int)getpid());
    len = snprintf(line, sizeof(line), "%d release\n", (int)getpid());
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd >= 0) {
        ok = write(fd, line, (size_t)len) == len;
        close(fd);
        if (ok && rename(tmp, VARNISH_GRAB) == 0) {
            for (int waited = 0; waited < VARNISH_GRAB_RELEASE_WAIT_MS; waited += 20) {
                if (!varnish_grab_owned())
                    return 0;
                usleep(20 * 1000);
            }
        } else {
            unlink(tmp);
        }
    }
    if (!varnish_grab_owned())
        return 0;
    return unlink(VARNISH_GRAB) == 0 || errno == ENOENT ? 0 : -1;
}

#endif /* VARNISH_CLIENT_H */
