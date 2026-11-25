#include "pch.h"
#include "resource.h"
#include "DLSS5SwapperDlg.h"

class CDLSS5MFCApp : public CWinApp
{
public:
    BOOL InitInstance() override
    {
        CWinApp::InitInstance();
        AfxEnableControlContainer();
        CDlss5SwapperDlg dlg;
        m_pMainWnd = &dlg;
        dlg.DoModal();
        return FALSE;
    }
};

CDLSS5MFCApp theApp;
