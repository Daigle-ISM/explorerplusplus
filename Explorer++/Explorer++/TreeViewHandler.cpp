// Copyright (C) Explorer++ Project
// SPDX-License-Identifier: GPL-3.0-only
// See LICENSE in the top level directory

#include "stdafx.h"
#include "Explorer++.h"
#include "Config.h"
#include "DarkModeHelper.h"
#include "HolderWindow.h"
#include "MainResource.h"
#include "MainToolbar.h"
#include "ResourceHelper.h"
#include "SetFileAttributesDialog.h"
#include "ShellBrowser/ShellBrowser.h"
#include "ShellBrowser/ShellNavigationController.h"
#include "ShellTreeView/ShellTreeView.h"
#include "TabContainer.h"
#include "../Helper/BulkClipboardWriter.h"
#include "../Helper/DpiCompatibility.h"
#include "../Helper/FileContextMenuManager.h"
#include "../Helper/Helper.h"
#include "../Helper/Macros.h"
#include "../Helper/ShellHelper.h"

LRESULT CALLBACK TreeViewHolderProcStub(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
	UINT_PTR uIdSubclass, DWORD_PTR dwRefData);
LRESULT CALLBACK TreeViewSubclassStub(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
	UINT_PTR uIdSubclass, DWORD_PTR dwRefData);

void Explorerplusplus::CreateFolderControls()
{
	UINT holderStyle = WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN;

	if (m_config->showFolders.get())
	{
		holderStyle |= WS_VISIBLE;
	}

	m_treeViewHolder = HolderWindow::Create(m_hContainer,
		ResourceHelper::LoadString(m_resourceInstance, IDS_FOLDERS_WINDOW_TEXT), holderStyle,
		ResourceHelper::LoadString(m_resourceInstance, IDS_HIDE_FOLDERS_PANE), this);
	m_treeViewHolder->SetCloseButtonClickedCallback(
		std::bind(&Explorerplusplus::ToggleFolders, this));
	m_treeViewHolder->SetResizedCallback(
		std::bind_front(&Explorerplusplus::OnTreeViewHolderResized, this));

	SetWindowSubclass(m_treeViewHolder->GetHWND(), TreeViewHolderProcStub, 0, (DWORD_PTR) this);

	m_shellTreeView = ShellTreeView::Create(m_treeViewHolder->GetHWND(), this,
		GetActivePane()->GetTabContainer(), &m_FileActionHandler, &m_cachedIcons);
	m_treeViewHolder->SetContentChild(m_shellTreeView->GetHWND());

	/* Now, subclass the treeview again. This is needed for messages
	such as WM_MOUSEWHEEL, which need to be intercepted before they
	reach the window procedure provided by ShellTreeView. */
	SetWindowSubclass(m_shellTreeView->GetHWND(), TreeViewSubclassStub, 1, (DWORD_PTR) this);

	m_InitializationFinished.addObserver(
		[this](bool newValue)
		{
			if (newValue)
			{
				// Updating the treeview selection is relatively expensive, so it's
				// not done at all during startup. Therefore, the selection will be
				// set a single time, once the application initialization is
				// complete and all tabs have been restored.
				UpdateTreeViewSelection();
			}
		});

	GetActivePane()->GetTabContainer()->tabCreatedSignal.AddObserver(
		[this](int tabId, BOOL switchToNewTab)
		{
			UNREFERENCED_PARAMETER(tabId);
			UNREFERENCED_PARAMETER(switchToNewTab);

			UpdateTreeViewSelection();
		});

	GetActivePane()->GetTabContainer()->tabNavigationCommittedSignal.AddObserver(
		[this](const Tab &tab, const NavigateParams &navigateParams)
		{
			UNREFERENCED_PARAMETER(tab);
			UNREFERENCED_PARAMETER(navigateParams);

			UpdateTreeViewSelection();
		});

	GetActivePane()->GetTabContainer()->tabSelectedSignal.AddObserver(
		[this](const Tab &tab)
		{
			UNREFERENCED_PARAMETER(tab);

			UpdateTreeViewSelection();
		});

	GetActivePane()->GetTabContainer()->tabRemovedSignal.AddObserver(
		[this](int tabId)
		{
			UNREFERENCED_PARAMETER(tabId);

			UpdateTreeViewSelection();
		});

	m_treeViewInitialized = true;
}

LRESULT CALLBACK TreeViewSubclassStub(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
	UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	UNREFERENCED_PARAMETER(uIdSubclass);

	auto *pContainer = (Explorerplusplus *) dwRefData;

	return pContainer->TreeViewSubclass(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK Explorerplusplus::TreeViewSubclass(HWND hwnd, UINT uMsg, WPARAM wParam,
	LPARAM lParam)
{
	switch (uMsg)
	{
	case WM_SETFOCUS:
		FocusChanged();
		break;
	}

	return DefSubclassProc(hwnd, uMsg, wParam, lParam);
}

void Explorerplusplus::OnShowTreeViewContextMenu(const POINT &ptScreen)
{
	HTREEITEM targetItem;
	POINT finalPoint;
	bool highlightTargetItem = false;

	if (ptScreen.x == -1 && ptScreen.y == -1)
	{
		HTREEITEM selection = TreeView_GetSelection(m_shellTreeView->GetHWND());

		RECT itemRect;
		TreeView_GetItemRect(m_shellTreeView->GetHWND(), selection, &itemRect, TRUE);

		finalPoint = { itemRect.left, itemRect.top + (itemRect.bottom - itemRect.top) / 2 };
		ClientToScreen(m_shellTreeView->GetHWND(), &finalPoint);

		targetItem = selection;
	}
	else
	{
		POINT ptClient = ptScreen;
		ScreenToClient(m_shellTreeView->GetHWND(), &ptClient);

		TVHITTESTINFO hitTestInfo = {};
		hitTestInfo.pt = ptClient;
		auto item = TreeView_HitTest(m_shellTreeView->GetHWND(), &hitTestInfo);

		if (!item)
		{
			return;
		}

		finalPoint = ptScreen;
		targetItem = item;
		highlightTargetItem = true;
	}

	if (highlightTargetItem)
	{
		TreeView_SetItemState(m_shellTreeView->GetHWND(), targetItem, TVIS_DROPHILITED,
			TVIS_DROPHILITED);
	}

	auto pidl = m_shellTreeView->GetNodePidl(targetItem);

	unique_pidl_child child(ILCloneChild(ILFindLastID(pidl.get())));

	ILRemoveLastID(pidl.get());

	FileContextMenuManager contextMenuManager(m_shellTreeView->GetHWND(), pidl.get(),
		{ child.get() });

	FileContextMenuInfo fcmi;
	fcmi.uFrom = FROM_TREEVIEW;

	contextMenuManager.ShowMenu(this, MIN_SHELL_MENU_ID, MAX_SHELL_MENU_ID, &finalPoint,
		m_pStatusBar, nullptr, reinterpret_cast<DWORD_PTR>(&fcmi), TRUE, IsKeyDown(VK_SHIFT));

	if (highlightTargetItem)
	{
		TreeView_SetItemState(m_shellTreeView->GetHWND(), targetItem, 0, TVIS_DROPHILITED);
	}
}

void Explorerplusplus::OnTreeViewCopyItemPath() const
{
	auto hItem = TreeView_GetSelection(m_shellTreeView->GetHWND());

	if (hItem != nullptr)
	{
		auto pidl = m_shellTreeView->GetNodePidl(hItem);

		std::wstring fullFileName;
		GetDisplayName(pidl.get(), SHGDN_FORPARSING, fullFileName);

		BulkClipboardWriter clipboardWriter;
		clipboardWriter.WriteText(fullFileName);
	}
}

void Explorerplusplus::OnTreeViewCopyUniversalPaths() const
{
	HTREEITEM hItem;
	UNIVERSAL_NAME_INFO uni;
	DWORD dwBufferSize;
	DWORD dwRet;

	hItem = TreeView_GetSelection(m_shellTreeView->GetHWND());

	if (hItem != nullptr)
	{
		auto pidl = m_shellTreeView->GetNodePidl(hItem);

		std::wstring fullFileName;
		GetDisplayName(pidl.get(), SHGDN_FORPARSING, fullFileName);

		dwBufferSize = sizeof(uni);
		dwRet = WNetGetUniversalName(fullFileName.c_str(), UNIVERSAL_NAME_INFO_LEVEL,
			(void **) &uni, &dwBufferSize);

		BulkClipboardWriter clipboardWriter;

		if (dwRet == NO_ERROR)
		{
			clipboardWriter.WriteText(uni.lpUniversalName);
		}
		else
		{
			clipboardWriter.WriteText(fullFileName);
		}
	}
}

void Explorerplusplus::OnTreeViewSelectionChangedTimer()
{
	// It's important that the timer be killed here, before the navigation has started. Otherwise,
	// what can happen is that if access to the folder is denied, a dialog will be shown and the
	// message loop will run. That will then cause the timer to fire again, which will start another
	// navigation, ad infinitum.
	KillTimer(m_treeViewHolder->GetHWND(), TREEVIEW_SELECTION_CHANGED_TIMER_ID);

	if (!m_treeViewSelectionChangedEventInfo)
	{
		throw std::runtime_error("Invalid selection change information");
	}

	HandleTreeViewSelectionChanged(&*m_treeViewSelectionChangedEventInfo);

	m_treeViewSelectionChangedEventInfo.reset();
}

void Explorerplusplus::OnTreeViewSelectionChanged(const NMTREEVIEW *eventInfo)
{
	if (!m_treeViewInitialized)
	{
		// The ShellTreeView will select an item initially (to ensure that there's always a selected
		// item). That will take place before the treeview has finished initializing. That initial
		// selection doesn't need to be handled in any way - either the selection will be updated
		// when a navigation occurs (if the synchronize treeview option is enabled), or the
		// selection will remain on the initial item (if the synchronize treeview option is
		// disabled), until the user manually selects another item.
		return;
	}

	KillTimer(m_treeViewHolder->GetHWND(), TREEVIEW_SELECTION_CHANGED_TIMER_ID);
	m_treeViewSelectionChangedEventInfo.reset();

	if (eventInfo->action == TVC_BYKEYBOARD && m_config->treeViewDelayEnabled)
	{
		m_treeViewSelectionChangedEventInfo = *eventInfo;

		// This makes it possible to navigate in the treeview using the keyboard, without triggering
		// a stream of navigations (in the case where a key is being held down and the selection is
		// continuously changing).
		SetTimer(m_treeViewHolder->GetHWND(), TREEVIEW_SELECTION_CHANGED_TIMER_ID,
			TREEVIEW_SELECTION_CHANGED_TIMEOUT, nullptr);
	}
	else
	{
		HandleTreeViewSelectionChanged(eventInfo);
	}
}

void Explorerplusplus::HandleTreeViewSelectionChanged(const NMTREEVIEW *eventInfo)
{
	Tab &selectedTab = GetActivePane()->GetTabContainer()->GetSelectedTab();
	auto pidlCurrentDirectory = selectedTab.GetShellBrowser()->GetDirectoryIdl();

	auto pidlDirectory = m_shellTreeView->GetNodePidl(eventInfo->itemNew.hItem);

	if (ArePidlsEquivalent(pidlDirectory.get(), pidlCurrentDirectory.get()))
	{
		return;
	}

	auto navigateParams = NavigateParams::Normal(pidlDirectory.get());
	HRESULT hr = selectedTab.GetShellBrowser()->GetNavigationController()->Navigate(navigateParams);

	if (SUCCEEDED(hr))
	{
		// The folder will only be expanded if the user explicitly selected it.
		if (m_config->treeViewAutoExpandSelected
			&& (eventInfo->action == TVC_BYMOUSE || eventInfo->action == TVC_BYKEYBOARD))
		{
			TreeView_Expand(m_shellTreeView->GetHWND(), eventInfo->itemNew.hItem, TVE_EXPAND);
		}
	}
	else
	{
		// The navigation failed, so the current folder hasn't changed. All that's needed is to
		// update the treeview selection back to the current folder.
		UpdateTreeViewSelection();
	}
}

LRESULT CALLBACK TreeViewHolderProcStub(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam,
	UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
{
	UNREFERENCED_PARAMETER(uIdSubclass);

	auto *pContainer = (Explorerplusplus *) dwRefData;

	return pContainer->TreeViewHolderProc(hwnd, uMsg, wParam, lParam);
}

LRESULT CALLBACK Explorerplusplus::TreeViewHolderProc(HWND hwnd, UINT msg, WPARAM wParam,
	LPARAM lParam)
{
	switch (msg)
	{
	// WM_CONTEXTMENU will be sent to the treeview window procedure when pressing Shift + F10 or
	// VK_APPS. However, when right-clicking, the WM_CONTEXTMENU message will be sent to the parent.
	// Since WM_CONTEXTMENU messages are sent to the parent if they're not handled, it's easiest to
	// simply handle WM_CONTEXTMENU here, which will cover all three ways in which it can be
	// triggered.
	case WM_CONTEXTMENU:
		if (reinterpret_cast<HWND>(wParam) == m_shellTreeView->GetHWND())
		{
			OnShowTreeViewContextMenu({ GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam) });
			return 0;
		}
		break;

	case WM_NOTIFY:
		return TreeViewHolderWindowNotifyHandler(hwnd, msg, wParam, lParam);

	case WM_TIMER:
		if (wParam == TREEVIEW_SELECTION_CHANGED_TIMER_ID)
		{
			OnTreeViewSelectionChangedTimer();
		}
		break;
	}

	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

LRESULT CALLBACK Explorerplusplus::TreeViewHolderWindowNotifyHandler(HWND hwnd, UINT msg,
	WPARAM wParam, LPARAM lParam)
{
	switch (((LPNMHDR) lParam)->code)
	{
	case TVN_SELCHANGED:
		OnTreeViewSelectionChanged(reinterpret_cast<NMTREEVIEW *>(lParam));
		break;
	}

	return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void Explorerplusplus::OnTreeViewSetFileAttributes() const
{
	auto hItem = TreeView_GetSelection(m_shellTreeView->GetHWND());

	if (hItem == nullptr)
	{
		return;
	}

	std::list<NSetFileAttributesDialogExternal::SetFileAttributesInfo> sfaiList;
	NSetFileAttributesDialogExternal::SetFileAttributesInfo sfai;

	auto pidlItem = m_shellTreeView->GetNodePidl(hItem);

	std::wstring fullFileName;
	HRESULT hr = GetDisplayName(pidlItem.get(), SHGDN_FORPARSING, fullFileName);

	if (hr == S_OK)
	{
		StringCchCopy(sfai.szFullFileName, SIZEOF_ARRAY(sfai.szFullFileName), fullFileName.c_str());

		HANDLE hFindFile = FindFirstFile(sfai.szFullFileName, &sfai.wfd);

		if (hFindFile != INVALID_HANDLE_VALUE)
		{
			FindClose(hFindFile);

			sfaiList.push_back(sfai);

			SetFileAttributesDialog setFileAttributesDialog(m_resourceInstance, m_hContainer,
				sfaiList);
			setFileAttributesDialog.ShowModalDialog();
		}
	}
}

void Explorerplusplus::UpdateTreeViewSelection()
{
	if (!m_InitializationFinished.get() || !m_config->synchronizeTreeview
		|| !m_config->showFolders.get())
	{
		return;
	}

	// When locating a folder in the treeview, each of the parent folders has to be enumerated. UNC
	// paths are contained within the Network folder and that folder can take a significant amount
	// of time to enumerate (e.g. 30 seconds).
	// Therefore, locating a UNC path can take a non-trivial amount of time, as the Network folder
	// will have to be enumerated first. As that work is all done on the main thread, the
	// application will hang while the enumeration completes, something that's especially noticeable
	// on startup.
	// Note that mapped drives don't have that specific issue, as they're contained within the This
	// PC folder. However, there is still the general problem that each parent folder has to be
	// enumerated and all the work is done on the main thread.
	if (!PathIsUNC(m_pActiveShellBrowser->GetDirectory().c_str()))
	{
		HTREEITEM hItem =
			m_shellTreeView->LocateItem(m_pActiveShellBrowser->GetDirectoryIdl().get());

		if (hItem != nullptr)
		{
			SendMessage(m_shellTreeView->GetHWND(), TVM_SELECTITEM, TVGN_CARET, (LPARAM) hItem);
		}
	}
}

void Explorerplusplus::OnTreeViewHolderResized(int newWidth)
{
	m_config->treeViewWidth = newWidth;

	UpdateLayout();
}
