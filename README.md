# WinHot-Plus (v1.0.0)

Windows向けの軽量・シンプルなショートカットキー（マクロ）作成・実行ツールです。
特定のキー操作に対して、複雑なキーコンボやテキスト入力、待機処理を自由に割り当てることができます。

![Platform: Windows](https://img.shields.io/badge/platform-Windows-blue.svg)
![License: MIT](https://img.shields.io/badge/license-MIT-green.svg)

---

## ⚠️ 注意事項
- **OS環境**: 本ソフトは **Windows専用** です。
- **セキュリティソフト**: キー入力監視（フック）および擬似入力を行う性質上、アンチウイルスソフト（McAfee, Windows Defender等）に誤検知される場合があります。動作の詳細はソースコードをご確認ください。

---

## 🚀 主な機能
- **自由なマクロ設定**: キーコンボ（Ctrl+C等）、テキスト入力、ミリ秒単位の待機を組み合わせ可能。
- **特殊キー対応**: 無変換、変換、ひらがな/カタカナ、各Ctrl/Shiftなどに対応。
- **常駐機能**: ウィンドウを閉じてもタスクトレイに格納され、作業を邪魔しません。
- **一時停止ショートカット**: マクロ自体のON/OFFをいつでも切り替え可能。

---

## 🛠 操作方法

### 基本操作
1. `WinHot-Plus.exe` を実行します。
2. 起動キー（ホットキー）と実行内容を設定し、「この設定で新規追加」を押すと有効になります。
3. 登録済み一覧から編集・削除も可能です。

### 固定ショートカット
| 操作 | 機能 |
| :--- | :--- |
| **Ctrl + F12** | マクロ機能の **一時停止 / 再開** |
| **F12** | アプリの **完全終了**（設定は自動保存されます） |
| **[×] ボタン** | ウィンドウを隠して **タスクトレイへ格納** |

---

## 🏗 ビルド方法（開発者向け）
このプロジェクトは C++ と Dear ImGui (DirectX11) で作成されています。
Visual Studio の開発者コマンドプロンプト等で、以下のコマンドを使用してビルド可能です。

```cmd
cl.exe /nologo /O2 /MT /W3 /utf-8 /DUNICODE /D_UNICODE main.cpp imgui*.cpp backends\imgui_impl_win32.cpp backends\imgui_impl_dx11.cpp /I. /I.\backends /link /SUBSYSTEM:WINDOWS /OUT:WinHot-Plus.exe d3d11.lib d3dcompiler.lib shell32.lib user32.lib gdi32.lib
