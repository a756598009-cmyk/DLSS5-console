#pragma once
#include "resource.h"
#include <afxwin.h>
#include <ddraw.h>
#include <gdiplus.h>
#include <string>
#include <vector>
#include <array>
#include <map>
#pragma comment(lib, "gdiplus.lib")

struct CaptureTargetChoice {
    std::wstring label;
    std::wstring processName;
    DWORD pid = 0;
};

struct NrUiSetting {
    std::wstring section, key, label, kind, group;
    int def=0, min=0, max=1, choices=0;
    int scale=1; // INI decimal scale: UI integer / scale = stored value
    std::wstring unit;
    bool advanced=false, pending=false, bridge=false, needsEncoding=false, needsFast=false;
};

class CDlss5SwapperDlg : public CDialog
{
public:
    explicit CDlss5SwapperDlg(CWnd* pParent = nullptr);
    virtual ~CDlss5SwapperDlg();

protected:
    virtual BOOL OnInitDialog();
    afx_msg void OnPaint();
    afx_msg BOOL OnEraseBkgnd(CDC* pDC);
    afx_msg void OnLButtonDown(UINT nFlags, CPoint point);
    afx_msg void OnLButtonUp(UINT nFlags, CPoint point);
    afx_msg void OnMouseMove(UINT nFlags, CPoint point);
    afx_msg BOOL OnMouseWheel(UINT nFlags, short zDelta, CPoint pt);
    afx_msg void OnKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
    afx_msg void OnSysKeyDown(UINT nChar, UINT nRepCnt, UINT nFlags);
    afx_msg void OnChar(UINT nChar, UINT nRepCnt, UINT nFlags);
    afx_msg void OnSize(UINT nType, int cx, int cy);
    afx_msg void OnSizing(UINT fwSide, LPRECT pRect);
    afx_msg LRESULT OnNcHitTest(CPoint point);
    afx_msg void OnGetMinMaxInfo(MINMAXINFO* lpMMI);
    afx_msg void OnDestroy();
    afx_msg void OnTimer(UINT_PTR nIDEvent);
    afx_msg LRESULT OnHotKey(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnUpdateCheckResult(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnDependencyInstallResult(WPARAM wParam, LPARAM lParam);
    afx_msg LRESULT OnDependencyInstallProgress(WPARAM wParam, LPARAM lParam);
    void StartUpdateCheck(bool silent);
    DECLARE_MESSAGE_MAP()

private:
    ULONG_PTR m_gdiplusToken = 0;
    bool m_draggingSlider = false;
    bool m_draggingWindow = false;
    bool m_draggingScroll = false;
    int m_scrollOffset = 0;
    int m_scrollMax = 0;
    int m_scrollDragStartY = 0;
    int m_scrollStartOffset = 0;
    bool m_captureHotkey = false;
    int m_activeSlider = -1;
    int m_activeHotkey = 0; // 0 none, 1 scale, 2 stop, 3 overlay, 4 bind-bypass text edit
    std::wstring m_hotkeyStatus;
    std::wstring m_bypassBeforeEdit;
    std::array<CRect, 3> m_hotkeyRects{};
    CRect m_bypassRect;
    int m_openCombo = -1;
    int m_tab = 0; // 0: magnify/FG, 1: neural rendering, 2: overlay
    bool m_moreNr = true;
    std::vector<NrUiSetting> m_nrSettings;
    std::vector<int> m_nrValues;
    std::vector<CRect> m_nrHitRects;
    std::vector<CRect> m_nrSliderRects;
    std::vector<std::pair<CRect,std::wstring>> m_nrGroupRects;
    std::map<std::wstring,bool> m_nrGroupExpanded;
    std::vector<bool> m_nrSupported;
    int m_activeNrSlider = -1;
    CRect m_nrMoreRect;
    int m_nrOpenChoice = -1;
    CRect m_nrChoicePopupRect;
    bool m_nrOpenRuntime = false;
    bool m_nrRuntimeChoiceSupported = false;
    bool m_morePicture = false;
    bool m_moreOverlay = false;
    bool m_settingsOpen = false;
    CRect m_settingsButtonRect;

    // Settings mirrored from the NRLive page and persisted to scaleconfig.ini.
    int m_upscaler = 0;      // FSR4, FSR3.1, Native
    bool m_upscalerSupported = false;
    int m_sharpness = 65;
    bool m_frameGeneration = true;
    bool m_frameLimiter = false;
    int m_fpsLimit = 60;
    int m_limiterMethod = 0;

    bool m_nrEnabled = true;
    bool m_originalModel = false;
    int m_nrStyle = 0;
    int m_nrStrength = 100;
    int m_nrPasses = 1;
    int m_nrLocalTone = 100;
    int m_nrLocalStructure = 100;
    int m_nrSkinStructure = 100;
    bool m_nrAutoMask = false;
    bool m_nrTemporal = true;
    int m_nrHistory = 100;
    bool m_nrStyleGrade = true;

    bool m_overlayEnabled = true; // launcher/profile switch: -nooverlay
    bool m_hudCounterEnabled = true; // [FPS].enabled
    std::vector<int> m_hudTileMap; // -1 = FPS.enabled, otherwise index in m_hudItems
    int m_hudPreset = 3;
    int m_hudLayout = 0;
    int m_hudPosition = 0;
    int m_hudScale = 100;
    int m_hudFontSize = 24;
    int m_hudBackgroundAlpha = 80;
    bool m_hudBackground = true;
    bool m_hudTextOutline = true;
    bool m_hudNew = false; // FPS.preset support => new HUD; otherwise legacy HUD
    std::array<bool, 17> m_hudItems = { true,true,true,true,true,true,true,true,true,true,true,true,false,false,false,false,false };

    int m_captureMode = 0; // WGC / DXGI
    int m_motionMode = 0;  // FastMV default / AMD optical flow
    int m_targetMode = 0;  // 0=foreground picker, 1=selected window PID, 2=selected process name
    int m_delay = 5;
    std::wstring m_targetText;
    std::wstring m_targetLabel;
    std::vector<CaptureTargetChoice> m_captureTargets;
    bool m_targetPopup = false;
    bool m_targetWaitNotice = false;
    int m_targetPage = 0;
    CRect m_targetItemRect;
    bool m_runAsAdmin = true;
    std::wstring m_scaleKey = L"ctrl+alt+s";
    std::wstring m_stopKey = L"ctrl+shift+a";
    std::wstring m_overlayKey = L"ctrl+home";
    std::wstring m_bypassKeys = L"home,insert,end,pageup,pagedown";
    std::wstring m_status = L"已就绪 · 倒计时 5 秒 · 点击启动后切换到游戏";
    HANDLE m_nrLiveProcess = nullptr;
    std::wstring m_nrLiveExe;
    std::wstring m_configDir;
    std::wstring m_configPath;

    std::array<float, 12> m_slider{};
    std::array<CRect, 12> m_sliderRects{};
    std::array<CRect, 3> m_tabRects{};
    std::array<CRect, 9> m_comboRects{};
    std::array<std::vector<std::wstring>, 9> m_comboOptions{};
    std::array<int, 9> m_comboSelections{};
    CRect m_closeRect, m_minimizeRect, m_maximizeRect, m_bilibiliRect, m_qqGroupRect;
    CRect m_menuButtonRect;
    std::array<CRect, 4> m_menuItemRects{};
    bool m_menuOpen = false;
    bool m_updateChecking = false;
    bool m_installingDependencies = false;
    int m_dependencyProgress = -1;
    int m_dependencyStage = 0;
    CRect m_installDependencyRect;
    CRect m_localCacheRect;
    CRect m_saveRect, m_startRect, m_headerDragRect;
    CRect m_startToggleRect;
    bool m_startSwitchOn = false;
    bool m_startCountdownActive = false;
    ULONGLONG m_startCountdownEnd = 0;
    CRect m_scrollThumb;

    void DrawInterface(Gdiplus::Graphics& g, int cx, int cy);
    void DrawTabs(Gdiplus::Graphics& g);
    void DrawPictureTab(Gdiplus::Graphics& g);
    void DrawNrTab(Gdiplus::Graphics& g);
    void InitNrSettings();
    void LoadNrSettings();
    void SaveNrSettings();
    bool IsNrSettingSupported(const NrUiSetting& s) const;
    void UpdateNrSliderAt(CPoint p);
    void DrawOverlayTab(Gdiplus::Graphics& g);
    void DrawSlider(Gdiplus::Graphics& g, int id, float x, float y, float width,
                    const wchar_t* label, int value, int minValue, int maxValue, const wchar_t* suffix = L"%");
    void DrawToggle(Gdiplus::Graphics& g, float x, float y, const wchar_t* label, bool value);
    void DrawCombo(Gdiplus::Graphics& g, float x, float y, float width, const wchar_t* label,
                   const std::vector<std::wstring>& options, int selected, int comboId);
    void DrawOpenComboPopup(Gdiplus::Graphics& g);
    void DrawNrChoicePopup(Gdiplus::Graphics& g);
    bool HandleNrChoicePopupClick(CPoint p);
    bool HandleComboPopupClick(CPoint p);
    void OpenCombo(int comboId);
    void InvalidateDesignRect(const CRect& designRect);
    void InvalidateComboRegion(int comboId, bool includePopup = true);
    void DrawButton(Gdiplus::Graphics& g, const Gdiplus::RectF& r, const wchar_t* label, bool primary = false);
    CPoint ToDesignPoint(CPoint p, int cx, int cy) const;
    void UpdateSliderAt(CPoint p);
    void LoadSettings();
    void SaveSettings();
    void SaveProfile();
    void FindNrLive();
    std::wstring GetAppDirectory() const;
    std::wstring GetCacheDirectory() const;
    bool HasLocalNrLive() const;
    void InstallDependencies();
    void StartNrLive();
    void RefreshCaptureTargets();
    bool BrowseCaptureExecutable();
    void DrawCaptureTargetPopup(Gdiplus::Graphics& g);
    bool HandleCaptureTargetClick(CPoint p);
    void RegisterHotkeys();
    bool ValidateHotkeySettings(std::wstring& reason) const;
    void CommitHotkeyCapture(const std::wstring& value);
    void SetStatus(const std::wstring& text);
    void ToggleAt(CPoint p);
    void HitTest(CPoint p);
    int GetCurrentContentBottom() const;
    void ApplyRoundedWindowRegion();
    void UpdateScrollRange();
    void DrawScrollBar(Gdiplus::Graphics& g);
    std::wstring IniRead(const wchar_t* section, const wchar_t* key, const wchar_t* fallback);
    void IniWrite(const wchar_t* section, const wchar_t* key, const std::wstring& value);
};
