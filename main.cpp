#pragma execution_character_set("utf-8")

#include <windows.h>
#include <iostream>
#include <cstdio>
#include <vector>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <algorithm>

// Dear ImGuiの内部的な数学演算子の定義を強制的に含めるためのマクロ。
// cl.exeでのビルド時に発生しやすいシンボル解決エラーを回避するのに役立ちます。
#define IMGUI_DEFINE_MATH_OPERATORS

// --- Dear ImGui 関係のヘッダー ---
#include "imgui.h"
#include "backends/imgui_impl_win32.h"
#include "backends/imgui_impl_dx11.h"

// DirectX 11 のヘッダー
#include <d3d11.h>

// アクションの種類
enum MacroActionType {
    ACTION_COMBO,   // 複数のキーを同時に押して離す (例: Ctrl+C)
    ACTION_TEXT,    // テキストを入力する
    ACTION_WAIT     // 待機する
};

// マクロの個々のアクション（操作）を定義する構造体
struct MacroAction {
    MacroActionType type;
    std::vector<WORD> comboKeys; // COMBO用: キーコードのリスト
    std::string text;            // TEXT用: 文字列 (UTF-8)
    int waitMs;                  // WAIT用: 待機時間
};

// マクロ全体を定義する構造体
struct Macro {
    std::vector<WORD> hotkeys;         // 複数のキーを保持できるように vector に変更
    std::vector<MacroAction> actions;  // 実行する一連の操作リスト
};

// UTF-8 (std::string) を Windows ワイド文字 (std::wstring / UTF-16) に変換する
std::wstring utf8_to_wstring(const std::string& str)
{
    if (str.empty()) return std::wstring();
    
    // 必要なバッファサイズを計算
    int size_needed = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), NULL, 0);
    
    // wstringのバッファを確保
    std::wstring wstrTo(size_needed, 0);
    
    // 実際に変換
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), (int)str.size(), &wstrTo[0], size_needed);
    
    return wstrTo;
}

// マクロ全体を保持するグローバルリスト
std::vector<Macro> global_macros;
// グローバルフックのハンドル（IDのようなもの）を格納する変数
HHOOK hKeyboardHook;

// マクロの有効/無効フラグ（F12 + Ctrlで切り替える）
bool g_macroEnabled = true;

#define WM_TRAYICON (WM_USER + 1) // トレイアイコンからの通知用メッセージ
NOTIFYICONDATAW g_nid = { 0 };    // トレイアイコンの設定データ

// --- Dear ImGui/DirectX 11 用のグローバル変数 ---
ID3D11Device* g_pd3dDevice = NULL;
ID3D11DeviceContext* g_pd3dImmediateContext = NULL;
IDXGISwapChain* g_pSwapChain = NULL;
ID3D11RenderTargetView* g_mainRenderTargetView = NULL;

// --- DirectX 11 関数の宣言 ---
bool CreateDeviceD3D(HWND hWnd);
void CleanupDeviceD3D();
void CreateRenderTarget();
void CleanupRenderTarget();
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// 文字列から仮想キーコード(WORD)に変換する
// 例: "VK_CONTROL" -> 0x11, "F11" -> 0x7A, "0x41" -> 0x41
WORD StringToVkCode(const std::string& str) {
    if (str.empty()) return 0;
    
    // 16進数 ('0x...') として解析を試みる
    if (str.size() > 2 && str[0] == '0' && (str[1] == 'x' || str[1] == 'X')) {
        try {
            return (WORD)std::stoul(str, nullptr, 16);
        } catch (...) {
            // 解析失敗
        }
    }

    // 単一文字（'A'?'Z'、'0'?'9'）の変換
    if (str.size() == 1) {
        char c = (char)std::toupper(str[0]); // キャストを追加
        // char c = std::toupper(str[0]); // 大文字に統一
        
        if (c >= 'A' && c <= 'Z') {
            // アルファベット ('A'?'Z') のキーコードは、ASCIIコードと一致
            return (WORD)c;
        }
        if (c >= '0' && c <= '9') {
            // 数字キー ('0'?'9') のキーコードも、ASCIIコードと一致
            return (WORD)c;
        }
        // 他の単一文字キー（例: '-', '=', ';' など）もここに追加可能
    }
    
    // 特殊なVKコード文字列を解析する (代表的なもののみ)
    if (str == "VK_CONTROL" || str == "CONTROL" || str == "Ctrl") return VK_CONTROL;
    if (str == "VK_SHIFT"   || str == "SHIFT") return VK_SHIFT;
    if (str == "VK_MENU"    || str == "ALT") return VK_MENU; // Altキー
    if (str == "VK_RETURN"  || str == "ENTER") return VK_RETURN; // Enterキー

    // ファンクションキー
    if (str == "F1" || str == "VK_F1") return VK_F1;
    if (str == "F2" || str == "VK_F2") return VK_F2;
    if (str == "F3" || str == "VK_F3") return VK_F3;
    if (str == "F4" || str == "VK_F4") return VK_F4;
    if (str == "F5" || str == "VK_F5") return VK_F5;
    if (str == "F6" || str == "VK_F6") return VK_F6;
    if (str == "F7" || str == "VK_F7") return VK_F7;
    if (str == "F8" || str == "VK_F8") return VK_F8;
    if (str == "F9" || str == "VK_F9") return VK_F9;
    if (str == "F11" || str == "VK_F11") return VK_F11;
    if (str == "F10" || str == "VK_F10") return VK_F10;
    if (str == "F12" || str == "VK_F12") return VK_F12;

    // ナビゲーションおよび編集キー
    if (str == "TAB" || str == "VK_TAB") return VK_TAB;
    if (str == "ESC" || str == "VK_ESCAPE") return VK_ESCAPE;
    if (str == "BACKSPACE" || str == "BKSP") return VK_BACK;
    if (str == "DELETE" || str == "DEL") return VK_DELETE;
    if (str == "INSERT" || str == "INS") return VK_INSERT;
    if (str == "HOME" || str == "END") return VK_END;

    // 矢印キー (Arrows)
    if (str == "UP" || str == "VK_UP") return VK_UP;
    if (str == "DOWN" || str == "VK_DOWN") return VK_DOWN;
    if (str == "LEFT" || str == "VK_LEFT") return VK_LEFT;
    if (str == "RIGHT" || str == "VK_RIGHT") return VK_RIGHT;
    
    // 上記以外の場合は、Windows APIの定義を利用するため、そのまま0を返すか、
    // ASCII/キーコードとして扱う（ファイル読み込みの簡略化のため、ここでは代表的なもののみ対応）
    return 0; 
}

// 待機関数（ミリ秒単位で処理を停止させる）
// ACTION_WAIT が指定されたときに実行されます。
void PerformWait(int ms) {
    if (ms > 0) {
        Sleep(ms); // windows.h の Sleep 関数を使用
    }
}

// --- 仮想入力 送信機能 ------------------------------------------
// 特定のキーコード（vkCode）に対応するキーを押す動作（KeyDown）を送信する関数
void SendKeyDown(WORD vkCode) {
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vkCode;
    // 右Ctrl(VK_RCONTROL)や右Alt(VK_RMENU)の場合、拡張キーフラグ(KEYEVENTF_EXTENDEDKEY)が必要
    if (vkCode == VK_RCONTROL || vkCode == VK_RMENU || vkCode == VK_RSHIFT ||
        vkCode == VK_UP || vkCode == VK_DOWN || vkCode == VK_LEFT || vkCode == VK_RIGHT ||
        vkCode == VK_DELETE || vkCode == VK_HOME || vkCode == VK_END || vkCode == VK_INSERT ||
        vkCode == VK_PRIOR || vkCode == VK_NEXT) {
        input.ki.dwFlags = KEYEVENTF_EXTENDEDKEY;
    }
    SendInput(1, &input, sizeof(INPUT));
}

// 特定のキーコード（vkCode）に対応するキーを離す動作（KeyUp）を送信する関数
void SendKeyUp(WORD vkCode) {
    INPUT input = {0};
    input.type = INPUT_KEYBOARD;
    input.ki.wVk = vkCode;
    input.ki.dwFlags = KEYEVENTF_KEYUP;
    if (vkCode == VK_RCONTROL || vkCode == VK_RMENU || vkCode == VK_RSHIFT ||
        vkCode == VK_UP || vkCode == VK_DOWN || vkCode == VK_LEFT || vkCode == VK_RIGHT ||
        vkCode == VK_DELETE || vkCode == VK_HOME || vkCode == VK_END || vkCode == VK_INSERT ||
        vkCode == VK_PRIOR || vkCode == VK_NEXT) {
        input.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
    }
    SendInput(1, &input, sizeof(INPUT));
}

// Unicode文字列送信
void SendText(const std::string& utf8Text) {
    std::wstring wstr = utf8_to_wstring(utf8Text);
    for (wchar_t c : wstr) {
        INPUT inputs[2] = {};
        // Down
        inputs[0].type = INPUT_KEYBOARD;
        inputs[0].ki.wScan = c;
        inputs[0].ki.dwFlags = KEYEVENTF_UNICODE;
        // Up
        inputs[1] = inputs[0];
        inputs[1].ki.dwFlags |= KEYEVENTF_KEYUP;
        
        SendInput(2, inputs, sizeof(INPUT));
    }
}

// 修正版: エラーに強い読み込み関数
void LoadMacrosFromFile(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) {
        // 初回起動時などはファイルがないのが普通なので、エラーにはしない
        std::cout << "[INFO] Macro file not found. Starting with empty list." << std::endl;
        return;
    }

    global_macros.clear();
    std::string line;

    while (std::getline(file, line)) {
        if (line.empty() || line[0] == '#') continue;
        
        // 最後の2つ (Type, Data) とそれ以前 (Hotkeys) に分ける
        size_t lastComma = line.find_last_of(',');
        if (lastComma == std::string::npos) continue;
        std::string dataPart = line.substr(lastComma + 1); // Data
        
        // Dataの前の部分 (Hotkeys..., Type)
        std::string preData = line.substr(0, lastComma);
        size_t typeComma = preData.find_last_of(',');
        if (typeComma == std::string::npos) continue;
        
        std::string typeStr = preData.substr(typeComma + 1); // Type
        std::string hotkeysPart = preData.substr(0, typeComma); // Hotkeys...

        // 空白除去
        auto trim = [](std::string& s) {
            s.erase(0, s.find_first_not_of(" \t"));
            s.erase(s.find_last_not_of(" \t") + 1);
        };
        trim(typeStr); trim(dataPart);

        // アクション作成
        MacroAction action;
        if (typeStr == "COMBO") {
            action.type = ACTION_COMBO;
            std::stringstream ss(dataPart);
            std::string codeStr;
            while (std::getline(ss, codeStr, ':')) {
                try { action.comboKeys.push_back((WORD)std::stoi(codeStr)); } catch(...) {}
            }
        } else if (typeStr == "TEXT") {
            action.type = ACTION_TEXT;
            action.text = dataPart;
        } else if (typeStr == "WAIT") {
            action.type = ACTION_WAIT;
            try { action.waitMs = std::stoi(dataPart); } catch(...) { action.waitMs = 0; }
        } else {
            // 旧フォーマット互換用 (KEYDOWN/KEYUPなど) は今回は簡易化のため省略
            // 必要ならここにロジック追加
            continue; 
        }

        // ホットキー解析
        std::vector<WORD> hks;
        std::stringstream ssHk(hotkeysPart);
        std::string hkToken;
        while (std::getline(ssHk, hkToken, ',')) {
            trim(hkToken);
            if (!hkToken.empty()) hks.push_back(StringToVkCode(hkToken));
        }

        if (!hks.empty()) {
            bool found = false;
            for (auto& m : global_macros) {
                if (m.hotkeys == hks) {
                    m.actions.push_back(action);
                    found = true;
                    break;
                }
            }
            if (!found) {
                Macro new_m;
                new_m.hotkeys = hks;
                new_m.actions.push_back(action);
                global_macros.push_back(new_m);
            }
        }
    }
    std::cout << "[INFO] Loaded macros." << std::endl;
}

// マクロをファイルに保存する関数
void SaveMacrosToFile(const std::string& filename) {
    std::ofstream file(filename);
    if (!file.is_open()) {
        std::cerr << "[ERROR] Could not open file for writing: " << filename << std::endl;
        return;
    }

    // ヘッダー（説明書き）
    file << "# Hotkey, Key, Type, WaitMs\n";

    for (const auto& macro : global_macros) {
        for (const auto& action : macro.actions) {
            // 1. ホットキーたちを書き出す
            for (size_t i = 0; i < macro.hotkeys.size(); i++) {
                char hkStr[16];
                sprintf_s(hkStr, "0x%02X, ", macro.hotkeys[i]);
                file << hkStr;
            }
            
            // 2. アクション内容を書き出す
            if (action.type == ACTION_COMBO) {
                file << "COMBO, ";
                for (size_t k = 0; k < action.comboKeys.size(); k++) {
                    file << (int)action.comboKeys[k] << (k == action.comboKeys.size() - 1 ? "" : ":");
                }
            } else if (action.type == ACTION_TEXT) {
                file << "TEXT, " << action.text;
            } else if (action.type == ACTION_WAIT) {
                file << "WAIT, " << action.waitMs;
            }
            file << "\n";
        }
    }
    std::cout << "[INFO] Macros saved to " << filename << std::endl;
}

// マクロ読み込み関数
void RunMacroAsync(std::vector<MacroAction> actions) {
    // 1. フック処理が落ち着くまでほんの少し待つ
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // 2. 邪魔になりそうな修飾キー（Ctrl, Shift, Alt）が押されていたら、一時的に「離す」信号を送る
    std::vector<WORD> modifiersToRestore;
    // チェックするキー: Shift, Ctrl, Alt (左右含む)
    WORD checkList[] = {VK_LSHIFT, VK_RSHIFT, VK_SHIFT, 
                        VK_LCONTROL, VK_RCONTROL, VK_CONTROL, 
                        VK_LMENU, VK_RMENU, VK_MENU };

    for (WORD vk : checkList) {
        // 物理的に押されているかチェック (最上位ビットが1なら押されている)
        if (GetAsyncKeyState(vk) & 0x8000) {
            SendKeyUp(vk);             // 一旦離す
            modifiersToRestore.push_back(vk); // 「離したよ」と記録しておく
        }
    }

    // 念のため少し待機
    std::this_thread::sleep_for(std::chrono::milliseconds(5));

    // 3. マクロ本番を実行
    for (const auto& action : actions) {
        if (action.type == ACTION_COMBO) {
            // 全押し
            for (WORD k : action.comboKeys) SendKeyDown(k);
            std::this_thread::sleep_for(std::chrono::milliseconds(10)); // 安定のため少し待つ
            // 全離し（逆順推奨だが同時なら順序問わず）
            for (WORD k : action.comboKeys) SendKeyUp(k);
        }
        else if (action.type == ACTION_TEXT) {
            SendText(action.text);
        }
        else if (action.type == ACTION_WAIT) {
            if (action.waitMs > 0) Sleep(action.waitMs);
        }
    }

    // 4. マクロ終了後の処理: 修飾キー復帰
    for (WORD vk : modifiersToRestore) {
        if (GetAsyncKeyState(vk) & 0x8000) {
            SendKeyDown(vk); // まだ指があるなら、論理的にも押した状態に戻す
        }
        // 指を離していたら何もしない（これで押しっぱなし地獄から解放されます）
    }
}

// --- 1. フックプロシージャ（監視関数） -----------------------------
LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        KBDLLHOOKSTRUCT* pKeyBoard = (KBDLLHOOKSTRUCT*)lParam;

        // 自分で送信した入力イベントは無視する
        if (pKeyBoard->flags & LLKHF_INJECTED) {
            return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
        }

        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            // F12 は常にハンドル（Ctrl+F12でマクロON/OFF、単独F12で終了）
            if (pKeyBoard->vkCode == VK_F12) {
                if (GetAsyncKeyState(VK_CONTROL) & 0x8000) {
                    g_macroEnabled = !g_macroEnabled; // ON/OFF反転
                    std::cout << "[INFO] Macro " << (g_macroEnabled ? "enabled" : "disabled") << std::endl;
                } else {
                    PostQuitMessage(0); // Ctrlなしなら終了
                }
                return 1;
            }

            // マクロが無効なら通常キー入力をそのまま通す
            if (!g_macroEnabled) {
                return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
            }

            // 登録されている全マクロをチェック
            for (const auto& macro : global_macros) {
                if (macro.hotkeys.empty()) continue;

                // 1. まず「今押されたキー」がホットキーの一部に含まれているか確認
                bool isTriggerKey = false;
                for (WORD hk : macro.hotkeys) {
                    // 左右の区別なく判定（今押されたのがLかRで、設定が共通コードなら一致とみなす）
                    if (pKeyBoard->vkCode == hk) {
                        isTriggerKey = true;
                    } else if (hk == VK_CONTROL && (pKeyBoard->vkCode == VK_LCONTROL || pKeyBoard->vkCode == VK_RCONTROL)) {
                        isTriggerKey = true;
                    } else if (hk == VK_MENU && (pKeyBoard->vkCode == VK_LMENU || pKeyBoard->vkCode == VK_RMENU)) {
                        isTriggerKey = true;
                    } else if (hk == VK_SHIFT && (pKeyBoard->vkCode == VK_LSHIFT || pKeyBoard->vkCode == VK_RSHIFT)) {
                        isTriggerKey = true;
                    }
                    if (isTriggerKey) break;
                }
                if (!isTriggerKey) continue; 

                // 2. ホットキーの構成キーが「全て」押されているかチェック
                bool allPressed = true;
                for (WORD hk : macro.hotkeys) {
                    bool isThisKeyPressed = false;

                    // 今まさに押されたキーそのものならOK
                    if (hk == pKeyBoard->vkCode) {
                        isThisKeyPressed = true;
                    } 
                    // 設定が共通Ctrlで、物理的にLかRのどちらかが押されているか
                    else if (hk == VK_CONTROL) {
                        if ((GetAsyncKeyState(VK_LCONTROL) & 0x8000) || (GetAsyncKeyState(VK_RCONTROL) & 0x8000)) isThisKeyPressed = true;
                    }
                    // 設定が共通Alt(MENU)で、物理的にLかRのどちらかが押されているか
                    else if (hk == VK_MENU) {
                        if ((GetAsyncKeyState(VK_LMENU) & 0x8000) || (GetAsyncKeyState(VK_RMENU) & 0x8000)) isThisKeyPressed = true;
                    }
                    // 設定が共通Shiftで、物理的にLかRのどちらかが押されているか
                    else if (hk == VK_SHIFT) {
                        if ((GetAsyncKeyState(VK_LSHIFT) & 0x8000) || (GetAsyncKeyState(VK_RSHIFT) & 0x8000)) isThisKeyPressed = true;
                    }
                    // その他の通常のキー
                    else if (GetAsyncKeyState(hk) & 0x8000) {
                        isThisKeyPressed = true;
                    }

                    if (!isThisKeyPressed) {
                        allPressed = false;
                        break;
                    }
                }

                if (allPressed) {
                    std::cout << "\n[MACRO] Detected. Executing in thread..." << std::endl;
                    // マクロ実行を別スレッドに投げる
                    std::thread(RunMacroAsync, macro.actions).detach();
                    return 1; // 入力をブロック
                }
            }
        }
    }

    return CallNextHookEx(hKeyboardHook, nCode, wParam, lParam);
}

// --- 2. フック設定 ------------------------------------------------
void SetHook() {
    std::cout << "[INFO] Setting up Global Keyboard Hook..." << std::endl;
    hKeyboardHook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc, NULL, 0);

    if (hKeyboardHook == NULL) {
        // MessageBoxWでエラーメッセージを表示（LPCWSTR引数が必要なため）
        MessageBoxW(NULL, L"フックのインストールに失敗しました！", L"エラー", MB_ICONERROR | MB_OK); 
        std::cerr << "[ERROR] Hook installation failed!" << std::endl;
    } else {
        std::cout << "[INFO] Hook successfully installed. Press F12 to exit." << std::endl;
    }
}

// フックを解除する関数
void UnHook() {
    UnhookWindowsHookEx(hKeyboardHook);
    std::cout << "[INFO] Hook successfully uninstalled." << std::endl;
}

static ImVec4 clear_color = ImVec4(0.15f, 0.15f, 0.15f, 1.00f);

// --- DirectX 11 の初期化処理 ---
bool CreateDeviceD3D(HWND hWnd) {
    // セットアップフラグ
    UINT createDeviceFlags = 0;
    // createDeviceFlags |= D3D11_CREATE_DEVICE_DEBUG; // デバッグ時に有効化

    D3D_DRIVER_TYPE driverType = D3D_DRIVER_TYPE_HARDWARE;
    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL featureLevelArray[2] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0, };
    
    DXGI_SWAP_CHAIN_DESC sd;
    ZeroMemory(&sd, sizeof(sd));
    sd.BufferCount = 2;
    sd.BufferDesc.Width = 0;
    sd.BufferDesc.Height = 0;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.SampleDesc.Quality = 0;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, driverType, NULL, createDeviceFlags, featureLevelArray, 2, D3D11_SDK_VERSION, &sd, &g_pSwapChain, &g_pd3dDevice, &featureLevel, &g_pd3dImmediateContext);
    if (hr != S_OK)
        return false;

    CreateRenderTarget();
    return true;
}

void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_pSwapChain) { g_pSwapChain->Release(); g_pSwapChain = NULL; }
    if (g_pd3dImmediateContext) { g_pd3dImmediateContext->Release(); g_pd3dImmediateContext = NULL; }
    if (g_pd3dDevice) { g_pd3dDevice->Release(); g_pd3dDevice = NULL; }
}

void CreateRenderTarget() {
    ID3D11Texture2D* pBackBuffer;
    g_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&pBackBuffer));
    g_pd3dDevice->CreateRenderTargetView(pBackBuffer, NULL, &g_mainRenderTargetView);
    pBackBuffer->Release();
}

void CleanupRenderTarget() {
    if (g_mainRenderTargetView) { g_mainRenderTargetView->Release(); g_mainRenderTargetView = NULL; }
}


// ImGui Win32 のためのメッセージ処理の転送関数を宣言
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

// Windowsプロシージャ（ウィンドウへのメッセージ処理）
LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    // ImGuiにメッセージを転送（これをしないとImGuiが操作できない）
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;

    switch (msg)
    {
    case WM_TRAYICON:
        // 右下のアイコンをダブルクリックしたらウィンドウを再表示
        if (lParam == WM_LBUTTONDBLCLK) {
            ShowWindow(hWnd, SW_SHOW);
            SetForegroundWindow(hWnd);
        }
        return 0;
    case WM_CLOSE:
        // ×ボタンが押されたら終了せず、隠すだけにする
        ShowWindow(hWnd, SW_HIDE);
        return 0;

    case WM_SIZE:
        if (g_pd3dDevice != NULL && wParam != SIZE_MINIMIZED)
        {
            // 1. 古いキャンバスを破棄
            CleanupRenderTarget();
            // // DX11のサイズ変更処理 (ここでは省略)
            // // CreateRenderTarget();
            
            // 2. スワップチェーン（画面バッファ）のサイズをウィンドウに合わせる
            g_pSwapChain->ResizeBuffers(0, (UINT)LOWORD(lParam), (UINT)HIWORD(lParam), DXGI_FORMAT_UNKNOWN, 0);
            
            // 3. 新しいキャンバスを作成（これを忘れていたのが原因！）
            CreateRenderTarget();
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) // Disable ALT application menu
            return 0;
        break;
    case WM_DESTROY:
        // アプリ終了時にトレイからアイコンを消す
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hWnd, msg, wParam, lParam);
}

// キーコードを読みやすい文字に変換するヘルパー関数
std::string VkCodeToString(WORD vk) {
    // 左右のキーを共通の名前に変換して表示
    if (vk == VK_CONTROL || vk == 162 || vk == 163) return "Ctrl";
    if (vk == VK_MENU    || vk == 164 || vk == 165) return "Alt";
    if (vk == VK_SHIFT   || vk == 160 || vk == 161) return "Shift";

    // 数字 (0-9) と アルファベット (A-Z)
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) {
        return std::string(1, (char)vk);
    }
    // ファンクションキー (F1 - F24)
    if (vk >= VK_F1 && vk <= VK_F24) {
        return "F" + std::to_string(vk - VK_F1 + 1);
    }
    // 特殊キー
    switch(vk) {
        case VK_RETURN: return "Enter";
        case VK_ESCAPE: return "Esc";
        case VK_BACK:   return "BackSpace";
        case VK_TAB:    return "Tab";
        case VK_SPACE:  return "Space";
        case VK_SHIFT:  return "Shift";
        case VK_CONTROL:return "Ctrl";
        case VK_MENU:   return "Alt";
        case VK_UP:     return "Up";
        case VK_DOWN:   return "Down";
        case VK_LEFT:   return "Left";
        case VK_RIGHT:  return "Right";
    }
    // その他は数値をそのまま表示
    return "Key:" + std::to_string(vk);
}

// --- 最終的なプログラムの開始点 ---
int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
    // --- 1. 初期化処理 ---

    // コンソールウィンドウの表示（デバッグ用）
    // AllocConsole(); // 新しいコンソールを割り当てる
    // freopen("CONOUT$", "w", stdout); // stdoutをコンソールにリダイレクト
    // // Windowsのコンソール出力コードページをUTF-8に設定
    // // std::cout の日本語表示と、フックログの日本語表示を可能にする
    // SetConsoleOutputCP(CP_UTF8);
    
    // A. マクロデータのロード
    LoadMacrosFromFile("macros.txt");
    
    // B. ウィンドウクラスの登録

    std::wstring class_name_w = utf8_to_wstring(u8"MacroTool クラス");
    std::wstring window_title_w = utf8_to_wstring(u8"WinHot Plus");

    WNDCLASSEXW wc = { 0 }; // 一旦すべて0で初期化
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.style = CS_CLASSDC;
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.lpszClassName = class_name_w.c_str();

    if (!RegisterClassExW(&wc)) {
        return 1;
    }
    
    // C. ウィンドウの作成
    HWND hwnd = CreateWindowW(
        class_name_w.c_str(),             // クラス名
        window_title_w.c_str(),           // ウィンドウタイトル (変換後の文字列を使用)
        WS_OVERLAPPEDWINDOW,
        100, 100, 
        375, 500,                         // ウィンドウサイズ (幅, 高さ)
        NULL, NULL, wc.hInstance, NULL
    );
    
    // D. DirectXの初期化
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, wc.hInstance);
        return 1;
    }

    // E. フックの設定
    SetHook(); // 既存のフック設定関数

    // F. ウィンドウの表示
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    // --- トレイアイコンの設定 ---
    g_nid.cbSize = sizeof(NOTIFYICONDATAW);
    g_nid.hWnd = hwnd;
    g_nid.uID = 1;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAYICON;
    // アイコンは標準のアプリケーションアイコンを使用
    g_nid.hIcon = LoadIcon(NULL, IDI_APPLICATION);
    // マウスを乗せたときに出る説明（日本語）
    wcscpy_s(g_nid.szTip, L"WinHot Plus (Ctrl+F12 to Pause)");
    
    // トレイに登録
    Shell_NotifyIconW(NIM_ADD, &g_nid);
    
    // G. ImGuiの初期化
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_pd3dDevice, g_pd3dImmediateContext);


    // 日本語フォント (メイリオ) を読み込む
    // サイズは 18.0f (少し大きめで見やすく) に設定
    // 第3引数 NULL, 第4引数に日本語の文字範囲 (GetGlyphRangesJapanese) を指定
    ImFont* font = io.Fonts->AddFontFromFileTTF("C:\\Windows\\Fonts\\meiryo.ttc", 18.0f, NULL, io.Fonts->GetGlyphRangesJapanese());
    
    // 万が一メイリオが無い場合のエラー回避（念のため）
    if (font == NULL) {
        io.Fonts->AddFontDefault();
    }

    // --- 2. メインループ（描画ループ） ---
    MSG msg;
    ZeroMemory(&msg, sizeof(msg));
    while (msg.message != WM_QUIT)
    {
        if (PeekMessage(&msg, NULL, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            continue;
        }

        // H. ImGuiの描画開始
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        RECT rect;
        GetClientRect(hwnd, &rect);
        float width = (float)(rect.right - rect.left);
        float height = (float)(rect.bottom - rect.top);

        // 次に作るImGuiウィンドウの位置とサイズを固定
        ImGui::SetNextWindowPos(ImVec2(0, 0));
        ImGui::SetNextWindowSize(ImVec2(width, height));

        // ウィンドウの装飾（タイトルバー、移動、リサイズ）をすべて無効化するフラグ
        ImGuiWindowFlags window_flags = ImGuiWindowFlags_NoTitleBar | 
                                        ImGuiWindowFlags_NoResize | 
                                        ImGuiWindowFlags_NoMove | 
                                        ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoBackground;

        // 一体化したウィンドウを開始
        ImGui::Begin(u8"MainPanel", nullptr, window_flags);

        //　特殊キーの定義と選択状態管理
        struct SpKeyDef { const char* name; WORD code; };
        static const std::vector<SpKeyDef> sp_keys = {
            { u8"無変換", VK_NONCONVERT }, { u8"変換", VK_CONVERT },
            { u8"ひらがな/カタカナ", 0xF2 }, { u8"半角/全角", 0x19 },
            { u8"左Ctrl", VK_LCONTROL }, { u8"右Ctrl", VK_RCONTROL },
            { u8"左Shift", VK_LSHIFT },  { u8"右Shift", VK_RSHIFT },
            { u8"左Alt", VK_LMENU },     { u8"右Alt", VK_RMENU },
            { u8"左Win", VK_LWIN },      { u8"右Win", VK_RWIN },
            { u8"Esc", VK_ESCAPE },      { u8"Tab", VK_TAB },
            { u8"Enter", VK_RETURN },    { u8"BackSpace", VK_BACK }
        };

        // 新規マクロ追加エリア
        static std::vector<WORD> new_hotkeys; // 登録予定のキーリスト
        static bool is_recording_hotkey = false;

        // 「どの項目をいま記録中か」を管理する変数
        static int recording_target = 0; // 0:なし, 1:起動キー, 2:操作キー

        static bool is_editing_mode = false;  // 現在編集モードかどうか
        static int editing_macro_index = -1;  // 編集中のマクロの番号
        static int selected_sp_hotkey_idx = 0; // 起動キー用 (0:なし, 1~:選択中)
        static int selected_sp_action_idx = 0; // アクション用

        static std::vector<MacroAction> new_actions; // 連続アクション用
        static bool is_rec_combo = false;
        static std::vector<WORD> temp_combo_keys;
        static char temp_text_buf[256] = "";
        static int temp_wait_ms = 100;

        // --- 特殊ページ専用の変数を追加 (sp_ を付与) ---
        static std::vector<WORD> sp_new_hotkeys; 
        static bool sp_is_recording_hotkey = false;
        static std::vector<MacroAction> sp_new_actions;
        static bool sp_is_rec_combo = false;
        static std::vector<WORD> sp_temp_combo_keys;

        static int current_tab = 0; // 0:基本, 1:特殊 を記録する変数

        static bool request_text_popup = false; // ポップアップを開く合図
        static bool request_wait_popup = false;

        // タブ機能の開始
        if (ImGui::BeginTabBar("MacroTabs")) {
            
            // =================================================================================
            // タブ1: 基本設定 (既存のUIをそのまま維持)
            // =================================================================================
            if (ImGui::BeginTabItem(u8"基本設定")) {
                current_tab = 0; // 基本タブであることを保存

                ImGui::TextColored(ImVec4(1,1,0,1), u8"【新規ショートカット作成 (通常)】"); // TextColoredで色付けタイトル

                // --- 起動キー設定  ---
                std::string hotkeys_display = "";
                for (auto hk : new_hotkeys) hotkeys_display += VkCodeToString(hk) + " + ";
                if (!hotkeys_display.empty()) hotkeys_display.pop_back(); else hotkeys_display = u8"(未設定)";
                // 最後の + を削除
                if (!hotkeys_display.empty() && hotkeys_display.back() == '+') hotkeys_display.pop_back();
                // 起動キーの表示
                ImGui::Text(u8"① 起動キー: %s", hotkeys_display.empty() ? u8"(未設定)" : hotkeys_display.c_str());
                
                // 起動キー記録ボタン
                if (is_recording_hotkey) {
                    ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.6f, 0.6f));
                    if (ImGui::Button(u8"記録完了")) is_recording_hotkey = false;
                    ImGui::PopStyleColor();
                    ImGui::SameLine(); // 以下も同じ行に配置
                    if (ImGui::Button(u8"1つ削除")) { 
                        if (!new_hotkeys.empty()) new_hotkeys.pop_back(); 
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(u8"キャンセル")) { 
                        is_recording_hotkey = false; new_hotkeys.clear(); 
                    }
                    ImGui::SameLine(); ImGui::Text(u8"キーを入力中...");

                    for (int k = 8; k < 240; k++) {
                        if (GetAsyncKeyState(k) & 0x8001 && k != VK_LBUTTON && k != VK_RBUTTON) {
                            WORD nmK = (WORD)k; // 通常のキー記録処理
                            if (k==VK_LCONTROL||k==VK_RCONTROL) nmK=VK_CONTROL;
                            if (k==VK_LSHIFT||k==VK_RSHIFT) nmK=VK_SHIFT;
                            if (k==VK_LMENU||k==VK_RMENU) nmK=VK_MENU;
                            if (std::find(new_hotkeys.begin(), new_hotkeys.end(), nmK) == new_hotkeys.end()) new_hotkeys.push_back(nmK);
                        }
                    }
                } else {
                    // 記録開始ボタンを押すと、入力をクリアして記録モードに入る
                    if (ImGui::Button(u8"記録開始")) { new_hotkeys.clear(); is_recording_hotkey = true; selected_sp_hotkey_idx = 0; } // 通常記録時は特殊選択リセット
                    ImGui::SameLine();
                    if (ImGui::Button(u8"クリア")) { new_hotkeys.clear(); }
                }

                ImGui::Separator();
                ImGui::Text(u8"② 実行内容");

                // --- アクション設定 (既存コード) ---
                if (is_rec_combo) {
                    ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.6f, 0.6f));
                    if (ImGui::Button(u8"コンボ確定")) { 
                        if (!temp_combo_keys.empty()) {
                            new_actions.push_back({ ACTION_COMBO, temp_combo_keys, "", 0 }); 
                        }
                        is_rec_combo = false; 
                        temp_combo_keys.clear(); 
                    }
                    ImGui::PopStyleColor();
                    ImGui::SameLine(); // 以下も同じ行に配置
                    if (ImGui::Button(u8"1つ削除##Del")) { 
                        if (!temp_combo_keys.empty()) temp_combo_keys.pop_back(); 
                    }
                    ImGui::SameLine(); 
                    if(ImGui::Button(u8"キャンセル")) { 
                        is_rec_combo = false; temp_combo_keys.clear(); 
                    }

                    // 入力中キーの表示
                    ImGui::SameLine(); 
                    ImGui::Text(u8"キーを同時押し入力中...");
                    std::string nDisp = "";
                    for (auto k : temp_combo_keys) nDisp += VkCodeToString(k) + " + ";
                    const std::string suffix = " + ";
                    if (!nDisp.empty() && nDisp.size() >= suffix.size() && nDisp.compare(nDisp.size() - suffix.size(), suffix.size(), suffix) == 0)
                        nDisp.erase(nDisp.size() - suffix.size());
                    ImGui::Text(u8"現在の組み合わせ: %s", nDisp.empty() ? u8"(なし)" : nDisp.c_str());
                    // for(auto k : temp_combo_keys) ImGui::SameLine(), ImGui::Text("%s", VkCodeToString(k).c_str());

                    for (int k = 8; k < 240; k++) { // コンボ記録処理
                        if (GetAsyncKeyState(k) & 0x8001 && k != VK_LBUTTON && k != VK_RBUTTON) {
                            WORD nmK = (WORD)k;
                            if (k==VK_LCONTROL||k==VK_RCONTROL) nmK=VK_CONTROL;
                            if (k==VK_LSHIFT||k==VK_RSHIFT) nmK=VK_SHIFT;
                            if (k==VK_LMENU||k==VK_RMENU) nmK=VK_MENU;
                            if (std::find(temp_combo_keys.begin(), temp_combo_keys.end(), nmK) == temp_combo_keys.end()) temp_combo_keys.push_back(nmK);
                        }
                    }
                } else {
                    if (ImGui::Button(u8"＋ 同時押し")) { 
                        is_rec_combo = true; temp_combo_keys.clear(); 
                        is_recording_hotkey = false; 
                    }
                    ImGui::SameLine(); 
                    if (ImGui::Button(u8"＋ テキスト")) { 
                        request_text_popup = true;
                        strncpy_s(temp_text_buf, "", 256); 
                    }
                    ImGui::SameLine(); 
                    if (ImGui::Button(u8"＋ 待機")) { 
                        request_wait_popup = true;
                        // ImGui::OpenPopup("AddWaitPopup"); 
                    }
                    ImGui::SameLine(); 
                    if (ImGui::Button(u8"クリア##ClearActionList")) new_actions.clear();
                }
                
                // 共通のポップアップ処理とリスト表示関数へ続く（タブ外または後述）
                // ※コード簡略化のため、ポップアップとリスト表示はこのブロックの最後に共通で記述します

                ImGui::EndTabItem(); 
            }

            // =================================================================================
            // タブ2: 特殊設定 (新しいUI)
            // =================================================================================
            if (ImGui::BeginTabItem(u8"特殊設定")) {
                current_tab = 1; // 特殊タブであることを保存

                ImGui::TextColored(ImVec4(0,1,1,1), u8"【特殊キー ショートカット作成】");

                // --- A. 起動キー設定 (ドロップダウン) ---
                ImGui::Text(u8"① 起動キー: ");
                ImGui::SameLine();
                ImGui::SetNextItemWidth(140);
                // ドロップダウン表示
                if (ImGui::BeginCombo(u8"##SpKeyHK", selected_sp_hotkey_idx == 0 ? u8"(なし)" : sp_keys[selected_sp_hotkey_idx-1].name)) {
                    if (ImGui::Selectable(u8"(なし)", selected_sp_hotkey_idx == 0)) {
                        selected_sp_hotkey_idx = 0;
                        sp_new_hotkeys.clear();
                    }
                    for (int i = 0; i < sp_keys.size(); i++) {
                        if (ImGui::Selectable(sp_keys[i].name, selected_sp_hotkey_idx == i + 1)) {
                            selected_sp_hotkey_idx = i + 1;
                            // 選ばれたキーを先頭にセット
                            sp_new_hotkeys.clear();
                            sp_new_hotkeys.push_back(sp_keys[i].code);
                            sp_is_recording_hotkey = false;
                        }
                    }
                    ImGui::EndCombo();
                }

                // 起動キーリスト表示と追加記録ボタン
                std::string hkDisp = ""; 
                for (auto k : sp_new_hotkeys) hkDisp += VkCodeToString(k) + " + ";
                if (!hkDisp.empty()) hkDisp.pop_back();
                if (!hkDisp.empty() && hkDisp.back() == '+') hkDisp.pop_back();
                ImGui::Text(u8"   現在: %s", hkDisp.empty() ? u8"(未設定)" : hkDisp.c_str());

                if (sp_is_recording_hotkey) {
                    if (ImGui::Button(u8"記録完了")) sp_is_recording_hotkey = false;
                    ImGui::SameLine();
                    if (ImGui::Button(u8"1つ削除")) { 
                        if (!sp_new_hotkeys.empty()) sp_new_hotkeys.pop_back(); 
                    }
                    ImGui::SameLine();
                    if (ImGui::Button(u8"キャンセル")) {
                        sp_is_recording_hotkey = false;
                    }
                    ImGui::SameLine(); 
                    ImGui::Text(u8"キー入力中...");
                    // 特殊タブ用の記録ループ
                    for (int k = 8; k < 240; k++) {
                        if (GetAsyncKeyState(k) & 0x8001 && k != VK_LBUTTON && k != VK_RBUTTON) {
                            WORD nmK = (WORD)k;
                            if (k==VK_LCONTROL||k==VK_RCONTROL) nmK=VK_CONTROL;
                            if (k==VK_LSHIFT||k==VK_RSHIFT) nmK=VK_SHIFT;
                            if (k==VK_LMENU||k==VK_RMENU) nmK=VK_MENU;
                            if (std::find(sp_new_hotkeys.begin(), sp_new_hotkeys.end(), nmK) == sp_new_hotkeys.end()) sp_new_hotkeys.push_back(nmK);
                        }
                    }
                } else {
                    if (ImGui::Button(u8"＋ 組み合わせキーを追加")) { 
                        // 特殊キー未選択ならクリアしてから開始
                        if (selected_sp_hotkey_idx == 0) sp_new_hotkeys.clear();
                        sp_is_recording_hotkey = true; 
                    }
                    ImGui::SameLine(); if (ImGui::Button(u8"クリア##ClearHotkey")) { sp_new_hotkeys.clear(); selected_sp_hotkey_idx = 0; }
                }

                ImGui::Separator();
                ImGui::Text(u8"② 実行内容");

                // --- B. 実行内容 (条件分岐) ---
                if (selected_sp_hotkey_idx == 0) {
                    // ケース1: 起動キーで特殊キーを選んでいない → 「特殊キーアクション」のみ追加可能
                    ImGui::Text(u8"実行する特殊キーを選択:");
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140);
                    ImGui::Combo(u8"##SpActCombo", &selected_sp_action_idx, [](void* data, int idx, const char** out_text) {
                        auto* items = (std::vector<SpKeyDef>*)data; *out_text = (*items)[idx].name; return true;
                    }, (void*)&sp_keys, (int)sp_keys.size());

                    ImGui::SameLine();
                    if (ImGui::Button(u8"追加##sp_act")) {
                        if (sp_new_actions.empty()) {
                            sp_new_actions.push_back({ ACTION_COMBO, { sp_keys[selected_sp_action_idx].code }, "", 0 });
                        }
                    }
                    if (!sp_new_actions.empty()) ImGui::TextColored(ImVec4(1, 0.5f, 0.5f, 1), u8"※ 特殊キーアクションは1つのみです");
                
                } else {
                    // ケース2: 起動キーで特殊キーを選んでいる → 通常のアクション（コンボ/テキスト/待機）が使用可能
                    if (sp_is_rec_combo) {
                        ImGui::PushStyleColor(ImGuiCol_Button, (ImVec4)ImColor::HSV(0.0f, 0.6f, 0.6f));
                        if (ImGui::Button(u8"コンボ確定")) { 
                            if (!sp_temp_combo_keys.empty()) 
                                sp_new_actions.push_back({ ACTION_COMBO, sp_temp_combo_keys, "", 0 }); 
                            sp_is_rec_combo = false; 
                            sp_temp_combo_keys.clear(); 
                        }
                        ImGui::PopStyleColor();
                        ImGui::SameLine(); // 以下も同じ行に配置
                        if (ImGui::Button(u8"1つ削除##SpDel")) { 
                            if (!sp_temp_combo_keys.empty()) sp_temp_combo_keys.pop_back(); 
                        }
                        ImGui::SameLine(); 
                        if(ImGui::Button(u8"キャンセル")) { 
                            sp_is_rec_combo = false; 
                            sp_temp_combo_keys.clear(); 
                        }

                        // 入力中キーの表示
                        ImGui::SameLine();
                        ImGui::Text(u8"キーを同時押し入力中...");
                        std::string nDisp = "";
                        for (auto k : sp_temp_combo_keys) nDisp += VkCodeToString(k) + " + ";
                        const std::string suffix_sp = " + ";
                        if (!nDisp.empty() && nDisp.size() >= suffix_sp.size() && nDisp.compare(nDisp.size() - suffix_sp.size(), suffix_sp.size(), suffix_sp) == 0)
                            nDisp.erase(nDisp.size() - suffix_sp.size());
                        ImGui::Text(u8"現在の組み合わせ: %s", nDisp.empty() ? u8"(なし)" : nDisp.c_str());
                        
                        // コンボ記録処理
                        for (int k = 8; k < 240; k++) {
                            if (GetAsyncKeyState(k) & 0x8001 && k != VK_LBUTTON && k != VK_RBUTTON) {
                                WORD nmK = (WORD)k;
                                if (k==VK_LCONTROL||k==VK_RCONTROL) nmK=VK_CONTROL;
                                if (k==VK_LSHIFT||k==VK_RSHIFT) nmK=VK_SHIFT;
                                if (k==VK_LMENU||k==VK_RMENU) nmK=VK_MENU;
                                if (std::find(sp_temp_combo_keys.begin(), sp_temp_combo_keys.end(), nmK) == sp_temp_combo_keys.end()) sp_temp_combo_keys.push_back(nmK);
                            }
                        }
                    } else {
                        if (ImGui::Button(u8"＋ 同時押し")) { sp_is_rec_combo = true; sp_temp_combo_keys.clear(); sp_is_recording_hotkey = false; }
                        ImGui::SameLine(); 
                        if (ImGui::Button(u8"＋ テキスト")) { 
                            // ImGui::OpenPopup("AddTextPopup");
                            request_text_popup = true;
                            strncpy_s(temp_text_buf, "", 256); 
                        }
                        ImGui::SameLine(); 
                        if (ImGui::Button(u8"＋ 待機")) { 
                            request_wait_popup = true;
                        }
                        ImGui::SameLine(); 
                        if (ImGui::Button(u8"クリア")) sp_new_actions.clear();
                    }
                }

                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();

            ImGui::Separator(); // 区切り線

            // --- 現在操作すべきデータを自動で切り替える ---
            std::vector<WORD>& active_hotkeys = (current_tab == 0) ? new_hotkeys : sp_new_hotkeys;
            std::vector<MacroAction>& active_actions = (current_tab == 0) ? new_actions : sp_new_actions;

            // --- 共通のアクションリスト表示 ---
            ImGui::Text(u8"現在の操作リスト (%s)", current_tab == 0 ? u8"基本" : u8"特殊");
            ImGui::BeginChild("ActionList", ImVec2(0, 150), true);
            for (int i = 0; i < (int)active_actions.size(); i++) {
                ImGui::PushID(i);
                if (ImGui::Button("X")) { 
                    active_actions.erase(active_actions.begin() + i); 
                    i--; ImGui::PopID(); continue; 
                }
                ImGui::SameLine();

                if (ImGui::Button(u8"✎")) { // 編集機能
                    auto target = active_actions[i];
                    if (target.type == ACTION_COMBO) { 
                        is_rec_combo = true; 
                        temp_combo_keys = target.comboKeys; 
                    }
                    // テキスト用
                    else if (target.type == ACTION_TEXT) { 
                        strncpy_s(temp_text_buf, target.text.c_str(), 256); 
                        request_text_popup = true; // 合図
                    }
                    // 待機用
                    else if (target.type == ACTION_WAIT) { 
                        temp_wait_ms = target.waitMs; 
                        request_wait_popup = true; // 合図
                    }
                    active_actions.erase(active_actions.begin() + i); 
                    i--; 
                    ImGui::PopID(); 
                    continue;
                }
                ImGui::SameLine();
                if (i > 0) { 
                    if (ImGui::Button("^")) std::swap(active_actions[i], active_actions[i-1]); 
                    ImGui::SameLine(); 
                }
                if (i < (int)active_actions.size() - 1) { 
                    if (ImGui::Button("v")) std::swap(active_actions[i], active_actions[i+1]); 
                    ImGui::SameLine(); 
                }

                std::string label = std::to_string(i + 1) + ". ";
                if (active_actions[i].type == ACTION_COMBO) {
                    label += "[キー] "; for (auto k : active_actions[i].comboKeys) label += VkCodeToString(k) + "+";
                    if (label.back() == '+') label.pop_back();
                } else if (active_actions[i].type == ACTION_TEXT) label += "[文字] " + active_actions[i].text;
                else label += "[待機] " + std::to_string(active_actions[i].waitMs) + " ms";
                ImGui::Text("%s", label.c_str());
                ImGui::PopID();
            }
            ImGui::EndChild();

            // --- 共通の保存ボタン (一番下に配置) ---
            std::string saveBtnLabel = is_editing_mode ? u8"更新 (上書き)" : u8"この設定で新規追加";
            if (ImGui::Button(saveBtnLabel.c_str(), ImVec2(-1, 40))) {
                // 今のタブに応じた hotkeys と actions が入っているかチェック
                if (!active_hotkeys.empty() && !active_actions.empty()) {
                    if (is_editing_mode && editing_macro_index != -1) {
                        global_macros[editing_macro_index].hotkeys = active_hotkeys;
                        global_macros[editing_macro_index].actions = active_actions;
                    } else {
                        Macro m;
                        m.hotkeys = active_hotkeys;
                        m.actions = active_actions;
                        global_macros.push_back(m);
                    }
                    // 保存後はすべてクリア
                    new_hotkeys.clear(); new_actions.clear();
                    sp_new_hotkeys.clear(); sp_new_actions.clear();
                    is_editing_mode = false; editing_macro_index = -1;
                    selected_sp_hotkey_idx = 0;
                }
            }
        }

        // 現在の稼働状態を色付きで表示
        if (g_macroEnabled) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), u8"● [稼働中] - Ctrl+F12で一時停止");
        } else {
            ImGui::TextColored(ImVec4(1.0f, 0.0f, 0.0f, 1.0f), u8"○ [停止中] - Ctrl+F12で再開");
        }
        ImGui::Separator();

        // この定義がポップアップより上に書いてあることを確認してください
        std::vector<MacroAction>& active_actions = (current_tab == 0) ? new_actions : sp_new_actions;
        std::vector<WORD>& active_hotkeys = (current_tab == 0) ? new_hotkeys : sp_new_hotkeys;

        //　合図があったら実際に親ウィンドウでOpenPopupする
        if (request_text_popup) {
            std::cout << "[DEBUG] request_text_popup true -> OpenPopup\n";
            ImGui::OpenPopup("AddTextPopup");
            request_text_popup = false; // 合図を戻す
        }
        if (request_wait_popup) {
            ImGui::OpenPopup("AddWaitPopup");
            request_wait_popup = false;
        }
        
        // --- 共通: ポップアップ処理 ---
        if (ImGui::BeginPopup("AddTextPopup")) {
            ImGui::Text(u8"入力したい文章:");
            ImGui::InputText("##text", temp_text_buf, 256);
            if (ImGui::Button(u8"追加")) {
                active_actions.push_back({ ACTION_TEXT, {}, std::string(temp_text_buf), 0 });
                ImGui::CloseCurrentPopup(); 
            }
            ImGui::EndPopup();
        }
        if (ImGui::BeginPopup("AddWaitPopup")) {
            ImGui::InputInt("ミリ秒", &temp_wait_ms);
            if (ImGui::Button(u8"追加")) { 
                active_actions.push_back({ ACTION_WAIT, {}, "", temp_wait_ms }); 
                ImGui::CloseCurrentPopup(); 
            }
            ImGui::EndPopup();
        }
        
        if (is_editing_mode && ImGui::Button(u8"編集をキャンセル", ImVec2(-1, 40))) {
            new_hotkeys.clear(); new_actions.clear();
            is_editing_mode = false; editing_macro_index = -1;
        }

        ImGui::Separator();

        // --- 保存ボタン ---
        if (ImGui::Button(u8"変更をファイルに保存")) {
            SaveMacrosToFile("macros.txt");
        }

        ImGui::Text(u8"【登録済みショートカット一覧】");

        for (int i = 0; i < global_macros.size(); i++) {
            ImGui::PushID(i);
            if (ImGui::Button(u8"削除")) { 
                global_macros.erase(global_macros.begin() + i); 
                i--; ImGui::PopID(); continue; 
            }
            ImGui::SameLine();

            // 編集ボタンを追加
            if (ImGui::Button(u8"編集")) {
                // 現在のデータを入力エリアにコピーする
                new_hotkeys = global_macros[i].hotkeys;
                new_actions = global_macros[i].actions;
                
                // モードを「編集」に切り替える
                is_editing_mode = true;
                editing_macro_index = i;
                
                // 画面の上（作成エリア）へ意識が向くようにスクロールさせることも可能
                ImGui::SetScrollHereY(0.0f); 
            }
            ImGui::SameLine();
            
            std::string hkStr = "";
            for (auto k : global_macros[i].hotkeys) hkStr += VkCodeToString(k) + "+";
            if (!hkStr.empty()) hkStr.pop_back();
            
            if (ImGui::CollapsingHeader((u8"起動: " + hkStr).c_str())) {
                for (const auto& act : global_macros[i].actions) {
                    if (act.type == ACTION_COMBO) {
                        std::string keys = ""; for(auto k: act.comboKeys) keys += VkCodeToString(k) + "+";
                        if(!keys.empty()) keys.pop_back();
                        ImGui::BulletText(u8"キー: %s", keys.c_str());
                    } else if (act.type == ACTION_TEXT) {
                        ImGui::BulletText(u8"文字: %s", act.text.c_str());
                    } else {
                        ImGui::BulletText(u8"待機: %d ms", act.waitMs);
                    }
                }
            }
            ImGui::PopID(); // PushIDに対応するPopID
        }

        ImGui::End();

        // J. 描画終了と画面への反映
        ImGui::Render();

        // --- DX11描画コマンドとSwapChain Presentの処理 ---
        const float clear_color_with_alpha[4] = { 0.1f, 0.1f, 0.1f, 1.0f };
        g_pd3dImmediateContext->OMSetRenderTargets(1, &g_mainRenderTargetView, NULL);
        g_pd3dImmediateContext->ClearRenderTargetView(g_mainRenderTargetView, clear_color_with_alpha);

        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());

        // Swap Chain Present
        g_pSwapChain->Present(1, 0); // V-Sync を有効にする場合は 1 を指定
    }

    // --- 3. 終了処理 ---
    UnHook(); 
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);

    return 0;
}