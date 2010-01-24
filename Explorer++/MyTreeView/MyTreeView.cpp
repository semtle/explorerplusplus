/******************************************************************
 *
 * Project: MyTreeView
 * File: MyTreeView.cpp
 * License: GPL - See COPYING in the top level directory
 *
 * Wraps a treeview control. Specifically handles
 * adding directories to it and selecting directories.
 * Each non-network drive in the system is also
 * monitored for changes.
 *
 * Notes:
 *  - All items are sorted alphabetically, except for:
 *     - Items on the desktop
 *     - Items in My Computer
 *
 * Written by David Erceg
 * www.explorerplusplus.com
 *
 *****************************************************************/

#include "stdafx.h"
#include "../Helper/Helper.h"
#include "../Helper/Buffer.h"
#include "MyTreeView.h"
#include "MyTreeViewInternal.h"


#define DEFAULT_ALTERED_ALLOCATION	250
#define DEFAULT_ITEM_ALLOCATION		100

typedef struct
{
	LPITEMIDLIST	pidlParentNode;
	LPITEMIDLIST	pidlDestination;
} QueuedItem_t;

LRESULT CALLBACK	TreeViewProcStub(HWND hwnd,UINT uMsg,WPARAM wParam,LPARAM lParam,UINT_PTR uIdSubclass,DWORD_PTR dwRefData);
void CALLBACK		Timer_DirectoryModified(HWND hwnd,UINT uMsg,UINT_PTR idEvent,DWORD dwTime);
DWORD WINAPI		Thread_SubFoldersStub(LPVOID pVoid);
DWORD WINAPI		Thread_MonitorAllDrives(LPVOID pParam);

DWORD	g_ThreadId;
WNDPROC	OldTreeViewProc;
UINT	DirWatchFlags = FILE_NOTIFY_CHANGE_DIR_NAME;

list<QueuedItem_t> g_ItemList;

CMyTreeView::CMyTreeView(HWND hTreeView,HWND hParent,IDirectoryMonitor *pDirMon)
{
	m_hTreeView = hTreeView;
	m_hParent = hParent;
	
	SetWindowSubclass(m_hTreeView,TreeViewProcStub,0,(DWORD_PTR)this);

	InitializeCriticalSection(&m_cs);

	m_iAlteredAllocation = DEFAULT_ALTERED_ALLOCATION;
	m_nAltered = 0;

	m_pAlteredFiles = (AlteredFiles_t *)malloc(m_iAlteredAllocation * sizeof(AlteredFiles_t));

	m_pDirMon = pDirMon;


	m_uItemMap = (int *)malloc(DEFAULT_ITEM_ALLOCATION * sizeof(int));
	m_pItemInfo = (ItemInfo_t *)malloc(DEFAULT_ITEM_ALLOCATION * sizeof(ItemInfo_t));

	m_iCurrentItemAllocation = DEFAULT_ITEM_ALLOCATION;

	int i = 0;

	for(i = 0;i < m_iCurrentItemAllocation;i++)
	{
		m_uItemMap[i] = 0;
	}


	AddRoot();

	m_bDragging = FALSE;

	m_bShowHidden = TRUE;

	InitializeDragDropHelpers();

	m_bQueryRemoveCompleted = FALSE;
	CreateThread(NULL,0,Thread_MonitorAllDrives,this,0,NULL);

	m_iProcessing = 0;
}

CMyTreeView::~CMyTreeView()
{

}

LRESULT CALLBACK TreeViewProcStub(HWND hwnd,UINT uMsg,WPARAM wParam,LPARAM lParam,
UINT_PTR uIdSubclass,DWORD_PTR dwRefData)
{
	CMyTreeView *pMyTreeView = (CMyTreeView *)dwRefData;

	return pMyTreeView->TreeViewProc(hwnd,uMsg,wParam,lParam);
}

LRESULT CALLBACK CMyTreeView::TreeViewProc(HWND hwnd,
UINT msg,WPARAM wParam,LPARAM lParam)
{
	switch(msg)
	{
		case WM_CREATE:
			break;

		case WM_SETFOCUS:
			SendMessage(m_hParent,WM_USER_TREEVIEW_GAINEDFOCUS,0,0);
			break;

		case WM_TIMER:
			DirectoryAltered();
			break;

		case WM_DEVICECHANGE:
			return OnDeviceChange(wParam,lParam);
			break;

		case WM_SETCURSOR:
			return OnSetCursor();
			break;

		case WM_NOTIFY:
			return OnNotify(hwnd,msg,wParam,lParam);
			break;
	}

	return DefSubclassProc(hwnd,msg,wParam,lParam);
}

LRESULT CALLBACK CMyTreeView::OnNotify(HWND hwnd,UINT Msg,WPARAM wParam,LPARAM lParam)
{
	NMHDR *nmhdr;
	nmhdr = (NMHDR *)lParam;

	switch(nmhdr->code)
	{
	case TVN_BEGINDRAG:
		OnBeginDrag(lParam);
		break;
	}

	return 0;
}

LRESULT CMyTreeView::OnSetCursor(void)
{
	/* If the "app starting" cursor needs
	to be shown, return TRUE to prevent the
	OS from automatically resetting the
	cursor; else return FALSE to allow the
	OS to set the default cursor. */
	if(m_iProcessing > 0)
	{
		SetCursor(LoadCursor(NULL,IDC_APPSTARTING));
		return TRUE;
	}

	/* Set the cursor back to the default. */
	SetCursor(LoadCursor(NULL,IDC_ARROW));
	return FALSE;
}

HTREEITEM CMyTreeView::AddRoot(void)
{
	IShellFolder	*pDesktopFolder = NULL;
	LPITEMIDLIST	pidl = NULL;
	TCHAR			szDesktopParsingPath[MAX_PATH];
	TCHAR			szDesktopDisplayName[MAX_PATH];
	SHFILEINFO		shfi;
	HRESULT			hr;
	TVINSERTSTRUCT	tvis;
	TVITEMEX		tvItem;
	HTREEITEM		hDesktop = NULL;
	int				iItemId;

	TreeView_DeleteAllItems(m_hTreeView);

	hr = SHGetFolderLocation(NULL,CSIDL_DESKTOP,NULL,0,&pidl);

	if(SUCCEEDED(hr))
	{
		GetVirtualFolderParsingPath(CSIDL_DESKTOP,szDesktopParsingPath);
		GetDisplayName(szDesktopParsingPath,szDesktopDisplayName,SHGDN_INFOLDER);

		SHGetFileInfo((LPTSTR)pidl,NULL,&shfi,NULL,SHGFI_PIDL|SHGFI_SYSICONINDEX);

		iItemId = GenerateUniqueItemId();
		m_pItemInfo[iItemId].pidl = ILClone(pidl);
		m_uItemMap[iItemId] = 1;

		tvItem.mask				= TVIF_TEXT|TVIF_IMAGE|TVIF_CHILDREN|TVIF_SELECTEDIMAGE|TVIF_PARAM;
		tvItem.pszText			= szDesktopDisplayName;
		tvItem.cchTextMax		= lstrlen(szDesktopDisplayName);
		tvItem.iImage			= shfi.iIcon;
		tvItem.iSelectedImage	= shfi.iIcon;
		tvItem.cChildren		= 1;
		tvItem.lParam			= (LPARAM)iItemId;

		tvis.hParent			= NULL;
		tvis.hInsertAfter		= TVI_SORT;
		tvis.itemex				= tvItem;

		hDesktop = TreeView_InsertItem(m_hTreeView,&tvis);

		if(hDesktop != NULL)
		{
			hr = SHGetDesktopFolder(&pDesktopFolder);

			if(SUCCEEDED(hr))
			{
				AddDirectoryInternal(pDesktopFolder,pidl,hDesktop);

				SendMessage(m_hTreeView,TVM_EXPAND,(WPARAM)TVE_EXPAND,
					(LPARAM)hDesktop);

				pDesktopFolder->Release();
			}
		}

		CoTaskMemFree(pidl);
	}

	return hDesktop;
}

/*
* Adds all the folder objects within a directory to the
* specified treeview control.
*/
HRESULT CMyTreeView::AddDirectory(HTREEITEM hParent,TCHAR *szParsingPath)
{
	LPITEMIDLIST	pidlDirectory = NULL;
	HRESULT			hr;

	hr = GetIdlFromParsingName(szParsingPath,&pidlDirectory);

	if(SUCCEEDED(hr))
	{
		hr = AddDirectory(hParent,pidlDirectory);

		CoTaskMemFree(pidlDirectory);
	}

	return hr;
}

HRESULT CMyTreeView::AddDirectory(HTREEITEM hParent,LPITEMIDLIST pidlDirectory)
{
	IShellFolder	*pDesktopFolder = NULL;
	IShellFolder	*pShellFolder = NULL;
	HRESULT			hr;

	hr = SHGetDesktopFolder(&pDesktopFolder);

	if(SUCCEEDED(hr))
	{
		if(IsNamespaceRoot(pidlDirectory))
		{
			hr = SHGetDesktopFolder(&pShellFolder);
		}
		else
		{
			hr = pDesktopFolder->BindToObject(pidlDirectory,NULL,
			IID_IShellFolder,(LPVOID *)&pShellFolder);
		}

		if(SUCCEEDED(hr))
		{
			AddDirectoryInternal(pShellFolder,pidlDirectory,hParent);

			pShellFolder->Release();
		}

		pDesktopFolder->Release();
	}

	return hr;
}

void CMyTreeView::AddDirectoryInternal(IShellFolder *pShellFolder,LPITEMIDLIST pidlDirectory,
HTREEITEM hParent)
{
	IEnumIDList		*pEnumIDList = NULL;
	LPITEMIDLIST	pidl = NULL;
	LPITEMIDLIST	rgelt = NULL;
	ThreadInfo_t	*pThreadInfo = NULL;
	SHCONTF			EnumFlags;
	TCHAR			szDirectory[MAX_PATH];
	TCHAR			szDirectory2[MAX_PATH];
	ULONG			uFetched;
	HTREEITEM		hItem;
	TVINSERTSTRUCT	tvis;
	TVITEMEX		tvItem;
	HRESULT			hr;
	BOOL			bMyComputer = FALSE;

	hr = GetDisplayName(pidlDirectory,szDirectory,SHGDN_FORPARSING);
	hr = GetDisplayName(pidlDirectory,szDirectory2,SHGDN_FORPARSING);

	hr = SHGetFolderLocation(NULL,CSIDL_DRIVES,NULL,0,&pidl);

	if(SUCCEEDED(hr))
	{
		if(ILIsEqual(pidlDirectory,pidl))
			bMyComputer = TRUE;

		CoTaskMemFree(pidl);
	}

	SendMessage(m_hTreeView,WM_SETREDRAW,(WPARAM)FALSE,(LPARAM)NULL);

	EnumFlags = SHCONTF_FOLDERS;

	if(m_bShowHidden)
		EnumFlags |= SHCONTF_INCLUDEHIDDEN;

	hr = pShellFolder->EnumObjects(NULL,EnumFlags,&pEnumIDList);

	if(SUCCEEDED(hr) && pEnumIDList != NULL)
	{
		/* Iterate over the subfolders items, and place them in the tree. */
		uFetched = 1;
		while(pEnumIDList->Next(1,&rgelt,&uFetched) == S_OK && (uFetched == 1))
		{
			SHFILEINFO shfi;
			ULONG Attributes = SFGAO_FOLDER|SFGAO_FILESYSTEM;

			/* Only retrieve the attributes for this item. */
			hr = pShellFolder->GetAttributesOf(1,(LPCITEMIDLIST *)&rgelt,&Attributes);

			if(SUCCEEDED(hr))
			{
				/* Is the item a folder? (SFGAO_STREAM is set on .zip files, along with
				SFGAO_FOLDER). */
				if((Attributes & SFGAO_FOLDER))
				{
					LPITEMIDLIST	pidlComplete = NULL;
					STRRET			str;
					TCHAR			ItemName[MAX_PATH];
					UINT			ItemMask;
					int				iItemId;

					hr = pShellFolder->GetDisplayNameOf(rgelt,SHGDN_NORMAL,&str);

					if(SUCCEEDED(hr))
					{
						StrRetToBuf(&str,rgelt,ItemName,SIZEOF_ARRAY(ItemName));

						pidlComplete = ILCombine(pidlDirectory,rgelt);

						SHGetFileInfo((LPTSTR)pidlComplete,NULL,
							&shfi,sizeof(shfi),SHGFI_PIDL|SHGFI_SYSICONINDEX);

						hr = GetDisplayName(pidlComplete,szDirectory,SHGDN_FORPARSING);

						iItemId = GenerateUniqueItemId();
						m_pItemInfo[iItemId].pidl = ILClone(pidlComplete);

						ItemMask = TVIF_TEXT|TVIF_IMAGE|TVIF_SELECTEDIMAGE|TVIF_PARAM|TVIF_CHILDREN;

						tvItem.mask				= ItemMask;
						tvItem.pszText			= ItemName;
						tvItem.iImage			= shfi.iIcon;
						tvItem.iSelectedImage	= shfi.iIcon;
						tvItem.lParam			= (LPARAM)iItemId;
						tvItem.cChildren		= 1;

						if(bMyComputer)
							tvis.hInsertAfter		= DetermineItemSortedPosition(hParent,szDirectory);
						else
							tvis.hInsertAfter		= TVI_LAST;

						tvis.hParent			= hParent;
						tvis.itemex				= tvItem;

						hItem = TreeView_InsertItem(m_hTreeView,&tvis);

						CoTaskMemFree(pidlComplete);
					}
				}
			}

			CoTaskMemFree(rgelt);
		}

		pEnumIDList->Release();
	}

	SendMessage(m_hTreeView,WM_SETREDRAW,(WPARAM)TRUE,(LPARAM)NULL);

	pThreadInfo = (ThreadInfo_t *)malloc(sizeof(ThreadInfo_t));
	pThreadInfo->hTreeView		= m_hTreeView;
	pThreadInfo->pidl			= ILClone(pidlDirectory);
	pThreadInfo->hParent		= hParent;
	pThreadInfo->pMyTreeView	= this;

	CreateThread(NULL,0,Thread_SubFoldersStub,pThreadInfo,0,&g_ThreadId);
}

int CMyTreeView::GenerateUniqueItemId(void)
{
	BOOL	bFound = FALSE;
	int		i = 0;

	for(i = 0;i < m_iCurrentItemAllocation;i++)
	{
		if(m_uItemMap[i] == 0)
		{
			m_uItemMap[i] = 1;
			bFound = TRUE;
			break;
		}
	}

	if(bFound)
		return i;
	else
	{
		int iCurrent;

		m_uItemMap = (int *)realloc(m_uItemMap,(m_iCurrentItemAllocation +
			DEFAULT_ITEM_ALLOCATION) * sizeof(int));
		m_pItemInfo = (ItemInfo_t *)realloc(m_pItemInfo,(m_iCurrentItemAllocation +
			DEFAULT_ITEM_ALLOCATION) * sizeof(ItemInfo_t));

		if(m_uItemMap != NULL && m_pItemInfo != NULL)
		{
			iCurrent = m_iCurrentItemAllocation;

			m_iCurrentItemAllocation += DEFAULT_ITEM_ALLOCATION;

			for(i = iCurrent;i < m_iCurrentItemAllocation;i++)
			{
				m_uItemMap[i] = 0;
			}

			/* Return the first of the new items. */
			m_uItemMap[iCurrent] = 1;
			return iCurrent;
		}
		else
		{
			return -1;
		}
	}
}

DWORD WINAPI Thread_SubFoldersStub(LPVOID pParam)
{
	CMyTreeView		*pMyTreeView = NULL;
	ThreadInfo_t	*pThreadInfo = NULL;

	pThreadInfo = (ThreadInfo_t *)pParam;

	pMyTreeView = pThreadInfo->pMyTreeView;

	CoInitializeEx(NULL,COINIT_APARTMENTTHREADED);

	pMyTreeView->Thread_SubFolders(pParam);

	CoUninitialize();

	return 0;
}

/* Check nearest ancestor that IS in treeview. If this ancestor is been expanded,
queue a callback operation; else expand it.
Once the ancestor has been expanded, the callback will be run. This will then
check whether the ancestor is the direct parent of the item, or if it is
farther up the tree. If it is the direct parent, the item will simply be
selected. If it isn't the direct parent, this procedure will repeat, except that
the nearest ancestor is now the ancestor that was just expanded. */
DWORD WINAPI CMyTreeView::Thread_AddDirectoryInternal(IShellFolder *pShellFolder,
LPITEMIDLIST pidlDirectory,HTREEITEM hParent)
{
	IEnumIDList		*pEnumIDList = NULL;
	LPITEMIDLIST	rgelt = NULL;
	ItemInfo_t		*pItemInfo = NULL;
	SHCONTF			EnumFlags;
	TCHAR			szDirectory[MAX_PATH];
	ULONG			uFetched;
	HTREEITEM		hItem;
	TVINSERTSTRUCT	tvis;
	TVITEMEX		tvItem;
	HRESULT			hr;
	int				iMonitorId = -1;

	hr = GetDisplayName(pidlDirectory,szDirectory,SHGDN_FORPARSING);

	EnumFlags = SHCONTF_FOLDERS;

	if(m_bShowHidden)
		EnumFlags |= SHCONTF_INCLUDEHIDDEN;

	hr = pShellFolder->EnumObjects(NULL,EnumFlags,&pEnumIDList);

	if(SUCCEEDED(hr))
	{
		/* Iterate over the subfolders items, and place them in the tree. */
		uFetched = 1;
		while(pEnumIDList->Next(1,&rgelt,&uFetched) == S_OK && (uFetched == 1))
		{
			SHFILEINFO shfi;
			ULONG Attributes = SFGAO_FOLDER|SFGAO_STREAM|SFGAO_FILESYSTEM;

			/* Only retrieve the attributes for this item. */
			hr = pShellFolder->GetAttributesOf(1,(LPCITEMIDLIST *)&rgelt,&Attributes);

			if(SUCCEEDED(hr))
			{
				/* Is the item a folder? (SFGAO_STREAM is set on .zip files, along with
				SFGAO_FOLDER). */
				if((Attributes & SFGAO_FOLDER) && !(Attributes & SFGAO_STREAM))
				{
					LPITEMIDLIST	pidlComplete = NULL;
					STRRET			str;
					TCHAR			ItemName[MAX_PATH];
					UINT			ItemMask;

					hr = pShellFolder->GetDisplayNameOf(rgelt,SHGDN_NORMAL,&str);

					if(SUCCEEDED(hr))
					{
						StrRetToBuf(&str,rgelt,ItemName,SIZEOF_ARRAY(ItemName));

						pidlComplete = ILCombine(pidlDirectory,rgelt);

						SHGetFileInfo((LPTSTR)pidlComplete,NULL,
						&shfi,sizeof(shfi),SHGFI_PIDL|SHGFI_SYSICONINDEX|SHGFI_ATTRIBUTES);

						iMonitorId = -1;

						hr = GetDisplayName(pidlComplete,szDirectory,SHGDN_FORPARSING);

						pItemInfo = (ItemInfo_t *)malloc(sizeof(ItemInfo_t));

						pItemInfo->pidl			= ILCombine(pidlDirectory,rgelt);

						ItemMask = TVIF_TEXT|TVIF_IMAGE|TVIF_SELECTEDIMAGE|TVIF_PARAM|TVIF_CHILDREN;

						tvItem.mask				= ItemMask;
						tvItem.pszText			= ItemName;
						tvItem.cchTextMax		= lstrlen(ItemName);
						tvItem.iImage			= shfi.iIcon;
						tvItem.iSelectedImage	= shfi.iIcon;
						tvItem.lParam			= (LPARAM)pItemInfo;
						tvItem.cChildren		= ((shfi.dwAttributes & SFGAO_HASSUBFOLDER) == SFGAO_HASSUBFOLDER) ? 1:0;

						tvis.hInsertAfter		= DetermineItemSortedPosition(hParent,szDirectory);
						tvis.hParent			= hParent;
						tvis.itemex				= tvItem;

						hItem = TreeView_InsertItem(m_hTreeView,&tvis);

						CoTaskMemFree(pidlComplete);
					}
				}
			}

			CoTaskMemFree(rgelt);
		}

		pEnumIDList->Release();
	}

	tvItem.mask		= TVIF_HANDLE|TVIF_PARAM;
	tvItem.hItem	= hParent;
	TreeView_GetItem(m_hTreeView,&tvItem);

	pItemInfo = (ItemInfo_t *)tvItem.lParam;

	TreeView_Expand(m_hTreeView,hParent,TVE_EXPAND);

	return 0;
}

DWORD WINAPI CMyTreeView::Thread_SubFolders(LPVOID pParam)
{
	IShellFolder	*pShellFolder = NULL;
	LPITEMIDLIST	pidlRelative = NULL;
	HTREEITEM		hItem;
	TVITEM			tvItem;
	ThreadInfo_t	*pThreadInfo = NULL;
	ULONG			Attributes;
	HRESULT			hr;
	BOOL			res;

	pThreadInfo = (ThreadInfo_t *)pParam;

	hItem = TreeView_GetChild(pThreadInfo->hTreeView,pThreadInfo->hParent);

	while(hItem != NULL)
	{
		Attributes = SFGAO_HASSUBFOLDER;

		tvItem.mask	= TVIF_PARAM|TVIF_HANDLE;
		tvItem.hItem	= hItem;

		res = TreeView_GetItem(pThreadInfo->hTreeView,&tvItem);

		if(res != FALSE)
		{
			ItemInfo_t *pItemInfo = NULL;

			pItemInfo = &m_pItemInfo[(int)tvItem.lParam];

			hr = SHBindToParent(pItemInfo->pidl,IID_IShellFolder,(void **)&pShellFolder,(LPCITEMIDLIST *)&pidlRelative);

			if(SUCCEEDED(hr))
			{
				/* Only retrieve the attributes for this item. */
				hr = pShellFolder->GetAttributesOf(1,(LPCITEMIDLIST *)&pidlRelative,&Attributes);

				if(SUCCEEDED(hr))
				{
					if((Attributes & SFGAO_HASSUBFOLDER) != SFGAO_HASSUBFOLDER)
					{
						tvItem.mask			= TVIF_CHILDREN;
						tvItem.cChildren	= 0;
						TreeView_SetItem(pThreadInfo->hTreeView,&tvItem);
					}

					pShellFolder->Release();
				}
			}
		}

		hItem = TreeView_GetNextSibling(pThreadInfo->hTreeView,hItem);
	}

	CoTaskMemFree(pThreadInfo->pidl);

	free(pThreadInfo);

	return 0;
}

HTREEITEM CMyTreeView::DetermineItemSortedPosition(HTREEITEM hParent,TCHAR *szItem)
{
	HTREEITEM	htInsertAfter = NULL;

	if(PathIsRoot(szItem))
	{
		return DetermineDriveSortedPosition(hParent,szItem);
	}
	else
	{
		HTREEITEM	htItem;
		HTREEITEM	hPreviousItem;
		TVITEMEX	Item;
		ItemInfo_t	*pItemInfo = NULL;
		SFGAOF		Attributes;
		TCHAR		szFullItemPath[MAX_PATH];

		/* Insert the item in its sorted position, skipping
		past any drives or any non-filesystem items (i.e.
		'My Computer', 'Recycle Bin', etc). */
		htItem = TreeView_GetChild(m_hTreeView,hParent);

		/* If the parent has no children, this item will
		be the first that appears. */
		if(htItem == NULL)
			return TVI_FIRST;

		hPreviousItem = TVI_FIRST;

		while(htInsertAfter == NULL)
		{
			Item.mask		= TVIF_PARAM|TVIF_HANDLE;
			Item.hItem		= htItem;
			TreeView_GetItem(m_hTreeView,&Item);

			pItemInfo = &m_pItemInfo[(int)Item.lParam];

			GetDisplayName(pItemInfo->pidl,szFullItemPath,SHGDN_FORPARSING);

			Attributes = SFGAO_FILESYSTEM;
			Attributes = GetFileAttributes(szFullItemPath);

			/* Only perform the comparison if the current item is a real
			file or folder. */
			if(!PathIsRoot(szFullItemPath) && ((Attributes & SFGAO_FILESYSTEM) != SFGAO_FILESYSTEM))
			{
				if(lstrcmp(szItem,szFullItemPath) < 0)
				{
					htInsertAfter = hPreviousItem;
				}
			}

			hPreviousItem = htItem;
			htItem = TreeView_GetNextSibling(m_hTreeView,htItem);

			if(htItem == NULL)
			{
				htInsertAfter = TVI_LAST;
			}
		}
	}

	return htInsertAfter;
}

HTREEITEM CMyTreeView::DetermineDriveSortedPosition(HTREEITEM hParent,TCHAR *szItemName)
{
	HTREEITEM	htItem;
	HTREEITEM	hPreviousItem;
	TVITEMEX	Item;
	ItemInfo_t	*pItemInfo = NULL;
	TCHAR		szFullItemPath[MAX_PATH];

	/* Drives will always be the first children of the specified
	item (usually 'My Computer'). Therefore, keep looping while
	there are more child items and the current item comes
	afterwards, or if there are no child items, place the item
	as the first child. */
	htItem = TreeView_GetChild(m_hTreeView,hParent);

	if(htItem == NULL)
		return TVI_FIRST;

	hPreviousItem = TVI_FIRST;

	while(htItem != NULL)
	{
		Item.mask		= TVIF_PARAM | TVIF_HANDLE;
		Item.hItem		= htItem;
		TreeView_GetItem(m_hTreeView,&Item);

		pItemInfo = &m_pItemInfo[(int)Item.lParam];

		GetDisplayName(pItemInfo->pidl,szFullItemPath,SHGDN_FORPARSING);

		if(PathIsRoot(szFullItemPath))
		{
			if(lstrcmp(szItemName,szFullItemPath) < 0)
				return hPreviousItem;
		}
		else
		{
			return hPreviousItem;
		}

		hPreviousItem = htItem;
		htItem = TreeView_GetNextSibling(m_hTreeView,htItem);
	}

	return htItem;
}

LPITEMIDLIST CMyTreeView::BuildPath(HTREEITEM hTreeItem)
{
	TVITEMEX	Item;
	ItemInfo_t	*pItemInfo = NULL;

	Item.mask			= TVIF_HANDLE|TVIF_PARAM;
	Item.hItem			= hTreeItem;
	TreeView_GetItem(m_hTreeView,&Item);

	pItemInfo = &m_pItemInfo[(int)Item.lParam];

	return ILClone(pItemInfo->pidl);
}

HTREEITEM CMyTreeView::LocateItem(TCHAR *szParsingPath)
{
	LPITEMIDLIST	pidl = NULL;
	HTREEITEM		hItem = NULL;
	HRESULT			hr;

	hr = GetIdlFromParsingName(szParsingPath,&pidl);

	if(SUCCEEDED(hr))
	{
		hItem = LocateItem(pidl);

		CoTaskMemFree(pidl);
	}

	return hItem;
}

HTREEITEM CMyTreeView::LocateItem(LPITEMIDLIST pidlDirectory)
{
	return LocateItemInternal(pidlDirectory,FALSE);
}

HTREEITEM CMyTreeView::LocateExistingItem(TCHAR *szParsingPath)
{
	LPITEMIDLIST	pidl = NULL;
	HTREEITEM		hItem;
	HRESULT			hr;

	hr = GetIdlFromParsingName(szParsingPath,&pidl);

	if(SUCCEEDED(hr))
	{
		hItem = LocateExistingItem(pidl);

		CoTaskMemFree(pidl);

		return hItem;
	}

	return NULL;
}

HTREEITEM CMyTreeView::LocateExistingItem(LPITEMIDLIST pidlDirectory)
{
	return LocateItemInternal(pidlDirectory,TRUE);
}

HTREEITEM CMyTreeView::LocateItemInternal(LPITEMIDLIST pidlDirectory,BOOL bOnlyLocateExistingItem)
{
	HTREEITEM	hRoot;
	HTREEITEM	hItem;
	TVITEMEX	Item;
	BOOL		bFound = FALSE;

	/* Get the root of the tree (root of namespace). */
	hRoot = TreeView_GetRoot(m_hTreeView);
	hItem = hRoot;

	Item.mask		= TVIF_PARAM|TVIF_HANDLE;
	Item.hItem		= hItem;
	TreeView_GetItem(m_hTreeView,&Item);

	/* Keep searching until the specified item
	is found or it is found the item does not
	exist in the trrview.
	Look through each item, once an ancestor is
	found, look through it's children, expanding
	the parent node if necessary. */
	while(!bFound && hItem != NULL)
	{
		ItemInfo_t *pItemInfo = NULL;

		pItemInfo = &m_pItemInfo[(int)Item.lParam];

		if(ILIsEqual((LPCITEMIDLIST)pItemInfo->pidl,pidlDirectory))
		{
			bFound = TRUE;

			break;
		}

		if(ILIsParent((LPCITEMIDLIST)pItemInfo->pidl,pidlDirectory,FALSE))
		{
			if((TreeView_GetChild(m_hTreeView,hItem)) == NULL)
			{
				if(bOnlyLocateExistingItem)
				{
					return NULL;
				}
				else
				{
					SendMessage(m_hTreeView,TVM_EXPAND,(WPARAM)TVE_EXPAND,
					(LPARAM)hItem);
				}
			}

			hItem = TreeView_GetChild(m_hTreeView,hItem);
		}
		else
		{
			hItem = TreeView_GetNextSibling(m_hTreeView,hItem);
		}

		Item.mask		= TVIF_PARAM|TVIF_HANDLE;
		Item.hItem		= hItem;
		TreeView_GetItem(m_hTreeView,&Item);
	}

	return hItem;
}

HTREEITEM CMyTreeView::LocateItemByPath(TCHAR *szItemPath,BOOL bExpand)
{
	LPITEMIDLIST	pidlMyComputer	= NULL;
	HTREEITEM		hMyComputer;
	HTREEITEM		hItem;
	HTREEITEM		hNextItem;
	TVITEMEX		Item;
	ItemInfo_t		*pItemInfo = NULL;
	TCHAR			*ptr = NULL;
	TCHAR			ItemText[MAX_PATH];
	TCHAR			FullItemPathCopy[MAX_PATH];
	TCHAR			szItemName[MAX_PATH];
	TCHAR			*next_token = NULL;

	StringCchCopy(FullItemPathCopy,SIZEOF_ARRAY(FullItemPathCopy),
	szItemPath);

	PathRemoveBackslash(FullItemPathCopy);

	SHGetFolderLocation(NULL,CSIDL_DRIVES,NULL,NULL,&pidlMyComputer);

	hMyComputer = LocateItem(pidlMyComputer);

	CoTaskMemFree(pidlMyComputer);

	/* First of drives in system. */
	hItem = TreeView_GetChild(m_hTreeView,hMyComputer);

	/* My Computer node may not be expanded. */
    if(hItem == NULL)
        return NULL;

	ptr = cstrtok_s(FullItemPathCopy,_T("\\"),&next_token);

	StringCchCopy(ItemText,SIZEOF_ARRAY(ItemText),ptr);
	StringCchCat(ItemText,SIZEOF_ARRAY(ItemText),_T("\\"));
	ptr = ItemText;

	Item.mask		= TVIF_HANDLE|TVIF_PARAM;
	Item.hItem		= hItem;
	TreeView_GetItem(m_hTreeView,&Item);

	pItemInfo = &m_pItemInfo[(int)Item.lParam];

	GetDisplayName(pItemInfo->pidl,szItemName,SHGDN_FORPARSING);

	while(StrCmpI(ptr,szItemName) != 0)
	{
		hItem = TreeView_GetNextSibling(m_hTreeView,hItem);

		if(hItem == NULL)
			return NULL;

		Item.mask		= TVIF_PARAM;
		Item.hItem		= hItem;
		TreeView_GetItem(m_hTreeView,&Item);

		pItemInfo = &m_pItemInfo[(int)Item.lParam];

		GetDisplayName(pItemInfo->pidl,szItemName,SHGDN_FORPARSING);
	}

	Item.mask = TVIF_TEXT;

	while((ptr = cstrtok_s(NULL,_T("\\"),&next_token)) != NULL)
	{
		if(TreeView_GetChild(m_hTreeView,hItem) == NULL)
		{
			if(bExpand)
				SendMessage(m_hTreeView,TVM_EXPAND,(WPARAM)TVE_EXPAND,(LPARAM)hItem);
			else
				return NULL;
		}

		hNextItem = TreeView_GetChild(m_hTreeView,hItem);
		hItem = hNextItem;

		Item.pszText	= ItemText;
		Item.cchTextMax	= MAX_PATH;
		Item.hItem		= hItem;
		TreeView_GetItem(m_hTreeView,&Item);

		while(StrCmpI(ptr,ItemText) != 0)
		{
			hItem = TreeView_GetNextSibling(m_hTreeView,hItem);

			if(hItem == NULL)
				return NULL;

			Item.pszText	= ItemText;
			Item.cchTextMax	= MAX_PATH;
			Item.hItem		= hItem;
			TreeView_GetItem(m_hTreeView,&Item);
		}
	}

	return hItem;
}

HTREEITEM CMyTreeView::CheckAgainstDesktop(TCHAR *szFullFileName)
{
	HTREEITEM	hItem;
	TVITEMEX	tvItem;
	TCHAR		szPath[MAX_PATH];
	TCHAR		szFileName[MAX_PATH];
	TCHAR		szDesktop[MAX_PATH];
	TCHAR		szCurrent[MAX_PATH];
	BOOL		bDesktop;

	StringCchCopy(szPath,MAX_PATH,szFullFileName);
	PathRemoveFileSpec(szPath);

	SHGetFolderPath(NULL,CSIDL_DESKTOP,NULL,SHGFP_TYPE_CURRENT,szDesktop);

	bDesktop = (lstrcmp(szPath,szDesktop) == 0);

	/* Is this item on the desktop? */
	if(bDesktop)
	{
		StringCchCopy(szFileName,MAX_PATH,szFullFileName);
		PathStripPath(szFileName);

		hItem = TreeView_GetChild(m_hTreeView,TreeView_GetRoot(m_hTreeView));

		while(hItem != NULL)
		{
			tvItem.mask			= TVIF_TEXT;
			tvItem.hItem		= hItem;
			tvItem.pszText		= szCurrent;
			tvItem.cchTextMax	= MAX_PATH;
			TreeView_GetItem(m_hTreeView,&tvItem);

			if(lstrcmp(szCurrent,szFileName) == 0)
				return hItem;

			hItem = TreeView_GetNextSibling(m_hTreeView,hItem);
		}
	}

	return NULL;
}

void CMyTreeView::EraseItems(HTREEITEM hParent)
{
	TVITEMEX	Item;
	HTREEITEM	hItem;
	ItemInfo_t	*pItemInfo = NULL;

	hItem = TreeView_GetChild(m_hTreeView,hParent);

	while(hItem != NULL)
	{
		Item.mask		= TVIF_PARAM|TVIF_HANDLE|TVIF_CHILDREN;
		Item.hItem		= hItem;
		TreeView_GetItem(m_hTreeView,&Item);

		if(Item.cChildren != 0)
			EraseItems(hItem);

		pItemInfo = &m_pItemInfo[(int)Item.lParam];

		CoTaskMemFree((LPVOID)pItemInfo->pidl);

		/* Free up this items id. */
		m_uItemMap[(int)Item.lParam] = 0;

		hItem = TreeView_GetNextSibling(m_hTreeView,hItem);
	}
}

void CMyTreeView::DirectoryAltered(void)
{
	static HTREEITEM	hRenamedItem;
	int					i = 0;

	EnterCriticalSection(&m_cs);

	KillTimer(m_hTreeView,0);

	for(i = 0;i < m_nAltered;i++)
	{
		switch(m_pAlteredFiles[i].dwAction)
		{
			case FILE_ACTION_ADDED:
				AddItem(m_pAlteredFiles[i].szFileName);
				break;

			case FILE_ACTION_REMOVED:
				RemoveItem(m_pAlteredFiles[i].szFileName);
				break;

			case FILE_ACTION_RENAMED_OLD_NAME:
				hRenamedItem = LocateItemByPath(m_pAlteredFiles[i].szFileName,FALSE);
				break;

			case FILE_ACTION_RENAMED_NEW_NAME:
				RenameItem(hRenamedItem,m_pAlteredFiles[i].szFileName);
				break;
		}
	}

	m_nAltered = 0;

	LeaveCriticalSection(&m_cs);
}

void CMyTreeView::AddDrive(TCHAR *szDrive)
{
	LPITEMIDLIST	pidlMyComputer = NULL;
	HTREEITEM		hMyComputer;
	HRESULT			hr;

	hr = SHGetFolderLocation(NULL,CSIDL_DRIVES,NULL,0,&pidlMyComputer);

	/* Don't use SUCCEEDED(hr). */
	if(hr == S_OK)
	{
		hMyComputer = LocateExistingItem(pidlMyComputer);

		if(hMyComputer != NULL)
		{
			AddItemInternal(hMyComputer,szDrive);
		}

		CoTaskMemFree(pidlMyComputer);
	}
}

void CMyTreeView::AddItem(TCHAR *szFullFileName)
{
	TCHAR			szDirectory[MAX_PATH];
	TCHAR			szDesktop[MAX_PATH];
	HTREEITEM		hParent;
	BOOL			bDesktop;

	/* If the specified item is a drive, it
	will need to be handled differently,
	as it is a child of my computer (and
	as such is not a regular file). */
	if(PathIsRoot(szFullFileName))
	{
		AddDrive(szFullFileName);
	}
	else
	{
		StringCchCopy(szDirectory,MAX_PATH,szFullFileName);
		PathRemoveFileSpec(szDirectory);

		SHGetFolderPath(NULL,CSIDL_DESKTOP,NULL,
			SHGFP_TYPE_CURRENT,szDesktop);

		bDesktop = (lstrcmp(szDirectory,szDesktop) == 0);

		hParent = LocateExistingItem(szDirectory);

		/* If this items' parent isn't currently
		shown on the treeview and the item is not
		on the desktop, exit without doing anything
		further. */
		if(hParent == NULL && !bDesktop)
			return;

		if(bDesktop)
			hParent = TreeView_GetRoot(m_hTreeView);

		AddItemInternal(hParent,szFullFileName);
	}
}

void CMyTreeView::AddItemInternal(HTREEITEM hParent,TCHAR *szFullFileName)
{
	IShellFolder	*pShellFolder = NULL;
	LPITEMIDLIST	pidlComplete = NULL;
	LPITEMIDLIST	pidlRelative = NULL;
	HTREEITEM		hItem;
	TVITEMEX		tvItem;
	TVINSERTSTRUCT	tvis;
	SHFILEINFO		shfi;
	SFGAOF			Attributes;
	TCHAR			szDisplayName[MAX_PATH];
	HRESULT			hr;
	BOOL			res;
	int				iItemId;
	int				nChildren = 0;

	hr = GetIdlFromParsingName(szFullFileName,&pidlComplete);

	if(!SUCCEEDED(hr))
		return;

	tvItem.mask		= TVIF_CHILDREN | TVIF_STATE;
	tvItem.hItem	= hParent;
	res = TreeView_GetItem(m_hTreeView,&tvItem);

	if(res)
	{
		/* If the parent node is currently collapsed,
		simply indicate that it has children (i.e. a
		plus sign will be shown next to the parent node). */
		if((tvItem.cChildren == 0) ||
			((tvItem.state & TVIS_EXPANDED) != TVIS_EXPANDED))
		{
			tvItem.mask			= TVIF_CHILDREN;
			tvItem.hItem		= hParent;
			tvItem.cChildren	= 1;
			TreeView_SetItem(m_hTreeView,&tvItem);
		}
		else
		{
			SHGetFileInfo(szFullFileName,NULL,&shfi,
				sizeof(shfi),SHGFI_SYSICONINDEX);

			hr = SHBindToParent(pidlComplete,IID_IShellFolder,
				(void **)&pShellFolder,(LPCITEMIDLIST *)&pidlRelative);

			if(SUCCEEDED(hr))
			{
				Attributes = SFGAO_HASSUBFOLDER;

				/* Only retrieve the attributes for this item. */
				hr = pShellFolder->GetAttributesOf(1,
					(LPCITEMIDLIST *)&pidlRelative,&Attributes);

				if(SUCCEEDED(hr))
				{
					if((Attributes & SFGAO_HASSUBFOLDER) != SFGAO_HASSUBFOLDER)
						nChildren = 0;
					else
						nChildren = 1;

					iItemId = GenerateUniqueItemId();

					m_pItemInfo[iItemId].pidl = ILClone(pidlComplete);

					GetDisplayName(szFullFileName,szDisplayName,SHGDN_NORMAL);

					tvItem.mask				= TVIF_TEXT|TVIF_IMAGE|TVIF_SELECTEDIMAGE|TVIF_PARAM|TVIF_CHILDREN;
					tvItem.pszText			= szDisplayName;
					tvItem.iImage			= shfi.iIcon;
					tvItem.iSelectedImage	= shfi.iIcon;
					tvItem.lParam			= (LPARAM)iItemId;
					tvItem.cChildren		= nChildren;

					if(hParent != NULL)
					{
						tvis.hParent			= hParent;
						tvis.hInsertAfter		= DetermineItemSortedPosition(hParent,szFullFileName);
						tvis.itemex				= tvItem;

						hItem = TreeView_InsertItem(m_hTreeView,&tvis);
					}
				}

				pShellFolder->Release();
			}
		}
	}

	CoTaskMemFree(pidlComplete);
}

void CMyTreeView::RenameItem(HTREEITEM hItem,TCHAR *szFullFileName)
{
	TVITEMEX	Item;
	ItemInfo_t	*pItemInfo = NULL;
	SHFILEINFO	shfi;
	TCHAR		szFileName[MAX_PATH];
	HRESULT		hr;
	BOOL		res;

	if(hItem == NULL)
		return;

	Item.mask		= TVIF_PARAM;
	Item.hItem		= hItem;
	res = TreeView_GetItem(m_hTreeView,&Item);

	if(res)
	{
		pItemInfo = &m_pItemInfo[(int)Item.lParam];

		CoTaskMemFree(pItemInfo->pidl);

		StringCchCopy(szFileName,MAX_PATH,szFullFileName);
		PathStripPath(szFileName);

		hr = GetIdlFromParsingName(szFullFileName,&pItemInfo->pidl);

		if(SUCCEEDED(hr))
		{
			SHGetFileInfo(szFullFileName,NULL,&shfi,sizeof(shfi),SHGFI_SYSICONINDEX);

			Item.mask			= TVIF_HANDLE|TVIF_TEXT|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
			Item.hItem			= hItem;
			Item.pszText		= szFileName;
			Item.iImage			= shfi.iIcon;
			Item.iSelectedImage	= shfi.iIcon;
			TreeView_SetItem(m_hTreeView,&Item);
		}
	}
}

void CMyTreeView::RemoveItem(TCHAR *szFullFileName)
{
	HTREEITEM		hItem;
	TVITEM			tvItem;
	SFGAOF			Attributes;
	LPITEMIDLIST	pidl = NULL;
	WCHAR			szParentW[MAX_PATH];
	TCHAR			szParent[MAX_PATH];
	TCHAR			szDesktop[MAX_PATH];
	TCHAR			szFileName[MAX_PATH];
	HRESULT			hr;
	BOOL			bDesktop = FALSE;

	StringCchCopy(szParent,MAX_PATH,szFullFileName);
	PathRemoveFileSpec(szParent);

	StringCchCopy(szFileName,MAX_PATH,szFullFileName);
	PathStripPath(szFileName);

	SHGetFolderPath(NULL,CSIDL_DESKTOP,NULL,SHGFP_TYPE_CURRENT,szDesktop);

	bDesktop = !lstrcmp(szParent,szDesktop);

	hItem = LocateExistingItem(szParent);

	if(hItem != NULL)
	{
		#ifndef UNICODE
		MultiByteToWideChar(CP_ACP,0,szParent,-1,szParentW,SIZEOF_ARRAY(szParentW));
		#else
		StringCchCopy(szParentW,SIZEOF_ARRAY(szParentW),szParent);
		#endif

		hr = SHParseDisplayName(szParentW,NULL,&pidl,SFGAO_HASSUBFOLDER,&Attributes);

		if(SUCCEEDED(hr))
		{
			tvItem.mask		= TVIF_CHILDREN;

			/* If the parent folder no longer has any sub-folders,
			set its number of children to 0. */
			if((Attributes & SFGAO_HASSUBFOLDER) != SFGAO_HASSUBFOLDER)
			{
				tvItem.cChildren	= 0;
				TreeView_Expand(m_hTreeView,hItem,TVE_COLLAPSE);
			}
			else
			{
				tvItem.cChildren	= 1;
			}

			tvItem.mask		= TVIF_CHILDREN;
			tvItem.hItem	= hItem;
			TreeView_SetItem(m_hTreeView,&tvItem);

			CoTaskMemFree(pidl);
		}
	}

    /* File has been deleted. Can't use its PIDL to locate it
    in the treeview. Parse the items path to find it. */
	hItem = LocateItemByPath(szFullFileName,FALSE);

	if(hItem != NULL)
	{
		EraseItems(hItem);

		TreeView_DeleteItem(m_hTreeView,hItem);
	}

	/* If the item is on the desktop, it will need to
	be deleted twice. */
	hItem = CheckAgainstDesktop(szFullFileName);

	if(hItem != NULL)
	{
		EraseItems(hItem);

		TreeView_DeleteItem(m_hTreeView,hItem);
	}
}

void CALLBACK Timer_DirectoryModified(HWND hwnd,UINT uMsg,UINT_PTR idEvent,DWORD dwTime)
{
	KillTimer(hwnd,idEvent);

	return;
}

void CMyTreeView::DirectoryAlteredCallback(TCHAR *szFileName,DWORD dwAction,
void *pData)
{
	DirectoryAltered_t	*pDirectoryAltered = NULL;
	CMyTreeView			*pMyTreeView = NULL;
	TCHAR				szFullFileName[MAX_PATH];

	pDirectoryAltered = (DirectoryAltered_t *)pData;

	pMyTreeView = (CMyTreeView *)pDirectoryAltered->pMyTreeView;

	StringCchCopy(szFullFileName,MAX_PATH,pDirectoryAltered->szPath);
	PathAppend(szFullFileName,szFileName);

	pMyTreeView->DirectoryModified(dwAction,szFullFileName);
}

void CMyTreeView::DirectoryModified(DWORD Action,TCHAR *szFullFileName)
{
	EnterCriticalSection(&m_cs);

	SetTimer(m_hTreeView,DIRECTORYMODIFIED_TIMER_ID,
		DIRECTORYMODIFIED_TIMER_ELAPSE,NULL);

	if(m_nAltered > (m_iAlteredAllocation - 1))
	{
		m_iAlteredAllocation += DEFAULT_ALTERED_ALLOCATION;

		m_pAlteredFiles = (AlteredFiles_t *)realloc(m_pAlteredFiles,
		m_iAlteredAllocation * sizeof(AlteredFiles_t));
	}

	StringCchCopy(m_pAlteredFiles[m_nAltered].szFileName,MAX_PATH,szFullFileName);
	m_pAlteredFiles[m_nAltered].dwAction = Action;
	m_nAltered++;

	LeaveCriticalSection(&m_cs);
}

LRESULT CALLBACK CMyTreeView::OnDeviceChange(WPARAM wParam,LPARAM lParam)
{
	switch(wParam)
	{
		/* Device has being added/inserted into the system. Update the
		treeview as neccessary. */
		case DBT_DEVICEARRIVAL:
			{
				DEV_BROADCAST_HDR *dbh;

				dbh = (DEV_BROADCAST_HDR *)lParam;

				if(dbh->dbch_devicetype == DBT_DEVTYP_VOLUME)
				{
					DEV_BROADCAST_VOLUME	*pdbv = NULL;
					SHFILEINFO				shfi;
					HTREEITEM				hItem;
					TVITEM					tvItem;
					TCHAR					DriveLetter;
					TCHAR					DriveName[4];
					TCHAR					szDisplayName[MAX_PATH];

					pdbv = (DEV_BROADCAST_VOLUME *)dbh;

					/* Build a string that will form the drive name. */
					DriveLetter = GetDriveNameFromMask(pdbv->dbcv_unitmask);
					StringCchPrintf(DriveName,SIZEOF_ARRAY(DriveName),_T("%c:\\"),DriveLetter);

					if(pdbv->dbcv_flags & DBTF_MEDIA)
					{
						hItem = LocateItemByPath(DriveName,FALSE);

						if(hItem != NULL)
						{
							SHGetFileInfo(DriveName,0,&shfi,sizeof(shfi),SHGFI_SYSICONINDEX);
							GetDisplayName(DriveName,szDisplayName,SHGDN_INFOLDER);

							/* Update the drives icon and display name. */
							tvItem.mask				= TVIF_HANDLE|TVIF_TEXT|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
							tvItem.hItem			= hItem;
							tvItem.iImage			= shfi.iIcon;
							tvItem.iSelectedImage	= shfi.iIcon;
							tvItem.pszText			= szDisplayName;
							TreeView_SetItem(m_hTreeView,&tvItem);
						}
					}
					else
					{
						/* Add the drive to the treeview. */
						AddItem(DriveName);

						MonitorDrive(DriveName);
					}
				}
			}
			break;

		case DBT_DEVICEQUERYREMOVE:
			{
				/* The system is looking for permission to remove
				a drive. Stop monitoring the drive. */
				DEV_BROADCAST_HDR				*dbh = NULL;
				DEV_BROADCAST_HANDLE			*pdbHandle = NULL;
				list<DriveEvent_t>::iterator	itr;

				dbh = (DEV_BROADCAST_HDR *)lParam;

				switch(dbh->dbch_devicetype)
				{
					case DBT_DEVTYP_HANDLE:
						{
							pdbHandle = (DEV_BROADCAST_HANDLE *)dbh;

							/* Loop through each of the registered drives to
							find the one that requested removal. Once it is
							found, stop monitoring it, close its handle,
							and allow the operating system to release the drive.
							Don't remove the drive from the treeview (until it
							has actually been removed). */
							for(itr = m_pDriveList.begin();itr != m_pDriveList.end();itr++)
							{
								if(itr->hDrive == pdbHandle->dbch_handle)
								{
									m_pDirMon->StopDirectoryMonitor(itr->iMonitorId);

									/* Log the removal. If a device removal failure message
									is later received, the last entry logged here will be
									restored. */
									m_bQueryRemoveCompleted = TRUE;
									StringCchCopy(m_szQueryRemove,SIZEOF_ARRAY(m_szQueryRemove),itr->szDrive);
									break;
								}
							}
						}
						break;
				}

				return TRUE;
			}
			break;

		case DBT_DEVICEQUERYREMOVEFAILED:
			{
				/* The device was not removed from the system. */
				DEV_BROADCAST_HDR				*dbh = NULL;
				DEV_BROADCAST_HANDLE			*pdbHandle = NULL;

				dbh = (DEV_BROADCAST_HDR *)lParam;

				switch(dbh->dbch_devicetype)
				{
					case DBT_DEVTYP_HANDLE:
						pdbHandle = (DEV_BROADCAST_HANDLE *)dbh;

						if(m_bQueryRemoveCompleted)
						{
						}
						break;
				}
			}
			break;

		case DBT_DEVICEREMOVECOMPLETE:
			{
				DEV_BROADCAST_HDR				*dbh = NULL;
				DEV_BROADCAST_HANDLE			*pdbHandle = NULL;
				list<DriveEvent_t>::iterator	itr;

				dbh = (DEV_BROADCAST_HDR *)lParam;

				switch(dbh->dbch_devicetype)
				{
					case DBT_DEVTYP_HANDLE:
						{
							pdbHandle = (DEV_BROADCAST_HANDLE *)dbh;

							/* The device was removed from the system.
							Unregister its notification handle. */
							UnregisterDeviceNotification(pdbHandle->dbch_hdevnotify);
						}
						break;

					case DBT_DEVTYP_VOLUME:
						{
							DEV_BROADCAST_VOLUME	*pdbv = NULL;
							SHFILEINFO				shfi;
							HTREEITEM				hItem;
							TVITEM					tvItem;
							TCHAR					DriveLetter;
							TCHAR					DriveName[4];
							TCHAR					szDisplayName[MAX_PATH];

							pdbv = (DEV_BROADCAST_VOLUME *)dbh;

							/* Build a string that will form the drive name. */
							DriveLetter = GetDriveNameFromMask(pdbv->dbcv_unitmask);
							StringCchPrintf(DriveName,SIZEOF_ARRAY(DriveName),_T("%c:\\"),DriveLetter);

							if(pdbv->dbcv_flags & DBTF_MEDIA)
							{
								hItem = LocateItemByPath(DriveName,FALSE);

								if(hItem != NULL)
								{
									SHGetFileInfo(DriveName,0,&shfi,sizeof(shfi),SHGFI_SYSICONINDEX);
									GetDisplayName(DriveName,szDisplayName,SHGDN_INFOLDER);

									/* Update the drives icon and display name. */
									tvItem.mask				= TVIF_HANDLE|TVIF_TEXT|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
									tvItem.hItem			= hItem;
									tvItem.iImage			= shfi.iIcon;
									tvItem.iSelectedImage	= shfi.iIcon;
									tvItem.pszText			= szDisplayName;
									TreeView_SetItem(m_hTreeView,&tvItem);
								}
							}
							else
							{
								/* Remove the drive from the treeview. */
								RemoveItem(DriveName);
							}
						}
						break;
				}

				return TRUE;
			}
			break;
	}

	return FALSE;
}

DWORD WINAPI Thread_MonitorAllDrives(LPVOID pParam)
{
	CMyTreeView *pMyTreeView = NULL;
	TCHAR	*pszDriveStrings = NULL;
	TCHAR	*ptrDrive = NULL;
	DWORD	dwSize;

	pMyTreeView = (CMyTreeView *)pParam;

	dwSize = GetLogicalDriveStrings(0,NULL);

	pszDriveStrings = (TCHAR *)malloc((dwSize + 1) * sizeof(TCHAR));

	if(pszDriveStrings == NULL)
		return 0;

	dwSize = GetLogicalDriveStrings(dwSize,pszDriveStrings);

	if(dwSize != 0)
	{
		ptrDrive = pszDriveStrings;

		while(*ptrDrive != '\0')
		{
			pMyTreeView->MonitorDrivePublic(ptrDrive);

			ptrDrive += lstrlen(ptrDrive) + 1;
		}
	}

	free(pszDriveStrings);

	return 1;
}

void CMyTreeView::MonitorDrivePublic(TCHAR *szDrive)
{
	MonitorDrive(szDrive);
}

void CMyTreeView::MonitorDrive(TCHAR *szDrive)
{
	DirectoryAltered_t		*pDirectoryAltered = NULL;
	DEV_BROADCAST_HANDLE	dbv;
	HANDLE					hDrive;
	HDEVNOTIFY				hDevNotify;
	DriveEvent_t			de;
	int						iMonitorId;

	/* Remote (i.e. network) drives will NOT be monitored. */
	if(GetDriveType(szDrive) != DRIVE_REMOTE)
	{
		hDrive = CreateFile(szDrive,
			FILE_LIST_DIRECTORY,FILE_SHARE_READ|
			FILE_SHARE_DELETE|FILE_SHARE_WRITE,
			NULL,OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|
			FILE_FLAG_OVERLAPPED,NULL);

		if(hDrive != INVALID_HANDLE_VALUE)
		{
			pDirectoryAltered = (DirectoryAltered_t *)malloc(sizeof(DirectoryAltered_t));

			StringCchCopy(pDirectoryAltered->szPath,MAX_PATH,szDrive);
			pDirectoryAltered->pMyTreeView	= this;

			iMonitorId = m_pDirMon->WatchDirectory(hDrive,szDrive,DirWatchFlags,
				CMyTreeView::DirectoryAlteredCallback,TRUE,(void *)pDirectoryAltered);

			dbv.dbch_size		= sizeof(dbv);
			dbv.dbch_devicetype	= DBT_DEVTYP_HANDLE;
			dbv.dbch_handle		= hDrive;

			/* Register to receive hardware events (i.e. insertion,
			removal, etc) for the specified drive. */
			hDevNotify = RegisterDeviceNotification(m_hTreeView,
				&dbv,DEVICE_NOTIFY_WINDOW_HANDLE);

			/* If the handle was successfully registered, log the
			drive path, handle and monitoring id. */
			if(hDevNotify != NULL)
			{
				StringCchCopy(de.szDrive,SIZEOF_ARRAY(de.szDrive),szDrive);
				de.hDrive = hDrive;
				de.iMonitorId = iMonitorId;

				m_pDriveList.push_back(de);
			}
		}
	}
}

HRESULT CMyTreeView::InitializeDragDropHelpers(void)
{
	HRESULT hr;

	/* Initialize the drag source helper, and use it to initialize the drop target helper. */
	hr = CoCreateInstance(CLSID_DragDropHelper,NULL,CLSCTX_INPROC_SERVER,
	IID_IDragSourceHelper,(LPVOID *)&m_pDragSourceHelper);

	if(SUCCEEDED(hr))
	{
		hr = m_pDragSourceHelper->QueryInterface(IID_IDropTargetHelper,(LPVOID *)&m_pDropTargetHelper);

		RegisterDragDrop(m_hTreeView,this);
	}

	return hr;
}

/* IUnknown interface members. */
HRESULT __stdcall CMyTreeView::QueryInterface(REFIID iid, void **ppvObject)
{
	*ppvObject = NULL;

	if(*ppvObject)
	{
		AddRef();
		return S_OK;
	}

	return E_NOINTERFACE;
}

ULONG __stdcall CMyTreeView::AddRef(void)
{
	return ++m_iRefCount;
}

ULONG __stdcall CMyTreeView::Release(void)
{
	m_iRefCount--;
	
	if(m_iRefCount == 0)
	{
		delete this;
		return 0;
	}

	return m_iRefCount;
}

BOOL CMyTreeView::QueryDragging(void)
{
	return m_bDragging;
}

void CMyTreeView::SetShowHidden(BOOL bShowHidden)
{
	m_bShowHidden = bShowHidden;
}

void CMyTreeView::RefreshAllIcons(void)
{
	HTREEITEM	hRoot;
	TVITEMEX	tvItem;
	ItemInfo_t	*pItemInfo = NULL;
	SHFILEINFO	shfi;

	/* Get the root of the tree (root of namespace). */
	hRoot = TreeView_GetRoot(m_hTreeView);

	tvItem.mask				= TVIF_HANDLE|TVIF_PARAM;
	tvItem.hItem			= hRoot;
	TreeView_GetItem(m_hTreeView,&tvItem);

	pItemInfo = &m_pItemInfo[(int)tvItem.lParam];

	SHGetFileInfo((LPCTSTR)pItemInfo->pidl,0,&shfi,sizeof(shfi),SHGFI_PIDL|SHGFI_SYSICONINDEX);

	tvItem.mask				= TVIF_HANDLE|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
	tvItem.hItem			= hRoot;
	tvItem.iImage			= shfi.iIcon;
	tvItem.iSelectedImage	= shfi.iIcon;
	TreeView_SetItem(m_hTreeView,&tvItem);

	RefreshAllIconsInternal(TreeView_GetChild(m_hTreeView,hRoot));
}

void CMyTreeView::RefreshAllIconsInternal(HTREEITEM hFirstSibling)
{
	HTREEITEM	hNextSibling;
	HTREEITEM	hChild;
	TVITEM		tvItem;
	ItemInfo_t	*pItemInfo = NULL;
	SHFILEINFO	shfi;

	hNextSibling = TreeView_GetNextSibling(m_hTreeView,hFirstSibling);

	tvItem.mask				= TVIF_HANDLE|TVIF_PARAM;
	tvItem.hItem			= hFirstSibling;
	TreeView_GetItem(m_hTreeView,&tvItem);

	pItemInfo = &m_pItemInfo[(int)tvItem.lParam];

	SHGetFileInfo((LPCTSTR)pItemInfo->pidl,0,&shfi,sizeof(shfi),SHGFI_PIDL|SHGFI_SYSICONINDEX);

	tvItem.mask				= TVIF_HANDLE|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
	tvItem.hItem			= hFirstSibling;
	tvItem.iImage			= shfi.iIcon;
	tvItem.iSelectedImage	= shfi.iIcon;
	TreeView_SetItem(m_hTreeView,&tvItem);

	hChild = TreeView_GetChild(m_hTreeView,hFirstSibling);

	if(hChild != NULL)
		RefreshAllIconsInternal(hChild);

	while(hNextSibling != NULL)
	{
		tvItem.mask				= TVIF_HANDLE|TVIF_PARAM;
		tvItem.hItem			= hNextSibling;
		TreeView_GetItem(m_hTreeView,&tvItem);

		pItemInfo = &m_pItemInfo[(int)tvItem.lParam];

		SHGetFileInfo((LPCTSTR)pItemInfo->pidl,0,&shfi,sizeof(shfi),SHGFI_PIDL|SHGFI_SYSICONINDEX);

		tvItem.mask				= TVIF_HANDLE|TVIF_IMAGE|TVIF_SELECTEDIMAGE;
		tvItem.hItem			= hNextSibling;
		tvItem.iImage			= shfi.iIcon;
		tvItem.iSelectedImage	= shfi.iIcon;
		TreeView_SetItem(m_hTreeView,&tvItem);

		hChild = TreeView_GetChild(m_hTreeView,hNextSibling);

		if(hChild != NULL)
			RefreshAllIconsInternal(hChild);

		hNextSibling = TreeView_GetNextSibling(m_hTreeView,hNextSibling);
	}
}

void CMyTreeView::OnBeginDrag(LPARAM lParam)
{
	IDataObject			*pDataObject = NULL;
	IDragSourceHelper	*pDragSourceHelper = NULL;
	IShellFolder		*pShellFolder = NULL;
	LPITEMIDLIST		ridl = NULL;
	ItemInfo_t			*pItemInfo = NULL;
	NMTREEVIEW			*pnmtv = NULL;
	DWORD				Effect;
	POINT				pt = {0,0};
	HRESULT				hr;

	pnmtv = (NMTREEVIEW *)lParam;

	hr = CoCreateInstance(CLSID_DragDropHelper,NULL,CLSCTX_ALL,
		IID_IDragSourceHelper,(LPVOID *)&pDragSourceHelper);

	if(SUCCEEDED(hr))
	{
		pItemInfo = &m_pItemInfo[(int)pnmtv->itemNew.lParam];

		hr = SHBindToParent(pItemInfo->pidl,IID_IShellFolder,
			(LPVOID *)&pShellFolder,(LPCITEMIDLIST *)&ridl);

		if(SUCCEEDED(hr))
		{
			/* Needs to be done from the parent folder for the drag/dop to work correctly.
			If done from the desktop folder, only links to files are created. They are
			not copied/moved. */
			pShellFolder->GetUIObjectOf(m_hTreeView,1,(LPCITEMIDLIST *)&ridl,
				IID_IDataObject,NULL,(LPVOID *)&pDataObject);

			hr = pDragSourceHelper->InitializeFromWindow(m_hTreeView,&pt,pDataObject);

			/* TODO: Fix. */
			m_DragType = DRAG_TYPE_LEFTCLICK;

			DoDragDrop(pDataObject,this,DROPEFFECT_COPY|DROPEFFECT_MOVE|
				DROPEFFECT_LINK,&Effect);

			pDataObject->Release();
			pShellFolder->Release();
		}

		pDragSourceHelper->Release();
	}
}