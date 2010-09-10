// Copyright (c) 2010 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/chromeos/tab_closeable_state_watcher.h"

#include "base/command_line.h"
#include "chrome/common/chrome_switches.h"
#include "chrome/browser/defaults.h"
#include "chrome/browser/profile.h"
#include "chrome/browser/tab_contents/tab_contents.h"
#include "chrome/browser/tab_contents/tab_contents_view.h"
#include "chrome/common/notification_service.h"
#include "chrome/common/url_constants.h"

namespace chromeos {

////////////////////////////////////////////////////////////////////////////////
// TabCloseableStateWatcher::TabStripWatcher, public:

TabCloseableStateWatcher::TabStripWatcher::TabStripWatcher(
    TabCloseableStateWatcher* main_watcher, const Browser* browser)
    : main_watcher_(main_watcher),
      browser_(browser) {
  browser_->tabstrip_model()->AddObserver(this);
}

TabCloseableStateWatcher::TabStripWatcher::~TabStripWatcher() {
  browser_->tabstrip_model()->RemoveObserver(this);
}

////////////////////////////////////////////////////////////////////////////////
// TabCloseableStateWatcher::TabStripWatcher,
//     TabStripModelObserver implementation:

void TabCloseableStateWatcher::TabStripWatcher::TabInsertedAt(
    TabContents* tab_contents, int index, bool foreground) {
  main_watcher_->OnTabStripChanged(browser_, false);
}

void TabCloseableStateWatcher::TabStripWatcher::TabClosingAt(
    TabContents* tab_contents, int index) {
  TabStripModel* tabstrip = browser_->tabstrip_model();
  if ((!browser_defaults::kPhantomTabsEnabled && tabstrip->count() == 1) ||
      (browser_defaults::kPhantomTabsEnabled &&
       tabstrip->GetNonPhantomTabCount() == 1 &&
       !tabstrip->IsTabPinned(index))) {
    // Closing last tab.
    main_watcher_->OnTabStripChanged(browser_, true);
  }
}

void TabCloseableStateWatcher::TabStripWatcher::TabDetachedAt(
    TabContents* tab_contents, int index) {
  main_watcher_->OnTabStripChanged(browser_, false);
}

void TabCloseableStateWatcher::TabStripWatcher::TabChangedAt(
    TabContents* tab_contents, int index, TabChangeType change_type) {
  main_watcher_->OnTabStripChanged(browser_, false);
}

////////////////////////////////////////////////////////////////////////////////
// TabCloseableStateWatcher, public:

TabCloseableStateWatcher::TabCloseableStateWatcher()
    : can_close_tab_(true),
      signing_off_(false),
      bwsi_session_(
          CommandLine::ForCurrentProcess()->HasSwitch(switches::kBWSI)) {
  BrowserList::AddObserver(this);
  notification_registrar_.Add(this, NotificationType::APP_EXITING,
      NotificationService::AllSources());
}

TabCloseableStateWatcher::~TabCloseableStateWatcher() {
  BrowserList::RemoveObserver(this);
  DCHECK(tabstrip_watchers_.empty());
}

bool TabCloseableStateWatcher::CanCloseTab(const Browser* browser) const {
  return browser->type() != Browser::TYPE_NORMAL ? true : can_close_tab_;
}

bool TabCloseableStateWatcher::CanCloseBrowser(Browser* browser) {
  BrowserActionType action_type;
  bool can_close = CanCloseBrowserImpl(browser, &action_type);
  if (action_type == OPEN_WINDOW) {
    browser->NewWindow();
  } else if (action_type == OPEN_NTP) {
    // NTP will be opened before closing last tab (via TabStripModelObserver::
    // TabClosingAt), close all tabs now.
    browser->CloseAllTabs();
  }
  return can_close;
}

void TabCloseableStateWatcher::OnWindowCloseCanceled(Browser* browser) {
  // This could be a call to cancel APP_EXITING if user doesn't proceed with
  // unloading handler.
  if (signing_off_) {
    signing_off_ = false;
    CheckAndUpdateState(browser);
  }
}

////////////////////////////////////////////////////////////////////////////////
// TabCloseableStateWatcher, BrowserList::Observer implementation:

void TabCloseableStateWatcher::OnBrowserAdded(const Browser* browser) {
  // Only normal browsers may affect closeable state.
  if (browser->type() != Browser::TYPE_NORMAL)
    return;

  // Create TabStripWatcher to observe tabstrip of new browser.
  tabstrip_watchers_.push_back(new TabStripWatcher(this, browser));

  // When a normal browser is just added, there's no tabs yet, so we wait till
  // TabInsertedAt notification to check for change in state.
}

void TabCloseableStateWatcher::OnBrowserRemoving(const Browser* browser) {
  // Only normal browsers may affect closeable state.
  if (browser->type() != Browser::TYPE_NORMAL)
    return;

  // Remove TabStripWatcher for browser that is being removed.
  for (std::vector<TabStripWatcher*>::iterator it = tabstrip_watchers_.begin();
       it != tabstrip_watchers_.end(); ++it) {
    if ((*it)->browser() == browser) {
      delete (*it);
      tabstrip_watchers_.erase(it);
      break;
    }
  }

  CheckAndUpdateState(NULL);
}

////////////////////////////////////////////////////////////////////////////////
// TabCloseableStateWatcher, NotificationObserver implementation:

void TabCloseableStateWatcher::Observe(NotificationType type,
    const NotificationSource& source, const NotificationDetails& details) {
  if (type.value != NotificationType::APP_EXITING)
    NOTREACHED();
  if (!signing_off_) {
    signing_off_ = true;
    SetCloseableState(true);
  }
}

////////////////////////////////////////////////////////////////////////////////
// TabCloseableStateWatcher, private

void TabCloseableStateWatcher::OnTabStripChanged(const Browser* browser,
    bool closing_last_tab) {
  if (!closing_last_tab) {
    CheckAndUpdateState(browser);
    return;
  }

  // Before closing last tab, open new window or NTP if necessary.
  BrowserActionType action_type;
  CanCloseBrowserImpl(browser, &action_type);
  if (action_type != NONE) {
    Browser* mutable_browser = const_cast<Browser*>(browser);
    if (action_type == OPEN_WINDOW)
      mutable_browser->NewWindow();
    else if (action_type == OPEN_NTP)
      mutable_browser->NewTab();
  }
}

void TabCloseableStateWatcher::CheckAndUpdateState(
    const Browser* browser_to_check) {
  // We shouldn't update state if we're signing off, or there's no normal
  // browser, or browser is always closeable.
  if (signing_off_ || tabstrip_watchers_.empty() ||
      (browser_to_check && browser_to_check->type() != Browser::TYPE_NORMAL))
    return;

  bool new_can_close;

  if (tabstrip_watchers_.size() > 1) {
    new_can_close = true;
  } else {  // There's only 1 normal browser.
    if (!browser_to_check)
      browser_to_check = tabstrip_watchers_[0]->browser();
    if (browser_to_check->profile()->IsOffTheRecord() && !bwsi_session_) {
      new_can_close = true;
    } else {
      TabStripModel* tabstrip_model = browser_to_check->tabstrip_model();
      // If NTP is the only non-phantom tab, it's not closeable.
      if (tabstrip_model->GetNonPhantomTabCount() == 1) {
        int first_non_phantom_tab = tabstrip_model->IndexOfFirstNonPhantomTab();
        new_can_close =
            tabstrip_model->GetTabContentsAt(first_non_phantom_tab)->GetURL() !=
                GURL(chrome::kChromeUINewTabURL);  // Tab is not NewTabPage.
      } else {
        new_can_close = true;
      }
    }
  }

  SetCloseableState(new_can_close);
}

void TabCloseableStateWatcher::SetCloseableState(bool closeable) {
  if (can_close_tab_ == closeable)  // No change in state.
    return;

  can_close_tab_ = closeable;

  // Notify of change in tab closeable state.
  NotificationService::current()->Notify(
      NotificationType::TAB_CLOSEABLE_STATE_CHANGED,
      NotificationService::AllSources(),
      Details<bool>(&can_close_tab_));
}

bool TabCloseableStateWatcher::CanCloseBrowserImpl(const Browser* browser,
    BrowserActionType* action_type) const {
  *action_type = NONE;

  // Browser is always closeable when signing off.
  if (signing_off_)
    return true;

  // Non-normal browsers are always closeable.
  if (browser->type() != Browser::TYPE_NORMAL)
    return true;

  // If this is not the last normal browser, it's always closeable.
  if (tabstrip_watchers_.size() > 1)
    return true;

  // If last normal browser is incognito, open a non-incognito window,
  // and allow closing of incognito one (if not BWSI).
  if (browser->profile()->IsOffTheRecord() && !bwsi_session_) {
    *action_type = OPEN_WINDOW;
    return true;
  }

  // If tab is not closeable, browser is not closeable.
  if (!can_close_tab_)
    return false;

  // Otherwise, close existing tabs, and deny closing of browser.
  // TabClosingAt will open NTP when the last tab is being closed.
  *action_type = OPEN_NTP;
  return false;
}

}  // namespace chromeos
