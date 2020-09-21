/****************************************************************************
**
** Copyright (C) 2019 Volker Krause <vkrause@kde.org>
** Contact: https://www.qt.io/licensing/
**
** This file is part of the plugins of the Qt Toolkit.
**
** $QT_BEGIN_LICENSE:LGPL$
** Commercial License Usage
** Licensees holding valid commercial Qt licenses may use this file in
** accordance with the commercial license agreement provided with the
** Software or, alternatively, in accordance with the terms contained in
** a written agreement between you and The Qt Company. For licensing terms
** and conditions see https://www.qt.io/terms-conditions. For further
** information use the contact form at https://www.qt.io/contact-us.
**
** GNU Lesser General Public License Usage
** Alternatively, this file may be used under the terms of the GNU Lesser
** General Public License version 3 as published by the Free Software
** Foundation and appearing in the file LICENSE.LGPL3 included in the
** packaging of this file. Please review the following information to
** ensure the GNU Lesser General Public License version 3 requirements
** will be met: https://www.gnu.org/licenses/lgpl-3.0.html.
**
** GNU General Public License Usage
** Alternatively, this file may be used under the terms of the GNU
** General Public License version 2.0 or (at your option) the GNU General
** Public license version 3 or any later version approved by the KDE Free
** Qt Foundation. The licenses are as published by the Free Software
** Foundation and appearing in the file LICENSE.GPL2 and LICENSE.GPL3
** included in the packaging of this file. Please review the following
** information to ensure the GNU General Public License requirements will
** be met: https://www.gnu.org/licenses/gpl-2.0.html and
** https://www.gnu.org/licenses/gpl-3.0.html.
**
** $QT_END_LICENSE$
**
****************************************************************************/

#include "androidcontentfileengine.h"

#include <private/qjni_p.h>
#include <private/qjnihelpers_p.h>

#include <QDebug>

AndroidContentFileEngine::AndroidContentFileEngine(const QString &f)
    : m_fd(-1), m_file(f), m_resolvedName(QString())
{
    setFileName(f);
    setResolvedFileName(f);
}

bool AndroidContentFileEngine::open(QIODevice::OpenMode openMode)
{
    QString openModeStr;
    if (openMode & QFileDevice::ReadOnly) {
        openModeStr += QLatin1Char('r');
    }
    if (openMode & QFileDevice::WriteOnly) {
        openModeStr += QLatin1Char('w');
    }
    if (openMode & QFileDevice::Truncate) {
        openModeStr += QLatin1Char('t');
    } else if (openMode & QFileDevice::Append) {
        openModeStr += QLatin1Char('a');
    }

    const auto fd = QJNIObjectPrivate::callStaticMethod<jint>("org/qtproject/qt5/android/QtNative",
        "openFdForContentUrl",
        "(Landroid/content/Context;Ljava/lang/String;Ljava/lang/String;)I",
        QtAndroidPrivate::context(),
        QJNIObjectPrivate::fromString(m_file).object(),
        QJNIObjectPrivate::fromString(openModeStr).object());

    if (fd < 0) {
        return false;
    }

    setFileDescriptor(fd);
    return QFSFileEngine::open(openMode, m_fd, QFile::AutoCloseHandle);
}

bool AndroidContentFileEngine::close()
{
    return QJNIObjectPrivate::callStaticMethod<jboolean>(
        "org/qtproject/qt5/android/QtNative", "closeFd",
        "(I)Z", m_fd);
}

qint64 AndroidContentFileEngine::size() const
{
    const jlong size = QJNIObjectPrivate::callStaticMethod<jlong>(
            "org/qtproject/qt5/android/QtNative", "getSize",
            "(Landroid/content/Context;Ljava/lang/String;)J", QtAndroidPrivate::context(),
            QJNIObjectPrivate::fromString(m_file).object());
    return (qint64)size;
}

AndroidContentFileEngine::FileFlags AndroidContentFileEngine::fileFlags(FileFlags type) const
{
    FileFlags commonFlags(ReadOwnerPerm|ReadUserPerm|ReadGroupPerm|ReadOtherPerm|ExistsFlag|WriteOwnerPerm|WriteUserPerm|WriteGroupPerm|WriteOtherPerm);
    FileFlags flags;
    const bool exists = QJNIObjectPrivate::callStaticMethod<jboolean>(
            "org/qtproject/qt5/android/QtNative", "checkFileExists",
            "(Landroid/content/Context;Ljava/lang/String;)Z", QtAndroidPrivate::context(),
            QJNIObjectPrivate::fromString(m_file).object());
    if (!exists)
        return flags;
    flags = FileType | commonFlags;
    return type & flags;
}

QString AndroidContentFileEngine::fileName(FileName f) const
{
    switch (f) {
        case DefaultName: {
            return m_resolvedName;
        }
        case PathName:
        case AbsoluteName:
        case AbsolutePathName:
        case CanonicalName:
        case CanonicalPathName:
            return m_file;

        case BaseName: {
            const int pos = m_resolvedName.lastIndexOf(QChar(QLatin1Char('/')));
            return m_resolvedName.mid(pos);
        }
        default:
            return QString();
    }
}

void AndroidContentFileEngine::setResolvedFileName(const QString& uri)
{
    QJNIObjectPrivate resolvedName = QJNIObjectPrivate::callStaticObjectMethod(
        "org/qtproject/qt5/android/QtNative",
        "getFileNameFromUri",
        "(Landroid/content/Context;Ljava/lang/String;)Ljava/lang/String;",
        QtAndroidPrivate::context(),
        QJNIObjectPrivate::fromString(uri).object());

    if (resolvedName.isValid()) {
        m_resolvedName = resolvedName.toString();
    } else {
        qWarning("setResolvedFileName: Couldn't resolve the URI");
    }
}

void AndroidContentFileEngine::setFileDescriptor(const int fd)
{
    m_fd = fd;
}


AndroidContentFileEngineHandler::AndroidContentFileEngineHandler() = default;
AndroidContentFileEngineHandler::~AndroidContentFileEngineHandler() = default;

QAbstractFileEngine* AndroidContentFileEngineHandler::create(const QString &fileName) const
{
    if (!fileName.startsWith(QLatin1String("content"))) {
        return nullptr;
    }

    return new AndroidContentFileEngine(fileName);
}
