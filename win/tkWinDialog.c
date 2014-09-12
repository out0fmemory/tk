/*
 * tkWinDialog.c --
 *
 *	Contains the Windows implementation of the common dialog boxes.
 *
 * Copyright (c) 1996-1997 Sun Microsystems, Inc.
 *
 * See the file "license.terms" for information on usage and redistribution of
 * this file, and for a DISCLAIMER OF ALL WARRANTIES.
 */

#include "tkWinInt.h"
#include "tkFileFilter.h"
#include "tkFont.h"

#include <commdlg.h>		/* includes common dialog functionality */
#ifdef _MSC_VER
#   pragma comment (lib, "comdlg32.lib")
#endif
#include <dlgs.h>		/* includes common dialog template defines */
#include <cderr.h>		/* includes the common dialog error codes */

#include <shlobj.h>		/* includes SHBrowseForFolder */
#include <shobjidl.h>

#ifdef _MSC_VER
#   pragma comment (lib, "shell32.lib")
#endif

/* These needed for compilation with VC++ 5.2 */
#ifndef BIF_EDITBOX
#define BIF_EDITBOX 0x10
#endif

#ifndef BIF_VALIDATE
#define BIF_VALIDATE 0x0020
#endif

#ifndef BIF_NEWDIALOGSTYLE
#define BIF_NEWDIALOGSTYLE 0x0040
#endif

#ifndef BFFM_VALIDATEFAILED
#ifdef UNICODE
#define BFFM_VALIDATEFAILED 4
#else
#define BFFM_VALIDATEFAILED 3
#endif
#endif /* BFFM_VALIDATEFAILED */

typedef struct ThreadSpecificData {
    int debugFlag;		/* Flags whether we should output debugging
				 * information while displaying a builtin
				 * dialog. */
    Tcl_Interp *debugInterp;	/* Interpreter to used for debugging. */
    UINT WM_LBSELCHANGED;	/* Holds a registered windows event used for
				 * communicating between the Directory Chooser
				 * dialog and its hook proc. */
    HHOOK hMsgBoxHook;		/* Hook proc for tk_messageBox and the */
    HICON hSmallIcon;		/* icons used by a parent to be used in */
    HICON hBigIcon;		/* the message box */
    int   useNewFileDialogs;
#define FDLG_STATE_INIT 0       /* Uninitialized */
#define FDLG_STATE_USE_NEW 1    /* Use the new dialogs */
#define FDLG_STATE_USE_OLD 2    /* Use the old dialogs */
} ThreadSpecificData;
static Tcl_ThreadDataKey dataKey;

/*
 * The following structures are used by Tk_MessageBoxCmd() to parse arguments
 * and return results.
 */

static const TkStateMap iconMap[] = {
    {MB_ICONERROR,		"error"},
    {MB_ICONINFORMATION,	"info"},
    {MB_ICONQUESTION,		"question"},
    {MB_ICONWARNING,		"warning"},
    {-1,			NULL}
};

static const TkStateMap typeMap[] = {
    {MB_ABORTRETRYIGNORE,	"abortretryignore"},
    {MB_OK,			"ok"},
    {MB_OKCANCEL,		"okcancel"},
    {MB_RETRYCANCEL,		"retrycancel"},
    {MB_YESNO,			"yesno"},
    {MB_YESNOCANCEL,		"yesnocancel"},
    {-1,			NULL}
};

static const TkStateMap buttonMap[] = {
    {IDABORT,			"abort"},
    {IDRETRY,			"retry"},
    {IDIGNORE,			"ignore"},
    {IDOK,			"ok"},
    {IDCANCEL,			"cancel"},
    {IDNO,			"no"},
    {IDYES,			"yes"},
    {-1,			NULL}
};

static const int buttonFlagMap[] = {
    MB_DEFBUTTON1, MB_DEFBUTTON2, MB_DEFBUTTON3, MB_DEFBUTTON4
};

static const struct {int type; int btnIds[3];} allowedTypes[] = {
    {MB_ABORTRETRYIGNORE,	{IDABORT, IDRETRY,  IDIGNORE}},
    {MB_OK,			{IDOK,	  -1,	    -1	    }},
    {MB_OKCANCEL,		{IDOK,	  IDCANCEL, -1	    }},
    {MB_RETRYCANCEL,		{IDRETRY, IDCANCEL, -1	    }},
    {MB_YESNO,			{IDYES,	  IDNO,	    -1	    }},
    {MB_YESNOCANCEL,		{IDYES,	  IDNO,	    IDCANCEL}}
};

#define NUM_TYPES (sizeof(allowedTypes) / sizeof(allowedTypes[0]))

/*
 * Abstract trivial differences between Win32 and Win64.
 */

#define TkWinGetHInstance(from) \
	((HINSTANCE) GetWindowLongPtr((from), GWLP_HINSTANCE))
#define TkWinGetUserData(from) \
	GetWindowLongPtr((from), GWLP_USERDATA)
#define TkWinSetUserData(to,what) \
	SetWindowLongPtr((to), GWLP_USERDATA, (LPARAM)(what))

/*
 * The value of TK_MULTI_MAX_PATH dictates how many files can be retrieved
 * with tk_get*File -multiple 1. It must be allocated on the stack, so make it
 * large enough but not too large. - hobbs
 *
 * The data is stored as <dir>\0<file1>\0<file2>\0...<fileN>\0\0. Since
 * MAX_PATH == 260 on Win2K/NT, *40 is ~10Kbytes.
 */

#define TK_MULTI_MAX_PATH	(MAX_PATH*40)

/*
 * The following structure is used to pass information between the directory
 * chooser function, Tk_ChooseDirectoryObjCmd(), and its dialog hook proc.
 */

typedef struct {
   TCHAR initDir[MAX_PATH];	/* Initial folder to use */
   TCHAR retDir[MAX_PATH];	/* Returned folder to use */
   Tcl_Interp *interp;
   int mustExist;		/* True if file must exist to return from
				 * callback */
} ChooseDir;

/*
 * The following structure is used to pass information between GetFileName
 * function and OFN dialog hook procedures. [Bug 2896501, Patch 2898255]
 */

typedef struct OFNData {
    Tcl_Interp *interp;		/* Interp, used only if debug is turned on,
				 * for setting the "tk_dialog" variable. */
    int dynFileBufferSize;	/* Dynamic filename buffer size, stored to
				 * avoid shrinking and expanding the buffer
				 * when selection changes */
    TCHAR *dynFileBuffer;	/* Dynamic filename buffer */
} OFNData;

/*
 * The following structure is used to gather options used by various
 * file dialogs
 */
typedef struct OFNOpts {
    Tk_Window tkwin;            /* Owner window for dialog */
    const char *extension;            /* Default extension */
    const char *title;                /* Title for dialog */
    Tcl_Obj *filterObj;         /* File type filter list */
    Tcl_Obj *typeVariableObj;   /* Variable in which to store type selected */
    Tcl_Obj *initialTypeObj;    /* Initial value of above, or NULL */
    Tcl_DString utfDirString;   /* Initial dir */
    int multi;                  /* Multiple selection enabled */
    int confirmOverwrite;                  /* Multiple selection enabled */
    TCHAR file[TK_MULTI_MAX_PATH]; /* File name
                                      XXX - fixed size because it was so
                                      historically. Why not malloc'ed ?
                                      XXX - also, TCHAR should really be WCHAR
                                      because TkWinGetUnicodeEncoding is always
                                      UCS2.
                                   */
} OFNOpts;

/*
 * The following definitions are required when using older versions of
 * Visual C++ (like 6.0) and possibly MingW. Those headers do not contain
 * required definitions for interfaces new to Vista that we need for
 * the new file dialogs. Duplicating definitions is OK because they
 * should forever remain unchanged.
 *
 * XXX - is there a better/easier way to use new data definitions with
 * older compilers? Should we prefix definitions with Tcl_ instead
 * of using the same names as in the SDK?
 */
#ifndef __IShellItemArray_INTERFACE_DEFINED__
#define __IShellItemArray_INTERFACE_DEFINED__

typedef enum SIATTRIBFLAGS {
    SIATTRIBFLAGS_AND	= 0x1,
    SIATTRIBFLAGS_OR	= 0x2,
    SIATTRIBFLAGS_APPCOMPAT	= 0x3,
    SIATTRIBFLAGS_MASK	= 0x3,
    SIATTRIBFLAGS_ALLITEMS	= 0x4000
} SIATTRIBFLAGS;
typedef struct IShellItemArray IShellItemArray;
typedef struct IShellItemArrayVtbl
{
    BEGIN_INTERFACE
        
    HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
        IShellItemArray * This,
        /* [in] */  REFIID riid,
        /* [annotation][iid_is][out] */ 
        void **ppvObject);
        
    ULONG ( STDMETHODCALLTYPE *AddRef )( 
         IShellItemArray * This);
        
    ULONG ( STDMETHODCALLTYPE *Release )( 
         IShellItemArray * This);
        
    HRESULT ( STDMETHODCALLTYPE *BindToHandler )( 
         IShellItemArray * This,
        /* [unique][in] */ IBindCtx *pbc,
        /* [in] */  REFGUID bhid,
        /* [in] */  REFIID riid,
        /* [iid_is][out] */ void **ppvOut);
        
    HRESULT ( STDMETHODCALLTYPE *GetPropertyStore )( 
         IShellItemArray * This,
         /* Actually enum GETPROPERTYSTOREFLAGS, but we do not use this call */
        /* [in] */ int flags,
        /* [in] */  REFIID riid,
        /* [iid_is][out] */ void **ppv);
        
    HRESULT ( STDMETHODCALLTYPE *GetPropertyDescriptionList )( 
         IShellItemArray * This,
         /* Actually REFPROPERTYKEY, but this call is not used */
        /* [in] */   void* keyType,
        /* [in] */  REFIID riid,
        /* [iid_is][out] */  void **ppv);
        
    HRESULT ( STDMETHODCALLTYPE *GetAttributes )( 
         IShellItemArray * This,
        /* [in] */ SIATTRIBFLAGS AttribFlags,
        /* [in] */ SFGAOF sfgaoMask,
        /* [out] */ SFGAOF *psfgaoAttribs);
        
    HRESULT ( STDMETHODCALLTYPE *GetCount )( 
         IShellItemArray * This,
        /* [out] */ DWORD *pdwNumItems);
        
    HRESULT ( STDMETHODCALLTYPE *GetItemAt )( 
         IShellItemArray * This,
        /* [in] */ DWORD dwIndex,
        /* [out] */ IShellItem **ppsi);
        
    HRESULT ( STDMETHODCALLTYPE *EnumItems )( 
         IShellItemArray * This,
         /* Actually IEnumShellItems **, but we do not use this call */
        /* [out] */ void **ppenumShellItems);
        
    END_INTERFACE
} IShellItemArrayVtbl;

struct IShellItemArray
{
    CONST_VTBL struct IShellItemArrayVtbl *lpVtbl;
};

#endif /* __IShellItemArray_INTERFACE_DEFINED__ */

#ifndef __IFileDialog_INTERFACE_DEFINED__

/* Forward declarations for structs that are referenced but not used */
typedef struct IPropertyStore IPropertyStore;
typedef struct IPropertyDescriptionList IPropertyDescriptionList;
typedef struct IFileOperationProgressSink IFileOperationProgressSink;
typedef enum FDAP {
    FDAP_BOTTOM	= 0,
    FDAP_TOP	= 1
} FDAP;

typedef struct _COMDLG_FILTERSPEC {
    LPCWSTR pszName;
    LPCWSTR pszSpec;
} COMDLG_FILTERSPEC;

static CLSID CLSID_FileOpenDialog = {
    0xDC1C5A9C, 0xE88A, 0X4DDE, 0xA5, 0xA1, 0x60, 0xF8, 0x2A, 0x20, 0xAE, 0xF7
};

static IID IID_IFileOpenDialog = {
    0xD57C7288, 0xD4AD, 0x4768, 0xBE, 0x02, 0x9D, 0x96, 0x95, 0x32, 0xD9, 0x60
};

static CLSID CLSID_FileSaveDialog = {
    0xC0B4E2F3, 0xBA21, 0x4773, 0x8D, 0xBA, 0x33, 0x5E, 0xC9, 0x46, 0xEB, 0x8B
};

static IID IID_IFileSaveDialog = {
    0x84BCCD23, 0x5FDE, 0x4CDB, 0xAE, 0xA4, 0xAF, 0x64, 0xB8, 0x3D, 0x78, 0xAB
};

enum _FILEOPENDIALOGOPTIONS {
    FOS_OVERWRITEPROMPT	= 0x2,
    FOS_STRICTFILETYPES	= 0x4,
    FOS_NOCHANGEDIR	= 0x8,
    FOS_PICKFOLDERS	= 0x20,
    FOS_FORCEFILESYSTEM	= 0x40,
    FOS_ALLNONSTORAGEITEMS	= 0x80,
    FOS_NOVALIDATE	= 0x100,
    FOS_ALLOWMULTISELECT	= 0x200,
    FOS_PATHMUSTEXIST	= 0x800,
    FOS_FILEMUSTEXIST	= 0x1000,
    FOS_CREATEPROMPT	= 0x2000,
    FOS_SHAREAWARE	= 0x4000,
    FOS_NOREADONLYRETURN	= 0x8000,
    FOS_NOTESTFILECREATE	= 0x10000,
    FOS_HIDEMRUPLACES	= 0x20000,
    FOS_HIDEPINNEDPLACES	= 0x40000,
    FOS_NODEREFERENCELINKS	= 0x100000,
    FOS_DONTADDTORECENT	= 0x2000000,
    FOS_FORCESHOWHIDDEN	= 0x10000000,
    FOS_DEFAULTNOMINIMODE	= 0x20000000,
    FOS_FORCEPREVIEWPANEON	= 0x40000000
} ;
typedef DWORD FILEOPENDIALOGOPTIONS;

typedef struct IFileDialog IFileDialog;
typedef struct IFileDialogVtbl
{
    BEGIN_INTERFACE
        
    HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
         IFileDialog * This,
        /* [in] */  REFIID riid,
        /* [annotation][iid_is][out] */ 
          void **ppvObject);
        
    ULONG ( STDMETHODCALLTYPE *AddRef )( 
         IFileDialog * This);
        
    ULONG ( STDMETHODCALLTYPE *Release )( 
         IFileDialog * This);
        
    /* [local] */ HRESULT ( STDMETHODCALLTYPE *Show )( 
        IFileDialog * This,
        /* [annotation][unique][in] */ 
        HWND hwndOwner);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileTypes )( 
         IFileDialog * This,
        /* [in] */ UINT cFileTypes,
        /* [size_is][in] */ const COMDLG_FILTERSPEC *rgFilterSpec);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileTypeIndex )( 
         IFileDialog * This,
        /* [in] */ UINT iFileType);
        
    HRESULT ( STDMETHODCALLTYPE *GetFileTypeIndex )( 
         IFileDialog * This,
        /* [out] */  UINT *piFileType);
        
    HRESULT ( STDMETHODCALLTYPE *Advise )( 
         IFileDialog * This,
         /* XXX - Actually pfde is IFileDialogEvents* but we do not use
            this call and do not want to define IFileDialogEvents as that
            pulls in a whole bunch of other stuff. */
        /* [in] */  void *pfde,
        /* [out] */  DWORD *pdwCookie);
        
    HRESULT ( STDMETHODCALLTYPE *Unadvise )( 
         IFileDialog * This,
        /* [in] */ DWORD dwCookie);
        
    HRESULT ( STDMETHODCALLTYPE *SetOptions )( 
         IFileDialog * This,
        /* [in] */ FILEOPENDIALOGOPTIONS fos);
        
    HRESULT ( STDMETHODCALLTYPE *GetOptions )( 
         IFileDialog * This,
        /* [out] */  FILEOPENDIALOGOPTIONS *pfos);
        
    HRESULT ( STDMETHODCALLTYPE *SetDefaultFolder )( 
         IFileDialog * This,
        /* [in] */  IShellItem *psi);
        
    HRESULT ( STDMETHODCALLTYPE *SetFolder )( 
         IFileDialog * This,
        /* [in] */  IShellItem *psi);
        
    HRESULT ( STDMETHODCALLTYPE *GetFolder )( 
         IFileDialog * This,
        /* [out] */  IShellItem **ppsi);
        
    HRESULT ( STDMETHODCALLTYPE *GetCurrentSelection )( 
         IFileDialog * This,
        /* [out] */  IShellItem **ppsi);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileName )( 
         IFileDialog * This,
        /* [string][in] */  LPCWSTR pszName);
        
    HRESULT ( STDMETHODCALLTYPE *GetFileName )( 
         IFileDialog * This,
        /* [string][out] */  LPWSTR *pszName);
        
    HRESULT ( STDMETHODCALLTYPE *SetTitle )( 
         IFileDialog * This,
        /* [string][in] */  LPCWSTR pszTitle);
        
    HRESULT ( STDMETHODCALLTYPE *SetOkButtonLabel )( 
         IFileDialog * This,
        /* [string][in] */  LPCWSTR pszText);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileNameLabel )( 
         IFileDialog * This,
        /* [string][in] */  LPCWSTR pszLabel);
        
    HRESULT ( STDMETHODCALLTYPE *GetResult )( 
         IFileDialog * This,
        /* [out] */  IShellItem **ppsi);
        
    HRESULT ( STDMETHODCALLTYPE *AddPlace )( 
         IFileDialog * This,
        /* [in] */  IShellItem *psi,
        /* [in] */ FDAP fdap);
        
    HRESULT ( STDMETHODCALLTYPE *SetDefaultExtension )( 
         IFileDialog * This,
        /* [string][in] */  LPCWSTR pszDefaultExtension);
        
    HRESULT ( STDMETHODCALLTYPE *Close )( 
         IFileDialog * This,
        /* [in] */ HRESULT hr);
        
    HRESULT ( STDMETHODCALLTYPE *SetClientGuid )( 
         IFileDialog * This,
        /* [in] */  REFGUID guid);
        
    HRESULT ( STDMETHODCALLTYPE *ClearClientData )( 
         IFileDialog * This);
        
    HRESULT ( STDMETHODCALLTYPE *SetFilter )( 
         IFileDialog * This,
         /* XXX - Actually IShellItemFilter. But deprecated in Win7 AND we do
            not use it anyways. So define as void* */
        /* [in] */  void *pFilter);
        
    END_INTERFACE
} IFileDialogVtbl;

struct IFileDialog {
    CONST_VTBL struct IFileDialogVtbl *lpVtbl;
};


typedef struct IFileSaveDialog IFileSaveDialog;
typedef struct IFileSaveDialogVtbl {
    BEGIN_INTERFACE
        
    HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
         IFileSaveDialog * This,
        /* [in] */  REFIID riid,
        /* [annotation][iid_is][out] */ 
          void **ppvObject);
        
    ULONG ( STDMETHODCALLTYPE *AddRef )( 
         IFileSaveDialog * This);
        
    ULONG ( STDMETHODCALLTYPE *Release )( 
         IFileSaveDialog * This);
        
    /* [local] */ HRESULT ( STDMETHODCALLTYPE *Show )( 
        IFileSaveDialog * This,
        /* [annotation][unique][in] */ 
        HWND hwndOwner);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileTypes )( 
         IFileSaveDialog * This,
        /* [in] */ UINT cFileTypes,
        /* [size_is][in] */ const COMDLG_FILTERSPEC *rgFilterSpec);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileTypeIndex )( 
         IFileSaveDialog * This,
        /* [in] */ UINT iFileType);
        
    HRESULT ( STDMETHODCALLTYPE *GetFileTypeIndex )( 
         IFileSaveDialog * This,
        /* [out] */  UINT *piFileType);
        
    HRESULT ( STDMETHODCALLTYPE *Advise )( 
         IFileSaveDialog * This,
         /* XXX - Actually pfde is IFileSaveDialogEvents* but we do not use
            this call and do not want to define IFileSaveDialogEvents as that
            pulls in a whole bunch of other stuff. */
        /* [in] */  void *pfde,
        /* [out] */  DWORD *pdwCookie);
        
    HRESULT ( STDMETHODCALLTYPE *Unadvise )( 
         IFileSaveDialog * This,
        /* [in] */ DWORD dwCookie);
        
    HRESULT ( STDMETHODCALLTYPE *SetOptions )( 
         IFileSaveDialog * This,
        /* [in] */ FILEOPENDIALOGOPTIONS fos);
        
    HRESULT ( STDMETHODCALLTYPE *GetOptions )( 
         IFileSaveDialog * This,
        /* [out] */  FILEOPENDIALOGOPTIONS *pfos);
        
    HRESULT ( STDMETHODCALLTYPE *SetDefaultFolder )( 
         IFileSaveDialog * This,
        /* [in] */  IShellItem *psi);
        
    HRESULT ( STDMETHODCALLTYPE *SetFolder )( 
         IFileSaveDialog * This,
        /* [in] */  IShellItem *psi);
        
    HRESULT ( STDMETHODCALLTYPE *GetFolder )( 
         IFileSaveDialog * This,
        /* [out] */  IShellItem **ppsi);
        
    HRESULT ( STDMETHODCALLTYPE *GetCurrentSelection )( 
         IFileSaveDialog * This,
        /* [out] */  IShellItem **ppsi);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileName )( 
         IFileSaveDialog * This,
        /* [string][in] */  LPCWSTR pszName);
        
    HRESULT ( STDMETHODCALLTYPE *GetFileName )( 
         IFileSaveDialog * This,
        /* [string][out] */  LPWSTR *pszName);
        
    HRESULT ( STDMETHODCALLTYPE *SetTitle )( 
         IFileSaveDialog * This,
        /* [string][in] */  LPCWSTR pszTitle);
        
    HRESULT ( STDMETHODCALLTYPE *SetOkButtonLabel )( 
         IFileSaveDialog * This,
        /* [string][in] */  LPCWSTR pszText);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileNameLabel )( 
         IFileSaveDialog * This,
        /* [string][in] */  LPCWSTR pszLabel);
        
    HRESULT ( STDMETHODCALLTYPE *GetResult )( 
         IFileSaveDialog * This,
        /* [out] */  IShellItem **ppsi);
        
    HRESULT ( STDMETHODCALLTYPE *AddPlace )( 
         IFileSaveDialog * This,
        /* [in] */  IShellItem *psi,
        /* [in] */ FDAP fdap);
        
    HRESULT ( STDMETHODCALLTYPE *SetDefaultExtension )( 
         IFileSaveDialog * This,
        /* [string][in] */  LPCWSTR pszDefaultExtension);
        
    HRESULT ( STDMETHODCALLTYPE *Close )( 
         IFileSaveDialog * This,
        /* [in] */ HRESULT hr);
        
    HRESULT ( STDMETHODCALLTYPE *SetClientGuid )( 
         IFileSaveDialog * This,
        /* [in] */  REFGUID guid);
        
    HRESULT ( STDMETHODCALLTYPE *ClearClientData )( 
         IFileSaveDialog * This);
        
    HRESULT ( STDMETHODCALLTYPE *SetFilter )( 
         IFileSaveDialog * This,
         /* XXX - Actually IShellItemFilter. But deprecated in Win7 AND we do
            not use it anyways. So define as void* */
        /* [in] */  void *pFilter);
        
    HRESULT ( STDMETHODCALLTYPE *SetSaveAsItem )( 
        IFileSaveDialog * This,
        /* [in] */ IShellItem *psi);
        
    HRESULT ( STDMETHODCALLTYPE *SetProperties )( 
        IFileSaveDialog * This,
        /* [in] */ IPropertyStore *pStore);
        
    HRESULT ( STDMETHODCALLTYPE *SetCollectedProperties )( 
        IFileSaveDialog * This,
        /* [in] */ IPropertyDescriptionList *pList,
        /* [in] */ BOOL fAppendDefault);
        
    HRESULT ( STDMETHODCALLTYPE *GetProperties )( 
        IFileSaveDialog * This,
        /* [out] */ IPropertyStore **ppStore);
        
    HRESULT ( STDMETHODCALLTYPE *ApplyProperties )( 
        IFileSaveDialog * This,
        /* [in] */  IShellItem *psi,
        /* [in] */  IPropertyStore *pStore,
            /* [unique][in] */  HWND hwnd,
            /* [unique][in] */  IFileOperationProgressSink *pSink);

    END_INTERFACE

} IFileSaveDialogVtbl;

struct IFileSaveDialog {
    CONST_VTBL struct IFileSaveDialogVtbl *lpVtbl;
};

typedef struct IFileOpenDialog IFileOpenDialog;
typedef struct IFileOpenDialogVtbl {
    BEGIN_INTERFACE
        
    HRESULT ( STDMETHODCALLTYPE *QueryInterface )( 
         IFileOpenDialog * This,
        /* [in] */  REFIID riid,
        /* [annotation][iid_is][out] */ 
          void **ppvObject);
        
    ULONG ( STDMETHODCALLTYPE *AddRef )( 
         IFileOpenDialog * This);
        
    ULONG ( STDMETHODCALLTYPE *Release )( 
         IFileOpenDialog * This);
        
    /* [local] */ HRESULT ( STDMETHODCALLTYPE *Show )( 
        IFileOpenDialog * This,
        /* [annotation][unique][in] */ 
        HWND hwndOwner);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileTypes )( 
         IFileOpenDialog * This,
        /* [in] */ UINT cFileTypes,
        /* [size_is][in] */ const COMDLG_FILTERSPEC *rgFilterSpec);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileTypeIndex )( 
         IFileOpenDialog * This,
        /* [in] */ UINT iFileType);
        
    HRESULT ( STDMETHODCALLTYPE *GetFileTypeIndex )( 
         IFileOpenDialog * This,
        /* [out] */  UINT *piFileType);
        
    HRESULT ( STDMETHODCALLTYPE *Advise )( 
         IFileOpenDialog * This,
         /* XXX - Actually pfde is IFileDialogEvents* but we do not use
            this call and do not want to define IFileDialogEvents as that
            pulls in a whole bunch of other stuff. */
        /* [in] */  void *pfde,
        /* [out] */  DWORD *pdwCookie);
        
    HRESULT ( STDMETHODCALLTYPE *Unadvise )( 
         IFileOpenDialog * This,
        /* [in] */ DWORD dwCookie);
        
    HRESULT ( STDMETHODCALLTYPE *SetOptions )( 
         IFileOpenDialog * This,
        /* [in] */ FILEOPENDIALOGOPTIONS fos);
        
    HRESULT ( STDMETHODCALLTYPE *GetOptions )( 
         IFileOpenDialog * This,
        /* [out] */  FILEOPENDIALOGOPTIONS *pfos);
        
    HRESULT ( STDMETHODCALLTYPE *SetDefaultFolder )( 
         IFileOpenDialog * This,
        /* [in] */  IShellItem *psi);
        
    HRESULT ( STDMETHODCALLTYPE *SetFolder )( 
         IFileOpenDialog * This,
        /* [in] */  IShellItem *psi);
        
    HRESULT ( STDMETHODCALLTYPE *GetFolder )( 
         IFileOpenDialog * This,
        /* [out] */  IShellItem **ppsi);
        
    HRESULT ( STDMETHODCALLTYPE *GetCurrentSelection )( 
         IFileOpenDialog * This,
        /* [out] */  IShellItem **ppsi);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileName )( 
         IFileOpenDialog * This,
        /* [string][in] */  LPCWSTR pszName);
        
    HRESULT ( STDMETHODCALLTYPE *GetFileName )( 
         IFileOpenDialog * This,
        /* [string][out] */  LPWSTR *pszName);
        
    HRESULT ( STDMETHODCALLTYPE *SetTitle )( 
         IFileOpenDialog * This,
        /* [string][in] */  LPCWSTR pszTitle);
        
    HRESULT ( STDMETHODCALLTYPE *SetOkButtonLabel )( 
         IFileOpenDialog * This,
        /* [string][in] */  LPCWSTR pszText);
        
    HRESULT ( STDMETHODCALLTYPE *SetFileNameLabel )( 
         IFileOpenDialog * This,
        /* [string][in] */  LPCWSTR pszLabel);
        
    HRESULT ( STDMETHODCALLTYPE *GetResult )( 
         IFileOpenDialog * This,
        /* [out] */  IShellItem **ppsi);
        
    HRESULT ( STDMETHODCALLTYPE *AddPlace )( 
         IFileOpenDialog * This,
        /* [in] */  IShellItem *psi,
        /* [in] */ FDAP fdap);
        
    HRESULT ( STDMETHODCALLTYPE *SetDefaultExtension )( 
         IFileOpenDialog * This,
        /* [string][in] */  LPCWSTR pszDefaultExtension);
        
    HRESULT ( STDMETHODCALLTYPE *Close )( 
         IFileOpenDialog * This,
        /* [in] */ HRESULT hr);
        
    HRESULT ( STDMETHODCALLTYPE *SetClientGuid )( 
         IFileOpenDialog * This,
        /* [in] */  REFGUID guid);
        
    HRESULT ( STDMETHODCALLTYPE *ClearClientData )( 
         IFileOpenDialog * This);
        
    HRESULT ( STDMETHODCALLTYPE *SetFilter )( 
         IFileOpenDialog * This,
         /* XXX - Actually IShellItemFilter. But deprecated in Win7 AND we do
            not use it anyways. So define as void* */
        /* [in] */  void *pFilter);
        
    HRESULT ( STDMETHODCALLTYPE *GetResults )( 
        IFileOpenDialog * This,
        /* [out] */ IShellItemArray **ppenum);
        
    HRESULT ( STDMETHODCALLTYPE *GetSelectedItems )( 
        IFileOpenDialog * This,
        /* [out] */ IShellItemArray **ppsai);

    END_INTERFACE
} IFileOpenDialogVtbl;

struct IFileOpenDialog
{
    CONST_VTBL struct IFileOpenDialogVtbl *lpVtbl;
};

#endif /* __IFileDialog_INTERFACE_DEFINED__ */

/*
 * Definitions of functions used only in this file.
 */

static UINT APIENTRY	ChooseDirectoryValidateProc(HWND hdlg, UINT uMsg,
			    LPARAM wParam, LPARAM lParam);
static UINT CALLBACK	ColorDlgHookProc(HWND hDlg, UINT uMsg, WPARAM wParam,
			    LPARAM lParam);
static void             CleanupOFNOptions(OFNOpts *optsPtr);
static int              ParseOFNOptions(ClientData clientData,
                            Tcl_Interp *interp, int objc,
                            Tcl_Obj *const objv[], int open, OFNOpts *optsPtr);
static int 		GetFileName(ClientData clientData,
			    Tcl_Interp *interp, int objc,
			    Tcl_Obj *const objv[], int isOpen);
static int 		MakeFilter(Tcl_Interp *interp, Tcl_Obj *valuePtr,
			    Tcl_DString *dsPtr, Tcl_Obj *initialPtr,
			    int *indexPtr);
static UINT APIENTRY	OFNHookProc(HWND hdlg, UINT uMsg, WPARAM wParam,
			    LPARAM lParam);
static LRESULT CALLBACK MsgBoxCBTProc(int nCode, WPARAM wParam, LPARAM lParam);
static void		SetTkDialog(ClientData clientData);
static const char *ConvertExternalFilename(TCHAR *filename,
			    Tcl_DString *dsPtr);

/*
 *-------------------------------------------------------------------------
 *
 * EatSpuriousMessageBugFix --
 *
 *	In the file open/save dialog, double clicking on a list item causes
 *	the dialog box to close, but an unwanted WM_LBUTTONUP message is sent
 *	to the window underneath. If the window underneath happens to be a
 *	windows control (eg a button) then it will be activated by accident.
 *
 * 	This problem does not occur in dialog boxes, because windows must do
 * 	some special processing to solve the problem. (separate message
 * 	processing functions are used to cope with keyboard navigation of
 * 	controls.)
 *
 * 	Here is one solution. After returning, we poll the message queue for
 * 	1/4s looking for WM_LBUTTON up messages. If we see one it's consumed.
 * 	If we get a WM_LBUTTONDOWN message, then we exit early, since the user
 * 	must be doing something new. This fix only works for the current
 * 	application, so the problem will still occur if the open dialog
 * 	happens to be over another applications button. However this is a
 * 	fairly rare occurrance.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Consumes an unwanted BUTTON messages.
 *
 *-------------------------------------------------------------------------
 */

static void
EatSpuriousMessageBugFix(void)
{
    MSG msg;
    DWORD nTime = GetTickCount() + 250;

    while (GetTickCount() < nTime) {
	if (PeekMessageA(&msg, 0, WM_LBUTTONDOWN, WM_LBUTTONDOWN, PM_NOREMOVE)){
	    break;
	}
	PeekMessageA(&msg, 0, WM_LBUTTONUP, WM_LBUTTONUP, PM_REMOVE);
    }
}

/*
 *-------------------------------------------------------------------------
 *
 * TkWinDialogDebug --
 *
 *	Function to turn on/off debugging support for common dialogs under
 *	windows. The variable "tk_debug" is set to the identifier of the
 *	dialog window when the modal dialog window pops up and it is safe to
 *	send messages to the dialog.
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	This variable only makes sense if just one dialog is up at a time.
 *
 *-------------------------------------------------------------------------
 */

void
TkWinDialogDebug(
    int debug)
{
    ThreadSpecificData *tsdPtr =
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    tsdPtr->debugFlag = debug;
}

/*
 *-------------------------------------------------------------------------
 *
 * Tk_ChooseColorObjCmd --
 *
 *	This function implements the color dialog box for the Windows
 *	platform. See the user documentation for details on what it does.
 *
 * Results:
 *	See user documentation.
 *
 * Side effects:
 *	A dialog window is created the first time this function is called.
 *	This window is not destroyed and will be reused the next time the
 *	application invokes the "tk_chooseColor" command.
 *
 *-------------------------------------------------------------------------
 */

int
Tk_ChooseColorObjCmd(
    ClientData clientData,	/* Main window associated with interpreter. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window tkwin = clientData, parent;
    HWND hWnd;
    int i, oldMode, winCode, result;
    CHOOSECOLOR chooseColor;
    static int inited = 0;
    static COLORREF dwCustColors[16];
    static long oldColor;		/* the color selected last time */
    static const char *const optionStrings[] = {
	"-initialcolor", "-parent", "-title", NULL
    };
    enum options {
	COLOR_INITIAL, COLOR_PARENT, COLOR_TITLE
    };

    result = TCL_OK;
    if (inited == 0) {
	/*
	 * dwCustColors stores the custom color which the user can modify. We
	 * store these colors in a static array so that the next time the
	 * color dialog pops up, the same set of custom colors remain in the
	 * dialog.
	 */

	for (i = 0; i < 16; i++) {
	    dwCustColors[i] = RGB(255-i * 10, i, i * 10);
	}
	oldColor = RGB(0xa0, 0xa0, 0xa0);
	inited = 1;
    }

    parent			= tkwin;
    chooseColor.lStructSize	= sizeof(CHOOSECOLOR);
    chooseColor.hwndOwner	= NULL;
    chooseColor.hInstance	= NULL;
    chooseColor.rgbResult	= oldColor;
    chooseColor.lpCustColors	= dwCustColors;
    chooseColor.Flags		= CC_RGBINIT | CC_FULLOPEN | CC_ENABLEHOOK;
    chooseColor.lCustData	= (LPARAM) NULL;
    chooseColor.lpfnHook	= (LPOFNHOOKPROC) ColorDlgHookProc;
    chooseColor.lpTemplateName	= (LPTSTR) interp;

    for (i = 1; i < objc; i += 2) {
	int index;
	const char *string;
	Tcl_Obj *optionPtr, *valuePtr;

	optionPtr = objv[i];
	valuePtr = objv[i + 1];

	if (Tcl_GetIndexFromObjStruct(interp, optionPtr, optionStrings,
		sizeof(char *), "option", TCL_EXACT, &index) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (i + 1 == objc) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "value for \"%s\" missing", Tcl_GetString(optionPtr)));
	    Tcl_SetErrorCode(interp, "TK", "COLORDIALOG", "VALUE", NULL);
	    return TCL_ERROR;
	}

	string = Tcl_GetString(valuePtr);
	switch ((enum options) index) {
	case COLOR_INITIAL: {
	    XColor *colorPtr;

	    colorPtr = Tk_GetColor(interp, tkwin, string);
	    if (colorPtr == NULL) {
		return TCL_ERROR;
	    }
	    chooseColor.rgbResult = RGB(colorPtr->red / 0x100,
		    colorPtr->green / 0x100, colorPtr->blue / 0x100);
	    break;
	}
	case COLOR_PARENT:
	    parent = Tk_NameToWindow(interp, string, tkwin);
	    if (parent == NULL) {
		return TCL_ERROR;
	    }
	    break;
	case COLOR_TITLE:
	    chooseColor.lCustData = (LPARAM) string;
	    break;
	}
    }

    Tk_MakeWindowExist(parent);
    chooseColor.hwndOwner = NULL;
    hWnd = Tk_GetHWND(Tk_WindowId(parent));
    chooseColor.hwndOwner = hWnd;

    oldMode = Tcl_SetServiceMode(TCL_SERVICE_ALL);
    winCode = ChooseColor(&chooseColor);
    (void) Tcl_SetServiceMode(oldMode);

    /*
     * Ensure that hWnd is enabled, because it can happen that we have updated
     * the wrapper of the parent, which causes us to leave this child disabled
     * (Windows loses sync).
     */

    EnableWindow(hWnd, 1);

    /*
     * Clear the interp result since anything may have happened during the
     * modal loop.
     */

    Tcl_ResetResult(interp);

    /*
     * 3. Process the result of the dialog
     */

    if (winCode) {
	/*
	 * User has selected a color
	 */

	Tcl_SetObjResult(interp, Tcl_ObjPrintf("#%02x%02x%02x",
		GetRValue(chooseColor.rgbResult),
		GetGValue(chooseColor.rgbResult),
		GetBValue(chooseColor.rgbResult)));
	oldColor = chooseColor.rgbResult;
	result = TCL_OK;
    }

    return result;
}

/*
 *-------------------------------------------------------------------------
 *
 * ColorDlgHookProc --
 *
 *	Provides special handling of messages for the Color common dialog box.
 *	Used to set the title when the dialog first appears.
 *
 * Results:
 *	The return value is 0 if the default dialog box function should handle
 *	the message, non-zero otherwise.
 *
 * Side effects:
 *	Changes the title of the dialog window.
 *
 *----------------------------------------------------------------------
 */

static UINT CALLBACK
ColorDlgHookProc(
    HWND hDlg,			/* Handle to the color dialog. */
    UINT uMsg,			/* Type of message. */
    WPARAM wParam,		/* First message parameter. */
    LPARAM lParam)		/* Second message parameter. */
{
    ThreadSpecificData *tsdPtr =
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));
    const char *title;
    CHOOSECOLOR *ccPtr;

    if (WM_INITDIALOG == uMsg) {

	/*
	 * Set the title string of the dialog.
	 */

	ccPtr = (CHOOSECOLOR *) lParam;
	title = (const char *) ccPtr->lCustData;

	if ((title != NULL) && (title[0] != '\0')) {
	    Tcl_DString ds;

	    SetWindowText(hDlg, Tcl_WinUtfToTChar(title,-1,&ds));
	    Tcl_DStringFree(&ds);
	}
	if (tsdPtr->debugFlag) {
	    tsdPtr->debugInterp = (Tcl_Interp *) ccPtr->lpTemplateName;
	    Tcl_DoWhenIdle(SetTkDialog, hDlg);
	}
	return TRUE;
    }
    return FALSE;
}

/*
 *----------------------------------------------------------------------
 *
 * Tk_GetOpenFileCmd --
 *
 *	This function implements the "open file" dialog box for the Windows
 *	platform. See the user documentation for details on what it does.
 *
 * Results:
 *	See user documentation.
 *
 * Side effects:
 *	A dialog window is created the first this function is called.
 *
 *----------------------------------------------------------------------
 */

int
Tk_GetOpenFileObjCmd(
    ClientData clientData,	/* Main window associated with interpreter. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    return GetFileName(clientData, interp, objc, objv, 1);
}

/*
 *----------------------------------------------------------------------
 *
 * Tk_GetSaveFileCmd --
 *
 *	Same as Tk_GetOpenFileCmd but opens a "save file" dialog box
 *	instead
 *
 * Results:
 *	Same as Tk_GetOpenFileCmd.
 *
 * Side effects:
 *	Same as Tk_GetOpenFileCmd.
 *
 *----------------------------------------------------------------------
 */

int
Tk_GetSaveFileObjCmd(
    ClientData clientData,	/* Main window associated with interpreter. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    return GetFileName(clientData, interp, objc, objv, 0);
}

/*
 *----------------------------------------------------------------------
 *
 * CleanupOFNOptions --
 *
 *	Cleans up any storage allocated by ParseOFNOptions
 *
 * Results:
 *	None.
 *
 * Side effects:
 *	Releases resources held by *optsPtr
 *----------------------------------------------------------------------
 */
static void CleanupOFNOptions(OFNOpts *optsPtr)
{
    Tcl_DStringFree(&optsPtr->utfDirString);
}



/*
 *----------------------------------------------------------------------
 *
 * ParseOFNOptions --
 *
 *	Option parsing for tk_get{Open,Save}File
 *
 * Results:
 *	TCL_OK on success, TCL_ERROR otherwise
 *
 * Side effects:
 *	Returns option values in *optsPtr. Note these may include string
 *      pointers into objv[]
 *----------------------------------------------------------------------
 */

static int
ParseOFNOptions(
    ClientData clientData,	/* Main window associated with interpreter. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[],	/* Argument objects. */
    int open,			/* 1 for Open, 0 for Save */
    OFNOpts *optsPtr)           /* Output, uninitialized on entry */
{
    int i;
    Tcl_DString ds;
    enum options {
	FILE_DEFAULT, FILE_TYPES, FILE_INITDIR, FILE_INITFILE, FILE_PARENT,
	FILE_TITLE, FILE_TYPEVARIABLE, FILE_MULTIPLE, FILE_CONFIRMOW
    };
    struct Options {
	const char *name;
	enum options value;
    };
    static const struct Options saveOptions[] = {
	{"-confirmoverwrite",	FILE_CONFIRMOW},
	{"-defaultextension",	FILE_DEFAULT},
	{"-filetypes",		FILE_TYPES},
	{"-initialdir",		FILE_INITDIR},
	{"-initialfile",	FILE_INITFILE},
	{"-parent",		FILE_PARENT},
	{"-title",		FILE_TITLE},
	{"-typevariable",	FILE_TYPEVARIABLE},
	{NULL,			FILE_DEFAULT/*ignored*/ }
    };
    static const struct Options openOptions[] = {
	{"-defaultextension",	FILE_DEFAULT},
	{"-filetypes",		FILE_TYPES},
	{"-initialdir",		FILE_INITDIR},
	{"-initialfile",	FILE_INITFILE},
	{"-multiple",		FILE_MULTIPLE},
	{"-parent",		FILE_PARENT},
	{"-title",		FILE_TITLE},
	{"-typevariable",	FILE_TYPEVARIABLE},
	{NULL,			FILE_DEFAULT/*ignored*/ }
    };
    const struct Options *const options = open ? openOptions : saveOptions;

    optsPtr->tkwin = clientData;
    optsPtr->extension = NULL;
    optsPtr->title = NULL;
    optsPtr->filterObj = NULL;
    optsPtr->typeVariableObj = NULL;
    optsPtr->initialTypeObj = NULL;
    optsPtr->multi = 0;
    optsPtr->confirmOverwrite = 1; /* By default we ask for confirmation */
    Tcl_DStringInit(&optsPtr->utfDirString);
    optsPtr->file[0] = 0;

    for (i = 1; i < objc; i += 2) {
	int index;
	const char *string;
	Tcl_Obj *valuePtr = objv[i + 1];

	if (Tcl_GetIndexFromObjStruct(interp, objv[i], options,
		sizeof(struct Options), "option", 0, &index) != TCL_OK) {
	    goto end;
	} else if (i + 1 == objc) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "value for \"%s\" missing", options[index].name));
	    Tcl_SetErrorCode(interp, "TK", "FILEDIALOG", "VALUE", NULL);
	    goto end;
	}

	string = Tcl_GetString(valuePtr);
	switch (options[index].value) {
	case FILE_DEFAULT:
	    if (string[0] == '.') {
		string++;
	    }
	    optsPtr->extension = string;
	    break;
	case FILE_TYPES:
	    optsPtr->filterObj = valuePtr;
	    break;
	case FILE_INITDIR:
	    Tcl_DStringFree(&optsPtr->utfDirString);
	    if (Tcl_TranslateFileName(interp, string,
                                      &optsPtr->utfDirString) == NULL) {
		goto end;
	    }
	    break;
	case FILE_INITFILE:
	    if (Tcl_TranslateFileName(interp, string, &ds) == NULL) {
		goto end;
	    }
	    Tcl_UtfToExternal(NULL, TkWinGetUnicodeEncoding(),
                              Tcl_DStringValue(&ds), Tcl_DStringLength(&ds), 0, NULL,
                              (char *) &optsPtr->file[0], sizeof(optsPtr->file),
                              NULL, NULL, NULL);
	    Tcl_DStringFree(&ds);
	    break;
	case FILE_PARENT:
            /* XXX - check */
	    optsPtr->tkwin = Tk_NameToWindow(interp, string, clientData);
	    if (optsPtr->tkwin == NULL) {
		goto end;
	    }
	    break;
	case FILE_TITLE:
	    optsPtr->title = string;
	    break;
	case FILE_TYPEVARIABLE:
	    optsPtr->typeVariableObj = valuePtr;
	    optsPtr->initialTypeObj = Tcl_ObjGetVar2(interp, valuePtr,
                                                     NULL, TCL_GLOBAL_ONLY);
	    break;
	case FILE_MULTIPLE:
	    if (Tcl_GetBooleanFromObj(interp, valuePtr, &optsPtr->multi) != TCL_OK) {
		return TCL_ERROR;
	    }
	    break;
	case FILE_CONFIRMOW:
	    if (Tcl_GetBooleanFromObj(interp, valuePtr,
                                      &optsPtr->confirmOverwrite) != TCL_OK) {
		return TCL_ERROR;
	    }
	    break;
	}
    }

    return TCL_OK;

end:                   /* interp should already hold error */
    CleanupOFNOptions(optsPtr);
    return TCL_ERROR;
}


/*
 *----------------------------------------------------------------------
 *
 * GetFileNameVista --
 *
 *	Displays the new file dialogs on Vista and later.
 *
 * Results:
 *	TCL_OK - if dialog was successfully displayed
 *      TCL_ERROR - error return
 *      TCL_CONTINUE - new dialogs not available. Caller should go
 *               on to display the old style dialogs.
 *
 * Side effects:
 *      Dialogs is displayed and results returned in interpreter on success.
 *      COM subsystem is initialized if not already done.
 *----------------------------------------------------------------------
 */
static int GetFileNameVista(Tcl_Interp *interp, OFNOpts *optsPtr, int open)
{
    HRESULT hr;
    IFileDialog *fdlgPtr = NULL;
    ThreadSpecificData *tsdPtr =
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    if (tsdPtr->useNewFileDialogs == FDLG_STATE_USE_OLD)
        return TCL_CONTINUE;    /* Not an error, go try old style dialogs */
    
    if (tsdPtr->useNewFileDialogs == FDLG_STATE_INIT) {
        tsdPtr->useNewFileDialogs = FDLG_STATE_USE_OLD;
        
        hr = CoInitialize(0);
        /* On failures we do not error. Instead we fall back to old method */
        if (FAILED(hr))
            return TCL_CONTINUE;

        /* Verify interfaces are available */
        if (open) {
            hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL,
                        CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, &fdlgPtr);
        } else {
            hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL,
                        CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, &fdlgPtr);
        }

        if (FAILED(hr)) {
            CoUninitialize();
            return TCL_CONTINUE;
        }        

        tsdPtr->useNewFileDialogs = FDLG_STATE_USE_NEW;
        /*
         * XXX - need to arrange for CoUninitialize to be called on thread
         * exit if useNewFileDialogs is FDLG_STATE_USE_NEW.
         */
    } else {
        /* FDLG_STATE_USE_NEW */
        if (open) {
            hr = CoCreateInstance(&CLSID_FileOpenDialog, NULL,
                        CLSCTX_INPROC_SERVER, &IID_IFileOpenDialog, &fdlgPtr);
        } else {
            hr = CoCreateInstance(&CLSID_FileSaveDialog, NULL,
                        CLSCTX_INPROC_SERVER, &IID_IFileSaveDialog, &fdlgPtr);
        }
    }

    /* At this point new interfaces are supposed to be available */
    fdlgPtr->lpVtbl->Show(fdlgPtr, NULL);
    fdlgPtr->lpVtbl->Release(fdlgPtr);
    return TCL_OK;
}




/*
 *----------------------------------------------------------------------
 *
 * GetFileName --
 *
 *	Calls GetOpenFileName() or GetSaveFileName().
 *
 * Results:
 *	See user documentation.
 *
 * Side effects:
 *	See user documentation.
 *
 *----------------------------------------------------------------------
 */

static int
GetFileName(
    ClientData clientData,	/* Main window associated with interpreter. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[],	/* Argument objects. */
    int open)			/* 1 to call GetOpenFileName(), 0 to call
				 * GetSaveFileName(). */
{
    OPENFILENAME ofn;
    OFNData ofnData;
    OFNOpts ofnOpts;
    int cdlgerr;
    int filterIndex = 0, result = TCL_ERROR, winCode, oldMode;
    HWND hWnd;
    Tcl_DString utfFilterString, ds;
    Tcl_DString extString, filterString, dirString, titleString;
    ThreadSpecificData *tsdPtr =
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    ZeroMemory(&ofnData, sizeof(OFNData));
    Tcl_DStringInit(&utfFilterString);

    /* Parse the arguments. */

    result = ParseOFNOptions(clientData, interp, objc, objv, open, &ofnOpts);
    if (result != TCL_OK)
        return result;

    result = GetFileNameVista(interp, &ofnOpts, open);
    if (result != TCL_CONTINUE) {
        CleanupOFNOptions(&ofnOpts);
        return result;
    }

    if (MakeFilter(interp, ofnOpts.filterObj, &utfFilterString,
                   ofnOpts.initialTypeObj, &filterIndex) != TCL_OK) {
	goto end;
    }

    Tk_MakeWindowExist(ofnOpts.tkwin);
    hWnd = Tk_GetHWND(Tk_WindowId(ofnOpts.tkwin));

    ZeroMemory(&ofn, sizeof(OPENFILENAME));
    ofn.lStructSize = sizeof(OPENFILENAME);
    ofn.hwndOwner = hWnd;
    ofn.hInstance = TkWinGetHInstance(ofn.hwndOwner);
    ofn.lpstrFile = ofnOpts.file;
    ofn.nMaxFile = TK_MULTI_MAX_PATH;
    ofn.Flags = OFN_HIDEREADONLY | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR
	    | OFN_EXPLORER | OFN_ENABLEHOOK| OFN_ENABLESIZING;
    ofn.lpfnHook = (LPOFNHOOKPROC) OFNHookProc;
    ofn.lCustData = (LPARAM) &ofnData;

    if (open != 0) {
	ofn.Flags |= OFN_FILEMUSTEXIST;
    } else if (ofnOpts.confirmOverwrite) {
	ofn.Flags |= OFN_OVERWRITEPROMPT;
    }
    if (tsdPtr->debugFlag != 0) {
	ofnData.interp = interp;
    }
    if (ofnOpts.multi != 0) {
	ofn.Flags |= OFN_ALLOWMULTISELECT;

	/*
	 * Starting buffer size. The buffer will be expanded by the OFN dialog
	 * procedure when necessary
	 */

	ofnData.dynFileBufferSize = 512;
	ofnData.dynFileBuffer = ckalloc(512 * sizeof(TCHAR));
    }

    if (ofnOpts.extension != NULL) {
	Tcl_WinUtfToTChar(ofnOpts.extension, -1, &extString);
	ofn.lpstrDefExt = (TCHAR *) Tcl_DStringValue(&extString);
    }

    Tcl_WinUtfToTChar(Tcl_DStringValue(&utfFilterString),
	    Tcl_DStringLength(&utfFilterString), &filterString);
    ofn.lpstrFilter = (TCHAR *) Tcl_DStringValue(&filterString);
    ofn.nFilterIndex = filterIndex;

    if (Tcl_DStringValue(&ofnOpts.utfDirString)[0] != '\0') {
	Tcl_WinUtfToTChar(Tcl_DStringValue(&ofnOpts.utfDirString),
		Tcl_DStringLength(&ofnOpts.utfDirString), &dirString);
    } else {
	/*
	 * NT 5.0 changed the meaning of lpstrInitialDir, so we have to ensure
	 * that we set the [pwd] if the user didn't specify anything else.
	 */

	Tcl_DString cwd;

	Tcl_DStringFree(&ofnOpts.utfDirString);
	if ((Tcl_GetCwd(interp, &ofnOpts.utfDirString) == NULL) ||
		(Tcl_TranslateFileName(interp,
                     Tcl_DStringValue(&ofnOpts.utfDirString), &cwd) == NULL)) {
	    Tcl_ResetResult(interp);
	} else {
	    Tcl_WinUtfToTChar(Tcl_DStringValue(&cwd),
		    Tcl_DStringLength(&cwd), &dirString);
	}
	Tcl_DStringFree(&cwd);
    }
    ofn.lpstrInitialDir = (TCHAR *) Tcl_DStringValue(&dirString);

    if (ofnOpts.title != NULL) {
	Tcl_WinUtfToTChar(ofnOpts.title, -1, &titleString);
	ofn.lpstrTitle = (TCHAR *) Tcl_DStringValue(&titleString);
    }

    /*
     * Popup the dialog.
     */

    oldMode = Tcl_SetServiceMode(TCL_SERVICE_ALL);
    if (open != 0) {
	winCode = GetOpenFileName(&ofn);
    } else {
	winCode = GetSaveFileName(&ofn);
    }
    Tcl_SetServiceMode(oldMode);
    EatSpuriousMessageBugFix();

    /*
     * Ensure that hWnd is enabled, because it can happen that we have updated
     * the wrapper of the parent, which causes us to leave this child disabled
     * (Windows loses sync).
     */

    EnableWindow(hWnd, 1);

    /*
     * Clear the interp result since anything may have happened during the
     * modal loop.
     */

    Tcl_ResetResult(interp);

    /*
     * Process the results.
     *
     * Use the CommDlgExtendedError() function to retrieve the error code.
     * This function can return one of about two dozen codes; most of these
     * indicate some sort of gross system failure (insufficient memory, bad
     * window handles, etc.). Most of the error codes will be ignored; as we
     * find we want more specific error messages for particular errors, we can
     * extend the code as needed.
     */

    cdlgerr = CommDlgExtendedError();

    /*
     * We now allow FNERR_BUFFERTOOSMALL when multiselection is enabled. The
     * filename buffer has been dynamically allocated by the OFN dialog
     * procedure to accomodate all selected files.
     */

    if ((winCode != 0)
	    || ((cdlgerr == FNERR_BUFFERTOOSMALL)
		    && (ofn.Flags & OFN_ALLOWMULTISELECT))) {
	int gotFilename = 0;	/* Flag for tracking whether we have any
				 * filename at all. For details, see
				 * http://stackoverflow.com/q/9227859/301832
				 */

	if (ofn.Flags & OFN_ALLOWMULTISELECT) {
	    /*
	     * The result in dynFileBuffer contains many items, separated by
	     * NUL characters. It is terminated with two nulls in a row. The
	     * first element is the directory path.
	     */

	    TCHAR *files = ofnData.dynFileBuffer;
	    Tcl_Obj *returnList = Tcl_NewObj();
	    int count = 0;

	    /*
	     * Get directory.
	     */

	    ConvertExternalFilename(files, &ds);

	    while (*files != '\0') {
		while (*files != '\0') {
		    files++;
		}
		files++;
		if (*files != '\0') {
		    Tcl_Obj *fullnameObj;
		    Tcl_DString filenameBuf;

		    count++;
		    ConvertExternalFilename(files, &filenameBuf);

		    fullnameObj = Tcl_NewStringObj(Tcl_DStringValue(&ds),
			    Tcl_DStringLength(&ds));
		    Tcl_AppendToObj(fullnameObj, "/", -1);
		    Tcl_AppendToObj(fullnameObj, Tcl_DStringValue(&filenameBuf),
			    Tcl_DStringLength(&filenameBuf));
		    gotFilename = 1;
		    Tcl_DStringFree(&filenameBuf);
		    Tcl_ListObjAppendElement(NULL, returnList, fullnameObj);
		}
	    }

	    if (count == 0) {
		/*
		 * Only one file was returned.
		 */

		Tcl_ListObjAppendElement(NULL, returnList,
			Tcl_NewStringObj(Tcl_DStringValue(&ds),
				Tcl_DStringLength(&ds)));
		gotFilename |= (Tcl_DStringLength(&ds) > 0);
	    }
	    Tcl_SetObjResult(interp, returnList);
	    Tcl_DStringFree(&ds);
	} else {
	    Tcl_SetObjResult(interp, Tcl_NewStringObj(
		    ConvertExternalFilename(ofn.lpstrFile, &ds), -1));
	    gotFilename = (Tcl_DStringLength(&ds) > 0);
	    Tcl_DStringFree(&ds);
	}
	result = TCL_OK;
	if ((ofn.nFilterIndex > 0) && gotFilename && ofnOpts.typeVariableObj
		&& ofnOpts.filterObj) {
	    int listObjc, count;
	    Tcl_Obj **listObjv = NULL;
	    Tcl_Obj **typeInfo = NULL;

	    if (Tcl_ListObjGetElements(interp, ofnOpts.filterObj,
		    &listObjc, &listObjv) != TCL_OK) {
		result = TCL_ERROR;
	    } else if (Tcl_ListObjGetElements(interp,
		    listObjv[ofn.nFilterIndex - 1], &count,
		    &typeInfo) != TCL_OK) {
		result = TCL_ERROR;
	    } else if (Tcl_ObjSetVar2(interp, ofnOpts.typeVariableObj, NULL,
                   typeInfo[0], TCL_GLOBAL_ONLY|TCL_LEAVE_ERR_MSG) == NULL) {
		result = TCL_ERROR;
	    }
	}
    } else if (cdlgerr == FNERR_INVALIDFILENAME) {
	Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		"invalid filename \"%s\"",
		ConvertExternalFilename(ofn.lpstrFile, &ds)));
	Tcl_SetErrorCode(interp, "TK", "FILEDIALOG", "INVALID_FILENAME",
		NULL);
	Tcl_DStringFree(&ds);
    } else {
	result = TCL_OK;
    }

    if (ofn.lpstrTitle != NULL) {
	Tcl_DStringFree(&titleString);
    }
    if (ofn.lpstrInitialDir != NULL) {
	Tcl_DStringFree(&dirString);
    }
    Tcl_DStringFree(&filterString);
    if (ofn.lpstrDefExt != NULL) {
	Tcl_DStringFree(&extString);
    }

  end:
    Tcl_DStringFree(&utfFilterString);
    if (ofnData.dynFileBuffer != NULL) {
	ckfree(ofnData.dynFileBuffer);
	ofnData.dynFileBuffer = NULL;
    }
    CleanupOFNOptions(&ofnOpts);

    return result;
}

/*
 *-------------------------------------------------------------------------
 *
 * OFNHookProc --
 *
 *	Dialog box hook function. This is used to sets the "tk_dialog"
 *	variable for test/debugging when the dialog is ready to receive
 *	messages. When multiple file selection is enabled this function
 *	is used to process the list of names.
 *
 * Results:
 *	Returns 0 to allow default processing of messages to occur.
 *
 * Side effects:
 *	None.
 *
 *-------------------------------------------------------------------------
 */

static UINT APIENTRY
OFNHookProc(
    HWND hdlg,			/* Handle to child dialog window. */
    UINT uMsg,			/* Message identifier */
    WPARAM wParam,		/* Message parameter */
    LPARAM lParam)		/* Message parameter */
{
    ThreadSpecificData *tsdPtr =
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));
    OPENFILENAME *ofnPtr;
    OFNData *ofnData;

    if (uMsg == WM_INITDIALOG) {
	TkWinSetUserData(hdlg, lParam);
    } else if (uMsg == WM_NOTIFY) {
	OFNOTIFY *notifyPtr = (OFNOTIFY *) lParam;

	/*
	 * This is weird... or not. The CDN_FILEOK is NOT sent when the
	 * selection exceeds declared buffer size (the nMaxFile member of the
	 * OPENFILENAME struct passed to GetOpenFileName function). So, we
	 * have to rely on the most recent CDN_SELCHANGE then. Unfortunately
	 * this means, that gathering the selected filenames happens twice
	 * when they fit into the declared buffer. Luckily, it's not frequent
	 * operation so it should not incur any noticeable delay. See [Bug
	 * 2987995]
	 */

	if (notifyPtr->hdr.code == CDN_FILEOK ||
		notifyPtr->hdr.code == CDN_SELCHANGE) {
	    int dirsize, selsize;
	    TCHAR *buffer;
	    int buffersize;

	    /*
	     * Change of selection. Unscramble the unholy mess that's in the
	     * selection buffer, resizing it if necessary.
	     */

	    ofnPtr = notifyPtr->lpOFN;
	    ofnData = (OFNData *) ofnPtr->lCustData;
	    buffer = ofnData->dynFileBuffer;
	    hdlg = GetParent(hdlg);

	    selsize = SendMessage(hdlg, CDM_GETSPEC, 0, 0);
	    dirsize = SendMessage(hdlg, CDM_GETFOLDERPATH, 0, 0);
	    buffersize = (selsize + dirsize + 1);

	    /*
	     * Just empty the buffer if dirsize indicates an error. [Bug
	     * 3071836]
	     */

	    if ((selsize > 1) && (dirsize > 0)) {
		if (ofnData->dynFileBufferSize < buffersize) {
		    buffer = ckrealloc(buffer, buffersize * sizeof(TCHAR));
		    ofnData->dynFileBufferSize = buffersize;
		    ofnData->dynFileBuffer = buffer;
		}

		SendMessage(hdlg, CDM_GETFOLDERPATH, dirsize, (LPARAM) buffer);
		buffer += dirsize;

		SendMessage(hdlg, CDM_GETSPEC, selsize, (LPARAM) buffer);

		/*
		 * If there are multiple files, delete the quotes and change
		 * every second quote to NULL terminator
		 */

		if (buffer[0] == '"') {
		    BOOL findquote = TRUE;
		    TCHAR *tmp = buffer;

		    while (*buffer != '\0') {
			if (findquote) {
			    if (*buffer == '"') {
				findquote = FALSE;
			    }
			    buffer++;
			} else {
			    if (*buffer == '"') {
				findquote = TRUE;
				*buffer = '\0';
			    }
			    *tmp++ = *buffer++;
			}
		    }
		    *tmp = '\0';		/* Second NULL terminator. */
		} else {

			/*
		     * Replace directory terminating NULL with a with a backslash,
		     * but only if not an absolute path.
		     */

		    Tcl_DString tmpfile;
		    ConvertExternalFilename(buffer, &tmpfile);
		    if (TCL_PATH_ABSOLUTE ==
			    Tcl_GetPathType(Tcl_DStringValue(&tmpfile))) {
			/* re-get the full path to the start of the buffer */
			buffer = (TCHAR *) ofnData->dynFileBuffer;
			SendMessage(hdlg, CDM_GETSPEC, selsize, (LPARAM) buffer);
		    } else {
			*(buffer-1) = '\\';
		    }
		    buffer[selsize] = '\0'; /* Second NULL terminator. */
		    Tcl_DStringFree(&tmpfile);
		}
	    } else {
		/*
		 * Nothing is selected, so just empty the string.
		 */

		if (buffer != NULL) {
		    *buffer = '\0';
		}
	    }
	}
    } else if (uMsg == WM_WINDOWPOSCHANGED) {
	/*
	 * This message is delivered at the right time to enable Tk to set the
	 * debug information. Unhooks itself so it won't set the debug
	 * information every time it gets a WM_WINDOWPOSCHANGED message.
	 */

	ofnPtr = (OPENFILENAME *) TkWinGetUserData(hdlg);
	if (ofnPtr != NULL) {
	    ofnData = (OFNData *) ofnPtr->lCustData;
	    if (ofnData->interp != NULL) {
		hdlg = GetParent(hdlg);
		tsdPtr->debugInterp = ofnData->interp;
		Tcl_DoWhenIdle(SetTkDialog, hdlg);
	    }
	    TkWinSetUserData(hdlg, NULL);
	}
    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * MakeFilter --
 *
 *	Allocate a buffer to store the filters in a format understood by
 *	Windows.
 *
 * Results:
 *	A standard TCL return value.
 *
 * Side effects:
 *	ofnPtr->lpstrFilter is modified.
 *
 *----------------------------------------------------------------------
 */

static int
MakeFilter(
    Tcl_Interp *interp,		/* Current interpreter. */
    Tcl_Obj *valuePtr,		/* Value of the -filetypes option */
    Tcl_DString *dsPtr,		/* Filled with windows filter string. */
    Tcl_Obj *initialPtr,	/* Initial type name  */
    int *indexPtr)		/* Index of initial type in filter string */
{
    char *filterStr;
    char *p;
    const char *initial = NULL;
    int pass;
    int ix = 0; /* index counter */
    FileFilterList flist;
    FileFilter *filterPtr;

    if (initialPtr) {
	initial = Tcl_GetString(initialPtr);
    }
    TkInitFileFilters(&flist);
    if (TkGetFileFilters(interp, &flist, valuePtr, 1) != TCL_OK) {
	return TCL_ERROR;
    }

    if (flist.filters == NULL) {
	/*
	 * Use "All Files (*.*) as the default filter if none is specified
	 */
	const char *defaultFilter = "All Files (*.*)";

	p = filterStr = ckalloc(30);

	strcpy(p, defaultFilter);
	p+= strlen(defaultFilter);

	*p++ = '\0';
	*p++ = '*';
	*p++ = '.';
	*p++ = '*';
	*p++ = '\0';
	*p++ = '\0';
	*p = '\0';

    } else {
	size_t len;

	if (valuePtr == NULL) {
	    len = 0;
	} else {
	    (void) Tcl_GetString(valuePtr);
	    len = valuePtr->length;
	}

	/*
	 * We format the filetype into a string understood by Windows: {"Text
	 * Documents" {.doc .txt} {TEXT}} becomes "Text Documents
	 * (*.doc,*.txt)\0*.doc;*.txt\0"
	 *
	 * See the Windows OPENFILENAME manual page for details on the filter
	 * string format.
	 */

	/*
	 * Since we may only add asterisks (*) to the filter, we need at most
	 * twice the size of the string to format the filter
	 */

	filterStr = ckalloc(len * 3);

	for (filterPtr = flist.filters, p = filterStr; filterPtr;
		filterPtr = filterPtr->next) {
	    const char *sep;
	    FileFilterClause *clausePtr;

	    /*
	     * Check initial index for match, set *indexPtr. Filter index is 1
	     * based so increment first
	     */

	    ix++;
	    if (indexPtr && initial
		    && (strcmp(initial, filterPtr->name) == 0)) {
		*indexPtr = ix;
	    }

	    /*
	     * First, put in the name of the file type.
	     */

	    strcpy(p, filterPtr->name);
	    p+= strlen(filterPtr->name);
	    *p++ = ' ';
	    *p++ = '(';

	    for (pass = 1; pass <= 2; pass++) {
		/*
		 * In the first pass, we format the extensions in the name
		 * field. In the second pass, we format the extensions in the
		 * filter pattern field
		 */

		sep = "";
		for (clausePtr=filterPtr->clauses;clausePtr;
			clausePtr=clausePtr->next) {
		    GlobPattern *globPtr;

		    for (globPtr = clausePtr->patterns; globPtr;
			    globPtr = globPtr->next) {
			strcpy(p, sep);
			p += strlen(sep);
			strcpy(p, globPtr->pattern);
			p += strlen(globPtr->pattern);

			if (pass == 1) {
			    sep = ",";
			} else {
			    sep = ";";
			}
		    }
		}
		if (pass == 1) {
		    *p ++ = ')';
		}
		*p++ = '\0';
	    }
	}

	/*
	 * Windows requires the filter string to be ended by two NULL
	 * characters.
	 */

	*p++ = '\0';
	*p = '\0';
    }

    Tcl_DStringAppend(dsPtr, filterStr, (int) (p - filterStr));
    ckfree(filterStr);

    TkFreeFileFilters(&flist);
    return TCL_OK;
}

/*
 *----------------------------------------------------------------------
 *
 * Tk_ChooseDirectoryObjCmd --
 *
 *	This function implements the "tk_chooseDirectory" dialog box for the
 *	Windows platform. See the user documentation for details on what it
 *	does. Uses the newer SHBrowseForFolder explorer type interface.
 *
 * Results:
 *	See user documentation.
 *
 * Side effects:
 *	A modal dialog window is created. Tcl_SetServiceMode() is called to
 *	allow background events to be processed
 *
 *----------------------------------------------------------------------
 *
 * The function tk_chooseDirectory pops up a dialog box for the user to select
 * a directory. The following option-value pairs are possible as command line
 * arguments:
 *
 * -initialdir dirname
 *
 * Specifies that the directories in directory should be displayed when the
 * dialog pops up. If this parameter is not specified, then the directories in
 * the current working directory are displayed. If the parameter specifies a
 * relative path, the return value will convert the relative path to an
 * absolute path. This option may not always work on the Macintosh. This is
 * not a bug. Rather, the General Controls control panel on the Mac allows the
 * end user to override the application default directory.
 *
 * -parent window
 *
 * Makes window the logical parent of the dialog. The dialog is displayed on
 * top of its parent window.
 *
 * -title titleString
 *
 * Specifies a string to display as the title of the dialog box. If this
 * option is not specified, then a default title will be displayed.
 *
 * -mustexist boolean
 *
 * Specifies whether the user may specify non-existant directories. If this
 * parameter is true, then the user may only select directories that already
 * exist. The default value is false.
 *
 * New Behaviour:
 *
 * - If mustexist = 0 and a user entered folder does not exist, a prompt will
 *   pop-up asking if the user wants another chance to change it. The old
 *   dialog just returned the bogus entry. On mustexist = 1, the entries MUST
 *   exist before exiting the box with OK.
 *
 *   Bugs:
 *
 * - If valid abs directory name is entered into the entry box and Enter
 *   pressed, the box will close returning the name. This is inconsistent when
 *   entering relative names or names with forward slashes, which are
 *   invalidated then corrected in the callback. After correction, the box is
 *   held open to allow further modification by the user.
 *
 * - Not sure how to implement localization of message prompts.
 *
 * - -title is really -message.
 *
 *----------------------------------------------------------------------
 */

int
Tk_ChooseDirectoryObjCmd(
    ClientData clientData,	/* Main window associated with interpreter. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    TCHAR path[MAX_PATH];
    int oldMode, result = TCL_ERROR, i;
    LPCITEMIDLIST pidl;		/* Returned by browser */
    BROWSEINFO bInfo;		/* Used by browser */
    ChooseDir cdCBData;	    /* Structure to pass back and forth */
    LPMALLOC pMalloc;		/* Used by shell */
    Tk_Window tkwin = clientData;
    HWND hWnd;
    const char *utfTitle = NULL;/* Title for window */
    TCHAR saveDir[MAX_PATH];
    Tcl_DString titleString;	/* Title */
    Tcl_DString initDirString;	/* Initial directory */
    Tcl_DString tempString;	/* temporary */
    Tcl_Obj *objPtr;
    static const char *const optionStrings[] = {
	"-initialdir", "-mustexist",  "-parent",  "-title", NULL
    };
    enum options {
	DIR_INITIAL,   DIR_EXIST,  DIR_PARENT, FILE_TITLE
    };

    /*
     * Initialize
     */

    path[0] = '\0';
    ZeroMemory(&cdCBData, sizeof(ChooseDir));
    cdCBData.interp = interp;

    /*
     * Process the command line options
     */

    for (i = 1; i < objc; i += 2) {
	int index;
	const char *string;
	const TCHAR *uniStr;
	Tcl_Obj *optionPtr, *valuePtr;

	optionPtr = objv[i];
	valuePtr = objv[i + 1];

	if (Tcl_GetIndexFromObjStruct(interp, optionPtr, optionStrings,
		sizeof(char *), "option", 0, &index) != TCL_OK) {
	    goto cleanup;
	}
	if (i + 1 == objc) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "value for \"%s\" missing", Tcl_GetString(optionPtr)));
	    Tcl_SetErrorCode(interp, "TK", "DIRDIALOG", "VALUE", NULL);
	    goto cleanup;
	}

	string = Tcl_GetString(valuePtr);
	switch ((enum options) index) {
	case DIR_INITIAL:
	    if (Tcl_TranslateFileName(interp,string,&initDirString) == NULL) {
		goto cleanup;
	    }
	    Tcl_WinUtfToTChar(Tcl_DStringValue(&initDirString), -1,
		    &tempString);
	    uniStr = (TCHAR *) Tcl_DStringValue(&tempString);

	    /*
	     * Convert possible relative path to full path to keep dialog
	     * happy.
	     */

	    GetFullPathName(uniStr, MAX_PATH, saveDir, NULL);
	    _tcsncpy(cdCBData.initDir, saveDir, MAX_PATH);
	    Tcl_DStringFree(&initDirString);
	    Tcl_DStringFree(&tempString);
	    break;
	case DIR_EXIST:
	    if (Tcl_GetBooleanFromObj(interp, valuePtr,
		    &cdCBData.mustExist) != TCL_OK) {
		goto cleanup;
	    }
	    break;
	case DIR_PARENT:
	    tkwin = Tk_NameToWindow(interp, string, tkwin);
	    if (tkwin == NULL) {
		goto cleanup;
	    }
	    break;
	case FILE_TITLE:
	    utfTitle = string;
	    break;
	}
    }

    /*
     * Get ready to call the browser
     */

    Tk_MakeWindowExist(tkwin);
    hWnd = Tk_GetHWND(Tk_WindowId(tkwin));

    /*
     * Setup the parameters used by SHBrowseForFolder
     */

    bInfo.hwndOwner = hWnd;
    bInfo.pszDisplayName = path;
    bInfo.pidlRoot = NULL;
    if (_tcslen(cdCBData.initDir) == 0) {
	GetCurrentDirectory(MAX_PATH, cdCBData.initDir);
    }
    bInfo.lParam = (LPARAM) &cdCBData;

    if (utfTitle != NULL) {
	Tcl_WinUtfToTChar(utfTitle, -1, &titleString);
	bInfo.lpszTitle = (LPTSTR) Tcl_DStringValue(&titleString);
    } else {
	bInfo.lpszTitle = TEXT("Please choose a directory, then select OK.");
    }

    /*
     * Set flags to add edit box, status text line and use the new ui. Allow
     * override with magic variable (ignore errors in retrieval). See
     * http://msdn.microsoft.com/en-us/library/bb773205(VS.85).aspx for
     * possible flag values.
     */

    bInfo.ulFlags = BIF_EDITBOX | BIF_STATUSTEXT | BIF_RETURNFSANCESTORS
	| BIF_VALIDATE | BIF_NEWDIALOGSTYLE;
    objPtr = Tcl_GetVar2Ex(interp, "::tk::winChooseDirFlags", NULL,
	    TCL_GLOBAL_ONLY);
    if (objPtr != NULL) {
	int flags;
	Tcl_GetIntFromObj(NULL, objPtr, &flags);
	bInfo.ulFlags = flags;
    }

    /*
     * Callback to handle events
     */

    bInfo.lpfn = (BFFCALLBACK) ChooseDirectoryValidateProc;

    /*
     * Display dialog in background and process result. We look to give the
     * user a chance to change their mind on an invalid folder if mustexist is
     * 0.
     */

    oldMode = Tcl_SetServiceMode(TCL_SERVICE_ALL);
    GetCurrentDirectory(MAX_PATH, saveDir);
    if (SHGetMalloc(&pMalloc) == NOERROR) {
	pidl = SHBrowseForFolder(&bInfo);

	/*
	 * This is a fix for Windows 2000, which seems to modify the folder
	 * name buffer even when the dialog is canceled (in this case the
	 * buffer contains garbage). See [Bug #3002230]
	 */

	path[0] = '\0';

	/*
	 * Null for cancel button or invalid dir, otherwise valid.
	 */

	if (pidl != NULL) {
	    if (!SHGetPathFromIDList(pidl, path)) {
		Tcl_SetObjResult(interp, Tcl_NewStringObj(
			"error: not a file system folder", -1));
		Tcl_SetErrorCode(interp, "TK", "DIRDIALOG", "PSEUDO", NULL);
	    }
	    pMalloc->lpVtbl->Free(pMalloc, (void *) pidl);
	} else if (_tcslen(cdCBData.retDir) > 0) {
	    _tcscpy(path, cdCBData.retDir);
	}
	pMalloc->lpVtbl->Release(pMalloc);
    }
    SetCurrentDirectory(saveDir);
    Tcl_SetServiceMode(oldMode);

    /*
     * Ensure that hWnd is enabled, because it can happen that we have updated
     * the wrapper of the parent, which causes us to leave this child disabled
     * (Windows loses sync).
     */

    EnableWindow(hWnd, 1);

    /*
     * Change the pathname to the Tcl "normalized" pathname, where back
     * slashes are used instead of forward slashes
     */

    Tcl_ResetResult(interp);
    if (*path) {
	Tcl_DString ds;

	Tcl_SetObjResult(interp, Tcl_NewStringObj(
		ConvertExternalFilename(path, &ds), -1));
	Tcl_DStringFree(&ds);
    }

    result = TCL_OK;

    if (utfTitle != NULL) {
	Tcl_DStringFree(&titleString);
    }

  cleanup:
    return result;
}

/*
 *----------------------------------------------------------------------
 *
 * ChooseDirectoryValidateProc --
 *
 *	Hook function called by the explorer ChooseDirectory dialog when
 *	events occur. It is used to validate the text entry the user may have
 *	entered.
 *
 * Results:
 *	Returns 0 to allow default processing of message, or 1 to tell default
 *	dialog function not to close.
 *
 *----------------------------------------------------------------------
 */

static UINT APIENTRY
ChooseDirectoryValidateProc(
    HWND hwnd,
    UINT message,
    LPARAM lParam,
    LPARAM lpData)
{
    TCHAR selDir[MAX_PATH];
    ChooseDir *chooseDirSharedData = (ChooseDir *) lpData;
    Tcl_DString tempString;
    Tcl_DString initDirString;
    TCHAR string[MAX_PATH];
    ThreadSpecificData *tsdPtr =
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    if (tsdPtr->debugFlag) {
	tsdPtr->debugInterp = (Tcl_Interp *) chooseDirSharedData->interp;
	Tcl_DoWhenIdle(SetTkDialog, hwnd);
    }
    chooseDirSharedData->retDir[0] = '\0';
    switch (message) {
    case BFFM_VALIDATEFAILED:
	/*
	 * First save and check to see if it is a valid path name, if so then
	 * make that path the one shown in the window. Otherwise, it failed
	 * the check and should be treated as such. Use
	 * Set/GetCurrentDirectory which allows relative path names and names
	 * with forward slashes. Use Tcl_TranslateFileName to make sure names
	 * like ~ are converted correctly.
	 */

	Tcl_WinTCharToUtf((TCHAR *) lParam, -1, &initDirString);
	if (Tcl_TranslateFileName(chooseDirSharedData->interp,
		Tcl_DStringValue(&initDirString), &tempString) == NULL) {
	    /*
	     * Should we expose the error (in the interp result) to the user
	     * at this point?
	     */

	    chooseDirSharedData->retDir[0] = '\0';
	    return 1;
	}
	Tcl_DStringFree(&initDirString);
	Tcl_WinUtfToTChar(Tcl_DStringValue(&tempString), -1, &initDirString);
	Tcl_DStringFree(&tempString);
	_tcsncpy(string, (TCHAR *) Tcl_DStringValue(&initDirString),
		MAX_PATH);
	Tcl_DStringFree(&initDirString);

	if (SetCurrentDirectory(string) == 0) {

	    /*
	     * Get the full path name to the user entry, at this point it does
	     * not exist so see if it is supposed to. Otherwise just return
	     * it.
	     */

	    GetFullPathName(string, MAX_PATH,
		    chooseDirSharedData->retDir, NULL);
	    if (chooseDirSharedData->mustExist) {
		/*
		 * User HAS to select a valid directory.
		 */

		wsprintf(selDir, TEXT("Directory '%s' does not exist,\n")
		        TEXT("please select or enter an existing directory."),
			chooseDirSharedData->retDir);
		MessageBox(NULL, selDir, NULL, MB_ICONEXCLAMATION|MB_OK);
		chooseDirSharedData->retDir[0] = '\0';
		return 1;
	    }
	} else {
	    /*
	     * Changed to new folder OK, return immediatly with the current
	     * directory in utfRetDir.
	     */

	    GetCurrentDirectory(MAX_PATH, chooseDirSharedData->retDir);
	    return 0;
	}
	return 0;

    case BFFM_SELCHANGED:
	/*
	 * Set the status window to the currently selected path and enable the
	 * OK button if a file system folder, otherwise disable the OK button
	 * for things like server names. Perhaps a new switch
	 * -enablenonfolders can be used to allow non folders to be selected.
	 *
	 * Not called when user changes edit box directly.
	 */

	if (SHGetPathFromIDList((LPITEMIDLIST) lParam, selDir)) {
	    SendMessage(hwnd, BFFM_SETSTATUSTEXT, 0, (LPARAM) selDir);
	    // enable the OK button
	    SendMessage(hwnd, BFFM_ENABLEOK, 0, (LPARAM) 1);
	} else {
	    // disable the OK button
	    SendMessage(hwnd, BFFM_ENABLEOK, 0, (LPARAM) 0);
	}
	UpdateWindow(hwnd);
	return 1;

    case BFFM_INITIALIZED: {
	/*
	 * Directory browser intializing - tell it where to start from, user
	 * specified parameter.
	 */

	TCHAR *initDir = chooseDirSharedData->initDir;

	SetCurrentDirectory(initDir);

	if (*initDir == '\\') {
	    /*
	     * BFFM_SETSELECTION only understands UNC paths as pidls, so
	     * convert path to pidl using IShellFolder interface.
	     */

	    LPMALLOC pMalloc;
	    LPSHELLFOLDER psfFolder;

	    if (SUCCEEDED(SHGetMalloc(&pMalloc))) {
		if (SUCCEEDED(SHGetDesktopFolder(&psfFolder))) {
		    LPITEMIDLIST pidlMain;
		    ULONG ulCount, ulAttr;

		    if (SUCCEEDED(psfFolder->lpVtbl->ParseDisplayName(
			    psfFolder, hwnd, NULL, (TCHAR *)
			    initDir, &ulCount,&pidlMain,&ulAttr))
			    && (pidlMain != NULL)) {
			SendMessage(hwnd, BFFM_SETSELECTION, FALSE,
				(LPARAM) pidlMain);
			pMalloc->lpVtbl->Free(pMalloc, pidlMain);
		    }
		    psfFolder->lpVtbl->Release(psfFolder);
		}
		pMalloc->lpVtbl->Release(pMalloc);
	    }
	} else {
	    SendMessage(hwnd, BFFM_SETSELECTION, TRUE, (LPARAM) initDir);
	}
	SendMessage(hwnd, BFFM_ENABLEOK, 0, (LPARAM) 1);
	break;
    }

    }
    return 0;
}

/*
 *----------------------------------------------------------------------
 *
 * Tk_MessageBoxObjCmd --
 *
 *	This function implements the MessageBox window for the Windows
 *	platform. See the user documentation for details on what it does.
 *
 * Results:
 *	See user documentation.
 *
 * Side effects:
 *	None. The MessageBox window will be destroy before this function
 *	returns.
 *
 *----------------------------------------------------------------------
 */

int
Tk_MessageBoxObjCmd(
    ClientData clientData,	/* Main window associated with interpreter. */
    Tcl_Interp *interp,		/* Current interpreter. */
    int objc,			/* Number of arguments. */
    Tcl_Obj *const objv[])	/* Argument objects. */
{
    Tk_Window tkwin = clientData, parent;
    HWND hWnd;
    Tcl_Obj *messageObj, *titleObj, *detailObj, *tmpObj;
    int defaultBtn, icon, type;
    int i, oldMode, winCode;
    UINT flags;
    static const char *const optionStrings[] = {
	"-default",	"-detail",	"-icon",	"-message",
	"-parent",	"-title",	"-type",	NULL
    };
    enum options {
	MSG_DEFAULT,	MSG_DETAIL,	MSG_ICON,	MSG_MESSAGE,
	MSG_PARENT,	MSG_TITLE,	MSG_TYPE
    };
    ThreadSpecificData *tsdPtr =
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    defaultBtn = -1;
    detailObj = NULL;
    icon = MB_ICONINFORMATION;
    messageObj = NULL;
    parent = tkwin;
    titleObj = NULL;
    type = MB_OK;

    for (i = 1; i < objc; i += 2) {
	int index;
	Tcl_Obj *optionPtr, *valuePtr;

	optionPtr = objv[i];
	valuePtr = objv[i + 1];

	if (Tcl_GetIndexFromObjStruct(interp, optionPtr, optionStrings,
		sizeof(char *), "option", TCL_EXACT, &index) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (i + 1 == objc) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "value for \"%s\" missing", Tcl_GetString(optionPtr)));
	    Tcl_SetErrorCode(interp, "TK", "MSGBOX", "VALUE", NULL);
	    return TCL_ERROR;
	}

	switch ((enum options) index) {
	case MSG_DEFAULT:
	    defaultBtn = TkFindStateNumObj(interp, optionPtr, buttonMap,
		    valuePtr);
	    if (defaultBtn < 0) {
		return TCL_ERROR;
	    }
	    break;

	case MSG_DETAIL:
	    detailObj = valuePtr;
	    break;

	case MSG_ICON:
	    icon = TkFindStateNumObj(interp, optionPtr, iconMap, valuePtr);
	    if (icon < 0) {
		return TCL_ERROR;
	    }
	    break;

	case MSG_MESSAGE:
	    messageObj = valuePtr;
	    break;

	case MSG_PARENT:
	    parent = Tk_NameToWindow(interp, Tcl_GetString(valuePtr), tkwin);
	    if (parent == NULL) {
		return TCL_ERROR;
	    }
	    break;

	case MSG_TITLE:
	    titleObj = valuePtr;
	    break;

	case MSG_TYPE:
	    type = TkFindStateNumObj(interp, optionPtr, typeMap, valuePtr);
	    if (type < 0) {
		return TCL_ERROR;
	    }
	    break;
	}
    }

    while (!Tk_IsTopLevel(parent)) {
	parent = Tk_Parent(parent);
    }
    Tk_MakeWindowExist(parent);
    hWnd = Tk_GetHWND(Tk_WindowId(parent));

    flags = 0;
    if (defaultBtn >= 0) {
	int defaultBtnIdx = -1;

	for (i = 0; i < (int) NUM_TYPES; i++) {
	    if (type == allowedTypes[i].type) {
		int j;

		for (j = 0; j < 3; j++) {
		    if (allowedTypes[i].btnIds[j] == defaultBtn) {
			defaultBtnIdx = j;
			break;
		    }
		}
		if (defaultBtnIdx < 0) {
		    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
			    "invalid default button \"%s\"",
			    TkFindStateString(buttonMap, defaultBtn)));
		    Tcl_SetErrorCode(interp, "TK", "MSGBOX", "DEFAULT", NULL);
		    return TCL_ERROR;
		}
		break;
	    }
	}
	flags = buttonFlagMap[defaultBtnIdx];
    }

    flags |= icon | type | MB_TASKMODAL | MB_SETFOREGROUND;

    tmpObj = messageObj ? Tcl_DuplicateObj(messageObj)
	    : Tcl_NewUnicodeObj(NULL, 0);
    Tcl_IncrRefCount(tmpObj);
    if (detailObj) {
	Tcl_AppendUnicodeToObj(tmpObj, L"\n\n", 2);
	Tcl_AppendObjToObj(tmpObj, detailObj);
    }

    oldMode = Tcl_SetServiceMode(TCL_SERVICE_ALL);

    /*
     * MessageBoxW exists for all platforms. Use it to allow unicode error
     * message to be displayed correctly where possible by the OS.
     *
     * In order to have the parent window icon reflected in a MessageBox, we
     * have to create a hook that will trigger when the MessageBox is being
     * created.
     */

    tsdPtr->hSmallIcon = TkWinGetIcon(parent, ICON_SMALL);
    tsdPtr->hBigIcon   = TkWinGetIcon(parent, ICON_BIG);
    tsdPtr->hMsgBoxHook = SetWindowsHookEx(WH_CBT, MsgBoxCBTProc, NULL,
	    GetCurrentThreadId());
    winCode = MessageBox(hWnd, Tcl_GetUnicode(tmpObj),
	    titleObj ? Tcl_GetUnicode(titleObj) : L"", flags);
    UnhookWindowsHookEx(tsdPtr->hMsgBoxHook);
    (void) Tcl_SetServiceMode(oldMode);

    /*
     * Ensure that hWnd is enabled, because it can happen that we have updated
     * the wrapper of the parent, which causes us to leave this child disabled
     * (Windows loses sync).
     */

    EnableWindow(hWnd, 1);

    Tcl_DecrRefCount(tmpObj);
    Tcl_SetObjResult(interp, Tcl_NewStringObj(
	    TkFindStateString(buttonMap, winCode), -1));
    return TCL_OK;
}

static LRESULT CALLBACK
MsgBoxCBTProc(
    int nCode,
    WPARAM wParam,
    LPARAM lParam)
{
    ThreadSpecificData *tsdPtr =
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    if (nCode == HCBT_CREATEWND) {
	/*
	 * Window owned by our task is being created. Since the hook is
	 * installed just before the MessageBox call and removed after the
	 * MessageBox call, the window being created is either the message box
	 * or one of its controls. Check that the class is WC_DIALOG to ensure
	 * that it's the one we want.
	 */

	LPCBT_CREATEWND lpcbtcreate = (LPCBT_CREATEWND) lParam;

	if (WC_DIALOG == lpcbtcreate->lpcs->lpszClass) {
	    HWND hwnd = (HWND) wParam;

	    SendMessage(hwnd, WM_SETICON, ICON_SMALL,
		    (LPARAM) tsdPtr->hSmallIcon);
	    SendMessage(hwnd, WM_SETICON, ICON_BIG, (LPARAM) tsdPtr->hBigIcon);
	}
    }

    /*
     * Call the next hook proc, if there is one
     */

    return CallNextHookEx(tsdPtr->hMsgBoxHook, nCode, wParam, lParam);
}

/*
 * ----------------------------------------------------------------------
 *
 * SetTkDialog --
 *
 *	Records the HWND for a native dialog in the 'tk_dialog' variable so
 *	that the test-suite can operate on the correct dialog window. Use of
 *	this is enabled when a test program calls TkWinDialogDebug by calling
 *	the test command 'tkwinevent debug 1'.
 *
 * ----------------------------------------------------------------------
 */

static void
SetTkDialog(
    ClientData clientData)
{
    ThreadSpecificData *tsdPtr =
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));
    char buf[32];

    sprintf(buf, "0x%p", (HWND) clientData);
    Tcl_SetVar2(tsdPtr->debugInterp, "tk_dialog", NULL, buf, TCL_GLOBAL_ONLY);
}

/*
 * Factored out a common pattern in use in this file.
 */

static const char *
ConvertExternalFilename(
    TCHAR *filename,
    Tcl_DString *dsPtr)
{
    char *p;

    Tcl_WinTCharToUtf(filename, -1, dsPtr);
    for (p = Tcl_DStringValue(dsPtr); *p != '\0'; p++) {
	/*
	 * Change the pathname to the Tcl "normalized" pathname, where back
	 * slashes are used instead of forward slashes
	 */

	if (*p == '\\') {
	    *p = '/';
	}
    }
    return Tcl_DStringValue(dsPtr);
}

/*
 * ----------------------------------------------------------------------
 *
 * GetFontObj --
 *
 *	Convert a windows LOGFONT into a Tk font description.
 *
 * Result:
 *	A list containing a Tk font description.
 *
 * ----------------------------------------------------------------------
 */

static Tcl_Obj *
GetFontObj(
    HDC hdc,
    LOGFONT *plf)
{
    Tcl_DString ds;
    Tcl_Obj *resObj;
    int pt = 0;

    resObj = Tcl_NewListObj(0, NULL);
    Tcl_WinTCharToUtf(plf->lfFaceName, -1, &ds);
    Tcl_ListObjAppendElement(NULL, resObj,
	    Tcl_NewStringObj(Tcl_DStringValue(&ds), -1));
    Tcl_DStringFree(&ds);
    pt = -MulDiv(plf->lfHeight, 72, GetDeviceCaps(hdc, LOGPIXELSY));
    Tcl_ListObjAppendElement(NULL, resObj, Tcl_NewIntObj(pt));
    if (plf->lfWeight >= 700) {
	Tcl_ListObjAppendElement(NULL, resObj, Tcl_NewStringObj("bold", -1));
    }
    if (plf->lfItalic) {
	Tcl_ListObjAppendElement(NULL, resObj,
		Tcl_NewStringObj("italic", -1));
    }
    if (plf->lfUnderline) {
	Tcl_ListObjAppendElement(NULL, resObj,
		Tcl_NewStringObj("underline", -1));
    }
    if (plf->lfStrikeOut) {
	Tcl_ListObjAppendElement(NULL, resObj,
		Tcl_NewStringObj("overstrike", -1));
    }
    return resObj;
}

static void
ApplyLogfont(
    Tcl_Interp *interp,
    Tcl_Obj *cmdObj,
    HDC hdc,
    LOGFONT *logfontPtr)
{
    int objc;
    Tcl_Obj **objv, **tmpv;

    Tcl_ListObjGetElements(NULL, cmdObj, &objc, &objv);
    tmpv = ckalloc(sizeof(Tcl_Obj *) * (objc + 2));
    memcpy(tmpv, objv, sizeof(Tcl_Obj *) * objc);
    tmpv[objc] = GetFontObj(hdc, logfontPtr);
    TkBackgroundEvalObjv(interp, objc+1, tmpv, TCL_EVAL_GLOBAL);
    ckfree(tmpv);
}

/*
 * ----------------------------------------------------------------------
 *
 * HookProc --
 *
 *	Font selection hook. If the user selects Apply on the dialog, we call
 *	the applyProc script with the currently selected font as arguments.
 *
 * ----------------------------------------------------------------------
 */

typedef struct HookData {
    Tcl_Interp *interp;
    Tcl_Obj *titleObj;
    Tcl_Obj *cmdObj;
    Tcl_Obj *parentObj;
    Tcl_Obj *fontObj;
    HWND hwnd;
    Tk_Window parent;
} HookData;

static UINT_PTR CALLBACK
HookProc(
    HWND hwndDlg,
    UINT msg,
    WPARAM wParam,
    LPARAM lParam)
{
    CHOOSEFONT *pcf = (CHOOSEFONT *) lParam;
    HWND hwndCtrl;
    static HookData *phd = NULL;
    ThreadSpecificData *tsdPtr =
	    Tcl_GetThreadData(&dataKey, sizeof(ThreadSpecificData));

    if (WM_INITDIALOG == msg && lParam != 0) {
	phd = (HookData *) pcf->lCustData;
	phd->hwnd = hwndDlg;
	if (tsdPtr->debugFlag) {
	    tsdPtr->debugInterp = phd->interp;
	    Tcl_DoWhenIdle(SetTkDialog, hwndDlg);
	}
	if (phd->titleObj != NULL) {
	    Tcl_DString title;

	    Tcl_WinUtfToTChar(Tcl_GetString(phd->titleObj), -1, &title);
	    if (Tcl_DStringLength(&title) > 0) {
		SetWindowText(hwndDlg, (LPCTSTR) Tcl_DStringValue(&title));
	    }
	    Tcl_DStringFree(&title);
	}

	/*
	 * Disable the colour combobox (0x473) and its label (0x443).
	 */

	hwndCtrl = GetDlgItem(hwndDlg, 0x443);
	if (IsWindow(hwndCtrl)) {
	    EnableWindow(hwndCtrl, FALSE);
	}
	hwndCtrl = GetDlgItem(hwndDlg, 0x473);
	if (IsWindow(hwndCtrl)) {
	    EnableWindow(hwndCtrl, FALSE);
	}
	TkSendVirtualEvent(phd->parent, "TkFontchooserVisibility");
	return 1; /* we handled the message */
    }

    if (WM_DESTROY == msg) {
	phd->hwnd = NULL;
	TkSendVirtualEvent(phd->parent, "TkFontchooserVisibility");
	return 0;
    }

    /*
     * Handle apply button by calling the provided command script as a
     * background evaluation (ie: errors dont come back here).
     */

    if (WM_COMMAND == msg && LOWORD(wParam) == 1026) {
	LOGFONT lf = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, {0, 0}};
	HDC hdc = GetDC(hwndDlg);

	SendMessage(hwndDlg, WM_CHOOSEFONT_GETLOGFONT, 0, (LPARAM) &lf);
	if (phd && phd->cmdObj) {
	    ApplyLogfont(phd->interp, phd->cmdObj, hdc, &lf);
	}
	if (phd && phd->parent) {
	    TkSendVirtualEvent(phd->parent, "TkFontchooserFontChanged");
	}
	return 1;
    }
    return 0; /* pass on for default processing */
}

/*
 * Helper for the FontchooserConfigure command to return the current value of
 * any of the options (which may be NULL in the structure)
 */

enum FontchooserOption {
    FontchooserParent, FontchooserTitle, FontchooserFont, FontchooserCmd,
    FontchooserVisible
};

static Tcl_Obj *
FontchooserCget(
    HookData *hdPtr,
    int optionIndex)
{
    Tcl_Obj *resObj = NULL;

    switch(optionIndex) {
    case FontchooserParent:
	if (hdPtr->parentObj) {
	    resObj = hdPtr->parentObj;
	} else {
	    resObj = Tcl_NewStringObj(".", 1);
	}
	break;
    case FontchooserTitle:
	if (hdPtr->titleObj) {
	    resObj = hdPtr->titleObj;
	} else {
	    resObj =  Tcl_NewStringObj("", 0);
	}
	break;
    case FontchooserFont:
	if (hdPtr->fontObj) {
	    resObj = hdPtr->fontObj;
	} else {
	    resObj = Tcl_NewStringObj("", 0);
	}
	break;
    case FontchooserCmd:
	if (hdPtr->cmdObj) {
	    resObj = hdPtr->cmdObj;
	} else {
	    resObj = Tcl_NewStringObj("", 0);
	}
	break;
    case FontchooserVisible:
	resObj = Tcl_NewBooleanObj(hdPtr->hwnd && IsWindow(hdPtr->hwnd));
	break;
    default:
	resObj = Tcl_NewStringObj("", 0);
    }
    return resObj;
}

/*
 * ----------------------------------------------------------------------
 *
 * FontchooserConfigureCmd --
 *
 *	Implementation of the 'tk fontchooser configure' ensemble command. See
 *	the user documentation for what it does.
 *
 * Results:
 *	See the user documentation.
 *
 * Side effects:
 *	Per-interp data structure may be modified
 *
 * ----------------------------------------------------------------------
 */

static int
FontchooserConfigureCmd(
    ClientData clientData,	/* Main window */
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tk_Window tkwin = clientData;
    HookData *hdPtr = NULL;
    int i, r = TCL_OK;
    static const char *const optionStrings[] = {
	"-parent", "-title", "-font", "-command", "-visible", NULL
    };

    hdPtr = Tcl_GetAssocData(interp, "::tk::fontchooser", NULL);

    /*
     * With no arguments we return all the options in a dict.
     */

    if (objc == 1) {
	Tcl_Obj *keyObj, *valueObj;
	Tcl_Obj *dictObj = Tcl_NewDictObj();

	for (i = 0; r == TCL_OK && optionStrings[i] != NULL; ++i) {
	    keyObj = Tcl_NewStringObj(optionStrings[i], -1);
	    valueObj = FontchooserCget(hdPtr, i);
	    r = Tcl_DictObjPut(interp, dictObj, keyObj, valueObj);
	}
	if (r == TCL_OK) {
	    Tcl_SetObjResult(interp, dictObj);
	}
	return r;
    }

    for (i = 1; i < objc; i += 2) {
	int optionIndex;

	if (Tcl_GetIndexFromObjStruct(interp, objv[i], optionStrings,
		sizeof(char *),  "option", 0, &optionIndex) != TCL_OK) {
	    return TCL_ERROR;
	}
	if (objc == 2) {
	    /*
	     * If one option and no arg - return the current value.
	     */

	    Tcl_SetObjResult(interp, FontchooserCget(hdPtr, optionIndex));
	    return TCL_OK;
	}
	if (i + 1 == objc) {
	    Tcl_SetObjResult(interp, Tcl_ObjPrintf(
		    "value for \"%s\" missing", Tcl_GetString(objv[i])));
	    Tcl_SetErrorCode(interp, "TK", "FONTDIALOG", "VALUE", NULL);
	    return TCL_ERROR;
	}
	switch (optionIndex) {
	case FontchooserVisible: {
	    static const char *msg = "cannot change read-only option "
		    "\"-visible\": use the show or hide command";

	    Tcl_SetObjResult(interp, Tcl_NewStringObj(msg, -1));
	    Tcl_SetErrorCode(interp, "TK", "FONTDIALOG", "READONLY", NULL);
	    return TCL_ERROR;
	}
	case FontchooserParent: {
	    Tk_Window parent = Tk_NameToWindow(interp,
		    Tcl_GetString(objv[i+1]), tkwin);

	    if (parent == None) {
		return TCL_ERROR;
	    }
	    if (hdPtr->parentObj) {
		Tcl_DecrRefCount(hdPtr->parentObj);
	    }
	    hdPtr->parentObj = objv[i+1];
	    if (Tcl_IsShared(hdPtr->parentObj)) {
		hdPtr->parentObj = Tcl_DuplicateObj(hdPtr->parentObj);
	    }
	    Tcl_IncrRefCount(hdPtr->parentObj);
	    break;
	}
	case FontchooserTitle:
	    if (hdPtr->titleObj) {
		Tcl_DecrRefCount(hdPtr->titleObj);
	    }
	    hdPtr->titleObj = objv[i+1];
	    if (Tcl_IsShared(hdPtr->titleObj)) {
		hdPtr->titleObj = Tcl_DuplicateObj(hdPtr->titleObj);
	    }
	    Tcl_IncrRefCount(hdPtr->titleObj);
	    break;
	case FontchooserFont:
	    if (hdPtr->fontObj) {
		Tcl_DecrRefCount(hdPtr->fontObj);
	    }
	    (void)Tcl_GetString(objv[i+1]);
	    if (objv[i+1]->length) {
		hdPtr->fontObj = objv[i+1];
		if (Tcl_IsShared(hdPtr->fontObj)) {
		    hdPtr->fontObj = Tcl_DuplicateObj(hdPtr->fontObj);
		}
		Tcl_IncrRefCount(hdPtr->fontObj);
	    } else {
		hdPtr->fontObj = NULL;
	    }
	    break;
	case FontchooserCmd:
	    if (hdPtr->cmdObj) {
		Tcl_DecrRefCount(hdPtr->cmdObj);
	    }
	    (void)Tcl_GetString(objv[i+1]);
	    if (objv[i+1]->length) {
		hdPtr->cmdObj = objv[i+1];
		if (Tcl_IsShared(hdPtr->cmdObj)) {
		    hdPtr->cmdObj = Tcl_DuplicateObj(hdPtr->cmdObj);
		}
		Tcl_IncrRefCount(hdPtr->cmdObj);
	    } else {
		hdPtr->cmdObj = NULL;
	    }
	    break;
	}
    }
    return TCL_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * FontchooserShowCmd --
 *
 *	Implements the 'tk fontchooser show' ensemble command. The per-interp
 *	configuration data for the dialog is held in an interp associated
 *	structure.
 *
 *	Calls the Win32 FontChooser API which provides a modal dialog. See
 *	HookProc where we make a few changes to the dialog and set some
 *	additional state.
 *
 * ----------------------------------------------------------------------
 */

static int
FontchooserShowCmd(
    ClientData clientData,	/* Main window */
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    Tcl_DString ds;
    Tk_Window tkwin = clientData, parent;
    CHOOSEFONT cf;
    LOGFONT lf;
    HDC hdc;
    HookData *hdPtr;
    int r = TCL_OK, oldMode = 0;

    hdPtr = Tcl_GetAssocData(interp, "::tk::fontchooser", NULL);

    parent = tkwin;
    if (hdPtr->parentObj) {
	parent = Tk_NameToWindow(interp, Tcl_GetString(hdPtr->parentObj),
		tkwin);
	if (parent == None) {
	    return TCL_ERROR;
	}
    }

    Tk_MakeWindowExist(parent);

    ZeroMemory(&cf, sizeof(CHOOSEFONT));
    ZeroMemory(&lf, sizeof(LOGFONT));
    lf.lfCharSet = DEFAULT_CHARSET;
    cf.lStructSize = sizeof(CHOOSEFONT);
    cf.hwndOwner = Tk_GetHWND(Tk_WindowId(parent));
    cf.lpLogFont = &lf;
    cf.nFontType = SCREEN_FONTTYPE;
    cf.Flags = CF_SCREENFONTS | CF_EFFECTS | CF_ENABLEHOOK;
    cf.rgbColors = RGB(0,0,0);
    cf.lpfnHook = HookProc;
    cf.lCustData = (INT_PTR) hdPtr;
    hdPtr->interp = interp;
    hdPtr->parent = parent;
    hdc = GetDC(cf.hwndOwner);

    if (hdPtr->fontObj != NULL) {
	TkFont *fontPtr;
	Tk_Font f = Tk_AllocFontFromObj(interp, tkwin, hdPtr->fontObj);

	if (f == NULL) {
	    return TCL_ERROR;
	}
	fontPtr = (TkFont *) f;
	cf.Flags |= CF_INITTOLOGFONTSTRUCT;
	Tcl_WinUtfToTChar(fontPtr->fa.family, -1, &ds);
	_tcsncpy(lf.lfFaceName, (TCHAR *)Tcl_DStringValue(&ds),
		LF_FACESIZE-1);
	Tcl_DStringFree(&ds);
	lf.lfFaceName[LF_FACESIZE-1] = 0;
	lf.lfHeight = -MulDiv(TkFontGetPoints(tkwin, fontPtr->fa.size),
	    GetDeviceCaps(hdc, LOGPIXELSY), 72);
	if (fontPtr->fa.weight == TK_FW_BOLD) {
	    lf.lfWeight = FW_BOLD;
	}
	if (fontPtr->fa.slant != TK_FS_ROMAN) {
	    lf.lfItalic = TRUE;
	}
	if (fontPtr->fa.underline) {
	    lf.lfUnderline = TRUE;
	}
	if (fontPtr->fa.overstrike) {
	    lf.lfStrikeOut = TRUE;
	}
	Tk_FreeFont(f);
    }

    if (TCL_OK == r && hdPtr->cmdObj != NULL) {
	int len = 0;

	r = Tcl_ListObjLength(interp, hdPtr->cmdObj, &len);
	if (len > 0) {
	    cf.Flags |= CF_APPLY;
	}
    }

    if (TCL_OK == r) {
	oldMode = Tcl_SetServiceMode(TCL_SERVICE_ALL);
	if (ChooseFont(&cf)) {
	    if (hdPtr->cmdObj) {
		ApplyLogfont(hdPtr->interp, hdPtr->cmdObj, hdc, &lf);
	    }
	    if (hdPtr->parent) {
		TkSendVirtualEvent(hdPtr->parent, "TkFontchooserFontChanged");
	    }
	}
	Tcl_SetServiceMode(oldMode);
	EnableWindow(cf.hwndOwner, 1);
    }

    ReleaseDC(cf.hwndOwner, hdc);
    return r;
}

/*
 * ----------------------------------------------------------------------
 *
 * FontchooserHideCmd --
 *
 *	Implementation of the 'tk fontchooser hide' ensemble. See the user
 *	documentation for details.
 *	As the Win32 FontChooser function is always modal all we do here is
 *	destroy the dialog
 *
 * ----------------------------------------------------------------------
 */

static int
FontchooserHideCmd(
    ClientData clientData,	/* Main window */
    Tcl_Interp *interp,
    int objc,
    Tcl_Obj *const objv[])
{
    HookData *hdPtr = Tcl_GetAssocData(interp, "::tk::fontchooser", NULL);

    if (hdPtr->hwnd && IsWindow(hdPtr->hwnd)) {
	EndDialog(hdPtr->hwnd, 0);
    }
    return TCL_OK;
}

/*
 * ----------------------------------------------------------------------
 *
 * DeleteHookData --
 *
 *	Clean up the font chooser configuration data when the interp is
 *	destroyed.
 *
 * ----------------------------------------------------------------------
 */

static void
DeleteHookData(ClientData clientData, Tcl_Interp *interp)
{
    HookData *hdPtr = clientData;

    if (hdPtr->parentObj) {
	Tcl_DecrRefCount(hdPtr->parentObj);
    }
    if (hdPtr->fontObj) {
	Tcl_DecrRefCount(hdPtr->fontObj);
    }
    if (hdPtr->titleObj) {
	Tcl_DecrRefCount(hdPtr->titleObj);
    }
    if (hdPtr->cmdObj) {
	Tcl_DecrRefCount(hdPtr->cmdObj);
    }
    ckfree(hdPtr);
}

/*
 * ----------------------------------------------------------------------
 *
 * TkInitFontchooser --
 *
 *	Associate the font chooser configuration data with the Tcl
 *	interpreter. There is one font chooser per interp.
 *
 * ----------------------------------------------------------------------
 */

MODULE_SCOPE const TkEnsemble tkFontchooserEnsemble[];
const TkEnsemble tkFontchooserEnsemble[] = {
    { "configure", FontchooserConfigureCmd, NULL },
    { "show", FontchooserShowCmd, NULL },
    { "hide", FontchooserHideCmd, NULL },
    { NULL, NULL, NULL }
};

int
TkInitFontchooser(Tcl_Interp *interp, ClientData clientData)
{
    HookData *hdPtr = ckalloc(sizeof(HookData));

    memset(hdPtr, 0, sizeof(HookData));
    Tcl_SetAssocData(interp, "::tk::fontchooser", DeleteHookData, hdPtr);
    return TCL_OK;
}

/*
 * Local Variables:
 * mode: c
 * c-basic-offset: 4
 * fill-column: 78
 * End:
 */
