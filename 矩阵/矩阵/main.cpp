#pragma comment(lib, "psapi.lib")

// Ensure Windows API features (like CONSOLE_FONT_INFOEX / SetCurrentConsoleFontEx)
// are exposed by targeting at least Windows Vista / Windows Server 2008.
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0600
#endif

#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <iostream>
#include <vector>
#include <string>
#include <cmath>
#include <iomanip>
#include <algorithm>
#include <thread>
#include <atomic>
#include <mutex>
#include <fstream>
#include <unordered_set>

// ============================================================
// 手动定义 ListView 常量（不依赖 commctrl.h）
// ============================================================

#ifndef LVM_FIRST
#define LVM_FIRST        0x1000
#endif
#define LVM_GETITEMCOUNT (LVM_FIRST + 4)
#define LVM_GETITEMTEXTW (LVM_FIRST + 115)
#define LVIF_TEXT        0x0001

struct MY_LVITEM {
    UINT   mask;
    int    iItem;
    int    iSubItem;
    UINT   state;
    UINT   stateMask;
    LPWSTR pszText;
    int    cchTextMax;
    int    iImage;
    LPARAM lParam;
    int    iIndent;
    int    iGroupId;
    UINT   cColumns;
    PUINT  puColumns;
    int* piColFmt;
    int    iGroup;
};

// ============================================================
// 数据结构
// ============================================================

struct Matrix4x4 { float m[4][4]; };

struct Vector3 { float x, y, z; };

struct MatrixCandidate {
    uintptr_t address;
    Matrix4x4 matrix;
};

struct ScanAddress {
    uintptr_t address;
};

// ============================================================
// EnumWindows 回调
// ============================================================

struct EnumByTitleData { const wchar_t* kw; HWND result; };
struct EnumByPIDData { DWORD pid; HWND result; };
struct FindLVData { HWND result; };

BOOL CALLBACK EnumByTitleProc(HWND hwnd, LPARAM lp) {
    EnumByTitleData* d = (EnumByTitleData*)lp;
    wchar_t buf[512]; buf[0] = 0;
    GetWindowTextW(hwnd, buf, 512);
    if (wcsstr(buf, d->kw)) { d->result = hwnd; return FALSE; }
    return TRUE;
}


// forward declarations
bool TryAdd(HANDLE hProc, uintptr_t addr, std::vector<MatrixCandidate>& res);
bool CheckMatrix4x4_Loose(const float* f);

bool TryAddUnique(HANDLE hProc,
    uintptr_t addr,
    std::vector<MatrixCandidate>& res,
    std::unordered_set<uintptr_t>& seen) {
    if (!seen.insert(addr).second) return false;
    return TryAdd(hProc, addr, res);
}

bool TryAddFromBuffer(uintptr_t addr,
    const unsigned char* data,
    std::vector<MatrixCandidate>& res,
    std::unordered_set<uintptr_t>& seen) {
    if (!seen.insert(addr).second) return false;

    float buf[16];
    memcpy(buf, data, sizeof(buf));
    if (!CheckMatrix4x4_Loose(buf)) return false;

    MatrixCandidate c; c.address = addr;
    memcpy(&c.matrix, buf, sizeof(buf));
    res.push_back(c);
    return true;
}

// Enhanced ScanList with progress reporting (no per-item console spam)
std::vector<MatrixCandidate> ScanList(HANDLE hProc,
    const std::vector<ScanAddress>& list,
    int range,
    std::atomic<int>* progress) {
    std::vector<MatrixCandidate> res;
    int total = (int)list.size();
    std::unordered_set<uintptr_t> seen;
    seen.reserve((size_t)total * ((size_t)range / 2 + 1));
    for (int i = 0; i < total; i++) {
        uintptr_t base = list[i].address;
        TryAddUnique(hProc, base, res, seen);
        for (int off = 4; off <= range; off += 4) {
            TryAddUnique(hProc, base + (uintptr_t)off, res, seen);
            if (base >= (uintptr_t)off)
                TryAddUnique(hProc, base - (uintptr_t)off, res, seen);
        }
        if (progress) progress->store(i + 1);
    }
    return res;
}

// Helper to access matrix with [col][row] as in the provided WorldToScreen
inline float MAT(const Matrix4x4& m, int col, int row) { return m.m[row][col]; }

// forward declaration
static bool IsFiniteF(float v);

bool WorldToScreen(const Vector3& pos, const Matrix4x4& matrix, float screenWidth, float screenHeight, float& outX, float& outY) {
    float w = MAT(matrix, 0, 3) * pos.x + MAT(matrix, 1, 3) * pos.y + MAT(matrix, 2, 3) * pos.z + MAT(matrix, 3, 3);
    if (w < 0.01f) return false;

    float x = MAT(matrix, 0, 0) * pos.x + MAT(matrix, 1, 0) * pos.y + MAT(matrix, 2, 0) * pos.z + MAT(matrix, 3, 0);
    float y = MAT(matrix, 0, 1) * pos.x + MAT(matrix, 1, 1) * pos.y + MAT(matrix, 2, 1) * pos.z + MAT(matrix, 3, 1);

    outX = (x / w + 1.0f) * 0.5f * screenWidth;
    outY = (1.0f - y / w) * 0.5f * screenHeight;
    return true;
}

// Filter matrices using WorldToScreen: require at least one sample maps inside screen
std::vector<MatrixCandidate> FilterByWorldToScreen(const std::vector<MatrixCandidate>& in, float screenW, float screenH) {
    std::vector<MatrixCandidate> out;
    Vector3 samples[3] = { {0,0,0}, {0,0,100}, {100,0,0} };
    for (const auto& c : in) {
        bool ok = false;
        for (auto& s : samples) {
            float sx, sy;
            if (WorldToScreen(s, c.matrix, screenW, screenH, sx, sy)) {
                if (sx >= 0 && sx <= screenW && sy >= 0 && sy <= screenH) { ok = true; break; }
            }
        }
        if (ok) out.push_back(c);
    }
    return out;
}

BOOL CALLBACK EnumByPIDProc(HWND hwnd, LPARAM lp) {
    EnumByPIDData* d = (EnumByPIDData*)lp;
    DWORD wpid = 0;
    GetWindowThreadProcessId(hwnd, &wpid);
    if (wpid == d->pid && IsWindowVisible(hwnd)) { d->result = hwnd; return FALSE; }
    return TRUE;
}

BOOL CALLBACK FindLVChildProc(HWND hwnd, LPARAM lp) {
    FindLVData* d = (FindLVData*)lp;
    wchar_t cls[64]; cls[0] = 0;
    GetClassNameW(hwnd, cls, 64);
    if (wcsstr(cls, L"SysListView32") ||
        wcsstr(cls, L"TListView") ||
        _wcsicmp(cls, L"ListView") == 0) {
        d->result = hwnd;
        return FALSE;
    }
    return TRUE;
}

// ============================================================
// 进程 / 窗口工具
// ============================================================

DWORD GetPIDByTitle(const wchar_t* kw) {
    EnumByTitleData d; d.kw = kw; d.result = NULL;
    EnumWindows(EnumByTitleProc, (LPARAM)&d);
    if (!d.result) return 0;
    DWORD pid = 0;
    GetWindowThreadProcessId(d.result, &pid);
    return pid;
}

HWND GetGameWindow(DWORD pid) {
    EnumByPIDData d; d.pid = pid; d.result = NULL;
    EnumWindows(EnumByPIDProc, (LPARAM)&d);
    return d.result;
}

void PrintWindowInfo(DWORD pid) {
    HWND hwnd = GetGameWindow(pid);
    if (!hwnd) { std::cout << "[窗口] 未找到\n"; return; }
    wchar_t title[512]; title[0] = 0;
    GetWindowTextW(hwnd, title, 512);
    RECT rect; GetWindowRect(hwnd, &rect);
    wprintf(L"[窗口] 标题: %s\n", title);
    std::cout << "[窗口] 句柄: 0x" << std::hex << (uintptr_t)hwnd << std::dec << "\n";
    std::cout << "[窗口] 尺寸: "
        << (rect.right - rect.left) << "x" << (rect.bottom - rect.top) << "\n";
}

// ============================================================
// 拖拽捕获
// ============================================================

struct CaptureResult { HWND hwnd; DWORD pid; };

CaptureResult WaitForUserPoint() {
    std::cout << "\n[拖拽模式]\n";
    std::cout << "  将鼠标移到 CE 搜索结果列表上\n";
    std::cout << "  按 ENTER 确认，按 ESC 取消\n\n";

    CaptureResult res; res.hwnd = NULL; res.pid = 0;
    HWND lastHw = NULL;

    // 如果用户刚按下了回车或 ESC（例如用于之前的输入），等待按键释放，避免立即触发捕获
    while (GetAsyncKeyState(VK_RETURN) & 0x8000) Sleep(50);
    while (GetAsyncKeyState(VK_ESCAPE) & 0x8000) Sleep(50);

    while (true) {
        POINT pt; GetCursorPos(&pt);
        HWND hw = WindowFromPoint(pt);

        if (hw && hw != lastHw) {
            wchar_t ttl[256]; ttl[0] = 0;
            wchar_t cls[256]; cls[0] = 0;
            GetWindowTextW(hw, ttl, 256);
            GetClassNameW(hw, cls, 256);
            DWORD wpid = 0;
            GetWindowThreadProcessId(hw, &wpid);
            wprintf(L"\r  标题=[%s] 类=[%s] pid=%u    ",
                ttl[0] ? ttl : L"(无)", cls, wpid);
            fflush(stdout);
            lastHw = hw;
        }

        if (GetAsyncKeyState(VK_RETURN) & 0x8000) {
            // 等待按键释放以避免被后续逻辑误判（去抖动）
            Sleep(150);
            while (GetAsyncKeyState(VK_RETURN) & 0x8000) Sleep(10);
            POINT pt2; GetCursorPos(&pt2);
            res.hwnd = WindowFromPoint(pt2);
            if (res.hwnd) GetWindowThreadProcessId(res.hwnd, &res.pid);
            std::cout << "\n\n[捕获] 已选定\n";
            break;
        }
        if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
            // 等待释放
            while (GetAsyncKeyState(VK_ESCAPE) & 0x8000) Sleep(10);
            std::cout << "\n[取消]\n";
            break;
        }
        Sleep(50);
    }
    return res;
}

// ============================================================
// 跨进程读取 ListView
// 
// ============================================================

std::vector<ScanAddress> ReadListView(HWND hLV, DWORD cePID) {
    std::vector<ScanAddress> list;

    int count = (int)SendMessage(hLV, LVM_GETITEMCOUNT, 0, 0);
    if (count <= 0) {
        std::cout << "[ListView] 行数=0 或读取失败\n";
        return list;
    }
    std::cout << "[ListView] 共 " << count << " 行\n";

    HANDLE hCE = OpenProcess(
        PROCESS_VM_READ | PROCESS_VM_WRITE |
        PROCESS_VM_OPERATION | PROCESS_QUERY_INFORMATION,
        FALSE, cePID);
    if (!hCE) {
        std::cout << "[错误] 无法打开CE进程 err=" << GetLastError() << "\n";
        return list;
    }

    const SIZE_T ITEM_SIZE = sizeof(MY_LVITEM);
    const SIZE_T TEXT_SIZE = 128 * sizeof(wchar_t);
    const SIZE_T TOTAL = ITEM_SIZE + TEXT_SIZE;

    LPVOID pRemote = VirtualAllocEx(hCE, NULL, TOTAL,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!pRemote) {
        std::cout << "[错误] VirtualAllocEx 失败 err=" << GetLastError() << "\n";
        CloseHandle(hCE);
        return list;
    }

    LPVOID pRemoteText = (BYTE*)pRemote + ITEM_SIZE;
    int maxRows = (count > 2000000) ? 2000000 : count;

    for (int i = 0; i < maxRows; i++) {
        if (i % 5000 == 0)
            std::cout << "\r[读取] " << i << "/" << maxRows << "    " << std::flush;

        MY_LVITEM lvi;
        ZeroMemory(&lvi, sizeof(lvi));
        lvi.mask = LVIF_TEXT;
        lvi.iItem = i;
        lvi.iSubItem = 0;
        lvi.pszText = (LPWSTR)pRemoteText;
        lvi.cchTextMax = 64;

        SIZE_T written = 0;
        if (!WriteProcessMemory(hCE, pRemote, &lvi, ITEM_SIZE, &written))
            continue;

        SendMessage(hLV, LVM_GETITEMTEXTW, (WPARAM)i, (LPARAM)pRemote);

        wchar_t textBuf[64]; textBuf[0] = 0;
        SIZE_T bytesRead = 0;
        ReadProcessMemory(hCE, pRemoteText, textBuf,
            sizeof(textBuf) - sizeof(wchar_t), &bytesRead);
        textBuf[63] = 0;

        wchar_t* p = textBuf;
        if (wcslen(p) >= 2 && p[0] == L'0' && (p[1] == L'x' || p[1] == L'X'))
            p += 2;
        if (p[0] == 0) continue;

        wchar_t* endp = NULL;
        unsigned long long addr = wcstoull(p, &endp, 16);
        if (addr != 0 && endp != p) {
            ScanAddress sa; sa.address = (uintptr_t)addr;
            list.push_back(sa);
        }
    }

    std::cout << "\r[读取] 完成，有效地址 " << list.size() << " 个\n";
    VirtualFreeEx(hCE, pRemote, 0, MEM_RELEASE);
    CloseHandle(hCE);
    return list;
}

HWND FindListView(HWND hwnd) {
    if (!hwnd) return NULL;
    wchar_t cls[64]; cls[0] = 0;
    GetClassNameW(hwnd, cls, 64);
    if (wcsstr(cls, L"SysListView32") || wcsstr(cls, L"TListView"))
        return hwnd;
    FindLVData d; d.result = NULL;
    EnumChildWindows(hwnd, FindLVChildProc, (LPARAM)&d);
    return d.result;
}

std::vector<ScanAddress> GetCEAddressesByDrag() {
    std::vector<ScanAddress> empty;

    CaptureResult cap = WaitForUserPoint();
    if (!cap.hwnd || !cap.pid) {
        std::cout << "[错误] 未捕获到有效窗口\n";
        return empty;
    }

    wchar_t title[256]; title[0] = 0;
    wchar_t cls[256];   cls[0] = 0;
    GetWindowTextW(cap.hwnd, title, 256);
    GetClassNameW(cap.hwnd, cls, 256);
    wprintf(L"[捕获] 标题: %s\n", title);
    wprintf(L"[捕获] 类名: %s\n", cls);
    std::cout << "[捕获] PID : " << cap.pid << "\n";

    HWND hLV = FindListView(cap.hwnd);
    if (!hLV) {
        HWND root = GetAncestor(cap.hwnd, GA_ROOT);
        hLV = FindListView(root);
    }
    if (!hLV) {
        std::cout << "[错误] 未找到 ListView\n";
        std::cout << "  请把鼠标移到CE扫描结果的地址列表区域\n";
        return empty;
    }

    std::cout << "[ListView] 0x" << std::hex << (uintptr_t)hLV << std::dec << "\n";
    return ReadListView(hLV, cap.pid);
}

// ============================================================
// 矩阵特征校验
// ============================================================

static bool IsFiniteF(float v) { return std::isfinite(v) && !std::isnan(v); }

static bool NearF(float a, float b, float eps) {
    return fabsf(a - b) <= eps;
}

// 宽松矩阵检查：优先保证“不漏掉真实矩阵”。
// 根据你给的高头 / 低头样本，真实矩阵明显具有：
//   f[4] 约等于 0
//   f[2]  约等于 f[3]
//   f[6]  约等于 f[7]
//   f[10] 约等于 f[11]
//   f[14] 约等于 f[15]
// 另外真实样本尾部已经超过 ±500，所以这里放宽到 ±10000。
bool CheckMatrix4x4_Loose(const float* f) {
    for (int i = 0; i < 16; ++i) {
        if (!IsFiniteF(f[i])) return false;
    }

    // 第 5 个 float：你给的两组样本都是 0.00。
    // 不建议用 f[4] == 0.0f，因为浮点读取时可能出现极小误差。
    if (fabsf(f[4]) > 0.0001f) return false;

    // 前 12 个通常是方向 / 投影相关值，样本范围大约在 -0.8 到 1.54。
    // 放宽到 [-3, 3]，避免因为游戏 FOV / 分辨率 / 姿态轻微变化而漏掉。
    for (int i = 0; i < 12; ++i) {
        if (f[i] < -3.0f || f[i] > 3.0f) return false;
    }

    // 尾部是位置 / 平移 / 深度相关值。你的真实样本已有 -1128、-1050、-739，不能限制 ±500。
    for (int i = 12; i < 16; ++i) {
        if (f[i] < -10000.0f || f[i] > 10000.0f) return false;
    }

    // 样本中的成对特征。
    if (fabsf(f[2] - f[3]) > 0.08f) return false;
    if (fabsf(f[6] - f[7]) > 0.08f) return false;
    if (fabsf(f[10] - f[11]) > 0.08f) return false;
    if (fabsf(f[14] - f[15]) > 8.0f) return false;

    return true;
}

// 强矩阵检查：用于最终缩小结果。
// 这些范围来自你给的两组真实矩阵：
// 高头：-0.72 -0.19 0.56 0.56 0.00 1.54 0.21 0.21 -0.51 0.27 -0.80 -0.80 122.54 352.69 -1128.37 -1127.15
// 低头：-0.72  0.35 0.53 0.53 0.00 1.44 -0.39 -0.39 -0.50 -0.50 -0.76 -0.76 126.14 -739.78 -1050.33 -1049.12
bool CheckMatrix4x4_Strong(const float* f) {
    if (!CheckMatrix4x4_Loose(f)) return false;

    if (f[0] < -1.05f || f[0] > -0.35f) return false;  // 样本约 -0.72
    if (f[2] < 0.20f || f[2] >  0.90f) return false;  // 样本约 0.53 ~ 0.56
    if (f[3] < 0.20f || f[3] >  0.90f) return false;  // 样本约 0.53 ~ 0.56
    if (f[5] < 1.00f || f[5] >  2.00f) return false;  // 样本约 1.44 ~ 1.54
    if (f[8] < -0.90f || f[8] > -0.20f) return false;  // 样本约 -0.50
    if (f[10] < -1.20f || f[10] > -0.40f) return false; // 样本约 -0.76 ~ -0.80
    if (f[11] < -1.20f || f[11] > -0.40f) return false; // 样本约 -0.76 ~ -0.80

    return true;
}

// 保留原函数名，避免其他地方调用失效。
// 这里默认用宽松检查，防止初筛把真实矩阵直接过滤掉。
bool CheckMatrix4x4(const float* f) {
    return CheckMatrix4x4_Loose(f);
}

// Filter matrices by matrix features.
// 最终特征过滤用强检查；如果强检查为空，主流程会退回 results 供人工筛选。
std::vector<MatrixCandidate> FilterByFeatures(const std::vector<MatrixCandidate>& in) {
    std::vector<MatrixCandidate> out;
    for (const auto& c : in) {
        const float* f = &c.matrix.m[0][0];
        if (CheckMatrix4x4_Strong(f)) out.push_back(c);
    }
    return out;
}

// Filter candidates by head pose (high or low).
// 这里匹配你提供的高头 / 低头姿态特征。
std::vector<MatrixCandidate> FilterByHeadPose(const std::vector<MatrixCandidate>& in, bool high) {
    std::vector<MatrixCandidate> out;

    const float t5 = high ? 1.54f : 1.44f;
    const float t6 = high ? 0.21f : -0.39f;
    const float t7 = high ? 0.21f : -0.39f;

    const float tol5 = 0.35f;
    const float tol67 = 0.35f;

    for (const auto& c : in) {
        const float* f = &c.matrix.m[0][0];

        if (!CheckMatrix4x4_Loose(f)) continue;
        if (fabsf(f[4]) > 0.0001f) continue;
        if (fabsf(f[5] - t5) > tol5) continue;
        if (fabsf(f[6] - t6) > tol67) continue;
        if (fabsf(f[7] - t7) > tol67) continue;

        out.push_back(c);
    }
    return out;
}

bool ReadFloat16(HANDLE hProc, uintptr_t addr, float* out) {
    SIZE_T n = 0;
    return ReadProcessMemory(hProc, (LPCVOID)addr, out, 64, &n) && n == 64;
}

bool TryAdd(HANDLE hProc, uintptr_t addr, std::vector<MatrixCandidate>& res) {
    for (size_t i = 0; i < res.size(); i++)
        if (res[i].address == addr) return false;

    float buf[16];
    if (!ReadFloat16(hProc, addr, buf)) return false;

    // 初筛使用宽松检查，避免真实矩阵被 ±500 尾部限制等条件杀掉。
    if (!CheckMatrix4x4_Loose(buf)) return false;

    MatrixCandidate c; c.address = addr;
    memcpy(&c.matrix, buf, 64);
    res.push_back(c);
    return true;
}

// 从 CE 地址列表读取候选，并额外扫描地址前后 range 字节。
// 这个用于高头 / 低头阶段，解决“CE 搜到的地址不是矩阵起始地址”的问题。
std::vector<MatrixCandidate> ReadCandidatesWithRange(HANDLE hProc,
    const std::vector<ScanAddress>& list,
    int range,
    std::atomic<int>* progress = nullptr) {
    std::vector<MatrixCandidate> out;
    const int total = (int)list.size();
    if (total == 0) return out;

    unsigned int hardwareThreads = std::thread::hardware_concurrency();
    if (hardwareThreads == 0) hardwareThreads = 1;
    int workerCount = total;
    if (workerCount > 6) workerCount = 6;
    if (workerCount > (int)hardwareThreads) workerCount = (int)hardwareThreads;
    std::atomic<int> nextIndex(0);
    std::atomic<int> completed(0);
    std::vector<std::vector<MatrixCandidate>> partial((size_t)workerCount);
    std::vector<std::thread> workers;
    workers.reserve((size_t)workerCount);

    for (int worker = 0; worker < workerCount; ++worker) {
        workers.emplace_back([&, worker]() {
            std::unordered_set<uintptr_t> seen;
            seen.reserve((size_t)total / (size_t)workerCount + 1);
            auto& local = partial[(size_t)worker];

            for (;;) {
                const int i = nextIndex.fetch_add(1);
                if (i >= total) break;

                const uintptr_t base = list[(size_t)i].address;
                bool usedBlock = false;

                // 一次读取整段，再在本地检查各偏移，显著减少跨进程读取次数。
                if (range > 0 && base >= (uintptr_t)range &&
                    base <= UINTPTR_MAX - (uintptr_t)range - 64) {
                    const uintptr_t start = base - (uintptr_t)range;
                    const size_t blockSize = (size_t)range * 2 + 64;
                    std::vector<unsigned char> block(blockSize);
                    SIZE_T bytesRead = 0;
                    if (ReadProcessMemory(hProc, (LPCVOID)start, block.data(),
                        block.size(), &bytesRead) && bytesRead == block.size()) {
                        for (size_t offset = 0; offset <= (size_t)range * 2; offset += 4)
                            TryAddFromBuffer(start + offset, block.data() + offset, local, seen);
                        usedBlock = true;
                    }
                }

                // 跨越不可读内存页时，退回逐地址读取，行为与旧版本一致。
                if (!usedBlock) {
                    TryAddUnique(hProc, base, local, seen);
                    for (int off = 4; off <= range; off += 4) {
                        TryAddUnique(hProc, base + (uintptr_t)off, local, seen);
                        if (base >= (uintptr_t)off)
                            TryAddUnique(hProc, base - (uintptr_t)off, local, seen);
                    }
                }

                const int done = completed.fetch_add(1) + 1;
                if (progress) progress->store(done);
            }
        });
    }

    for (auto& worker : workers) worker.join();

    std::unordered_set<uintptr_t> merged;
    for (const auto& local : partial) {
        for (const auto& candidate : local) {
            if (merged.insert(candidate.address).second) out.push_back(candidate);
        }
    }
    std::sort(out.begin(), out.end(), [](const MatrixCandidate& a, const MatrixCandidate& b) {
        return a.address < b.address;
    });

    return out;
}

std::vector<MatrixCandidate> ScanList(HANDLE hProc,
    const std::vector<ScanAddress>& list,
    int range) {
    std::vector<MatrixCandidate> res;
    int total = (int)list.size();
    std::unordered_set<uintptr_t> seen;
    seen.reserve((size_t)total * ((size_t)range / 2 + 1));
    for (int i = 0; i < total; i++) {
        uintptr_t base = list[i].address;
        std::cout << "\r[矩阵] " << (i + 1) << "/" << total
            << "  0x" << std::hex << base << std::dec << "      " << std::flush;
        TryAddUnique(hProc, base, res, seen);
        for (int off = 4; off <= range; off += 4) {
            TryAddUnique(hProc, base + (uintptr_t)off, res, seen);
            if (base >= (uintptr_t)off)
                TryAddUnique(hProc, base - (uintptr_t)off, res, seen);
        }
    }
    std::cout << "\n";
    return res;
}

// ============================================================
// 打印 4x4 矩阵
// ============================================================

void PrintMatrix(const MatrixCandidate& c) {
    const float* f = &c.matrix.m[0][0];
    std::cout << "\n+----------+----------+----------+----------+\n";
    std::cout << "  0x" << std::hex << c.address << std::dec << "\n";
    std::cout << "+----------+----------+----------+----------+\n";
    const char* lbl[4] = { "[头部 R0]","[     R1]","[     R2]","[尾部 R3]" };
    for (int r = 0; r < 4; r++) {
        std::cout << "|";
        for (int col = 0; col < 4; col++)
            std::cout << std::fixed << std::setprecision(4)
            << std::setw(10) << std::right << f[r * 4 + col] << " ";
        std::cout << "| " << lbl[r] << "\n";
    }
    std::cout << "+----------+----------+----------+----------+\n";
}

// ============================================================
// 主程序
// ============================================================

int main() {
    // 编码设置
    SetConsoleOutputCP(65001);
    SetConsoleCP(65001);

    // 尝试设置控制台字体为等宽 Consolas，避免非 ASCII 字符在某些终端出现显示问题
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    CONSOLE_FONT_INFOEX cfi;
    ZeroMemory(&cfi, sizeof(cfi));
    cfi.cbSize = sizeof(cfi);
    cfi.dwFontSize.X = 0;
    cfi.dwFontSize.Y = 18; // 根据需要可调整字体高度
    cfi.FontFamily = FF_DONTCARE;
    cfi.FontWeight = FW_NORMAL;
    wcscpy_s(cfi.FaceName, LF_FACESIZE, L"Consolas");
    SetCurrentConsoleFontEx(hOut, FALSE, &cfi);

    // 设置窗口标题
    SetConsoleTitleW(L"4x4 Matrix Filter");

    std::cout << "========================================\n";
    std::cout << "   4x4 矩阵过滤器  [作者:小士]\n";
    std::cout << "========================================\n\n";

    // 步骤1：通过拖拽捕获游戏窗口（将鼠标移到游戏窗口上，按 ENTER）
    std::cout << "[1] 将鼠标移到游戏窗口上，按 ENTER 捕获目标窗口\n";
    CaptureResult cap = WaitForUserPoint();
    if (!cap.hwnd || !cap.pid) {
        std::cerr << "[错误] 未捕获到有效游戏窗口\n";
        system("pause"); return 1;
    }
    DWORD gamePID = cap.pid;
    wchar_t gtitle[512]; gtitle[0] = 0;
    GetWindowTextW(cap.hwnd, gtitle, _countof(gtitle));
    wprintf(L"[捕获] 游戏标题: %s\n", gtitle);
    std::cout << "[游戏] PID=" << gamePID << "\n";
    PrintWindowInfo(gamePID);

    // 步骤2：打开游戏进程
    HANDLE hGame = OpenProcess(
        PROCESS_VM_READ | PROCESS_QUERY_INFORMATION, FALSE, gamePID);
    if (!hGame) {
        std::cerr << "[错误] OpenProcess失败 err=" << GetLastError()
            << "\n以管理员身份运行\n";
        system("pause"); return 1;
    }

    // 步骤3：拖拽读取CE地址
    std::cout << "\n[2] 鼠标移到CE搜索结果列表上，按 ENTER 读取\n";
    std::vector<ScanAddress> ceList = GetCEAddressesByDrag();

    if (ceList.empty()) {
        std::cerr << "[警告] 未获取到地址\n";
        CloseHandle(hGame); system("pause"); return 1;
    }
    std::cout << "[CE] 获取 " << ceList.size() << " 个地址\n\n";

    // 调试导出：把部分原始内存块导出为 CSV，便于检查矩阵布局和数值
    auto DumpRawMatrices = [&](HANDLE proc, const std::vector<ScanAddress>& list, int maxDump) {
        std::ofstream df("matrices_raw.csv");
        df << "address";
        for (int i = 0; i < 16; ++i) df << ",v" << i;
        df << "\n";
        int n = (int)list.size();
        if (maxDump > n) maxDump = n;
        for (int i = 0; i < maxDump; ++i) {
            float buf[16]; SIZE_T read = 0;
            BOOL ok = ReadProcessMemory(proc, (LPCVOID)list[i].address, buf, sizeof(buf), &read);
            if (!ok || read != sizeof(buf)) {
                // write empty row with zeros if read failed
                df << std::hex << "0x" << list[i].address << std::dec;
                for (int k = 0; k < 16; ++k) df << "," << 0.0f;
                df << "\n";
                continue;
            }
            df << std::hex << "0x" << list[i].address << std::dec;
            for (int k = 0; k < 16; ++k) df << "," << buf[k];
            df << "\n";
        }
        df.close();
        std::cout << "[调试] 已导出前 " << maxDump << " 个地址的原始 16-float 数据到 matrices_raw.csv\n";
        };

    // 导出前 200 个地址用于检查（可根据需要修改）
    DumpRawMatrices(hGame, ceList, 200);

    // 读取所有候选的原始 16-float 数据到 rawCandidates（不做任何过滤）
    std::vector<MatrixCandidate> rawCandidates;
    rawCandidates.reserve(ceList.size());
    std::cout << "开始读取所有候选的原始矩阵（不做过滤）...\n";
    for (size_t i = 0; i < ceList.size(); ++i) {
        float buf[16]; SIZE_T read = 0;
        BOOL ok = ReadProcessMemory(hGame, (LPCVOID)ceList[i].address, buf, sizeof(buf), &read);
        if (ok && read == sizeof(buf)) {
            MatrixCandidate c; c.address = ceList[i].address; memcpy(&c.matrix, buf, sizeof(buf));
            rawCandidates.push_back(c);
        }
    }
    std::cout << "读取完成，共获得 " << rawCandidates.size() << " 个原始候选\n";

    // 指导用户先抬头（高头）一次过滤
    std::cout << "\n请让目标角色抬头到最高，准备好后按回车开始高头过滤..."; std::string _tmp; std::getline(std::cin, _tmp);
    // 给用户额外的反应时间（5秒倒计时）以便调整姿态
    for (int t = 5; t > 0; --t) {
        std::cout << "开始读取前倒计时: " << t << "s\r" << std::flush;
        Sleep(1000);
    }
    std::cout << "\n正在读取内存并执行高头过滤...\n";
    std::atomic<int> highProgress(0);
    std::vector<MatrixCandidate> curCandidates = ReadCandidatesWithRange(hGame, ceList, 256, &highProgress);
    std::cout << "高头阶段读取候选: " << curCandidates.size() << " 个（已扫描每个地址 ±256 字节）\n";
    std::vector<MatrixCandidate> highFiltered = FilterByHeadPose(curCandidates, true);
    std::cout << "高头过滤结果: " << highFiltered.size() << " 个候选，已保存 matrices_high.csv\n";
    {
        std::ofstream hf("matrices_high.csv"); hf << "address"; for (int r = 0;r < 4;++r) for (int c = 0;c < 4;++c) hf << ",m" << r << c; hf << "\n";
        for (auto& m : highFiltered) { hf << std::hex << "0x" << m.address << std::dec; for (int r = 0;r < 4;++r) for (int c = 0;c < 4;++c) hf << "," << m.matrix.m[r][c]; hf << "\n"; }
    }

    // 指导用户低头一次过滤
    std::cout << "\n请让目标角色低头到最低，准备好后按回车开始低头过滤..."; std::string _tmp2; std::getline(std::cin, _tmp2);
    for (int t = 5; t > 0; --t) {
        std::cout << "开始读取前倒计时: " << t << "s\r" << std::flush;
        Sleep(1000);
    }
    std::cout << "\n正在读取内存并执行低头过滤...\n";
    std::atomic<int> lowProgress(0);
    std::vector<ScanAddress> lowScanList;
    lowScanList.reserve(highFiltered.size());
    for (const auto& c : highFiltered) lowScanList.push_back({ c.address });
    curCandidates = ReadCandidatesWithRange(hGame, lowScanList, 0, &lowProgress);
    std::cout << "低头阶段读取候选: " << curCandidates.size() << " 个（仅重读高头过滤后的地址）\n";
    std::vector<MatrixCandidate> lowFiltered = FilterByHeadPose(curCandidates, false);
    std::cout << "低头过滤结果: " << lowFiltered.size() << " 个候选，已保存 matrices_low.csv\n";
    {
        std::ofstream lf("matrices_low.csv"); lf << "address"; for (int r = 0;r < 4;++r) for (int c = 0;c < 4;++c) lf << ",m" << r << c; lf << "\n";
        for (auto& m : lowFiltered) { lf << std::hex << "0x" << m.address << std::dec; for (int r = 0;r < 4;++r) for (int c = 0;c < 4;++c) lf << "," << m.matrix.m[r][c]; lf << "\n"; }
    }

    // 合并高低头的交集作为最终候选（同时满足高低的很可能是精确矩阵），或者按需使用并集
    std::vector<MatrixCandidate> finalCandidates;
    for (auto& h : highFiltered) for (auto& l : lowFiltered) if (h.address == l.address) finalCandidates.push_back(h);
    std::cout << "交集结果(同时满足高/低): " << finalCandidates.size() << " 个\n";

    // 如果交集已非空，直接在控制台打印这些候选并退出（无需人工复核）
    if (!finalCandidates.empty()) {
        std::cout << "交集非空，直接在控制台输出以下候选矩阵 (共 " << finalCandidates.size() << " 个)：\n";
        for (auto& c : finalCandidates) {
            PrintMatrix(c);
        }
        CloseHandle(hGame);
        std::cout << "完成，程序退出。\n";
        system("pause");
        return 0;
    }


    // 步骤4：矩阵扫描（后台）并显示简洁进度
    std::cout << "[3] 开始矩阵特征扫描...（后台）\n";
    std::atomic<int> progress(0);
    std::vector<MatrixCandidate> results;
    std::thread worker([&]() {
        results = ScanList(hGame, ceList, 256, &progress);
        });

    int total = (int)ceList.size();
    while (progress.load() < total) {
        int cur = progress.load();
        float pct = total ? (cur * 100.0f / total) : 100.0f;
        std::cout << "\r[进度] " << std::fixed << std::setprecision(1) << pct << "% (" << cur << "/" << total << ")    ";
        std::cout.flush();
        Sleep(80);
    }
    worker.join();
    std::cout << "\r[进度] 100.0% (" << total << "/" << total << ")    \n";

    // 按特征过滤（矩阵头在[-1,1]，第五个值近似0，矩阵尾在[-500,500]）
    std::vector<MatrixCandidate> filtered = FilterByFeatures(results);

    // 步骤5：简洁输出并保存到文件（保存完整 4x4 矩阵）
    std::cout << "\n========================================\n";
    std::cout << "  原始候选: " << results.size() << "， 过滤后: " << filtered.size() << " 个4x4矩阵\n";
    std::cout << "========================================\n";

    std::sort(filtered.begin(), filtered.end(),
        [](const MatrixCandidate& a, const MatrixCandidate& b) {
            return a.address < b.address;
        });

    // 交互式用户过滤（可选）
    // 优先使用高/低头交集 finalCandidates 作为人工复核列表；否则使用 filtered，再否则使用全部 results
    std::vector<MatrixCandidate> toReview;
    if (!finalCandidates.empty()) toReview = finalCandidates;
    else if (!filtered.empty()) toReview = filtered;
    else toReview = results; // 若严格过滤无结果，则展示原始候选供用户筛选

    std::cout << "\n是否进入交互式筛选候选矩阵？(y/n): ";
    std::string doInteractive; std::getline(std::cin, doInteractive);

    std::vector<MatrixCandidate> selected;
    if (!doInteractive.empty() && (doInteractive[0] == 'y' || doInteractive[0] == 'Y')) {
        std::cout << "交互式说明：输入 y 保留，n 跳过，r 显示完整矩阵，a 全部保留，q 退出。\n";
        for (size_t i = 0; i < toReview.size(); ++i) {
            auto& c = toReview[i];
            float tx = c.matrix.m[3][0];
            float ty = c.matrix.m[3][1];
            float tz = c.matrix.m[3][2];
            std::cout << std::dec << (i + 1) << "/" << toReview.size() << "  ";
            std::cout << std::hex << "0x" << c.address << std::dec << "  -> ";
            std::cout << "tx=" << tx << " ty=" << ty << " tz=" << tz << "\n";
            std::cout << "操作? (y/n/r/a/q): ";
            std::string op; std::getline(std::cin, op);
            if (op.empty()) continue;
            char ch = op[0];
            if (ch == 'y' || ch == 'Y') {
                selected.push_back(c);
            }
            else if (ch == 'n' || ch == 'N') {
                continue;
            }
            else if (ch == 'r' || ch == 'R') {
                PrintMatrix(c);
                std::cout << "再次选择 (y/n/a/q): ";
                std::getline(std::cin, op);
                if (!op.empty()) {
                    char c0 = op[0];
                    if (c0 == 'y' || c0 == 'Y') selected.push_back(c);
                    else if (c0 == 'a' || c0 == 'A') { selected.insert(selected.end(), toReview.begin() + i, toReview.end()); break; }
                    else if (c0 == 'q' || c0 == 'Q') break;
                }
            }
            else if (ch == 'a' || ch == 'A') {
                selected.insert(selected.end(), toReview.begin() + i, toReview.end());
                break;
            }
            else if (ch == 'q' || ch == 'Q') {
                break;
            }
        }
    }
    else {
        // 非交互模式：直接把 filtered 作为最终结果
        selected = toReview;
    }

    // 输出最终选定结果到控制台
    std::cout << "\n=== 最终选定矩阵（共 " << selected.size() << " 个）===\n";
    for (auto& c : selected) PrintMatrix(c);

    CloseHandle(hGame);
    std::cout << "\n[完成] ";
    system("pause");
    return 0;
}
