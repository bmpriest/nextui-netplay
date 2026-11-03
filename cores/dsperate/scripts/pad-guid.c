// Print the built-in pad's SDL joystick GUID and name, one per line.
//
// DSperate opens the pad through SDL_IsGameController / SDL_GameControllerOpen
// with no joystick fallback (src/frontend/sdl/input.cpp), and MinUI/NextUI ship
// no GameController mapping, so launch.sh has to supply one through
// SDL_GAMECONTROLLERCONFIG. A mapping line starts with the joystick's GUID,
// which SDL derives from evdev bus/vendor/product/version and hashes
// differently across SDL versions -- so it is read here at runtime rather than
// hardcoded per device.
//
// Prints nothing and exits 1 when no joystick is present, so the caller can
// tell "no pad" from "pad with an unknown GUID".
//
// --probe <seconds> instead logs every control pressed for that long: the
// joystick's buttons, hats and axes as SDL numbers them, which is what a
// mapping body names (b9, h0.1, a2), and every key on every evdev node with
// the node's name. A spare button that shows up only as an evdev key, on a
// node that is not the pad, cannot be put in a GameController mapping.
#include <SDL2/SDL.h>
#include <dirent.h>
#include <fcntl.h>
#include <linux/input.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_NODES 16

static int probe(int seconds) {
    if (SDL_NumJoysticks() > 0) {
        SDL_Joystick *js = SDL_JoystickOpen(0);
        if (js)
            printf("probe: joystick 0 is %s, %d buttons, %d hats, %d axes\n", SDL_JoystickName(js),
                   SDL_JoystickNumButtons(js), SDL_JoystickNumHats(js), SDL_JoystickNumAxes(js));
    } else {
        printf("probe: no joystick, evdev keys only\n");
    }

    int fds[MAX_NODES];
    char names[MAX_NODES][128];
    int n = 0;
    DIR *d = opendir("/dev/input");
    struct dirent *e;
    while (d && n < MAX_NODES && (e = readdir(d))) {
        if (strncmp(e->d_name, "event", 5) != 0) continue;
        char path[300];
        snprintf(path, sizeof(path), "/dev/input/%s", e->d_name);
        const int fd = open(path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        names[n][0] = '\0';
        ioctl(fd, EVIOCGNAME(sizeof(names[n])), names[n]);
        printf("probe: %s is %s\n", path, names[n]);
        fds[n++] = fd;
    }
    if (d) closedir(d);
    printf("probe: press each button for the next %d seconds\n", seconds);
    fflush(stdout);

    // Axes report constantly; only a crossing of half travel is a press.
    int axis_out[32] = {0};
    const Uint32 end = SDL_GetTicks() + (Uint32)seconds * 1000;
    while (SDL_GetTicks() < end) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_JOYBUTTONDOWN) {
                printf("probe: sdl button b%d down\n", ev.jbutton.button);
            } else if (ev.type == SDL_JOYHATMOTION && ev.jhat.value) {
                printf("probe: sdl hat h%d.%d\n", ev.jhat.hat, ev.jhat.value);
            } else if (ev.type == SDL_JOYAXISMOTION && ev.jaxis.axis < 32) {
                const int out = ev.jaxis.value > 16384 ? 1 : ev.jaxis.value < -16384 ? -1 : 0;
                if (out != axis_out[ev.jaxis.axis] && out)
                    printf("probe: sdl axis %sa%d\n", out > 0 ? "+" : "-", ev.jaxis.axis);
                axis_out[ev.jaxis.axis] = out;
            }
        }
        for (int i = 0; i < n; ++i) {
            struct input_event ie;
            while (read(fds[i], &ie, sizeof(ie)) == (ssize_t)sizeof(ie))
                if (ie.type == EV_KEY && ie.value == 1) printf("probe: evdev key %d on %s\n", ie.code, names[i]);
        }
        fflush(stdout);
        SDL_Delay(10);
    }
    for (int i = 0; i < n; ++i) close(fds[i]);
    printf("probe: done\n");
    return 0;
}

int main(int argc, char **argv) {
    // Joystick only, and never a window: this runs before the emulator and must
    // not touch the panel.
    SDL_SetHint(SDL_HINT_JOYSTICK_ALLOW_BACKGROUND_EVENTS, "1");
    if (SDL_Init(SDL_INIT_JOYSTICK) != 0) {
        fprintf(stderr, "pad-guid: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    int rc = 1;
    if (argc == 3 && strcmp(argv[1], "--probe") == 0) {
        const int seconds = atoi(argv[2]);
        rc = probe(seconds > 0 ? seconds : 15);
    } else if (SDL_NumJoysticks() > 0) {
        char guid[33];
        SDL_JoystickGetGUIDString(SDL_JoystickGetDeviceGUID(0), guid, sizeof(guid));
        const char *name = SDL_JoystickNameForIndex(0);
        printf("%s\n%s\n", guid, name ? name : "Controller");
        rc = 0;
    } else {
        fprintf(stderr, "pad-guid: no joystick found\n");
    }

    SDL_Quit();
    return rc;
}
