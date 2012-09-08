
/*
 * Copyright (c) 2011-2012, Robin Burchell
 * Copyright (c) 2012, Timur Kristóf <venemo@fedoraproject.org>
 * Copyright (c) 2011, Intel Corporation.
 *
 * This program is licensed under the terms and conditions of the
 * Apache License, version 2.0.  The full text of the Apache License is at
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 */

#include <QDir>
#include <QFileInfoList>
#include <QFileSystemWatcher>
#include <QRegExp>
#include <QX11Info>

#include "switchermodel.h"
#include "xtools/xatomcache.h"

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>

// Define this if you'd like to see debug messages from the switcher
#ifdef DEBUG_SWITCHER
#define SWITCHER_DEBUG(things) qDebug() << Q_FUNC_INFO << things
#else
#define SWITCHER_DEBUG(things)
#endif

SwitcherModel::SwitcherModel(QObject *parent)
    : QObjectListModel(parent)
{
}

SwitcherModel::~SwitcherModel()
{
}

bool SwitcherModel::handleXEvent(const XEvent &event)
{
    if (event.type == PropertyNotify &&
            event.xproperty.window == DefaultRootWindow(QX11Info::display()) &&
            event.xproperty.atom == AtomCache::atom("_NET_CLIENT_LIST"))
    {
        updateWindowList();
        return true;
    }
    else if (event.type == ClientMessage &&
             event.xclient.message_type == AtomCache::atom("_NET_CLOSE_WINDOW"))
    {
        SWITCHER_DEBUG("Got close WindowInfo event for " << event.xclient.window);
        if (!windowsBeingClosed.contains(event.xclient.window))
        {
            windowsBeingClosed.append(event.xclient.window);
        }
        updateWindowList();
        return true;
    }
    else if (event.type == PropertyNotify &&
             (event.xproperty.atom == AtomCache::atom("_NET_WM_WINDOW_TYPE") ||
              event.xproperty.atom == AtomCache::atom("_NET_WM_STATE")))
    {
        updateWindowList();
        return true;
    }
    else if (event.type == PropertyNotify &&
             event.xproperty.atom == AtomCache::atom("_NET_ACTIVE_WINDOW"))
    {
        updateWindowList();
        return true;
    }

    return false;
}

static QVector<Atom> getNetWmState(Display *display, Window window)
{
    QVector<Atom> atomList;

    Atom actualType;
    int actualFormat;
    ulong propertyLength;
    ulong bytesLeft;
    uchar *propertyData = 0;

    // Step 1: Get the size of the list
    bool result = XGetWindowProperty(display, window, AtomCache::atom("_NET_WM_STATE"), 0, 0,
                                     false, XA_ATOM, &actualType,
                                     &actualFormat, &propertyLength,
                                     &bytesLeft, &propertyData);
    if (result == Success && actualType == XA_ATOM && actualFormat == 32)
    {
        atomList.resize(bytesLeft / sizeof(Atom));
        XFree(propertyData);

        // Step 2: Get the actual list
        if (XGetWindowProperty(display, window, AtomCache::atom("_NET_WM_STATE"), 0,
                               atomList.size(), false, XA_ATOM,
                               &actualType, &actualFormat,
                               &propertyLength, &bytesLeft,
                               &propertyData) == Success)
        {
            if (propertyLength != (ulong)atomList.size())
            {
                atomList.resize(propertyLength);
            }

            if (!atomList.isEmpty())
            {
                memcpy(atomList.data(), propertyData,
                       atomList.size() * sizeof(Atom));
            }
            XFree(propertyData);
        }
        else
        {
            qWarning("Unable to retrieve window properties: %i", (int) window);
            atomList.clear();
        }
    }

    return atomList;
}

void SwitcherModel::updateWindowList()
{
    SWITCHER_DEBUG("Updating window list");

    Display *dpy = QX11Info::display();
    XWindowAttributes wAttributes;
    Atom actualType;
    int actualFormat;
    unsigned long numWindowItems, bytesLeft;
    unsigned char *windowData = NULL;

    int result = XGetWindowProperty(dpy,
                                    DefaultRootWindow(dpy),
                                    AtomCache::atom("_NET_CLIENT_LIST"),
                                    0, 0x7fffffff,
                                    false, XA_WINDOW,
                                    &actualType,
                                    &actualFormat,
                                    &numWindowItems,
                                    &bytesLeft,
                                    &windowData);

    if (result != Success || windowData == None)
        return;

    SWITCHER_DEBUG("Read list of " << numWindowItems << " windows");
    QList<WindowInfo*> *windowList = new QList<WindowInfo*>();
    Window *wins = (Window *)windowData;

    for (unsigned int i = 0; i < numWindowItems; i++)
    {
        if (XGetWindowAttributes(dpy, wins[i], &wAttributes) != 0
                && wAttributes.width > 0 &&
                wAttributes.height > 0 && wAttributes.c_class == InputOutput &&
                wAttributes.map_state != IsUnmapped)
        {
            unsigned char *typeData = NULL;
            unsigned long numTypeItems;

            result = XGetWindowProperty(dpy,
                                        wins[i],
                                        AtomCache::atom("_NET_WM_WINDOW_TYPE"),
                                        0L, 16L, false,
                                        XA_ATOM,
                                        &actualType,
                                        &actualFormat,
                                        &numTypeItems,
                                        &bytesLeft,
                                        &typeData);
            if (result == Success)
            {
                Atom *type = (Atom *)typeData;

                bool includeInWindowList = false;

                // plain Xlib windows do not have a type
                if (numTypeItems == 0)
                    includeInWindowList = true;
                for (unsigned int n = 0; n < numTypeItems; n++)
                {
                    if (type[n] == AtomCache::atom("_NET_WM_WINDOW_TYPE_DESKTOP") ||
                            type[n] == AtomCache::atom("_NET_WM_WINDOW_TYPE_NOTIFICATION") ||
                            type[n] == AtomCache::atom("_NET_WM_WINDOW_TYPE_DOCK") ||
                            type[n] == AtomCache::atom("_NET_WM_WINDOW_TYPE_MENU"))
                    {
                        includeInWindowList = false;
                        break;
                    }
                    if (type[n] == AtomCache::atom("_NET_WM_WINDOW_TYPE_NORMAL"))
                    {
                        includeInWindowList = true;
                    }
                }

                if (includeInWindowList)
                {
                    if (getNetWmState(dpy, wins[i]).contains(AtomCache::atom("_NET_WM_STATE_SKIP_TASKBAR")))
                    {
                        includeInWindowList = false;
                    }
                }

                if (includeInWindowList)
                {
                    if (!windowsBeingClosed.contains(wins[i]))
                    {
                        WindowInfo *wi = WindowInfo::windowFor(wins[i]);
                        windowList->append(wi);
                    }
                    else
                    {
                        windowsStillBeingClosed.append(wins[i]);
                    }
                }
                XFree(typeData);
            }
        }
    }

    XFree(wins);

    for (int i = windowsBeingClosed.count() - 1; i >= 0; i--)
    {
        Window window = windowsBeingClosed.at(i);
        if (!windowsStillBeingClosed.contains(window))
        {
            windowsBeingClosed.removeAt(i);
        }
    }

    SWITCHER_DEBUG("Deleting WindowInfos for " << windowsStillBeingClosed);
    foreach (Window wid, windowsStillBeingClosed) {
        WindowInfo *wi = WindowInfo::windowFor(wid);
        delete wi;
    }

    windowsStillBeingClosed.clear();
    setList(windowList);
    emit itemCountChanged();
}
