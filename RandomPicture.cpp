#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <shlobj.h>
#include <string>
#include <vector>
#include <random>
#include <filesystem>
#include <gdiplus.h>
#include <sstream>
#include <shellapi.h>
#include <algorithm>
#include <shobjidl.h> 
#include <shlwapi.h>

#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "gdiplus.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace fs = std::filesystem;

// Structure pour gerer l'etat de l'application
struct AppState {
    std::wstring currentImage;
    std::vector<std::wstring> imageFiles;
    std::vector<std::wstring> history;
    size_t historyIndex = 0;
    std::wstring currentFolder;
    ULONG_PTR gdiplusToken;
    bool gdiplusInitialized = false;
    POINT dragStartPos;
    bool isDragging = false;
    bool englishLanguage = false;
    bool showHistory = false;
};

// Implementation de IDropTarget pour recevoir les fichiers
class DropTarget : public IDropTarget {
public:
    DropTarget(HWND hwnd, AppState* pState) : m_hwnd(hwnd), m_pState(pState), m_refCount(1) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IDropTarget || riid == IID_IUnknown) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override { return ++m_refCount; }
    STDMETHODIMP_(ULONG) Release() override {
        if (--m_refCount == 0) {
            delete this;
            return 0;
        }
        return m_refCount;
    }

    // IDropTarget
    STDMETHODIMP DragEnter(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }

    STDMETHODIMP DragOver(DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }

    STDMETHODIMP DragLeave() override { return S_OK; }

    STDMETHODIMP Drop(IDataObject* pDataObj, DWORD grfKeyState, POINTL pt, DWORD* pdwEffect) override {
        FORMATETC fmt = { CF_HDROP, NULL, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
        STGMEDIUM stg = { TYMED_HGLOBAL };

        if (SUCCEEDED(pDataObj->GetData(&fmt, &stg))) {
            HDROP hDrop = (HDROP)stg.hGlobal;
            UINT fileCount = DragQueryFile(hDrop, 0xFFFFFFFF, NULL, 0);

            for (UINT i = 0; i < fileCount; ++i) {
                wchar_t filePath[MAX_PATH];
                if (DragQueryFile(hDrop, i, filePath, MAX_PATH)) {
                    std::wstring ext = fs::path(filePath).extension().wstring();
                    std::transform(ext.begin(), ext.end(), ext.begin(), towlower);

                    if (ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".bmp") {
                        m_pState->currentImage = filePath;
                        m_pState->history.push_back(filePath);
                        m_pState->historyIndex = m_pState->history.size() - 1;
                        InvalidateRect(m_hwnd, NULL, TRUE);
                        break; // on prend la première image valide
                    }
                }
            }

            ReleaseStgMedium(&stg);
        }

        *pdwEffect = DROPEFFECT_COPY;
        return S_OK;
    }

private:
    HWND m_hwnd;
    AppState* m_pState;
    LONG m_refCount;
};

// Implementation de IDropSource pour envoyer des fichiers
class DropSource : public IDropSource {
private:
    LONG m_refCount;

public:
    DropSource() : m_refCount(1) {}

    // IUnknown
    STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
        if (riid == IID_IDropSource || riid == IID_IUnknown) {
            *ppv = this;
            AddRef();
            return S_OK;
        }
        *ppv = NULL;
        return E_NOINTERFACE;
    }

    STDMETHODIMP_(ULONG) AddRef() override {
        return InterlockedIncrement(&m_refCount);
    }

    STDMETHODIMP_(ULONG) Release() override {
        ULONG count = InterlockedDecrement(&m_refCount);
        if (count == 0) {
            delete this;
        }
        return count;
    }

    // IDropSource
    STDMETHODIMP QueryContinueDrag(BOOL fEscapePressed, DWORD grfKeyState) override {
        if (fEscapePressed)
            return DRAGDROP_S_CANCEL;
        if (!(grfKeyState & MK_LBUTTON))
            return DRAGDROP_S_DROP;
        return S_OK;
    }

    STDMETHODIMP GiveFeedback(DWORD dwEffect) override {
        return DRAGDROP_S_USEDEFAULTCURSORS;
    }
};

bool InitializeGDIplus(AppState& state) {
    if (!state.gdiplusInitialized) {
        Gdiplus::GdiplusStartupInput gdiplusStartupInput;
        if (Gdiplus::GdiplusStartup(&state.gdiplusToken, &gdiplusStartupInput, NULL) == Gdiplus::Ok) {
            state.gdiplusInitialized = true;
        }
    }
    return state.gdiplusInitialized;
}

std::wstring SelectFolder(HWND hwnd) {
    IFileDialog* pfd;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&pfd)))) {
        DWORD dwOptions;
        if (SUCCEEDED(pfd->GetOptions(&dwOptions))) {
            pfd->SetOptions(dwOptions | FOS_PICKFOLDERS);
        }

        if (SUCCEEDED(pfd->Show(hwnd))) {
            IShellItem* psi;
            if (SUCCEEDED(pfd->GetResult(&psi))) {
                PWSTR pszPath;
                if (SUCCEEDED(psi->GetDisplayName(SIGDN_FILESYSPATH, &pszPath))) {
                    std::wstring path(pszPath);
                    CoTaskMemFree(pszPath);
                    psi->Release();
                    pfd->Release();
                    return path;
                }
                psi->Release();
            }
        }
        pfd->Release();
    }
    return L"";
}

void ScanDirectoryRecursive(const std::wstring& directory, std::vector<std::wstring>& images) {
    try {
        for (const auto& entry : fs::recursive_directory_iterator(directory)) {
            if (entry.is_regular_file()) {
                std::wstring ext = entry.path().extension().wstring();
                std::transform(ext.begin(), ext.end(), ext.begin(), towlower);

                if (ext == L".jpg" || ext == L".jpeg" || ext == L".png" || ext == L".bmp") {
                    images.push_back(entry.path().wstring());
                }
            }
        }
        std::sort(images.begin(), images.end());
    }
    catch (const std::exception& e) {
        std::wstringstream ss;
        ss << L"Erreur : " << e.what();
        OutputDebugStringW(ss.str().c_str());
    }
}

std::vector<std::wstring> GetImageFiles(const std::wstring& folder) {
    std::vector<std::wstring> images;
    ScanDirectoryRecursive(folder, images);
    return images;
}

std::wstring GetRandomImage(const std::vector<std::wstring>& images) {
    if (images.empty()) return L"";
    std::random_device rd;
    std::mt19937 gen(rd());
    std::uniform_int_distribution<size_t> distrib(0, images.size() - 1);
    return images[distrib(gen)];
}

void DisplayImage(HWND hwnd, const std::wstring& imagePath, AppState& state) {
    if (!InitializeGDIplus(state)) return;

    Gdiplus::Image image(imagePath.c_str());
    if (image.GetLastStatus() != Gdiplus::Ok || image.GetWidth() == 0 || image.GetHeight() == 0) {
        MessageBoxW(hwnd,
            state.englishLanguage ? L"Unable to load image or invalid image" : L"Impossible de charger l'image ou image invalide",
            state.englishLanguage ? L"Error" : L"Erreur",
            MB_ICONERROR);
        return;
    }

    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);

    RECT clientRect;
    GetClientRect(hwnd, &clientRect);

    float imageRatio = (float)image.GetWidth() / image.GetHeight();
    float clientRatio = (float)clientRect.right / clientRect.bottom;

    int drawWidth, drawHeight;
    if (clientRatio > imageRatio) {
        drawHeight = clientRect.bottom;
        drawWidth = (int)(drawHeight * imageRatio);
    }
    else {
        drawWidth = clientRect.right;
        drawHeight = (int)(drawWidth / imageRatio);
    }

    int x = (clientRect.right - drawWidth) / 2;
    int y = (clientRect.bottom - drawHeight) / 2;

    Gdiplus::Graphics graphics(hdc);
    graphics.DrawImage(&image, x, y, drawWidth, drawHeight);

    // Afficher le nom du fichier
    std::wstring fileName = fs::path(imagePath).filename().wstring();
    SetBkMode(hdc, TRANSPARENT);
    SetTextColor(hdc, RGB(0, 0, 0));
    TextOutW(hdc, 10, 80, fileName.c_str(), (int)fileName.length());

    // affiocher l'historique
    if (state.showHistory)
    {
        EndPaint(hwnd, &ps);
        return;
    }
    int pos = 100;
    std::wstring message = state.englishLanguage ? L"History :" : L"Historique";
    TextOutW(hdc, 10, pos, message.c_str(), (int)message.length());
    for (std::wstring fileName : state.history)
    {
        pos += 15;
        TextOutW(hdc, 10, pos, fileName.c_str(), (int)fileName.length());
    }
    EndPaint(hwnd, &ps);
}

void LoadNewRandomImage(HWND hwnd, AppState& state) {
    if (!state.currentFolder.empty() && !state.imageFiles.empty()) {
        std::wstring newImage = GetRandomImage(state.imageFiles);
        if (!newImage.empty()) {
            state.currentImage = newImage;
            state.history.push_back(newImage);
            state.historyIndex = state.history.size() - 1;
            InvalidateRect(hwnd, NULL, TRUE);
        }
    }
    else {
        MessageBoxW(hwnd,
            state.englishLanguage ? L"No images found in the folder." : L"Aucune image trouvee dans le dossier.",
            state.englishLanguage ? L"Information" : L"Information",
            MB_ICONINFORMATION);
    }
}

void OpenFileLocation(const std::wstring& filePath) {
    if (!filePath.empty()) {
        PIDLIST_ABSOLUTE pidl = ILCreateFromPathW(filePath.c_str());
        if (pidl) {
            SHOpenFolderAndSelectItems(pidl, 0, NULL, 0);
            ILFree(pidl);
        }
    }
}

HRESULT CreateDropDataObject(const std::wstring& filePath, IDataObject** ppDataObject) {
    *ppDataObject = nullptr;

    if (!PathFileExistsW(filePath.c_str())) {
        return E_INVALIDARG;
    }

    size_t pathLen = filePath.length();
    size_t bufferSize = sizeof(DROPFILES) + ((pathLen + 2) * sizeof(WCHAR));

    HGLOBAL hMem = GlobalAlloc(GHND | GMEM_SHARE, bufferSize);
    if (!hMem) return E_OUTOFMEMORY;

    DROPFILES* pDropFiles = (DROPFILES*)GlobalLock(hMem);
    pDropFiles->pFiles = sizeof(DROPFILES);
    pDropFiles->fWide = TRUE;

    WCHAR* pFilePath = (WCHAR*)((BYTE*)pDropFiles + sizeof(DROPFILES));
    wcscpy_s(pFilePath, pathLen + 1, filePath.c_str());
    pFilePath[pathLen + 1] = L'\0';

    GlobalUnlock(hMem);

    class DropDataObject : public IDataObject {
    private:
        LONG m_refCount;
        HGLOBAL m_hMem;

    public:
        DropDataObject(HGLOBAL hMem) : m_refCount(1), m_hMem(hMem) {}

        ~DropDataObject() {
            if (m_hMem) {
                GlobalFree(m_hMem);
            }
        }

        STDMETHODIMP QueryInterface(REFIID riid, void** ppv) override {
            if (riid == IID_IDataObject || riid == IID_IUnknown) {
                *ppv = this;
                AddRef();
                return S_OK;
            }
            *ppv = nullptr;
            return E_NOINTERFACE;
        }

        STDMETHODIMP_(ULONG) AddRef() override {
            return InterlockedIncrement(&m_refCount);
        }

        STDMETHODIMP_(ULONG) Release() override {
            ULONG count = InterlockedDecrement(&m_refCount);
            if (count == 0) {
                delete this;
            }
            return count;
        }

        STDMETHODIMP GetData(FORMATETC* pFormat, STGMEDIUM* pMedium) override {
            if (!pFormat || !pMedium) return E_INVALIDARG;
            if (pFormat->cfFormat != CF_HDROP || !(pFormat->tymed & TYMED_HGLOBAL)) {
                return DV_E_FORMATETC;
            }

            pMedium->tymed = TYMED_HGLOBAL;
            pMedium->hGlobal = OleDuplicateData(m_hMem, CF_HDROP, 0);
            pMedium->pUnkForRelease = nullptr;
            return pMedium->hGlobal ? S_OK : E_OUTOFMEMORY;
        }

        STDMETHODIMP GetDataHere(FORMATETC* pFormat, STGMEDIUM* pMedium) override {
            return E_NOTIMPL;
        }

        STDMETHODIMP QueryGetData(FORMATETC* pFormat) override {
            if (!pFormat) return E_INVALIDARG;
            return (pFormat->cfFormat == CF_HDROP && (pFormat->tymed & TYMED_HGLOBAL)) ? S_OK : DV_E_FORMATETC;
        }

        STDMETHODIMP GetCanonicalFormatEtc(FORMATETC* pFormatIn, FORMATETC* pFormatOut) override {
            return DATA_S_SAMEFORMATETC;
        }

        STDMETHODIMP SetData(FORMATETC* pFormat, STGMEDIUM* pMedium, BOOL fRelease) override {
            return E_NOTIMPL;
        }

        STDMETHODIMP EnumFormatEtc(DWORD dwDirection, IEnumFORMATETC** ppEnum) override {
            if (dwDirection == DATADIR_GET) {
                FORMATETC fmtetc = { CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL };
                return SHCreateStdEnumFmtEtc(1, &fmtetc, ppEnum);
            }
            return E_NOTIMPL;
        }

        STDMETHODIMP DAdvise(FORMATETC* pFormat, DWORD advf, IAdviseSink* pAdvSink, DWORD* pdwConnection) override {
            return OLE_E_ADVISENOTSUPPORTED;
        }

        STDMETHODIMP DUnadvise(DWORD dwConnection) override {
            return OLE_E_ADVISENOTSUPPORTED;
        }

        STDMETHODIMP EnumDAdvise(IEnumSTATDATA** ppEnumAdvise) override {
            return OLE_E_ADVISENOTSUPPORTED;
        }
    };
    *ppDataObject = new DropDataObject(hMem);
    return S_OK;
}

void StartDragOperation(HWND hwnd, AppState& state) {
    if (state.currentImage.empty()) return;

    OleInitialize(nullptr);

    IDataObject* pDataObject = nullptr;
    HRESULT hr = CreateDropDataObject(state.currentImage, &pDataObject);

    if (SUCCEEDED(hr)) {
        DropSource* pDropSource = new DropSource();
        DWORD dwEffect;
        DoDragDrop(pDataObject, pDropSource, DROPEFFECT_COPY, &dwEffect);
        pDropSource->Release();
        pDataObject->Release();
    }

    OleUninitialize();
}

void NavigateHistory(HWND hwnd, AppState& state, bool forward) {
    if (state.history.empty()) return;

    if (forward && state.historyIndex < state.history.size() - 1) {
        state.historyIndex++;
        state.currentImage = state.history[state.historyIndex];
        InvalidateRect(hwnd, NULL, TRUE);
    }
    else if (!forward && state.historyIndex > 0) {
        state.historyIndex--;
        state.currentImage = state.history[state.historyIndex];
        InvalidateRect(hwnd, NULL, TRUE);
    }
}

void ToggleLanguage(AppState& state) {
    state.englishLanguage = !state.englishLanguage;
}

void UpdateUI(HWND hwnd, AppState& state) {
    SetWindowTextW(hwnd, state.englishLanguage ? L"Random Image Viewer" : L"Visionneuse d'images aleatoires");
    SetDlgItemTextW(hwnd, 1, state.englishLanguage ?
        L"Select folder (R: Pick a random picture)" : L"Choisir un dossier (R: nouvelle image)");
    SetDlgItemTextW(hwnd, 3, state.englishLanguage ?
        L"History ON/OFF" : L"Historique ON/OFF");
    InvalidateRect(hwnd, NULL, TRUE);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam) {
    static AppState state;
    static DropTarget* pDropTarget = nullptr;

    switch (uMsg) {
    case WM_CREATE: {
        CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        pDropTarget = new DropTarget(hwnd, &state);
        RegisterDragDrop(hwnd, pDropTarget);

        // Bouton pour selectionner le dossier
        CreateWindowW(
            L"BUTTON", L"Choisir un dossier (R: nouvelle image)",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_DEFPUSHBUTTON,
            10, 10, 250, 30,
            hwnd, (HMENU)1, ((LPCREATESTRUCT)lParam)->hInstance, NULL
        );

        // Bouton pour changer la langue
        CreateWindowW(
            L"BUTTON", L"EN/FR",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            270, 10, 60, 30,
            hwnd, (HMENU)2, ((LPCREATESTRUCT)lParam)->hInstance, NULL
        );
        // Bouton pour activer ou desactiver l'historique
        CreateWindowW(
            L"BUTTON", L"Historique ON/OFF",
            WS_TABSTOP | WS_VISIBLE | WS_CHILD | BS_PUSHBUTTON,
            10, 50, 250, 30,
            hwnd, (HMENU)3, ((LPCREATESTRUCT)lParam)->hInstance, NULL
        );
        break;
    }
    case WM_COMMAND:
        if (LOWORD(wParam) == 1) {
            std::wstring folder = SelectFolder(hwnd);
            if (!folder.empty()) {
                state.currentFolder = folder;
                state.imageFiles = GetImageFiles(folder);
                LoadNewRandomImage(hwnd, state);
            }
            SetFocus(hwnd);
        }
        else if (LOWORD(wParam) == 2) {
            ToggleLanguage(state);
            UpdateUI(hwnd, state);
            SetFocus(hwnd);
        }
        else if (LOWORD(wParam) == 3) {
            state.showHistory = !state.showHistory;
            UpdateUI(hwnd, state);
            SetFocus(hwnd);
        }
        break;

    case WM_KEYDOWN:
        if (wParam == 'R' || wParam == 'r') {
            LoadNewRandomImage(hwnd, state);
        }
        else if (wParam == VK_LEFT) {
            NavigateHistory(hwnd, state, false);
        }
        else if (wParam == VK_RIGHT) {
            NavigateHistory(hwnd, state, true);
        }
        break;

    case WM_LBUTTONDOWN:
        state.dragStartPos.x = GET_X_LPARAM(lParam);
        state.dragStartPos.y = GET_Y_LPARAM(lParam);
        state.isDragging = true;
        SetCapture(hwnd);
        break;

    case WM_LBUTTONDBLCLK:
        OpenFileLocation(state.currentImage);
        break;

    case WM_MOUSEMOVE:
        if (state.isDragging) {
            int dx = abs(GET_X_LPARAM(lParam) - state.dragStartPos.x);
            int dy = abs(GET_Y_LPARAM(lParam) - state.dragStartPos.y);
            if (dx > 5 || dy > 5) {
                state.isDragging = false;
                ReleaseCapture();
                StartDragOperation(hwnd, state);
            }
        }
        break;

    case WM_LBUTTONUP:
        state.isDragging = false;
        ReleaseCapture();
        break;

    case WM_PAINT:
        if (!state.currentImage.empty()) {
            DisplayImage(hwnd, state.currentImage, state);
        }
        else {
            PAINTSTRUCT ps;
            HDC hdc = BeginPaint(hwnd, &ps);
            FillRect(hdc, &ps.rcPaint, (HBRUSH)(COLOR_WINDOW + 1));

            std::wstring message = state.englishLanguage ?
                L"Drag and drop an image here\nor click the button" :
                L"Glissez-deposez une image ici\nou cliquez sur le bouton";
            DrawTextW(hdc, message.c_str(), -1, &ps.rcPaint,
                DT_CENTER | DT_VCENTER | DT_WORDBREAK);

            EndPaint(hwnd, &ps);
        }
        break;

    case WM_SIZE:
        InvalidateRect(hwnd, NULL, TRUE);
        break;

    case WM_DESTROY:
        RevokeDragDrop(hwnd);
        if (pDropTarget) {
            RevokeDragDrop(hwnd);
            pDropTarget->Release();
            pDropTarget = nullptr;
        }
        CoUninitialize();
        if (state.gdiplusInitialized) {
            Gdiplus::GdiplusShutdown(state.gdiplusToken);
        }
        PostQuitMessage(0);
        break;

    default:
        return DefWindowProc(hwnd, uMsg, wParam, lParam);
    }
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow) {
    UNREFERENCED_PARAMETER(hPrevInstance);
    UNREFERENCED_PARAMETER(lpCmdLine);

    OleInitialize(NULL);

    if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE))) {
        return 1;
    }

    Gdiplus::GdiplusStartupInput gdiplusStartupInput;
    ULONG_PTR gdiplusToken;
    Gdiplus::GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

    const wchar_t CLASS_NAME[] = L"ImageRandomViewerClass";

    WNDCLASSW wc = {};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = CLASS_NAME;
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.style = CS_DBLCLKS;

    if (!RegisterClassW(&wc)) {
        MessageBoxW(NULL, L"Echec de l'enregistrement de la classe de fenêtre", L"Erreur", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    HWND hwnd = CreateWindowExW(
        0,
        CLASS_NAME,
        L"Selecteur d'Image (Glissez-deposez)",
        WS_OVERLAPPEDWINDOW,
        CW_USEDEFAULT, CW_USEDEFAULT, 800, 600,
        NULL, NULL, hInstance, NULL
    );

    if (!hwnd) {
        MessageBoxW(NULL, L"Echec de la creation de la fenêtre", L"Erreur", MB_ICONERROR);
        CoUninitialize();
        return 1;
    }

    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    MSG msg = {};
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    Gdiplus::GdiplusShutdown(gdiplusToken);
    CoUninitialize();
    OleUninitialize();
    return (int)msg.wParam;
}