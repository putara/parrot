#define _CRT_SECURE_NO_WARNINGS
#define _CRT_NONSTDC_NO_WARNINGS
#define _CRT_NON_CONFORMING_SWPRINTFS
#define _CRTDBG_MAP_ALLOC
#define UNICODE
#define _UNICODE

#include <stdio.h>
#include <stdlib.h>
#include <malloc.h>
#include <math.h>
#include <stdarg.h>
#include <stdint.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlwapi.h>
#include <wincrypt.h>
#include <wincodec.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <tchar.h>
#include <crtdbg.h>
#include <intrin.h>

#include "res.rc"

EXTERN_C IMAGE_DOS_HEADER __ImageBase;
#define HINST_THISCOMPONENT   reinterpret_cast<HINSTANCE>(&__ImageBase)

#pragma warning(disable: 4100)

struct zero_init_t {} zero_init;

inline void* operator new(size_t cb, const zero_init_t&)
{
  void* p = ::operator new(cb);
  if (p != NULL) {
    ::memset(p, 0, cb);
  }
  return p;
}

inline void* operator new[](size_t cb, const zero_init_t&)
{
  void* p = ::operator new[](cb);
  if (p != NULL) {
    ::memset(p, 0, cb);
  }
  return p;
}

void Print(__format_string LPCWSTR format, ...)
{
  va_list argPtr;
  va_start(argPtr, format);
  WCHAR text[1024];
  ::_vsnwprintf_s(text, ARRAYSIZE(text), _TRUNCATE, format, argPtr);
  if (::IsDebuggerPresent()) {
    ::OutputDebugStringW(text);
  } else {
    static HANDLE hstdout;
    if (hstdout == NULL) {
      if (::AttachConsole(ATTACH_PARENT_PROCESS) == FALSE && ::GetLastError() == ERROR_INVALID_HANDLE) {
        ::AllocConsole();
      }
      hstdout = ::GetStdHandle(STD_OUTPUT_HANDLE);
    }
    if (hstdout != INVALID_HANDLE_VALUE) {
      ::WriteConsoleW(hstdout, text, static_cast<DWORD>(::wcslen(text)), NULL, NULL);
    }
  }
  va_end(argPtr);
}

class CpuMonitor
{
private:
  HQUERY hq_;
  HCOUNTER* hcnts_;
  DWORD cores_;
  DWORD threads_;

  static bool GetNumberOfProcessors(__out DWORD* coreCount, __out DWORD* threadCount)
  {
    DWORD cb = 0;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION buffer = NULL;

    do {
      if (::GetLogicalProcessorInformation(buffer, &cb)) {
        break;
      }
      DWORD error = ::GetLastError();
      ::free(buffer);
      buffer = NULL;
      if (error == ERROR_INSUFFICIENT_BUFFER) {
        buffer = static_cast<PSYSTEM_LOGICAL_PROCESSOR_INFORMATION>(::malloc(cb));
      }
    } while (buffer != NULL);

    if (buffer == NULL) {
      return false;
    }

    DWORD cores = 0;
    DWORD threads = 0;

    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION ptr = buffer;
    PSYSTEM_LOGICAL_PROCESSOR_INFORMATION last = ptr + (cb / sizeof(SYSTEM_LOGICAL_PROCESSOR_INFORMATION));

    for (; ptr < last; ptr++) {
      if (ptr->Relationship == RelationProcessorCore) {
        cores++;
        const DWORD bits = sizeof(void*) * 8;
        ULONG_PTR bitTest = static_cast<ULONG_PTR>(1) << (bits - 1);
        DWORD count = 0;
        for (DWORD i = 0; i < bits; i++, bitTest >>= 1) {
          count += (ptr->ProcessorMask & bitTest) ? 1 : 0;
        }
        threads += count;
      }
    }

    ::free(buffer);
    *coreCount = cores;
    *threadCount = threads;
    return true;
  }

public:
  CpuMonitor()
  {
    this->hq_ = NULL;
    this->hcnts_ = NULL;
    this->cores_ = 0;
    this->threads_ = 0;
  }
  ~CpuMonitor()
  {
    delete[] this->hcnts_;
    if (this->hq_ != NULL) {
      ::PdhCloseQuery(this->hq_);
    }
  }
  bool Init()
  {
    if (GetNumberOfProcessors(&this->cores_, &this->threads_) == false) {
      return false;
    }
    this->hcnts_ = new (zero_init) HCOUNTER[this->threads_];
    PDH_STATUS ret = ::PdhOpenQueryW(NULL, NULL, &this->hq_);
    if (ret != ERROR_SUCCESS) {
      this->hq_ = NULL;
      return false;
    }
    for (DWORD i = 0; i < this->threads_; i++) {
      WCHAR path[256];
      ::wnsprintfW(path, ARRAYSIZE(path), L"\\Processor(%u)\\%% Processor Time", i);
      ret = ::PdhAddCounterW(this->hq_, path, 0, &this->hcnts_[i]);
      if (ret != ERROR_SUCCESS) {
        this->hcnts_[i] = NULL;
        return false;
      }
    }
    return true;
  }
  bool Collect()
  {
    PDH_STATUS ret = ::PdhCollectQueryData(this->hq_);
    if (ret != ERROR_SUCCESS) {
      return false;
    }
    return true;
  }
  DWORD GetProcessorCount() const
  {
    return this->threads_;
  }
  bool GetUsage(DWORD cpu, LONG* value)
  {
    if (cpu >= this->GetProcessorCount()) {
      return false;
    }
    PDH_FMT_COUNTERVALUE buffer = {};
    PDH_STATUS ret = PdhGetFormattedCounterValue(this->hcnts_[cpu], PDH_FMT_LONG, NULL, &buffer);
    if (ret != ERROR_SUCCESS || buffer.CStatus != ERROR_SUCCESS) {
      return false;
    }
    *value = buffer.longValue;
    return true;
  }
};

class NotifyCpuIcons
{
private:
  class NotifyIconData : public NOTIFYICONDATAW
  {
  public:
    static const UINT MAX_TIPS = RTL_FIELD_SIZE(NOTIFYICONDATAW, szTip) / sizeof(WCHAR);

    NotifyIconData(HWND hwnd)
    {
      ZeroMemory(static_cast<NOTIFYICONDATAW*>(this), sizeof(NOTIFYICONDATAW));
      this->cbSize = sizeof(NOTIFYICONDATAW);
      this->hWnd = hwnd;
    }
    NotifyIconData& HiddenIconId(UINT icon)
    {
      this->uID = ~icon;
      return *this;
    }
    NotifyIconData& SharedIconId(const GUID& guid, UINT cpu)
    {
      WCHAR str[32];
      ::wnsprintfW(str, ARRAYSIZE(str), L"%u", cpu);
      if (CreateGUIDV5(guid, str, static_cast<DWORD>(::wcslen(str) * sizeof(WCHAR)), &this->guidItem)) {
        this->uFlags |= NIF_GUID;
      } else {
        this->uID = cpu + 1;
      }
      return *this;
    }
    NotifyIconData& HiddenIcon(UINT icon, HICON hicon)
    {
      this->HiddenIconId(icon);
      this->uFlags |= NIF_STATE | NIF_ICON;
      this->hIcon = hicon;
      this->dwState = NIS_HIDDEN;
      this->dwStateMask = NIS_HIDDEN | NIS_SHAREDICON;
      return *this;
    }
    NotifyIconData& SharedIcon(const GUID& guid, UINT cpu, HICON hicon)
    {
      this->SharedIconId(guid, cpu);
      this->uFlags |= NIF_STATE | NIF_ICON;
      this->hIcon = hicon;
      this->dwState = NIS_SHAREDICON;
      this->dwStateMask = NIS_HIDDEN | NIS_SHAREDICON;
      return *this;
    }
    NotifyIconData& Message(UINT message)
    {
      this->uFlags |= NIF_MESSAGE;
      this->uCallbackMessage = message;
      return *this;
    }
    NotifyIconData& Tip(const WCHAR (&tip)[MAX_TIPS])
    {
      this->uFlags |= NIF_TIP;
      ::memcpy(this->szTip, tip, sizeof(this->szTip));
      this->szTip[MAX_TIPS - 1] = L'\0';
      return *this;
    }
    bool Add()
    {
      return this->Invoke(NIM_ADD);
    }
    bool Delete()
    {
      return this->Invoke(NIM_DELETE);
    }
    bool Modify()
    {
      return this->Invoke(NIM_MODIFY);
    }
    bool Invoke(DWORD message)
    {
      return ::Shell_NotifyIconW(message, this) != FALSE;
    }
  };

  HWND hwnd_;
  HICON* hicons_;
  UINT* indices_;
  UINT iconCount_;
  UINT cpuCount_;
  GUID guid_;

  static bool CreateGUIDV5(const GUID& nsid, __in_bcount(cbName) const void* name, DWORD cbName, __out GUID* guid)
  {
    if (IsEqualGUID(nsid, GUID_NULL)) {
      ::memset(guid, 0, 16);
      return false;
    }
#define HTONL _byteswap_ulong
#define HTONS _byteswap_ushort
#define NTOHL _byteswap_ulong
#define NTOHS _byteswap_ushort
    // roughly based on RFC 4122.
    GUID net_nsid = nsid;
    net_nsid.Data1 = HTONL(net_nsid.Data1);
    net_nsid.Data2 = HTONS(net_nsid.Data2);
    net_nsid.Data3 = HTONS(net_nsid.Data3);
    BYTE hash[20];
    HCRYPTPROV hctx = NULL;
    HCRYPTHASH hhash = NULL;
    bool success = false;
    if (::CryptAcquireContextW(&hctx, NULL, NULL, PROV_RSA_FULL, 0)) {
      if (::CryptCreateHash(hctx, CALG_SHA1, 0, 0, &hhash)) {
        if (::CryptHashData(hhash, reinterpret_cast<const BYTE*>(&net_nsid), 16, 0)) {
          if (::CryptHashData(hhash, static_cast<const BYTE*>(name), cbName, 0)) {
            DWORD cbHash = 0;
            if (::CryptGetHashParam(hhash, HP_HASHVAL, NULL, &cbHash, NULL) && cbHash == 20) {
              if (::CryptGetHashParam(hhash, HP_HASHVAL, hash, &cbHash, NULL)) {
                success = true;
              }
            }
          }
        }
        ::CryptDestroyHash(hhash);
      }
      ::CryptReleaseContext(hctx, 0);
    }
    if (success) {
      ::memcpy(guid, hash, 16);
      guid->Data1 = NTOHL(guid->Data1);
      guid->Data2 = NTOHS(guid->Data2);
      guid->Data3 = NTOHS(guid->Data3);
      guid->Data3 &= 0x0FFF;
      guid->Data3 |= 0x5000;
      guid->Data4[0] &= 0x3F;
      guid->Data4[0] |= 0x80;
    } else {
      ::memset(guid, 0, 16);
    }
    return success;
#undef HTONL
#undef HTONS
#undef NTOHL
#undef NTOHS
  }

  void Clear()
  {
    this->hwnd_ = NULL;
    this->hicons_ = NULL;
    this->indices_ = NULL;
    this->iconCount_ = 0;
    this->cpuCount_ = 0;
  }

public:
  NotifyCpuIcons()
  {
    this->Clear();
  }

  ~NotifyCpuIcons()
  {
  }

  bool Init(HWND hwnd, UINT message, UINT cpuCount, HIMAGELIST himl)
  {
    // {2513E0A2-3F2F-4a7c-9293-7530A4588648}
    static const GUID rootGuid = { 0x2513e0a2, 0x3f2f, 0x4a7c, { 0x92, 0x93, 0x75, 0x30, 0xa4, 0x58, 0x86, 0x48 } };
    WCHAR path[MAX_PATH];
    DWORD len = ::GetModuleFileNameW(NULL, path, ARRAYSIZE(path));
    CreateGUIDV5(rootGuid, path, len * sizeof(WCHAR), &this->guid_);

    const int count = ::ImageList_GetImageCount(himl);
    if (count <= 0) {
      return false;
    }
    const UINT iconCount = count;
    this->hwnd_ = hwnd;
    this->hicons_ = new HICON[iconCount];
    this->indices_ = new UINT[cpuCount];
    this->iconCount_ = iconCount;
    this->cpuCount_ = cpuCount;
    bool ret = true;
    for (UINT i = 0; i < iconCount && ret; i++) {
      this->hicons_[i] = ::ImageList_ExtractIcon(NULL, himl, i);
      ret = NotifyIconData(hwnd).HiddenIcon(i, this->hicons_[i]).Add();
    }
    for (UINT i = 0; i < cpuCount && ret; i++) {
      this->indices_[i] = i % iconCount;
      WCHAR tip[NotifyIconData::MAX_TIPS];
      ::wnsprintfW(tip, ARRAYSIZE(tip), L"CPU %u", i);
      ret = NotifyIconData(hwnd).SharedIcon(this->guid_, i, this->hicons_[this->indices_[i]]).Message(message).Tip(tip).Add();
    }
    if (ret == false) {
      this->Exit();
    }
    return ret;
  }

  void Exit()
  {
    for (UINT i = this->cpuCount_; i--; ) {
      NotifyIconData(this->hwnd_).SharedIconId(this->guid_, i).Delete();
    }
    for (UINT i = this->iconCount_; i--; ) {
      NotifyIconData(this->hwnd_).HiddenIconId(i).Delete();
      ::DestroyIcon(this->hicons_[i]);
    }
    delete[] this->hicons_;
    delete[] this->indices_;
    this->Clear();
  }

  UINT GetIconCount() const
  {
    return this->iconCount_;
  }

  bool UpdateCpuUsage(UINT cpu, UINT usage)
  {
    if (cpu >= this->cpuCount_) {
      return false;
    }
    WCHAR tip[NotifyIconData::MAX_TIPS];
    ::wnsprintfW(tip, ARRAYSIZE(tip), L"CPU %u: %u%%", cpu, usage);
    return NotifyIconData(this->hwnd_).SharedIconId(this->guid_, cpu).Tip(tip).Modify();
  }

  bool UpdateCpuIcon(UINT cpu, UINT icon)
  {
    if (cpu >= this->cpuCount_) {
      return false;
    }
    icon %= this->iconCount_;
    if (this->indices_[cpu] == icon) {
      return true;
    }
    // Print(L"%u: %u\n", cpu, icon);
    this->indices_[cpu] = icon;
    return NotifyIconData(this->hwnd_).SharedIcon(this->guid_, cpu, this->hicons_[icon]).Modify();
  }

  bool AnimateCpuIcon(UINT cpu)
  {
    if (cpu >= this->cpuCount_) {
      return false;
    }
    this->indices_[cpu] = (this->indices_[cpu] + 1) % this->iconCount_;
    return NotifyIconData(this->hwnd_).SharedIcon(this->guid_, cpu, this->hicons_[this->indices_[cpu]]).Modify();
  }
};

class Cue
{
private:
  double offset_;
  UINT freq_;
  UINT frame_;
  double nextDue_;
  UINT nextFreq_;

  static double GetTime()
  {
    LARGE_INTEGER freq, time;
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&time);
    return static_cast<double>(time.QuadPart) / freq.QuadPart;
  }

  void Update(double time)
  {
    if (this->nextDue_ && time >= this->nextDue_) {
      this->frame_ = this->GetFrame(this->nextDue_);
      this->offset_ = this->nextDue_;
      this->freq_ = this->nextFreq_;
      this->nextDue_ = 0;
    }
  }

  UINT GetFrame(double time)
  {
    return this->frame_ + static_cast<int>(::floor((time - this->offset_) * this->freq_));
  }

public:
  static const UINT DEFAULT = 10;
  Cue()
  {
    this->offset_ = GetTime();
    this->freq_ = DEFAULT;
    this->frame_ = 0;
    this->nextDue_ = 0;
    this->nextFreq_ = 1;
  }
  void Schedule(DWORD fps)
  {
    double time = GetTime();
    this->Update(time);
    this->nextDue_ = this->offset_ + ceil((time - this->offset_) * this->freq_) / this->freq_;
    this->nextFreq_ = fps;
  }
  UINT GetIndex(UINT count)
  {
    double time = GetTime();
    this->Update(time);
    UINT frame = this->GetFrame(time);
    return static_cast<UINT>(frame % count);
  }
};

class FileList
{
private:
  struct Item
  {
    WCHAR path[MAX_PATH];
    Item* next;
  };
  Item* first_;
  Item* current_;

public:
  FileList()
  {
    this->first_ = NULL;
    this->current_ = NULL;
  }
  ~FileList()
  {
    for (Item* item = this->first_; item != NULL; ) {
      Item* next = item->next;
      delete item;
      item = next;
    }
  }
  void Init()
  {
    if (this->first_ != NULL) {
      return;
    }
    WCHAR pat[MAX_PATH];
    ::GetModuleFileNameW(NULL, pat, ARRAYSIZE(pat));
    ::PathRemoveFileSpecW(pat);
    ::PathAppendW(pat, L"gifs\\*.gif");
    WIN32_FIND_DATAW wfd = {};
    HANDLE hf = ::FindFirstFileW(pat, &wfd);
    if (hf == INVALID_HANDLE_VALUE) {
      return;
    }
    ::PathRemoveFileSpecW(pat);
    UINT items = 0;
    do {
      if (++items >= 100) {
        break;
      }
      Item* item = new (zero_init) Item;
      item->next = this->first_;
      this->first_ = item;
      ::wcscpy_s(item->path, ARRAYSIZE(item->path), pat);
      ::PathAppendW(item->path, wfd.cFileName);
    } while (::FindNextFileW(hf, &wfd));
    ::FindClose(hf);
  }
  void AddMenuItems(HMENU hm)
  {
    MENUITEMINFOW mi = { sizeof(mi), MIIM_SUBMENU };
    if (::GetMenuItemInfoW(hm, IDM_GIF, FALSE, &mi) == FALSE || mi.hSubMenu == NULL) {
      return;
    }
    hm = mi.hSubMenu;
    while (::GetMenuItemCount(hm) > 1) {
      if (::GetMenuItemID(hm, 0) == IDM_DEFAULT) {
        ::RemoveMenu(hm, 1, MF_BYPOSITION);
      } else {
        ::RemoveMenu(hm, 0, MF_BYPOSITION);
      }
    }
    UINT id = IDM_FIRST, idCheck = IDM_DEFAULT;
    for (Item* item = this->first_; item != NULL; item = item->next) {
      if (item == this->current_) {
        idCheck = id;
      }
      WCHAR text[MAX_PATH];
      ::wcscpy_s(text, ARRAYSIZE(text), ::PathFindFileNameW(item->path));
      ::PathRemoveExtensionW(text);
      MENUITEMINFOW mi = { sizeof(mi), MIIM_DATA | MIIM_ID | MIIM_TYPE, MFT_STRING };
      mi.wID = id++;
      mi.dwItemData = reinterpret_cast<DWORD_PTR>(item);
      mi.dwTypeData = text;
      ::InsertMenuItemW(hm, 0, TRUE, &mi);
    }
    ::CheckMenuRadioItem(hm, IDM_DEFAULT, IDM_LAST, idCheck, MF_BYCOMMAND);
  }
  HIMAGELIST Load(UINT width, UINT height)
  {
    extern HIMAGELIST ImageList_LoadAnimatedGif(__in_opt LPCWSTR path, UINT iconWidth, UINT iconHeight, __in_bcount_opt(cbData) const void* data = NULL, DWORD cbData = 0);
    if (this->current_ != NULL) {
      return ImageList_LoadAnimatedGif(this->current_->path, width, height);
    }
    HRSRC hrsc = ::FindResourceW(HINST_THISCOMPONENT, MAKEINTRESOURCEW(IDR_MAIN), reinterpret_cast<LPCWSTR>(RT_RCDATA));
    if (hrsc != NULL) {
      DWORD cb = ::SizeofResource(HINST_THISCOMPONENT, hrsc);
      const void* data = static_cast<void*>(::LoadResource(HINST_THISCOMPONENT, hrsc));
      if (data != NULL) {
        return ImageList_LoadAnimatedGif(NULL, width, height, data, cb);
      }
    }
    return NULL;
  }
  bool Select(HMENU hm, UINT id)
  {
    if (id < IDM_DEFAULT || IDM_LAST < id) {
      return false;
    }
    MENUITEMINFOW mi = { sizeof(mi), MIIM_DATA };
    if (::GetMenuItemInfoW(hm, id, FALSE, &mi) == FALSE) {
      return false;
    }
    Item* item = reinterpret_cast<Item*>(mi.dwItemData);
    if (this->current_ == item) {
      return false;
    }
    this->current_ = item;
    return true;
  }
};

template <class I>
class ComPtr
{
private:
  I* ptr;

  class NoAddRefRelease : public I
  {
  private:
      STDMETHOD_(ULONG, AddRef)() = 0;
      STDMETHOD_(ULONG, Release)() = 0;
  };

public:
  ComPtr()
  {
    this->ptr = NULL;
  }
  ~ComPtr()
  {
    this->Release();
  }
  ComPtr(const ComPtr<I>& src)
  {
    this->ptr = NULL;
    operator =(src);
  }
  void Attach(I* ptr)
  {
    this->Release();
    this->ptr = ptr;
  }
  I* Detach()
  {
    I* ptr = this->ptr;
    this->ptr = NULL;
    return ptr;
  }
  void Release()
  {
    if (this->ptr != NULL) {
      this->ptr->Release();
      this->ptr = NULL;
    }
  }
  ComPtr<I>& operator =(I* src)
  {
    if (src != NULL) {
      src->AddRef();
    }
    this->Release();
    this->ptr = src;
    return *this;
  }
  operator I*() const
  {
    return this->ptr;
  }
  I* operator ->() const
  {
    _ASSERT(this->ptr != NULL);
    return static_cast<NoAddRefRelease*>(this->ptr);
  }
  I** operator &()
  {
    _ASSERT(this->ptr == NULL);
    return &this->ptr;
  }
};

struct PropVariant : public PROPVARIANT
{
  PropVariant() throw()
  {
    ::PropVariantInit(this);
  }
  ~PropVariant() throw()
  {
    ::PropVariantClear(this);
  }
  template <typename T>
  static HRESULT ReadUInt8(IWICMetadataQueryReader* reader, LPCWSTR name, __out T* value)
  {
    PropVariant propvar;
    HRESULT hr = reader->GetMetadataByName(name, &propvar);
    if (SUCCEEDED(hr)) {
      hr = (propvar.vt == VT_UI1) ? S_OK : WINCODEC_ERR_BADMETADATAHEADER;
      if (SUCCEEDED(hr)) {
        *value = static_cast<T>(propvar.bVal);
      }
    }
    return hr;
  }
  template <typename T>
  static HRESULT ReadUInt16(IWICMetadataQueryReader* reader, LPCWSTR name, __out T* value)
  {
    PropVariant propvar;
    HRESULT hr = reader->GetMetadataByName(name, &propvar);
    if (SUCCEEDED(hr)) {
      hr = (propvar.vt == VT_UI2) ? S_OK : WINCODEC_ERR_BADMETADATAHEADER;
      if (SUCCEEDED(hr)) {
        *value = static_cast<T>(propvar.uiVal);
      }
    }
    return hr;
  }
  static HRESULT CompareBuffer(IWICMetadataQueryReader* reader, LPCWSTR name, __in_ecount(cb) const void* src1, __in_ecount(cb) const void* src2, __in UINT cb)
  {
    PropVariant propvar;
    HRESULT hr = reader->GetMetadataByName(name, &propvar);
    if (SUCCEEDED(hr)) {
      hr = (propvar.vt == (VT_UI1 | VT_VECTOR) && propvar.caub.cElems == cb) && (::memcmp(propvar.caub.pElems, src1, cb) == 0 || ::memcmp(propvar.caub.pElems, src2, cb) == 0) ? S_OK : WINCODEC_ERR_BADMETADATAHEADER;
    }
    return hr;
  }
};

struct CoInitialiser
{
  HRESULT hr;
  CoInitialiser()
  {
    this->hr = ::CoInitializeEx(NULL, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
  }
  ~CoInitialiser()
  {
    if (SUCCEEDED(this->hr)) {
      ::CoUninitialize();
    }
  }
};


HIMAGELIST ImageList_LoadAnimatedGif(__in_opt LPCWSTR path, UINT iconWidth, UINT iconHeight, __in_bcount_opt(cbData) const void* data = NULL, DWORD cbData = 0)
{
#define FAIL_BAIL if (FAILED(hr)) { return hr; } hr
  class Gif
  {
  private:
    struct Buff
    {
      BYTE* p;
      Buff()
      {
        this->p = NULL;
      }
      ~Buff()
      {
        ::free(this->p);
      }
      HRESULT Alloc(size_t cb)
      {
        this->p = static_cast<BYTE*>(::calloc(cb, 1));
        return this->p != NULL ? S_OK : E_OUTOFMEMORY;
      }
    };
    ComPtr<IWICImagingFactory> factory_;
    ComPtr<IWICBitmap> bitmap_;
    ComPtr<IWICBitmap> saved_;
    WICRect rect_;
    UINT disposal_;
    HRESULT Clear(const WICRect& rect)
    {
      ComPtr<IWICBitmapLock> lock;
      HRESULT hr = S_OK;
      FAIL_BAIL = this->bitmap_->Lock(&rect, WICBitmapLockWrite, &lock);
      UINT stride = 0;
      FAIL_BAIL = lock->GetStride(&stride);
      UINT width = 0, height = 0;
      FAIL_BAIL = lock->GetSize(&width, &height);
      UINT cb = 0;
      UINT offset = 0;
      BYTE* data = NULL;
      FAIL_BAIL = lock->GetDataPointer(&cb, &data);
      FAIL_BAIL = S_OK;
      while (height-- > 0 && offset + width * 4 < cb) {
        ::memset(data + offset, 0, width * 4);
        offset += stride;
      }
      return hr;
    }
    HRESULT Restore()
    {
      if (this->saved_ != NULL) {
        this->bitmap_.Release();
        return this->factory_->CreateBitmapFromSource(this->saved_, WICBitmapCacheOnDemand, &this->bitmap_);
      }
      return S_OK;
    }
    HRESULT Save()
    {
      this->saved_.Release();
      return this->factory_->CreateBitmapFromSource(this->bitmap_, WICBitmapCacheOnDemand, &this->saved_);
    }
    HRESULT CopyFrom(IWICBitmapFrameDecode* frame, const WICRect& rect)
    {
      ComPtr<IWICFormatConverter> converter;
      ComPtr<IWICBitmapLock> lock;
      HRESULT hr = S_OK;
      FAIL_BAIL = this->factory_->CreateFormatConverter(&converter);
      FAIL_BAIL = converter->Initialize(frame, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, NULL, 0., WICBitmapPaletteTypeMedianCut);
      FAIL_BAIL = this->bitmap_->Lock(&rect, WICBitmapLockRead | WICBitmapLockWrite, &lock);
      UINT stride = 0;
      FAIL_BAIL = lock->GetStride(&stride);
      UINT cb = 0;
      BYTE* destination = NULL;
      FAIL_BAIL = lock->GetDataPointer(&cb, &destination);
      Buff buff;
      FAIL_BAIL = buff.Alloc(cb);
      BYTE* source = buff.p;
      FAIL_BAIL = converter->CopyPixels(NULL, stride, cb, source);
      FAIL_BAIL = S_OK;
      const UINT skip = stride - rect.Width * 4;
      UINT i = 0;
      for (UINT y = rect.Height; y-- > 1; ) {
        for (UINT x = rect.Width; x-- > 0; i += 4) {
          if (source[i + 3] == 0) {
            // do nothing
          } else if (destination[i + 3] == 0 || source[i + 3] == 255) {
            destination[i + 0] = source[i + 0];
            destination[i + 1] = source[i + 1];
            destination[i + 2] = source[i + 2];
            destination[i + 3] = source[i + 3];
          } else {
            const BYTE srcA = source[i + 3];
            const BYTE invA = 255 - srcA;
            const BYTE dstA = destination[i + 3];
            destination[i + 0] = Clamp(Div255(srcA * source[i + 0] + Div255(dstA * destination[i + 0] * invA)));
            destination[i + 1] = Clamp(Div255(srcA * source[i + 1] + Div255(dstA * destination[i + 1] * invA)));
            destination[i + 2] = Clamp(Div255(srcA * source[i + 2] + Div255(dstA * destination[i + 2] * invA)));
            destination[i + 3] = Clamp(Div255(srcA + dstA * invA));
          }
        }
        i += skip;
      }
      return hr;
    }
    static int Div255(int x)
    {
      return ((x + 1) * 257) >> 16;
    }
    static inline BYTE Clamp(int x)
    {
      return x < 0 ? 0 : (x > 255 ? 255 : static_cast<BYTE>(x));
    }
  public:
    Gif(IWICImagingFactory* factory, UINT width, UINT height)
    {
      this->factory_ = factory;
      factory->CreateBitmap(width, height, GUID_WICPixelFormat32bppBGRA, WICBitmapCacheOnDemand, &this->bitmap_);
      ::memset(&this->rect_, 0, sizeof(this->rect_));
      this->disposal_ = 0;
    }
    HRESULT Draw(IWICBitmapFrameDecode* frame, const WICRect& rect, UINT disposal)
    {
      if (this->bitmap_ == NULL) {
        return E_FAIL;
      }
      HRESULT hr = S_OK;
      if (this->disposal_ == 2) {
        FAIL_BAIL = this->Clear(this->rect_);
      } else if (this->disposal_ == 3) {
        FAIL_BAIL = this->Restore();
      }
      if (disposal == 3) {
        FAIL_BAIL = this->Save();
      }
      FAIL_BAIL = this->CopyFrom(frame, rect);
      FAIL_BAIL = S_OK;
      this->rect_ = rect;
      this->disposal_ = disposal;
      return hr;
    }
    HRESULT ToBitmap(UINT width, UINT height, HBITMAP* phbmpImage, HBITMAP* phbmpMask)
    {
      *phbmpImage = NULL;
      *phbmpMask = NULL;
      if (width == 0 || height == 0 || height > UINT_MAX / 4 / width || height > INT_MAX) {
        return E_INVALIDARG;
      }
      if (this->bitmap_ == NULL) {
        return E_FAIL;
      }
#undef FAIL_BAIL
#define FAIL_BAIL if (FAILED(hr)) { if (hbmp != NULL) ::DeleteObject(hbmp); return hr; } hr
      ComPtr<IWICBitmapScaler> scaler;
      HBITMAP hbmp = NULL;
      HRESULT hr = S_OK;
      FAIL_BAIL = this->factory_->CreateBitmapScaler(&scaler);
      FAIL_BAIL = scaler->Initialize(this->bitmap_, width, height, WICBitmapInterpolationModeFant);
      BYTE* bits = NULL;
      BITMAPINFO hdr = { sizeof(BITMAPINFOHEADER), width, -static_cast<LONG>(height), 1, 32 };
      hbmp = ::CreateDIBSection(NULL, &hdr, DIB_RGB_COLORS, reinterpret_cast<void**>(&bits), NULL, 0);
      FAIL_BAIL = hbmp != NULL ? S_OK : E_OUTOFMEMORY;
      FAIL_BAIL = scaler->CopyPixels(NULL, width * 4, width * height * 4, bits);
      Buff buff;
      FAIL_BAIL = buff.Alloc((width + 7) / 8 * height);
      BYTE* dst = buff.p;
      const UINT padUp = -static_cast<int>((width + 7) / 8) & 1; // word aligned
      for (UINT y = 0; y < height; y++) {
        int maskbits = 0;
        BYTE mask = 0;
        for (UINT x = 0; x < width; x++) {
          mask = (mask << 1) | (bits[3] ? 0 : 1);
          bits += 4;
          if (++maskbits >= 8) {
            *dst++ = mask;
            maskbits = 0;
            mask = 0;
          }
        }
        if (maskbits > 0) {
          *dst = mask;
        }
        dst += padUp;
      }
      *phbmpImage = hbmp;
      *phbmpMask = ::CreateBitmap(width, height, 1, 1, buff.p);
      return hr;
    }
  };
#undef FAIL_BAIL
#define FAIL_BAIL if (FAILED(hr)) { return NULL; } hr
  ComPtr<IWICImagingFactory> factory;
  ComPtr<IWICStream> stream;
  ComPtr<IWICBitmapDecoder> decoder;
  ComPtr<IWICMetadataQueryReader> reader;
  UINT width = 0, height = 0, count = 0;
  HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  FAIL_BAIL = factory->CreateStream(&stream);
  if (path != NULL) {
    FAIL_BAIL = stream->InitializeFromFilename(path, GENERIC_READ);
  } else {
    FAIL_BAIL = stream->InitializeFromMemory(const_cast<BYTE*>(static_cast<const BYTE*>(data)), cbData);
  }
  FAIL_BAIL = factory->CreateDecoderFromStream(stream, NULL, WICDecodeMetadataCacheOnLoad, &decoder);
  FAIL_BAIL = decoder->GetMetadataQueryReader(&reader);
  FAIL_BAIL = PropVariant::ReadUInt16(reader, L"/logscrdesc/Width", &width);
  FAIL_BAIL = PropVariant::ReadUInt16(reader, L"/logscrdesc/Height", &height);
  FAIL_BAIL = PropVariant::CompareBuffer(reader, L"/appext/application", "NETSCAPE2.0", "ANIMEXTS1.0", 11);
  reader.Release();
  FAIL_BAIL = decoder->GetFrameCount(&count);
  FAIL_BAIL = S_OK;
  HIMAGELIST himl = ::ImageList_Create(iconWidth, iconHeight, ILC_COLOR32 | ILC_MASK, 0, 0);
  if (himl == NULL) {
    return NULL;
  }
  ::ImageList_SetImageCount(himl, count);
  Gif gif(factory, width, height);
  for (UINT i = 0; i < count; i++) {
    ComPtr<IWICBitmapFrameDecode> frame;
    ComPtr<IWICMetadataQueryReader> reader;
    WICRect rect = {};
    UINT disposal = 0;
    FAIL_BAIL = decoder->GetFrame(i, &frame);
    FAIL_BAIL = frame->GetMetadataQueryReader(&reader);
    FAIL_BAIL = PropVariant::ReadUInt16(reader, L"/imgdesc/Left", &rect.X);
    FAIL_BAIL = PropVariant::ReadUInt16(reader, L"/imgdesc/Top", &rect.Y);
    FAIL_BAIL = PropVariant::ReadUInt16(reader, L"/imgdesc/Width", &rect.Width);
    FAIL_BAIL = PropVariant::ReadUInt16(reader, L"/imgdesc/Height", &rect.Height);
    FAIL_BAIL = PropVariant::ReadUInt8(reader, L"/grctlext/Disposal", &disposal);
    FAIL_BAIL = S_OK;
    gif.Draw(frame, rect, disposal);
    HBITMAP hbmImage, hbmMask;
    FAIL_BAIL = gif.ToBitmap(iconWidth, iconHeight, &hbmImage, &hbmMask);
    FAIL_BAIL = S_OK;
    ::ImageList_Replace(himl, i, hbmImage, hbmMask);
    ::DeleteObject(hbmImage);
    ::DeleteObject(hbmMask);
  }
#undef FAIL_BAIL
  return himl;
}

class NotifyIconWindow
{
private:
  HWND hwnd_;
  UINT taskbarCreated_;
  NotifyCpuIcons icons_;
  CpuMonitor mon_;
  FileList list_;
  Cue* cues_;
  HMENU hm_;
  HIMAGELIST himl_;
  UINT (WINAPI* api_GetDpiForWindow_)(HWND);
  int (WINAPI* api_GetSystemMetricsForDpi_)(int, UINT);

  static const UINT ID_MONITOR = 7133;
  static const UINT ID_ANIM = 4213;
  static const UINT WM_USER_NOTIFY = WM_USER + 100;
  static const UINT WM_USER_UPDATE = WM_USER + 101;

  static LRESULT CALLBACK StaticWindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
  {
    NotifyIconWindow* self = reinterpret_cast<NotifyIconWindow*>(::GetWindowLongPtrW(hwnd, 0));
    if (self == NULL) {
      if (message != WM_NCCREATE) {
        return ::DefWindowProcW(hwnd, message, wParam, lParam);
      } else {
        self = reinterpret_cast<NotifyIconWindow*>(reinterpret_cast<LPCREATESTRUCT>(lParam)->lpCreateParams);
        self->hwnd_ = hwnd;
        ::SetWindowLongPtrW(hwnd, 0, reinterpret_cast<LONG_PTR>(self));
      }
    }
    LRESULT ret = self->WindowProc(hwnd, message, wParam, lParam);
    if (message == WM_NCDESTROY) {
      ::SetWindowLongPtrW(hwnd, 0, 0);
      self->hwnd_ = NULL;
    }
    return ret;
  }

  bool GetIconSize(HWND hwnd, UINT* width, UINT* height)
  {
    int iconX = 0, iconY = 0;
    if (this->api_GetDpiForWindow_ != NULL) {
      // move to primary monitor
      ::MoveWindow(hwnd, 0, 0, 0, 0, FALSE);
      UINT dpi = this->api_GetDpiForWindow_(hwnd);
      iconX = this->api_GetSystemMetricsForDpi_(SM_CXSMICON, dpi);
      iconY = this->api_GetSystemMetricsForDpi_(SM_CYSMICON, dpi);
    }
    if (iconX <= 0 || iconY <= 0) {
      iconX = ::GetSystemMetrics(SM_CXSMICON);
      iconY = ::GetSystemMetrics(SM_CYSMICON);
    }
    if (0 < iconX && iconX <= INT_MAX && 0 < iconY && iconY <= INT_MAX) {
      *width = iconX;
      *height = iconY;
      return true;
    }
    return false;
  }

  bool Init(HWND hwnd)
  {
    UINT width = 0, height = 0;
    if (this->GetIconSize(hwnd, &width, &height) == false) {
      return false;
    }
    HIMAGELIST himl = this->list_.Load(width, height);
    DWORD count = this->mon_.GetProcessorCount();
    this->cues_ = new Cue[count];
    bool ret = this->icons_.Init(hwnd, WM_USER_NOTIFY, count, himl != NULL ? himl : this->himl_);
    if (himl != NULL) {
      ::ImageList_Destroy(himl);
    }
    return ret;
  }

  bool Reinit(HWND hwnd)
  {
    this->icons_.Exit();
    return this->Init(hwnd);
  }

  bool OnCreate(HWND hwnd)
  {
    if (this->mon_.Init() == false) {
      return false;
    }
    // FIXME: hard-coded 32x32
    HIMAGELIST himl = ::ImageList_Create(32, 32, ILC_COLOR32 | ILC_MASK, 1, 0);
    if (himl == NULL) {
      return false;
    }
    ::ImageList_SetImageCount(himl, 1);
    ::ImageList_ReplaceIcon(himl, 0, LoadIconW(NULL, MAKEINTRESOURCEW(IDI_EXCLAMATION)));
    this->himl_ = himl;
    bool ret = this->Init(hwnd);
    if (ret) {
      ::SetTimer(hwnd, ID_MONITOR, 1000, NULL);
      ::SetTimer(hwnd, ID_ANIM, 25, NULL);
      const UINT count = this->mon_.GetProcessorCount();
      for (DWORD i = 0; i < count; i++) {
        this->cues_[i].Schedule(Cue::DEFAULT);
      }
    }
    return ret;
  }

  void OnDestroy(HWND hwnd)
  {
   ::KillTimer(hwnd, ID_MONITOR);
   ::KillTimer(hwnd, ID_ANIM);
    this->icons_.Exit();
    ::PostQuitMessage(0);
  }

  void OnTimer(HWND hwnd, UINT_PTR id)
  {
    const UINT count = this->mon_.GetProcessorCount();
    if (id == ID_MONITOR) {
      this->mon_.Collect();
      for (DWORD i = 0; i < count; i++) {
        LONG value;
        if (this->mon_.GetUsage(i, &value)) {
          if (value > 100) {
            value = 100;
          } else if (value < 0) {
            value = 0;
          }
          this->icons_.UpdateCpuUsage(i, value);
          double x = static_cast<double>(value) / 100.;
          // double y = 6 * ::pow(x, 5) - 15 * ::pow(x, 4) + 10 * ::pow(x, 3);
          // double y = 1 - (1 - x) * (1 - x);
          double y = ::pow(x, 1.7);
          UINT fps = static_cast<int>(y * 30. + Cue::DEFAULT);
          this->cues_[i].Schedule(fps);
        }
      }
    } else if (id == ID_ANIM) {
      for (DWORD i = 0; i < count; i++) {
        UINT icon = i + this->cues_[i].GetIndex(this->icons_.GetIconCount());
        this->icons_.UpdateCpuIcon(i, icon);
      }
    }
  }

  void OnNotifyIconRButtonUp(HWND hwnd, UINT id)
  {
    if (this->hm_ == NULL) {
      HMENU hm = ::LoadMenuW(HINST_THISCOMPONENT, MAKEINTRESOURCEW(IDR_MAIN));
      if (hm != NULL) {
        this->hm_ = ::GetSubMenu(hm, 0);
        ::RemoveMenu(hm, 0, MF_BYPOSITION);
        ::DestroyMenu(hm);
      }
    }
    if (this->hm_ == NULL) {
      ::DestroyWindow(hwnd);
      return;
    }

    this->list_.Init();
    this->list_.AddMenuItems(this->hm_);

    ::SetForegroundWindow(hwnd);
    POINT pt = {};
    ::GetCursorPos(&pt);

    const UINT cmd = ::TrackPopupMenu(this->hm_, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd, NULL);
    switch (cmd) {
    case 0:
      break;
    case IDM_EXIT:
      ::DestroyWindow(hwnd);
      break;
    default:
      if (this->list_.Select(this->hm_, cmd)) {
        if (this->Reinit(hwnd) == false) {
          ::DestroyWindow(hwnd);
        }
      }
      break;
    }
    ::PostMessageW(hwnd, WM_NULL, 0, 0);
  }

  void OnSettingChange(HWND hwnd)
  {
  }

  void OnTaskbarCreated(HWND hwnd)
  {
    if (this->Reinit(hwnd) == false) {
      ::DestroyWindow(hwnd);
    }
  }

  LRESULT CALLBACK WindowProc(HWND hwnd, UINT message, WPARAM wParam, LPARAM lParam)
  {
    switch (message) {
    case WM_CREATE:
      return this->OnCreate(hwnd) ? 0 : -1;
    case WM_DESTROY:
      this->OnDestroy(hwnd);
      break;
    case WM_TIMER:
      this->OnTimer(hwnd, wParam);
      break;
    case WM_USER_NOTIFY:
      if (lParam == WM_RBUTTONUP) {
        this->OnNotifyIconRButtonUp(hwnd, static_cast<UINT>(wParam));
      }
      break;
    case WM_SETTINGCHANGE:
      this->OnSettingChange(hwnd);
      break;
    default:
      if (message == this->taskbarCreated_) {
        this->OnTaskbarCreated(hwnd);
        break;
      }
      return ::DefWindowProcW(hwnd, message, wParam, lParam);
    }
    return 0;
  }

public:
  NotifyIconWindow()
  {
    this->hwnd_ = NULL;
    this->cues_ = NULL;
    this->hm_ = NULL;
    this->himl_ = NULL;
    this->taskbarCreated_ = ::RegisterWindowMessageW(L"TaskbarCreated");
    if (this->taskbarCreated_ == 0) {
      this->taskbarCreated_ = ~0U;
    }
    HMODULE hmod = GetModuleHandleW(L"user32.dll");
    this->api_GetDpiForWindow_ = NULL;
    this->api_GetSystemMetricsForDpi_ = NULL;
    FARPROC gdfw = GetProcAddress(hmod, "GetDpiForWindow");
    FARPROC gsmfd = GetProcAddress(hmod, "GetSystemMetricsForDpi");
    if (gdfw != NULL && gsmfd != NULL) {
      reinterpret_cast<FARPROC&>(this->api_GetDpiForWindow_) = gdfw;
      reinterpret_cast<FARPROC&>(this->api_GetSystemMetricsForDpi_) = gsmfd;
    }
  }
  ~NotifyIconWindow()
  {
    delete[] this->cues_;
    if (this->himl_ != NULL) {
      ::ImageList_Destroy(this->himl_);
    }
  }

  int Run()
  {
    LPCWSTR className = L"ParrotCpuMonitor";
    HWND hwndFound = ::FindWindowW(className, NULL);
    if (hwndFound != NULL) {
      return 1;
    }

    CoInitialiser init;
    if (FAILED(init.hr)) {
      return 2;
    }

    const WNDCLASSW wc = { 0, StaticWindowProc, 0, sizeof(NotifyIconWindow*), HINST_THISCOMPONENT, NULL, NULL, NULL, NULL, className };
    const ATOM atom = ::RegisterClassW(&wc);
    if (atom == 0) {
      return 3;
    }

    HWND hwnd = ::CreateWindowW(MAKEINTRESOURCEW(atom), NULL, WS_OVERLAPPED, 0, 0, 0, 0, NULL, NULL, NULL, this);
    if (hwnd == NULL) {
      return 4;
    }

    ::SetPriorityClass(::GetCurrentProcess(), HIGH_PRIORITY_CLASS);

    MSG msg;
    while (::GetMessageW(&msg, NULL, 0, 0)) {
      ::DispatchMessageW(&msg);
    }
    return static_cast<int>(msg.wParam);
  }
};

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
  NotifyIconWindow wnd;
  return wnd.Run();
}
