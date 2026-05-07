#ifdef _WIN32
#include <windows.h>
const int KEY_W = 'W', KEY_A = 'A', KEY_S = 'S', KEY_D = 'D';
const int KEY_SPACE = VK_SPACE, KEY_INSERT = VK_INSERT, KEY_F7 = VK_F7;
const int KEY_HOME_K = VK_HOME;
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
const int KEY_HOME_K = KEY_HOME;
#endif
#include <iostream>
#include <chrono>
#include <cstdlib>
#include <string>

bool running = true;
bool wReleaseEnabled = true;
bool woolEnabled = false;
bool spaceHeld = false;

long long NowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct VirtualKey {
    int key = 0;
    int delayMs = 0;
    bool active = false;
    bool hasPending = false;
    long long pendingTime = 0;
    bool pendingState = false;
};

struct Axis {
    bool posPhysical = false, negPhysical = false;
    VirtualKey pos, neg;
    int lastPressed = 0;
    int woolKey = 0;
};
Axis vertical, horizontal;

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

void ApplyVirtual(VirtualKey& vk, bool shouldBeActive) {
    if (shouldBeActive != vk.active) {
        SendKey(vk.key, shouldBeActive);
        vk.active = shouldBeActive;
    }
    vk.hasPending = false;
}

void SetVirtualState(VirtualKey& vk, bool shouldBeActive, bool instant) {
    if (vk.delayMs <= 0 || (instant && shouldBeActive)) { ApplyVirtual(vk, shouldBeActive); return; }
    if (shouldBeActive == vk.active) { vk.hasPending = false; return; }
    vk.hasPending = true;
    vk.pendingTime = NowMs() + vk.delayMs;
    vk.pendingState = shouldBeActive;
}

void ProcessPending(VirtualKey& vk, long long now) {
    if (vk.hasPending && now >= vk.pendingTime) ApplyVirtual(vk, vk.pendingState);
}

void ProcessAllPending() {
    long long now = NowMs();
    ProcessPending(vertical.pos, now);
    ProcessPending(vertical.neg, now);
    ProcessPending(horizontal.pos, now);
    ProcessPending(horizontal.neg, now);
}

long long NextPendingTime() {
    long long earliest = -1;
    VirtualKey* keys[] = {&vertical.pos, &vertical.neg, &horizontal.pos, &horizontal.neg};
    for (VirtualKey* vk : keys)
        if (vk->hasPending && (earliest < 0 || vk->pendingTime < earliest)) earliest = vk->pendingTime;
    return earliest;
}

void UpdateAxis(Axis& a) {
    bool posTarget = a.posPhysical && (!a.negPhysical || a.lastPressed == a.pos.key);
    bool negTarget = a.negPhysical && (!a.posPhysical || a.lastPressed == a.neg.key);
    if (woolEnabled && spaceHeld && &a == &horizontal && !a.posPhysical && !a.negPhysical) {
        if (a.woolKey == a.pos.key) posTarget = true;
        else if (a.woolKey == a.neg.key) negTarget = true;
    }
    bool axisIdle = !a.pos.active && !a.neg.active && !a.pos.hasPending && !a.neg.hasPending;
    SetVirtualState(a.pos, posTarget, axisIdle);
    SetVirtualState(a.neg, negTarget, axisIdle);
}

Axis* AxisForKey(int key) {
    if (key == horizontal.pos.key || key == horizontal.neg.key) return &horizontal;
    if (key == vertical.pos.key || key == vertical.neg.key) return &vertical;
    return nullptr;
}

void HandleKey(int key, bool isKeyDown, bool isKeyUp) {
    if (key == KEY_INSERT && isKeyDown) {
        wReleaseEnabled = !wReleaseEnabled;
        std::cout << "w release: " << (wReleaseEnabled ? "on" : "off") << std::endl;
    }
    else if (key == KEY_HOME_K && isKeyDown) {
        woolEnabled = !woolEnabled;
        std::cout << "wool: " << (woolEnabled ? "on" : "off") << std::endl;
        if (!woolEnabled) { horizontal.woolKey = 0; UpdateAxis(horizontal); }
    }
    else if (key == KEY_F7 && isKeyDown) {
        running = false;
#ifdef _WIN32
        PostQuitMessage(0);
#endif
    }
    else if (key == KEY_SPACE) {
        if (isKeyDown && !spaceHeld) {
            spaceHeld = true;
            if (wReleaseEnabled) {
                if (vertical.pos.active) { SendKey(vertical.pos.key, false); vertical.pos.active = false; }
                vertical.pos.hasPending = false;
            }
        } else if (isKeyUp) {
            spaceHeld = false;
            horizontal.woolKey = 0;
            UpdateAxis(horizontal);
        }
    }
}

bool HandleMovement(int key, bool isKeyDown, bool isKeyUp) {
    Axis* a = AxisForKey(key);
    if (!a) return false;
    bool isHorizontal = (a == &horizontal);
    bool& physical = (key == a->pos.key) ? a->posPhysical : a->negPhysical;
    if (isKeyDown && !physical) {
        physical = true;
        a->lastPressed = key;
        if (isHorizontal) a->woolKey = 0;
        UpdateAxis(*a);
    } else if (isKeyUp) {
        physical = false;
        if (isHorizontal && woolEnabled && spaceHeld && !a->posPhysical && !a->negPhysical)
            a->woolKey = (key == a->pos.key) ? a->neg.key : a->pos.key;
        UpdateAxis(*a);
    }
    return true;
}

void ReleaseAllActive() {
    VirtualKey* keys[] = {&vertical.pos, &vertical.neg, &horizontal.pos, &horizontal.neg};
    for (VirtualKey* vk : keys) {
        if (vk->active) { SendKey(vk->key, false); vk->active = false; }
        vk->hasPending = false;
    }
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
    while (running) {
        long long next = NextPendingTime();
        DWORD timeoutMs = 100;
        if (next >= 0) {
            long long diff = next - NowMs();
            if (diff <= 0) timeoutMs = 0;
            else if (diff < 100) timeoutMs = (DWORD)diff;
        }
        MsgWaitForMultipleObjects(0, NULL, FALSE, timeoutMs, QS_ALLINPUT);
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        ProcessAllPending();
    }
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
        long long next = NextPendingTime();
        int timeoutMs = 100;
        if (next >= 0) {
            long long diff = next - NowMs();
            if (diff <= 0) timeoutMs = 0;
            else if (diff < 100) timeoutMs = (int)diff;
        }
        int pr = poll(pollfds.data(), pollfds.size(), timeoutMs);
        if (pr < 0) { ProcessAllPending(); continue; }
        if (pr > 0) {
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
        ProcessAllPending();
    }
    for (int fd : inputFds) { ioctl(fd, EVIOCGRAB, 0); close(fd); }
    ioctl(uinputFd, UI_DEV_DESTROY);
    close(uinputFd);
    return 0;
}
#endif

bool MatchPrefix(const std::string& a, const char* p, int& outVal) {
    size_t n = strlen(p);
    if (a.size() <= n || a.compare(0, n, p) != 0) return false;
    outVal = std::atoi(a.c_str() + n);
    return true;
}

int main(int argc, char** argv) {
    vertical.pos.key = KEY_W;
    vertical.neg.key = KEY_S;
    horizontal.pos.key = KEY_D;
    horizontal.neg.key = KEY_A;

    int v;
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        if (arg == "--wool") woolEnabled = true;
        else if (MatchPrefix(arg, "--delay-w=", v)) vertical.pos.delayMs = v;
        else if (MatchPrefix(arg, "--delay-s=", v)) vertical.neg.delayMs = v;
        else if (MatchPrefix(arg, "--delay-d=", v)) horizontal.pos.delayMs = v;
        else if (MatchPrefix(arg, "--delay-a=", v)) horizontal.neg.delayMs = v;
    }

    std::cout << "insert: toggle w release (on by default)\n"
              << "home: toggle wool (currently " << (woolEnabled ? "on" : "off") << ")\n"
              << "f7: exit\n"
              << "delays (ms): w=" << vertical.pos.delayMs
              << " a=" << horizontal.neg.delayMs
              << " s=" << vertical.neg.delayMs
              << " d=" << horizontal.pos.delayMs << std::endl;

#ifdef _WIN32
    int rc = RunWindows();
#else
    int rc = RunLinux();
#endif
    ReleaseAllActive();
    return rc;
}
