#ifdef _WIN32
#include <windows.h>
const int KEY_W = 'W', KEY_A = 'A', KEY_S = 'S', KEY_D = 'D';
const int KEY_SPACE = VK_SPACE, KEY_INSERT = VK_INSERT, KEY_F7 = VK_F7;
#else
#include <linux/uinput.h>
#include <linux/input.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <cstring>
#include <poll.h>
#include <signal.h>
#include <vector>
#endif
#include <iostream>

bool running = true;
bool wReleaseEnabled = true;

struct Axis {
    int posKey, negKey;
    bool posPhysical = false, negPhysical = false;
    bool posActive = false, negActive = false;
    int lastPressed = 0;
};
Axis vertical = {KEY_W, KEY_S};
Axis horizontal = {KEY_D, KEY_A};

#ifdef _WIN32
HHOOK keyboardHook = NULL;
#else
int uinputFd = -1;
std::vector<int> inputFds;
#endif

#ifdef _WIN32
void SendKey(int key, bool down) {
    INPUT input = {};
    input.type = INPUT_KEYBOARD;
    input.ki.wScan = MapVirtualKey(key, MAPVK_VK_TO_VSC);
    input.ki.dwFlags = KEYEVENTF_SCANCODE | (down ? 0 : KEYEVENTF_KEYUP);
    SendInput(1, &input, sizeof(INPUT));
}
#else
void SendKeyValue(int key, int value) {
    struct input_event ev[2] = {};
    ev[0].type = EV_KEY;
    ev[0].code = key;
    ev[0].value = value;
    ev[1].type = EV_SYN;
    ev[1].code = SYN_REPORT;
    ev[1].value = 0;
    write(uinputFd, ev, sizeof(ev));
}
void SendKey(int key, bool down) { SendKeyValue(key, down ? 1 : 0); }
#endif

void SetKeyState(bool& active, bool shouldBeActive, int key) {
    if (shouldBeActive != active) {
        SendKey(key, shouldBeActive);
        active = shouldBeActive;
    }
}

void UpdateAxis(Axis& a) {
    SetKeyState(a.posActive, a.posPhysical && (!a.negPhysical || a.lastPressed == a.posKey), a.posKey);
    SetKeyState(a.negActive, a.negPhysical && (!a.posPhysical || a.lastPressed == a.negKey), a.negKey);
}

Axis* AxisForKey(int key) {
    if (key == horizontal.posKey || key == horizontal.negKey) return &horizontal;
    if (key == vertical.posKey || key == vertical.negKey) return &vertical;
    return nullptr;
}

void HandleKey(int key, bool isKeyDown, bool isKeyUp) {
    if (key == KEY_INSERT && isKeyDown) {
        wReleaseEnabled = !wReleaseEnabled;
        std::cout << "w release: " << (wReleaseEnabled ? "on" : "off") << std::endl;
    }
    else if (key == KEY_F7 && isKeyDown) {
        running = false;
#ifdef _WIN32
        PostQuitMessage(0);
#endif
    }
    else if (key == KEY_SPACE && isKeyDown && wReleaseEnabled && vertical.posActive) {
        SendKey(vertical.posKey, false);
        vertical.posActive = false;
    }
}

bool HandleMovement(int key, bool isKeyDown, bool isKeyUp) {
    Axis* a = AxisForKey(key);
    if (!a) return false;
    bool& physical = (key == a->posKey) ? a->posPhysical : a->negPhysical;
    if (isKeyDown && !physical) { physical = true; a->lastPressed = key; UpdateAxis(*a); }
    else if (isKeyUp) { physical = false; UpdateAxis(*a); }
    return true;
}

void ReleaseAllActive() {
    SetKeyState(vertical.posActive, false, vertical.posKey);
    SetKeyState(vertical.negActive, false, vertical.negKey);
    SetKeyState(horizontal.posActive, false, horizontal.posKey);
    SetKeyState(horizontal.negActive, false, horizontal.negKey);
}

#ifdef _WIN32
LRESULT CALLBACK LowLevelKeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode == HC_ACTION) {
        KBDLLHOOKSTRUCT* kbStruct = (KBDLLHOOKSTRUCT*)lParam;
        if (kbStruct->flags & LLKHF_INJECTED) return CallNextHookEx(keyboardHook, nCode, wParam, lParam);
        int key = kbStruct->vkCode;
        bool isKeyDown = (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN);
        bool isKeyUp = (wParam == WM_KEYUP || wParam == WM_SYSKEYUP);
        HandleKey(key, isKeyDown, isKeyUp);
        if (HandleMovement(key, isKeyDown, isKeyUp)) return 1;
    }
    return CallNextHookEx(keyboardHook, nCode, wParam, lParam);
}

int RunWindows() {
    keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    if (!keyboardHook) { std::cerr << "hook failed: " << GetLastError() << std::endl; return 1; }
    MSG message;
    while (running && GetMessage(&message, NULL, 0, 0)) { TranslateMessage(&message); DispatchMessage(&message); }
    UnhookWindowsHookEx(keyboardHook);
    return 0;
}
#else
int SetupUinput() {
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK);
    if (fd < 0) return -1;
    ioctl(fd, UI_SET_EVBIT, EV_KEY);
    ioctl(fd, UI_SET_EVBIT, EV_SYN);
    ioctl(fd, UI_SET_EVBIT, EV_MSC);
    ioctl(fd, UI_SET_MSCBIT, MSC_SCAN);
    for (int i = 0; i < KEY_MAX; i++) ioctl(fd, UI_SET_KEYBIT, i);
    struct uinput_setup setup = {};
    strcpy(setup.name, "tommy-nulls");
    setup.id.bustype = BUS_USB;
    setup.id.vendor = 0x1234;
    setup.id.product = 0x5678;
    setup.id.version = 1;
    ioctl(fd, UI_DEV_SETUP, &setup);
    ioctl(fd, UI_DEV_CREATE);
    usleep(200000);
    return fd;
}

std::vector<int> FindKeyboards() {
    std::vector<int> fds;
    DIR* dir = opendir("/dev/input");
    if (!dir) return fds;
    struct dirent* entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, "event", 5) != 0) continue;
        char path[280];
        snprintf(path, sizeof(path), "/dev/input/%s", entry->d_name);
        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;
        char name[256] = {};
        ioctl(fd, EVIOCGNAME(sizeof(name)), name);
        if (strstr(name, "tommy-nulls")) { close(fd); continue; }
        unsigned long evbit = 0;
        ioctl(fd, EVIOCGBIT(0, sizeof(evbit)), &evbit);
        if (evbit & (1 << EV_KEY)) {
            unsigned long keybit[KEY_MAX / (sizeof(long) * 8) + 1] = {};
            ioctl(fd, EVIOCGBIT(EV_KEY, sizeof(keybit)), keybit);
            if (keybit[KEY_W / (sizeof(long) * 8)] & (1UL << (KEY_W % (sizeof(long) * 8)))) {
                ioctl(fd, EVIOCGRAB, 1);
                fds.push_back(fd);
                continue;
            }
        }
        close(fd);
    }
    closedir(dir);
    return fds;
}

void SignalHandler(int) { running = false; }

int RunLinux() {
    if (getuid() != 0) { std::cerr << "run as root" << std::endl; return 1; }
    signal(SIGINT, SignalHandler);
    signal(SIGTERM, SignalHandler);
    uinputFd = SetupUinput();
    if (uinputFd < 0) { std::cerr << "uinput failed" << std::endl; return 1; }
    inputFds = FindKeyboards();
    if (inputFds.empty()) { std::cerr << "no keyboards found" << std::endl; close(uinputFd); return 1; }
    std::vector<pollfd> pollfds;
    for (int fd : inputFds) pollfds.push_back({fd, POLLIN, 0});
    while (running) {
        if (poll(pollfds.data(), pollfds.size(), 100) <= 0) continue;
        for (auto& p : pollfds) {
            if (!(p.revents & POLLIN)) continue;
            struct input_event ev;
            if (read(p.fd, &ev, sizeof(ev)) != sizeof(ev)) continue;
            if (ev.type != EV_KEY) { write(uinputFd, &ev, sizeof(ev)); continue; }
            bool isMovement = AxisForKey(ev.code) != nullptr;
            if (ev.value == 2) { if (!isMovement) SendKeyValue(ev.code, 2); continue; }
            HandleKey(ev.code, ev.value == 1, ev.value == 0);
            if (!HandleMovement(ev.code, ev.value == 1, ev.value == 0)) SendKeyValue(ev.code, ev.value);
        }
    }
    for (int fd : inputFds) { ioctl(fd, EVIOCGRAB, 0); close(fd); }
    ioctl(uinputFd, UI_DEV_DESTROY);
    close(uinputFd);
    return 0;
}
#endif

int main() {
    std::cout << "insert: toggle w release (on by default)\nf7: exit" << std::endl;
#ifdef _WIN32
    int rc = RunWindows();
#else
    int rc = RunLinux();
#endif
    ReleaseAllActive();
    return rc;
}
