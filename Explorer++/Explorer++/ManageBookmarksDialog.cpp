/******************************************************************
 *
 * Project: Explorer++
 * File: ManageBookmarksDialog.cpp
 * License: GPL - See COPYING in the top level directory
 *
 * Handles the 'Manage Bookmarks' dialog.
 *
 * Written by David Erceg
 * www.explorerplusplus.com
 *
 *****************************************************************/

#include "stdafx.h"
#include <stack>
#include "Explorer++_internal.h"
#include "ManageBookmarksDialog.h"
#include "BookmarkHelper.h"
#include "MainResource.h"
#include "../Helper/ListViewHelper.h"


namespace NManageBookmarksDialog
{
	int CALLBACK		SortBookmarksStub(LPARAM lParam1,LPARAM lParam2,LPARAM lParamSort);

	LRESULT CALLBACK	EditSearchProcStub(HWND hwnd,UINT uMsg,WPARAM wParam,LPARAM lParam,UINT_PTR uIdSubclass,DWORD_PTR dwRefData);
}

const TCHAR CManageBookmarksDialogPersistentSettings::SETTINGS_KEY[] = _T("ManageBookmarks");

CManageBookmarksDialog::CManageBookmarksDialog(HINSTANCE hInstance,int iResource,HWND hParent,
	CBookmarkFolder &AllBookmarks) :
m_AllBookmarks(AllBookmarks),
m_guidCurrentFolder(AllBookmarks.GetGUID()),
m_SortMode(NBookmarkHelper::SM_NAME),
m_bSortAscending(true),
m_bListViewInitialized(false),
m_bSearchFieldBlank(true),
m_bEditingSearchField(false),
m_hEditSearchFont(NULL),
CBaseDialog(hInstance,iResource,hParent,true)
{
	m_pmbdps = &CManageBookmarksDialogPersistentSettings::GetInstance();

	if(!m_pmbdps->m_bInitialized)
	{
		m_pmbdps->m_guidSelected = AllBookmarks.GetGUID();
		m_pmbdps->m_setExpansion.insert(AllBookmarks.GetGUID());

		m_pmbdps->m_bInitialized = true;
	}
}

CManageBookmarksDialog::~CManageBookmarksDialog()
{

}

BOOL CManageBookmarksDialog::OnInitDialog()
{
	SetDialogIcon();
	SetupSearchField();
	SetupToolbar();
	SetupTreeView();
	SetupListView();

	UpdateToolbarState();

	SetFocus(GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_LISTVIEW));

	return 0;
}

void CManageBookmarksDialog::SetDialogIcon()
{
	HIMAGELIST himl = ImageList_Create(16,16,ILC_COLOR32|ILC_MASK,0,48);
	HBITMAP hBitmap = LoadBitmap(GetInstance(),MAKEINTRESOURCE(IDB_SHELLIMAGES));
	ImageList_Add(himl,hBitmap,NULL);

	m_hDialogIcon = ImageList_GetIcon(himl,SHELLIMAGES_FAV,ILD_NORMAL);
	SetClassLongPtr(m_hDlg,GCLP_HICONSM,reinterpret_cast<LONG_PTR>(m_hDialogIcon));

	DeleteObject(hBitmap);
	ImageList_Destroy(himl);
}

void CManageBookmarksDialog::SetupSearchField()
{
	HWND hEdit = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_EDITSEARCH);
	SetWindowSubclass(hEdit,NManageBookmarksDialog::EditSearchProcStub,
		0,reinterpret_cast<DWORD_PTR>(this));
	SetSearchFieldDefaultState();
}

void CManageBookmarksDialog::SetupToolbar()
{
	m_hToolbar = CreateToolbar(m_hDlg,
		WS_CHILD|WS_VISIBLE|WS_CLIPSIBLINGS|WS_CLIPCHILDREN|
		TBSTYLE_TOOLTIPS|TBSTYLE_LIST|TBSTYLE_TRANSPARENT|
		TBSTYLE_FLAT|CCS_NODIVIDER|CCS_NORESIZE,
		TBSTYLE_EX_MIXEDBUTTONS|TBSTYLE_EX_DRAWDDARROWS|
		TBSTYLE_EX_DOUBLEBUFFER|TBSTYLE_EX_HIDECLIPPEDBUTTONS);

	SendMessage(m_hToolbar,TB_SETBITMAPSIZE,0,MAKELONG(16,16));
	SendMessage(m_hToolbar,TB_BUTTONSTRUCTSIZE,static_cast<WPARAM>(sizeof(TBBUTTON)),0);

	m_himlToolbar = ImageList_Create(16,16,ILC_COLOR32|ILC_MASK,0,48);
	HBITMAP hBitmap = LoadBitmap(GetInstance(),MAKEINTRESOURCE(IDB_SHELLIMAGES));
	ImageList_Add(m_himlToolbar,hBitmap,NULL);
	DeleteObject(hBitmap);
	SendMessage(m_hToolbar,TB_SETIMAGELIST,0,reinterpret_cast<LPARAM>(m_himlToolbar));

	TBBUTTON tbb;
	TCHAR szTemp[64];

	tbb.iBitmap		= SHELLIMAGES_BACK;
	tbb.idCommand	= TOOLBAR_ID_BACK;
	tbb.fsState		= TBSTATE_ENABLED;
	tbb.fsStyle		= BTNS_BUTTON|BTNS_AUTOSIZE;
	tbb.dwData		= 0;
	SendMessage(m_hToolbar,TB_INSERTBUTTON,0,reinterpret_cast<LPARAM>(&tbb));

	tbb.iBitmap		= SHELLIMAGES_FORWARD;
	tbb.idCommand	= TOOLBAR_ID_FORWARD;
	tbb.fsState		= TBSTATE_ENABLED;
	tbb.fsStyle		= BTNS_BUTTON|BTNS_AUTOSIZE;
	tbb.dwData		= 0;
	SendMessage(m_hToolbar,TB_INSERTBUTTON,1,reinterpret_cast<LPARAM>(&tbb));

	LoadString(GetInstance(),IDS_MANAGE_BOOKMARKS_TOOLBAR_ORGANIZE,szTemp,SIZEOF_ARRAY(szTemp));

	tbb.iBitmap		= SHELLIMAGES_COPY;
	tbb.idCommand	= TOOLBAR_ID_ORGANIZE;
	tbb.fsState		= TBSTATE_ENABLED;
	tbb.fsStyle		= BTNS_BUTTON|BTNS_AUTOSIZE|BTNS_SHOWTEXT|BTNS_DROPDOWN;
	tbb.dwData		= 0;
	tbb.iString		= reinterpret_cast<INT_PTR>(szTemp);
	SendMessage(m_hToolbar,TB_INSERTBUTTON,2,reinterpret_cast<LPARAM>(&tbb));

	LoadString(GetInstance(),IDS_MANAGE_BOOKMARKS_TOOLBAR_VIEWS,szTemp,SIZEOF_ARRAY(szTemp));

	tbb.iBitmap		= SHELLIMAGES_VIEWS;
	tbb.idCommand	= TOOLBAR_ID_VIEWS;
	tbb.fsState		= TBSTATE_ENABLED;
	tbb.fsStyle		= BTNS_BUTTON|BTNS_AUTOSIZE|BTNS_SHOWTEXT|BTNS_DROPDOWN;
	tbb.dwData		= 0;
	tbb.iString		= reinterpret_cast<INT_PTR>(szTemp);
	SendMessage(m_hToolbar,TB_INSERTBUTTON,3,reinterpret_cast<LPARAM>(&tbb));

	RECT rcTreeView;
	GetWindowRect(GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_TREEVIEW),&rcTreeView);
	MapWindowPoints(HWND_DESKTOP,m_hDlg,reinterpret_cast<LPPOINT>(&rcTreeView),2);

	RECT rcSearch;
	GetWindowRect(GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_EDITSEARCH),&rcSearch);
	MapWindowPoints(HWND_DESKTOP,m_hDlg,reinterpret_cast<LPPOINT>(&rcSearch),2);

	DWORD dwButtonSize = static_cast<DWORD>(SendMessage(m_hToolbar,TB_GETBUTTONSIZE,0,0));
	SetWindowPos(m_hToolbar,NULL,rcTreeView.left,rcSearch.top - (HIWORD(dwButtonSize) - GetRectHeight(&rcSearch)) / 2,
		rcSearch.left - rcTreeView.left - 10,HIWORD(dwButtonSize),0);
}

void CManageBookmarksDialog::SetupTreeView()
{
	HWND hTreeView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_TREEVIEW);

	m_pBookmarkTreeView = new CBookmarkTreeView(hTreeView);
	m_pBookmarkTreeView->InsertFoldersIntoTreeView(m_AllBookmarks,
		m_pmbdps->m_guidSelected,m_pmbdps->m_setExpansion);
}

void CManageBookmarksDialog::SetupListView()
{
	HWND hListView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_LISTVIEW);

	m_pBookmarkListView = new CBookmarkListView(hListView);

	int iColumn = 0;

	for each(auto ci in m_pmbdps->m_vectorColumnInfo)
	{
		if(ci.bActive)
		{
			LVCOLUMN lvCol;
			TCHAR szTemp[128];

			GetColumnString(ci.ColumnType,szTemp,SIZEOF_ARRAY(szTemp));
			lvCol.mask		= LVCF_TEXT|LVCF_WIDTH;
			lvCol.pszText	= szTemp;
			lvCol.cx		= ci.iWidth;
			ListView_InsertColumn(hListView,iColumn,&lvCol);

			++iColumn;
		}
	}

	m_pBookmarkListView->InsertBookmarksIntoListView(m_AllBookmarks);

	int iItem = 0;

	/* Update the data for each of the sub-items. */
	for(auto itr = m_AllBookmarks.begin();itr != m_AllBookmarks.end();++itr)
	{
		int iSubItem = 1;

		for each(auto ci in m_pmbdps->m_vectorColumnInfo)
		{
			/* The name column will always appear first in
			the set of columns and can be skipped here. */
			if(ci.bActive && ci.ColumnType != CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_NAME)
			{
				TCHAR szColumn[256];
				GetBookmarkItemColumnInfo(*itr,ci.ColumnType,szColumn,SIZEOF_ARRAY(szColumn));
				ListView_SetItemText(hListView,iItem,iSubItem,szColumn);

				++iSubItem;
			}
		}

		++iItem;
	}

	ListView_SortItems(hListView,NManageBookmarksDialog::SortBookmarksStub,reinterpret_cast<LPARAM>(this));

	NListView::ListView_SelectItem(hListView,0,TRUE);

	m_bListViewInitialized = true;
}

int CALLBACK NManageBookmarksDialog::SortBookmarksStub(LPARAM lParam1,LPARAM lParam2,LPARAM lParamSort)
{
	assert(lParamSort != NULL);

	CManageBookmarksDialog *pmbd = reinterpret_cast<CManageBookmarksDialog *>(lParamSort);

	return pmbd->SortBookmarks(lParam1,lParam2);
}

int CALLBACK CManageBookmarksDialog::SortBookmarks(LPARAM lParam1,LPARAM lParam2)
{
	/* TODO: Need to be able to retrieve items using their lParam
	value. */
	HWND hTreeView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_TREEVIEW);
	HTREEITEM hSelected = TreeView_GetSelection(hTreeView);
	CBookmarkFolder &BookmarkFolder = m_pBookmarkTreeView->GetBookmarkFolderFromTreeView(hSelected,m_AllBookmarks);

	NBookmarkHelper::variantBookmark_t variantBookmark1 = m_pBookmarkListView->GetBookmarkItemFromListView(BookmarkFolder,0);
	NBookmarkHelper::variantBookmark_t variantBookmark2 = m_pBookmarkListView->GetBookmarkItemFromListView(BookmarkFolder,1);

	int iRes = 0;

	switch(m_SortMode)
	{
	case NBookmarkHelper::SM_NAME:
		iRes = NBookmarkHelper::SortByName(variantBookmark1,variantBookmark2);
		break;
	}

	if(!m_bSortAscending)
	{
		iRes = -iRes;
	}

	return iRes;
}

/* Changes the font within the search edit
control, and sets the default text. */
void CManageBookmarksDialog::SetSearchFieldDefaultState()
{
	HWND hEdit = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_EDITSEARCH);

	LOGFONT lf;
	HFONT hCurentFont = reinterpret_cast<HFONT>(SendMessage(hEdit,WM_GETFONT,0,0));
	GetObject(hCurentFont,sizeof(lf),reinterpret_cast<LPVOID>(&lf));

	HFONT hPrevEditSearchFont = m_hEditSearchFont;

	lf.lfItalic = TRUE;
	m_hEditSearchFont = CreateFontIndirect(&lf);
	SendMessage(hEdit,WM_SETFONT,reinterpret_cast<WPARAM>(m_hEditSearchFont),MAKEWORD(TRUE,0));

	if(hPrevEditSearchFont != NULL)
	{
		DeleteFont(hPrevEditSearchFont);
	}

	TCHAR szTemp[64];
	LoadString(GetInstance(),IDS_MANAGE_BOOKMARKS_DEFAULT_SEARCH_TEXT,szTemp,SIZEOF_ARRAY(szTemp));
	SetWindowText(hEdit,szTemp);
}

/* Resets the font within the search
field and removes any text. */
void CManageBookmarksDialog::RemoveSearchFieldDefaultState()
{
	HWND hEdit = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_EDITSEARCH);

	LOGFONT lf;
	HFONT hCurentFont = reinterpret_cast<HFONT>(SendMessage(hEdit,WM_GETFONT,0,0));
	GetObject(hCurentFont,sizeof(lf),reinterpret_cast<LPVOID>(&lf));

	HFONT hPrevEditSearchFont = m_hEditSearchFont;

	lf.lfItalic = FALSE;
	m_hEditSearchFont = CreateFontIndirect(&lf);
	SendMessage(hEdit,WM_SETFONT,reinterpret_cast<WPARAM>(m_hEditSearchFont),MAKEWORD(TRUE,0));

	DeleteFont(hPrevEditSearchFont);

	SetWindowText(hEdit,EMPTY_STRING);
}

LRESULT CALLBACK NManageBookmarksDialog::EditSearchProcStub(HWND hwnd,UINT uMsg,
	WPARAM wParam,LPARAM lParam,UINT_PTR uIdSubclass,DWORD_PTR dwRefData)
{
	CManageBookmarksDialog *pmbd = reinterpret_cast<CManageBookmarksDialog *>(dwRefData);

	return pmbd->EditSearchProc(hwnd,uMsg,wParam,lParam);
}

LRESULT CALLBACK CManageBookmarksDialog::EditSearchProc(HWND hwnd,UINT Msg,WPARAM wParam,LPARAM lParam)
{
	switch(Msg)
	{
	case WM_SETFOCUS:
		if(m_bSearchFieldBlank)
		{
			RemoveSearchFieldDefaultState();
		}

		m_bEditingSearchField = true;
		break;

	case WM_KILLFOCUS:
		if(GetWindowTextLength(hwnd) == 0)
		{
			m_bSearchFieldBlank = true;
			SetSearchFieldDefaultState();
		}
		else
		{
			m_bSearchFieldBlank = false;
		}

		m_bEditingSearchField = false;
		break;
	}

	return DefSubclassProc(hwnd,Msg,wParam,lParam);
}

INT_PTR CManageBookmarksDialog::OnCtlColorEdit(HWND hwnd,HDC hdc)
{
	if(hwnd == GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_EDITSEARCH))
	{
		if(m_bSearchFieldBlank &&
			!m_bEditingSearchField)
		{
			SetTextColor(hdc,SEARCH_TEXT_COLOR);
			SetBkMode(hdc,TRANSPARENT);
			return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
		}
		else
		{
			SetTextColor(hdc,GetSysColor(COLOR_WINDOWTEXT));
			SetBkMode(hdc,TRANSPARENT);
			return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_WINDOW));
		}
	}

	return 0;
}

BOOL CManageBookmarksDialog::OnAppCommand(HWND hwnd,UINT uCmd,UINT uDevice,DWORD dwKeys)
{
	switch(uCmd)
	{
	case APPCOMMAND_BROWSER_BACKWARD:
		BrowseBack();
		break;

	case APPCOMMAND_BROWSER_FORWARD:
		BrowseForward();
		break;
	}

	return 0;
}

BOOL CManageBookmarksDialog::OnCommand(WPARAM wParam,LPARAM lParam)
{
	switch(LOWORD(wParam))
	{
	case EN_CHANGE:
		OnEnChange(reinterpret_cast<HWND>(lParam));
		break;

	/* TODO: */
	case IDM_MB_BOOKMARK_OPEN:
		break;

	case IDM_MB_BOOKMARK_OPENINNEWTAB:
		break;

	case IDM_MB_BOOKMARK_OPENINNEWWINDOW:
		break;

	case IDM_MB_BOOKMARK_CUT:
		break;

	case IDM_MB_BOOKMARK_COPY:
		break;

	case IDM_MB_BOOKMARK_DELETE:
		break;

	case IDOK:
		OnOk();
		break;

	case IDCANCEL:
		OnCancel();
		break;
	}

	return 0;
}

BOOL CManageBookmarksDialog::OnNotify(NMHDR *pnmhdr)
{
	switch(pnmhdr->code)
	{
	case NM_DBLCLK:
		OnDblClk(pnmhdr);
		break;

	case NM_RCLICK:
		OnRClick(pnmhdr);
		break;

	case TBN_DROPDOWN:
		OnTbnDropDown(reinterpret_cast<NMTOOLBAR *>(pnmhdr));
		break;

	case TVN_SELCHANGED:
		OnTvnSelChanged(reinterpret_cast<NMTREEVIEW *>(pnmhdr));
		break;

	case LVN_KEYDOWN:
		OnLvnKeyDown(reinterpret_cast<NMLVKEYDOWN *>(pnmhdr));
		break;
	}

	return 0;
}

void CManageBookmarksDialog::OnEnChange(HWND hEdit)
{
	if(hEdit != GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_EDITSEARCH))
	{
		return;
	}

	std::wstring strSearch;
	GetWindowString(hEdit,strSearch);

	if(strSearch.size() > 0)
	{

	}
}

void CManageBookmarksDialog::OnListViewRClick()
{
	DWORD dwCursorPos = GetMessagePos();

	POINT ptCursor;
	ptCursor.x = GET_X_LPARAM(dwCursorPos);
	ptCursor.y = GET_Y_LPARAM(dwCursorPos);

	HWND hListView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_LISTVIEW);

	LVHITTESTINFO lvhti;
	lvhti.pt = ptCursor;
	ScreenToClient(hListView,&lvhti.pt);
	int iItem = ListView_HitTest(hListView,&lvhti);

	if(iItem == -1)
	{
		return;
	}

	HMENU hMenu = LoadMenu(GetInstance(),MAKEINTRESOURCE(IDR_MANAGEBOOKMARKS_BOOKMARK_RCLICK_MENU));
	SetMenuDefaultItem(GetSubMenu(hMenu,0),IDM_MB_BOOKMARK_OPEN,FALSE);

	TrackPopupMenu(GetSubMenu(hMenu,0),TPM_LEFTALIGN,ptCursor.x,ptCursor.y,0,m_hDlg,NULL);
	DestroyMenu(hMenu);
}

void CManageBookmarksDialog::OnListViewHeaderRClick()
{
	DWORD dwCursorPos = GetMessagePos();

	POINT ptCursor;
	ptCursor.x = GET_X_LPARAM(dwCursorPos);
	ptCursor.y = GET_Y_LPARAM(dwCursorPos);

	HMENU hMenu = CreatePopupMenu();
	int iItem = 0;

	for each(auto ci in m_pmbdps->m_vectorColumnInfo)
	{
		TCHAR szColumn[128];
		GetColumnString(ci.ColumnType,szColumn,SIZEOF_ARRAY(szColumn));

		MENUITEMINFO mii;
		mii.cbSize		= sizeof(mii);
		mii.fMask		= MIIM_ID|MIIM_STRING|MIIM_STATE;
		mii.wID			= ci.ColumnType;
		mii.dwTypeData	= szColumn;
		mii.fState		= 0;

		if(ci.bActive)
		{
			mii.fState |= MFS_CHECKED;
		}

		/* The name column cannot be removed. */
		if(ci.ColumnType == CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_NAME)
		{
			mii.fState |= MFS_DISABLED;
		}

		InsertMenuItem(hMenu,iItem,TRUE,&mii);

		++iItem;
	}

	int iCmd = TrackPopupMenu(hMenu,TPM_LEFTALIGN|TPM_RETURNCMD,ptCursor.x,ptCursor.y,0,m_hDlg,NULL);
	DestroyMenu(hMenu);

	int iColumn = 0;

	for(auto itr = m_pmbdps->m_vectorColumnInfo.begin();itr != m_pmbdps->m_vectorColumnInfo.end();++itr)
	{
		if(itr->ColumnType == iCmd)
		{
			HWND hListView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_LISTVIEW);

			if(itr->bActive)
			{
				itr->iWidth = ListView_GetColumnWidth(hListView,iColumn);
				ListView_DeleteColumn(hListView,iColumn);
			}
			else
			{
				LVCOLUMN lvCol;
				TCHAR szTemp[128];

				GetColumnString(itr->ColumnType,szTemp,SIZEOF_ARRAY(szTemp));
				lvCol.mask		= LVCF_TEXT|LVCF_WIDTH;
				lvCol.pszText	= szTemp;
				lvCol.cx		= itr->iWidth;
				ListView_InsertColumn(hListView,iColumn,&lvCol);

				HWND hTreeView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_TREEVIEW);
				HTREEITEM hSelected = TreeView_GetSelection(hTreeView);
				CBookmarkFolder &BookmarkFolder = m_pBookmarkTreeView->GetBookmarkFolderFromTreeView(hSelected,m_AllBookmarks);

				int iItem = 0;

				for(auto itrBookmarks = BookmarkFolder.begin();itrBookmarks != BookmarkFolder.end();++itrBookmarks)
				{
					TCHAR szColumn[256];
					GetBookmarkItemColumnInfo(*itrBookmarks,itr->ColumnType,szColumn,SIZEOF_ARRAY(szColumn));
					ListView_SetItemText(hListView,iItem,iColumn,szColumn);

					++iItem;
				}
			}

			itr->bActive = !itr->bActive;

			break;
		}
		else
		{
			if(itr->bActive)
			{
				++iColumn;
			}
		}
	}
}

void CManageBookmarksDialog::OnLvnKeyDown(NMLVKEYDOWN *pnmlvkd)
{
	switch(pnmlvkd->wVKey)
	{
	case 'A':
		if((GetKeyState(VK_CONTROL) & 0x80) &&
			!(GetKeyState(VK_SHIFT) & 0x80) &&
			!(GetKeyState(VK_MENU) & 0x80))
		{
			HWND hListView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_LISTVIEW);
			NListView::ListView_SelectAllItems(hListView,TRUE);
			SetFocus(hListView);
		}
		break;
	}
}

void CManageBookmarksDialog::GetColumnString(CManageBookmarksDialogPersistentSettings::ColumnType_t ColumnType,
	TCHAR *szColumn,UINT cchBuf)
{
	UINT uResourceID = 0;

	switch(ColumnType)
	{
	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_NAME:
		uResourceID = IDS_MANAGE_BOOKMARKS_COLUMN_NAME;
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_LOCATION:
		uResourceID = IDS_MANAGE_BOOKMARKS_COLUMN_LOCATION;
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_VISIT_DATE:
		uResourceID = IDS_MANAGE_BOOKMARKS_COLUMN_VISIT_DATE;
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_VISIT_COUNT:
		uResourceID = IDS_MANAGE_BOOKMARKS_COLUMN_VISIT_COUNT;
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_ADDED:
		uResourceID = IDS_MANAGE_BOOKMARKS_COLUMN_ADDED;
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_LAST_MODIFIED:
		uResourceID = IDS_MANAGE_BOOKMARKS_COLUMN_LAST_MODIFIED;
		break;

	default:
		assert(FALSE);
		break;
	}

	LoadString(GetInstance(),uResourceID,szColumn,cchBuf);
}

void CManageBookmarksDialog::GetBookmarkItemColumnInfo(const NBookmarkHelper::variantBookmark_t variantBookmark,
	CManageBookmarksDialogPersistentSettings::ColumnType_t ColumnType,TCHAR *szColumn,size_t cchBuf)
{
	if(variantBookmark.type() == typeid(CBookmarkFolder))
	{
		const CBookmarkFolder &BookmarkFolder = boost::get<CBookmarkFolder>(variantBookmark);
		GetBookmarkFolderColumnInfo(BookmarkFolder,ColumnType,szColumn,cchBuf);
	}
	else
	{
		const CBookmark &Bookmark = boost::get<CBookmark>(variantBookmark);
		GetBookmarkColumnInfo(Bookmark,ColumnType,szColumn,cchBuf);
	}
}

void CManageBookmarksDialog::GetBookmarkColumnInfo(const CBookmark &Bookmark,
	CManageBookmarksDialogPersistentSettings::ColumnType_t ColumnType,
	TCHAR *szColumn,size_t cchBuf)
{
	switch(ColumnType)
	{
	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_NAME:
		StringCchCopy(szColumn,cchBuf,Bookmark.GetName().c_str());
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_LOCATION:
		StringCchCopy(szColumn,cchBuf,Bookmark.GetLocation().c_str());
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_VISIT_DATE:
		{
			/* TODO: Friendly dates. */
			FILETIME ftLastVisited = Bookmark.GetDateLastVisited();
			CreateFileTimeString(&ftLastVisited,szColumn,static_cast<int>(cchBuf),FALSE);
		}
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_VISIT_COUNT:
		StringCchPrintf(szColumn,cchBuf,_T("%d"),Bookmark.GetVisitCount());
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_ADDED:
		{
			/* TODO: Friendly dates. */
			FILETIME ftCreated = Bookmark.GetDateCreated();
			CreateFileTimeString(&ftCreated,szColumn,static_cast<int>(cchBuf),FALSE);
		}
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_LAST_MODIFIED:
		{
			/* TODO: Friendly dates. */
			FILETIME ftModified = Bookmark.GetDateModified();
			CreateFileTimeString(&ftModified,szColumn,static_cast<int>(cchBuf),FALSE);
		}
		break;

	default:
		assert(FALSE);
		break;
	}
}

void CManageBookmarksDialog::GetBookmarkFolderColumnInfo(const CBookmarkFolder &BookmarkFolder,
	CManageBookmarksDialogPersistentSettings::ColumnType_t ColumnType,TCHAR *szColumn,size_t cchBuf)
{
	switch(ColumnType)
	{
	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_NAME:
		StringCchCopy(szColumn,cchBuf,BookmarkFolder.GetName().c_str());
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_LOCATION:
		StringCchCopy(szColumn,cchBuf,EMPTY_STRING);
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_VISIT_DATE:
		StringCchCopy(szColumn,cchBuf,EMPTY_STRING);
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_VISIT_COUNT:
		StringCchCopy(szColumn,cchBuf,EMPTY_STRING);
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_ADDED:
		{
			/* TODO: Friendly dates. */
			FILETIME ftCreated = BookmarkFolder.GetDateCreated();
			CreateFileTimeString(&ftCreated,szColumn,static_cast<int>(cchBuf),FALSE);
		}
		break;

	case CManageBookmarksDialogPersistentSettings::COLUMN_TYPE_LAST_MODIFIED:
		{
			/* TODO: Friendly dates. */
			FILETIME ftModified = BookmarkFolder.GetDateModified();
			CreateFileTimeString(&ftModified,szColumn,static_cast<int>(cchBuf),FALSE);
		}
		break;

	default:
		assert(FALSE);
		break;
	}
}

void CManageBookmarksDialog::OnTbnDropDown(NMTOOLBAR *nmtb)
{
	switch(nmtb->iItem)
	{
	case TOOLBAR_ID_VIEWS:
		{
			HMENU hMenu = LoadMenu(GetInstance(),MAKEINTRESOURCE(IDR_MANAGEBOOKMARKS_VIEW_MENU));

			RECT rcButton;
			SendMessage(m_hToolbar,TB_GETRECT,TOOLBAR_ID_VIEWS,reinterpret_cast<LPARAM>(&rcButton));

			POINT pt;
			pt.x = rcButton.left;
			pt.y = rcButton.bottom;
			ClientToScreen(m_hToolbar,&pt);

			TrackPopupMenu(GetSubMenu(hMenu,0),TPM_LEFTALIGN,pt.x,pt.y,0,m_hDlg,NULL);
			DestroyMenu(hMenu);
		}
		break;

	case TOOLBAR_ID_ORGANIZE:
		{
			HMENU hMenu = LoadMenu(GetInstance(),MAKEINTRESOURCE(IDR_MANAGEBOOKMARKS_ORGANIZE_MENU));

			RECT rcButton;
			SendMessage(m_hToolbar,TB_GETRECT,TOOLBAR_ID_ORGANIZE,reinterpret_cast<LPARAM>(&rcButton));

			POINT pt;
			pt.x = rcButton.left;
			pt.y = rcButton.bottom;
			ClientToScreen(m_hToolbar,&pt);

			TrackPopupMenu(GetSubMenu(hMenu,0),TPM_LEFTALIGN,pt.x,pt.y,0,m_hDlg,NULL);
			DestroyMenu(hMenu);
		}
		break;
	}
}

void CManageBookmarksDialog::OnTvnSelChanged(NMTREEVIEW *pnmtv)
{
	/* This message will come in once before the listview has been
	properly initialized (due to the selection been set in
	the treeview), and can be ignored. */
	if(!m_bListViewInitialized)
	{
		return;
	}

	CBookmarkFolder &BookmarkFolder = m_pBookmarkTreeView->GetBookmarkFolderFromTreeView(pnmtv->itemNew.hItem,m_AllBookmarks);

	if(IsEqualGUID(BookmarkFolder.GetGUID(),m_guidCurrentFolder))
	{
		return;
	}

	BrowseBookmarkFolder(BookmarkFolder);
}

void CManageBookmarksDialog::OnDblClk(NMHDR *pnmhdr)
{
	HWND hListView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_LISTVIEW);

	if(pnmhdr->hwndFrom == hListView)
	{
		NMITEMACTIVATE *pnmia = reinterpret_cast<NMITEMACTIVATE *>(pnmhdr);

		if(pnmia->iItem == -1)
		{
			return;
		}

		HWND hTreeView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_TREEVIEW);
		HTREEITEM hSelected = TreeView_GetSelection(hTreeView);
		CBookmarkFolder &ParentBookmarkFolder = m_pBookmarkTreeView->GetBookmarkFolderFromTreeView(hSelected,m_AllBookmarks);
		NBookmarkHelper::variantBookmark_t variantBookmark = m_pBookmarkListView->GetBookmarkItemFromListView(
			ParentBookmarkFolder,pnmia->iItem);

		if(variantBookmark.type() == typeid(CBookmarkFolder))
		{
			CBookmarkFolder &BookmarkFolder = boost::get<CBookmarkFolder>(variantBookmark);
			BrowseBookmarkFolder(BookmarkFolder);
		}
		else if(variantBookmark.type() == typeid(CBookmark))
		{
			/* TODO: Send the bookmark back to the main
			window to open. */
		}
	}
}

void CManageBookmarksDialog::BrowseBookmarkFolder(const CBookmarkFolder &BookmarkFolder)
{
	m_stackBack.push(m_guidCurrentFolder);

	m_guidCurrentFolder = BookmarkFolder.GetGUID();
	m_pBookmarkTreeView->SelectFolder(BookmarkFolder.GetGUID());
	m_pBookmarkListView->InsertBookmarksIntoListView(BookmarkFolder);

	UpdateToolbarState();
}

void CManageBookmarksDialog::BrowseBack()
{
	if(m_stackBack.size() == 0)
	{
		return;
	}

	GUID guid = m_stackBack.top();
	m_stackBack.pop();
	m_stackForward.push(guid);

	/* TODO: Browse back. */
}

void CManageBookmarksDialog::BrowseForward()
{
	if(m_stackForward.size() == 0)
	{
		return;
	}
}

void CManageBookmarksDialog::UpdateToolbarState()
{
	SendMessage(m_hToolbar,TB_ENABLEBUTTON,TOOLBAR_ID_BACK,m_stackBack.size() != 0);
	SendMessage(m_hToolbar,TB_ENABLEBUTTON,TOOLBAR_ID_FORWARD,m_stackForward.size() != 0);
}

void CManageBookmarksDialog::OnRClick(NMHDR *pnmhdr)
{
	HWND hListView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_LISTVIEW);
	HWND hTreeView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_TREEVIEW);

	if(pnmhdr->hwndFrom == hListView)
	{
		OnListViewRClick();
	}
	else if(pnmhdr->hwndFrom == ListView_GetHeader(hListView))
	{
		OnListViewHeaderRClick();
	}
	else if(pnmhdr->hwndFrom == hTreeView)
	{

	}
}

void CManageBookmarksDialog::OnOk()
{
	DestroyWindow(m_hDlg);
}

void CManageBookmarksDialog::OnCancel()
{
	DestroyWindow(m_hDlg);
}

BOOL CManageBookmarksDialog::OnClose()
{
	DestroyWindow(m_hDlg);
	return 0;
}

BOOL CManageBookmarksDialog::OnDestroy()
{
	DestroyIcon(m_hDialogIcon);
	DeleteFont(m_hEditSearchFont);
	ImageList_Destroy(m_himlToolbar);

	return 0;
}

void CManageBookmarksDialog::SaveState()
{
	m_pmbdps->SaveDialogPosition(m_hDlg);

	HWND hListView = GetDlgItem(m_hDlg,IDC_MANAGEBOOKMARKS_LISTVIEW);
	int iColumn = 0;

	for(auto itr = m_pmbdps->m_vectorColumnInfo.begin();itr != m_pmbdps->m_vectorColumnInfo.end();++itr)
	{
		if(itr->bActive)
		{
			itr->iWidth = ListView_GetColumnWidth(hListView,iColumn);
			++iColumn;
		}
	}

	m_pmbdps->m_bStateSaved = TRUE;
}

CManageBookmarksDialogPersistentSettings::CManageBookmarksDialogPersistentSettings() :
CDialogSettings(SETTINGS_KEY)
{
	m_bInitialized = false;

	SetupDefaultColumns();

	/* TODO: Save listview selection information. */
}

CManageBookmarksDialogPersistentSettings::~CManageBookmarksDialogPersistentSettings()
{
	
}

CManageBookmarksDialogPersistentSettings& CManageBookmarksDialogPersistentSettings::GetInstance()
{
	static CManageBookmarksDialogPersistentSettings mbdps;
	return mbdps;
}

void CManageBookmarksDialogPersistentSettings::SetupDefaultColumns()
{
	ColumnInfo_t ci;

	ci.ColumnType	= COLUMN_TYPE_NAME;
	ci.iWidth		= DEFAULT_MANAGE_BOOKMARKS_COLUMN_WIDTH;
	ci.bActive		= TRUE;
	m_vectorColumnInfo.push_back(ci);

	ci.ColumnType	= COLUMN_TYPE_LOCATION;
	ci.iWidth		= DEFAULT_MANAGE_BOOKMARKS_COLUMN_WIDTH;
	ci.bActive		= TRUE;
	m_vectorColumnInfo.push_back(ci);

	ci.ColumnType	= COLUMN_TYPE_VISIT_DATE;
	ci.iWidth		= DEFAULT_MANAGE_BOOKMARKS_COLUMN_WIDTH;
	ci.bActive		= FALSE;
	m_vectorColumnInfo.push_back(ci);

	ci.ColumnType	= COLUMN_TYPE_VISIT_COUNT;
	ci.iWidth		= DEFAULT_MANAGE_BOOKMARKS_COLUMN_WIDTH;
	ci.bActive		= FALSE;
	m_vectorColumnInfo.push_back(ci);

	ci.ColumnType	= COLUMN_TYPE_ADDED;
	ci.iWidth		= DEFAULT_MANAGE_BOOKMARKS_COLUMN_WIDTH;
	ci.bActive		= FALSE;
	m_vectorColumnInfo.push_back(ci);

	ci.ColumnType	= COLUMN_TYPE_LAST_MODIFIED;
	ci.iWidth		= DEFAULT_MANAGE_BOOKMARKS_COLUMN_WIDTH;
	ci.bActive		= FALSE;
	m_vectorColumnInfo.push_back(ci);
}