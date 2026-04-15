#ifdef _WIN32
#include <windows.h>
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
bool physicalW = false, physicalA = false, physicalS = false, physicalD = false;
bool activeW = false, activeA = false, activeS = false, activeD = false;
int lastHorizontal = 0, lastVertical = 0;

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

#ifdef _WIN32
const int KEY_W = 'W', KEY_A = 'A', KEY_S = 'S', KEY_D = 'D', KEY_SPACE = VK_SPACE, KEY_INSERT = VK_INSERT, KEY_F7 = VK_F7;
#endif

void SetKeyState(bool& active, bool shouldBeActive, int key) {
    if (shouldBeActive != active) {
        SendKey(key, shouldBeActive);
        active = shouldBeActive;
    }
}

void UpdateHorizontal() {
    SetKeyState(activeA, physicalA && (!physicalD || lastHorizontal == KEY_A), KEY_A);
    SetKeyState(activeD, physicalD && (!physicalA || lastHorizontal == KEY_D), KEY_D);
}

void UpdateVertical() {
    SetKeyState(activeW, physicalW && (!physicalS || lastVertical == KEY_W), KEY_W);
    SetKeyState(activeS, physicalS && (!physicalW || lastVertical == KEY_S), KEY_S);
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
    else if (key == KEY_SPACE && isKeyDown && wReleaseEnabled && activeW) {
        SendKey(KEY_W, false);
        activeW = false;
    }
}

bool HandleMovement(int key, bool isKeyDown, bool isKeyUp) {
    if (key == KEY_A || key == KEY_D) {
        bool& physical = (key == KEY_A) ? physicalA : physicalD;
        if (isKeyDown && !physical) { physical = true; lastHorizontal = key; UpdateHorizontal(); return true; }
        else if (isKeyUp) { physical = false; UpdateHorizontal(); return true; }
        else if (isKeyDown) return true;
    }
    else if (key == KEY_W || key == KEY_S) {
        bool& physical = (key == KEY_W) ? physicalW : physicalS;
        if (isKeyDown && !physical) { physical = true; lastVertical = key; UpdateVertical(); return true; }
        else if (isKeyUp) { physical = false; UpdateVertical(); return true; }
        else if (isKeyDown) return true;
    }
    return false;
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
#endif

int main() {
    std::cout << "insert: toggle w release (on by default)\nf7: exit" << std::endl;
    
#ifdef _WIN32
    keyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandle(NULL), 0);
    if (!keyboardHook) { std::cerr << "hook failed: " << GetLastError() << std::endl; return 1; }
    MSG message;
    while (running && GetMessage(&message, NULL, 0, 0)) { TranslateMessage(&message); DispatchMessage(&message); }
    UnhookWindowsHookEx(keyboardHook);
#else
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
        for (size_t i = 0; i < pollfds.size(); i++) {
            if (!(pollfds[i].revents & POLLIN)) continue;
            struct input_event ev;
            ssize_t n = read(pollfds[i].fd, &ev, sizeof(ev));
            if (n != sizeof(ev)) continue;
            if (ev.type != EV_KEY) { write(uinputFd, &ev, sizeof(ev)); continue; }
            bool isMovement = (ev.code == KEY_W || ev.code == KEY_A || ev.code == KEY_S || ev.code == KEY_D);
            if (ev.value == 2) { if (!isMovement) SendKeyValue(ev.code, 2); continue; }
            HandleKey(ev.code, ev.value == 1, ev.value == 0);
            if (!HandleMovement(ev.code, ev.value == 1, ev.value == 0)) SendKeyValue(ev.code, ev.value);
        }
    }
    for (int fd : inputFds) { ioctl(fd, EVIOCGRAB, 0); close(fd); }
    ioctl(uinputFd, UI_DEV_DESTROY);
    close(uinputFd);
#endif
    
    if (activeW) SendKey(KEY_W, false);
    if (activeA) SendKey(KEY_A, false);
    if (activeS) SendKey(KEY_S, false);
    if (activeD) SendKey(KEY_D, false);
    return 0;
}
