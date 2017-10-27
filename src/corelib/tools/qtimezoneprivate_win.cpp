/****************************************************************************
**
** Copyright (C) 2013 John Layt <jlayt@kde.org>
** Contact: https://www.qt.io/licensing/
**
** This file is part of the QtCore module of the Qt Toolkit.
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

#include "qtimezone.h"
#include "qtimezoneprivate_p.h"

#include "qdatetime.h"

#include "qdebug.h"

#include <algorithm>

QT_BEGIN_NAMESPACE

#ifndef Q_OS_WINRT
#define QT_USE_REGISTRY_TIMEZONE 1
#endif

/*
    Private

    Windows system implementation
*/

#define MAX_KEY_LENGTH 255
#define FILETIME_UNIX_EPOCH Q_UINT64_C(116444736000000000)

// MSDN home page for Time support
// http://msdn.microsoft.com/en-us/library/windows/desktop/ms724962%28v=vs.85%29.aspx

// For Windows XP and later refer to MSDN docs on TIME_ZONE_INFORMATION structure
// http://msdn.microsoft.com/en-gb/library/windows/desktop/ms725481%28v=vs.85%29.aspx

// Vista introduced support for historic data, see MSDN docs on DYNAMIC_TIME_ZONE_INFORMATION
// http://msdn.microsoft.com/en-gb/library/windows/desktop/ms724253%28v=vs.85%29.aspx
#ifdef QT_USE_REGISTRY_TIMEZONE
static const char tzRegPath[] = "SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Time Zones";
static const char currTzRegPath[] = "SYSTEM\\CurrentControlSet\\Control\\TimeZoneInformation";
#endif

enum {
    MIN_YEAR = -292275056,
    MAX_YEAR = 292278994,
    MSECS_PER_DAY = 86400000,
    TIME_T_MAX = 2145916799,  // int maximum 2037-12-31T23:59:59 UTC
    JULIAN_DAY_FOR_EPOCH = 2440588 // result of julianDayFromDate(1970, 1, 1)
};

// Copied from MSDN, see above for link
typedef struct _REG_TZI_FORMAT
{
    LONG Bias;
    LONG StandardBias;
    LONG DaylightBias;
    SYSTEMTIME StandardDate;
    SYSTEMTIME DaylightDate;
} REG_TZI_FORMAT;

// Fast and reliable conversion from msecs to date for all values
// Adapted from QDateTime msecsToDate
static QDate msecsToDate(qint64 msecs)
{
    qint64 jd = JULIAN_DAY_FOR_EPOCH;

    if (qAbs(msecs) >= MSECS_PER_DAY) {
        jd += (msecs / MSECS_PER_DAY);
        msecs %= MSECS_PER_DAY;
    }

    if (msecs < 0) {
        qint64 ds = MSECS_PER_DAY - msecs - 1;
        jd -= ds / MSECS_PER_DAY;
    }

    return QDate::fromJulianDay(jd);
}

static bool equalSystemtime(const SYSTEMTIME &t1, const SYSTEMTIME &t2)
{
    return (t1.wYear == t2.wYear
            && t1.wMonth == t2.wMonth
            && t1.wDay == t2.wDay
            && t1.wDayOfWeek == t2.wDayOfWeek
            && t1.wHour == t2.wHour
            && t1.wMinute == t2.wMinute
            && t1.wSecond == t2.wSecond
            && t1.wMilliseconds == t2.wMilliseconds);
}

static bool equalTzi(const TIME_ZONE_INFORMATION &tzi1, const TIME_ZONE_INFORMATION &tzi2)
{
    return(tzi1.Bias == tzi2.Bias
           && tzi1.StandardBias == tzi2.StandardBias
           && equalSystemtime(tzi1.StandardDate, tzi2.StandardDate)
           && wcscmp(tzi1.StandardName, tzi2.StandardName) == 0
           && tzi1.DaylightBias == tzi2.DaylightBias
           && equalSystemtime(tzi1.DaylightDate, tzi2.DaylightDate)
           && wcscmp(tzi1.DaylightName, tzi2.DaylightName) == 0);
}

#ifdef QT_USE_REGISTRY_TIMEZONE
static bool openRegistryKey(const QString &keyPath, HKEY *key)
{
    return (RegOpenKeyEx(HKEY_LOCAL_MACHINE, (const wchar_t*)keyPath.utf16(), 0, KEY_READ, key)
            == ERROR_SUCCESS);
}

static QString readRegistryString(const HKEY &key, const wchar_t *value)
{
    wchar_t buffer[MAX_PATH] = {0};
    DWORD size = sizeof(wchar_t) * MAX_PATH;
    RegQueryValueEx(key, (LPCWSTR)value, NULL, NULL, (LPBYTE)buffer, &size);
    return QString::fromWCharArray(buffer);
}

static int readRegistryValue(const HKEY &key, const wchar_t *value)
{
    DWORD buffer;
    DWORD size = sizeof(buffer);
    RegQueryValueEx(key, (LPCWSTR)value, NULL, NULL, (LPBYTE)&buffer, &size);
    return buffer;
}

static QWinTimeZonePrivate::QWinTransitionRule readRegistryRule(const HKEY &key,
                                                                const wchar_t *value, bool *ok)
{
    *ok = false;
    QWinTimeZonePrivate::QWinTransitionRule rule;
    REG_TZI_FORMAT tzi;
    DWORD tziSize = sizeof(tzi);
    if (RegQueryValueEx(key, (LPCWSTR)value, NULL, NULL, (BYTE *)&tzi, &tziSize)
        == ERROR_SUCCESS) {
        rule.startYear = 0;
        rule.standardTimeBias = tzi.Bias + tzi.StandardBias;
        rule.daylightTimeBias = tzi.Bias + tzi.DaylightBias - rule.standardTimeBias;
        rule.standardTimeRule = tzi.StandardDate;
        rule.daylightTimeRule = tzi.DaylightDate;
        *ok = true;
    }
    return rule;
}

static TIME_ZONE_INFORMATION getRegistryTzi(const QByteArray &windowsId, bool *ok)
{
    *ok = false;
    TIME_ZONE_INFORMATION tzi;
    REG_TZI_FORMAT regTzi;
    DWORD regTziSize = sizeof(regTzi);
    HKEY key = NULL;
    const QString tziKeyPath = QString::fromUtf8(tzRegPath) + QLatin1Char('\\')
                               + QString::fromUtf8(windowsId);

    if (openRegistryKey(tziKeyPath, &key)) {

        DWORD size = sizeof(tzi.DaylightName);
        RegQueryValueEx(key, L"Dlt", NULL, NULL, (LPBYTE)tzi.DaylightName, &size);

        size = sizeof(tzi.StandardName);
        RegQueryValueEx(key, L"Std", NULL, NULL, (LPBYTE)tzi.StandardName, &size);

        if (RegQueryValueEx(key, L"TZI", NULL, NULL, (BYTE *) &regTzi, &regTziSize)
            == ERROR_SUCCESS) {
            tzi.Bias = regTzi.Bias;
            tzi.StandardBias = regTzi.StandardBias;
            tzi.DaylightBias = regTzi.DaylightBias;
            tzi.StandardDate = regTzi.StandardDate;
            tzi.DaylightDate = regTzi.DaylightDate;
            *ok = true;
        }

        RegCloseKey(key);
    }

    return tzi;
}
#else // QT_USE_REGISTRY_TIMEZONE
struct QWinDynamicTimeZone
{
    QString standardName;
    QString daylightName;
    QString timezoneName;
    qint32 bias;
    bool daylightTime;
};

typedef QHash<QByteArray, QWinDynamicTimeZone> QWinRTTimeZoneHash;

Q_GLOBAL_STATIC(QWinRTTimeZoneHash, gTimeZones)

static void enumerateTimeZones()
{
    DYNAMIC_TIME_ZONE_INFORMATION dtzInfo;
    quint32 index = 0;
    QString prevTimeZoneKeyName;
    while (SUCCEEDED(EnumDynamicTimeZoneInformation(index++, &dtzInfo))) {
        QWinDynamicTimeZone item;
        item.timezoneName = QString::fromWCharArray(dtzInfo.TimeZoneKeyName);
        // As soon as key name repeats, break. Some systems continue to always
        // return the last item independent of index being out of range
        if (item.timezoneName == prevTimeZoneKeyName)
            break;
        item.standardName = QString::fromWCharArray(dtzInfo.StandardName);
        item.daylightName = QString::fromWCharArray(dtzInfo.DaylightName);
        item.daylightTime = !dtzInfo.DynamicDaylightTimeDisabled;
        item.bias = dtzInfo.Bias;
        gTimeZones->insert(item.timezoneName.toUtf8(), item);
        prevTimeZoneKeyName = item.timezoneName;
    }
}

static DYNAMIC_TIME_ZONE_INFORMATION dynamicInfoForId(const QByteArray &windowsId)
{
    DYNAMIC_TIME_ZONE_INFORMATION dtzInfo;
    quint32 index = 0;
    QString prevTimeZoneKeyName;
    while (SUCCEEDED(EnumDynamicTimeZoneInformation(index++, &dtzInfo))) {
        const QString timeZoneName = QString::fromWCharArray(dtzInfo.TimeZoneKeyName);
        if (timeZoneName == QLatin1String(windowsId))
            break;
        if (timeZoneName == prevTimeZoneKeyName)
            break;
        prevTimeZoneKeyName = timeZoneName;
    }
    return dtzInfo;
}

static QWinTimeZonePrivate::QWinTransitionRule
readDynamicRule(DYNAMIC_TIME_ZONE_INFORMATION &dtzi, int year, bool *ok)
{
    TIME_ZONE_INFORMATION tzi;
    QWinTimeZonePrivate::QWinTransitionRule rule;
    *ok = GetTimeZoneInformationForYear(year, &dtzi, &tzi);
    if (*ok) {
        rule.startYear = 0;
        rule.standardTimeBias = tzi.Bias + tzi.StandardBias;
        rule.daylightTimeBias = tzi.Bias + tzi.DaylightBias - rule.standardTimeBias;
        rule.standardTimeRule = tzi.StandardDate;
        rule.daylightTimeRule = tzi.DaylightDate;
    }
    return rule;
}
#endif // QT_USE_REGISTRY_TIMEZONE

static bool isSameRule(const QWinTimeZonePrivate::QWinTransitionRule &last,
                       const QWinTimeZonePrivate::QWinTransitionRule &rule)
{
    // In particular, when this is true and either wYear is 0, so is the other;
    // so if one rule is recurrent and they're equal, so is the other.  If
    // either rule *isn't* recurrent, it has non-0 wYear which shall be
    // different from the other's.  Note that we don't compare .startYear, since
    // that will always be different.
    return equalSystemtime(last.standardTimeRule, rule.standardTimeRule)
        && equalSystemtime(last.daylightTimeRule, rule.daylightTimeRule)
        && last.standardTimeBias == rule.standardTimeBias
        && last.daylightTimeBias == rule.daylightTimeBias;
}

static QList<QByteArray> availableWindowsIds()
{
#ifdef QT_USE_REGISTRY_TIMEZONE
    // TODO Consider caching results in a global static, very unlikely to change.
    QList<QByteArray> list;
    HKEY key = NULL;
    if (openRegistryKey(QString::fromUtf8(tzRegPath), &key)) {
        DWORD idCount = 0;
        if (RegQueryInfoKey(key, 0, 0, 0, &idCount, 0, 0, 0, 0, 0, 0, 0) == ERROR_SUCCESS
            && idCount > 0) {
            for (DWORD i = 0; i < idCount; ++i) {
                DWORD maxLen = MAX_KEY_LENGTH;
                TCHAR buffer[MAX_KEY_LENGTH];
                if (RegEnumKeyEx(key, i, buffer, &maxLen, 0, 0, 0, 0) == ERROR_SUCCESS)
                    list.append(QString::fromWCharArray(buffer).toUtf8());
            }
        }
        RegCloseKey(key);
    }
    return list;
#else // QT_USE_REGISTRY_TIMEZONE
    if (gTimeZones->isEmpty())
        enumerateTimeZones();
    return gTimeZones->keys();
#endif // QT_USE_REGISTRY_TIMEZONE
}

static QByteArray windowsSystemZoneId()
{
#ifdef QT_USE_REGISTRY_TIMEZONE
    // On Vista and later is held in the value TimeZoneKeyName in key currTzRegPath
    QString id;
    HKEY key = NULL;
    QString tziKeyPath = QString::fromUtf8(currTzRegPath);
    if (openRegistryKey(tziKeyPath, &key)) {
        id = readRegistryString(key, L"TimeZoneKeyName");
        RegCloseKey(key);
        if (!id.isEmpty())
            return std::move(id).toUtf8();
    }

    // On XP we have to iterate over the zones until we find a match on
    // names/offsets with the current data
    TIME_ZONE_INFORMATION sysTzi;
    GetTimeZoneInformation(&sysTzi);
    bool ok = false;
    const auto winIds = availableWindowsIds();
    for (const QByteArray &winId : winIds) {
        if (equalTzi(getRegistryTzi(winId, &ok), sysTzi))
            return winId;
    }
#else // QT_USE_REGISTRY_TIMEZONE
    DYNAMIC_TIME_ZONE_INFORMATION dtzi;
    if (SUCCEEDED(GetDynamicTimeZoneInformation(&dtzi)))
        return QString::fromWCharArray(dtzi.TimeZoneKeyName).toLocal8Bit();
#endif // QT_USE_REGISTRY_TIMEZONE

    // If we can't determine the current ID use UTC
    return QTimeZonePrivate::utcQByteArray();
}

static QDate calculateTransitionLocalDate(const SYSTEMTIME &rule, int year)
{
    // If month is 0 then there is no date
    if (rule.wMonth == 0)
        return QDate();

    SYSTEMTIME time = rule;
    // If the year isn't set, then the rule date is relative
    if (time.wYear == 0) {
        if (time.wDayOfWeek == 0)
            time.wDayOfWeek = 7;
        QDate date(year, time.wMonth, 1);
        int startDow = date.dayOfWeek();
        if (startDow <= time.wDayOfWeek)
            date = date.addDays(time.wDayOfWeek - startDow - 7);
        else
            date = date.addDays(time.wDayOfWeek - startDow);
        date = date.addDays(time.wDay * 7);
        while (date.month() != time.wMonth)
            date = date.addDays(-7);
        return date;
    }

    // If the year is set then is an absolute date
    return QDate(time.wYear, time.wMonth, time.wDay);
}

// Converts a date/time value into msecs
static inline qint64 timeToMSecs(const QDate &date, const QTime &time)
{
    return ((date.toJulianDay() - JULIAN_DAY_FOR_EPOCH) * MSECS_PER_DAY)
           + time.msecsSinceStartOfDay();
}

static qint64 calculateTransitionForYear(const SYSTEMTIME &rule, int year, int bias)
{
    // TODO Consider caching the calculated values
    const QDate date = calculateTransitionLocalDate(rule, year);
    const QTime time = QTime(rule.wHour, rule.wMinute, rule.wSecond);
    if (date.isValid() && time.isValid())
        return timeToMSecs(date, time) + bias * 60000;
    return QTimeZonePrivate::invalidMSecs();
}

namespace {
struct TransitionTimePair
{
    // Transition times after the epoch, in ms:
    qint64 std, dst;
    TransitionTimePair(const QWinTimeZonePrivate::QWinTransitionRule &rule, int year)
        // The local time in Daylight Time of the switch to Standard Time
        : std(calculateTransitionForYear(rule.standardTimeRule, year,
                                         rule.standardTimeBias + rule.daylightTimeBias)),
          // The local time in Standard Time of the switch to Daylight Time
          dst(calculateTransitionForYear(rule.daylightTimeRule, year, rule.standardTimeBias))
    {}
};
} // anonymous namespace

static QLocale::Country userCountry()
{
    const GEOID id = GetUserGeoID(GEOCLASS_NATION);
    wchar_t code[3];
    const int size = GetGeoInfo(id, GEO_ISO2, code, 3, 0);
    return (size == 3) ? QLocalePrivate::codeToCountry(QStringView(code, size))
                       : QLocale::AnyCountry;
}

// Index of last rule in rules with .startYear <= year:
static int ruleIndexForYear(const QList<QWinTimeZonePrivate::QWinTransitionRule> &rules, int year)
{
    if (rules.last().startYear <= year)
        return rules.count() - 1;
    // We don't have a rule for before the first, but the first is the best we can offer:
    if (rules.first().startYear > year)
        return 0;

    // Otherwise, use binary chop:
    int lo = 0, hi = rules.count();
    // invariant: rules[i].startYear <= year < rules[hi].startYear
    // subject to treating rules[rules.count()] as "off the end of time"
    while (lo + 1 < hi) {
        const int mid = (lo + hi) / 2;
        // lo + 2 <= hi, so lo + 1 <= mid <= hi - 1, so lo < mid < hi
        // In particular, mid < rules.count()
        const int midYear = rules.at(mid).startYear;
        if (midYear > year)
            hi = mid;
        else if (midYear < year)
            lo = mid;
        else // No two rules have the same startYear:
            return mid;
    }
    return lo;
}

// Create the system default time zone
QWinTimeZonePrivate::QWinTimeZonePrivate()
                   : QTimeZonePrivate()
{
    init(QByteArray());
}

// Create a named time zone
QWinTimeZonePrivate::QWinTimeZonePrivate(const QByteArray &ianaId)
                   : QTimeZonePrivate()
{
    init(ianaId);
}

QWinTimeZonePrivate::QWinTimeZonePrivate(const QWinTimeZonePrivate &other)
                   : QTimeZonePrivate(other), m_windowsId(other.m_windowsId),
                     m_displayName(other.m_displayName), m_standardName(other.m_standardName),
                     m_daylightName(other.m_daylightName), m_tranRules(other.m_tranRules)
{
}

QWinTimeZonePrivate::~QWinTimeZonePrivate()
{
}

QWinTimeZonePrivate *QWinTimeZonePrivate::clone() const
{
    return new QWinTimeZonePrivate(*this);
}

void QWinTimeZonePrivate::init(const QByteArray &ianaId)
{
    if (ianaId.isEmpty()) {
        m_windowsId = windowsSystemZoneId();
        m_id = systemTimeZoneId();
    } else {
        m_windowsId = ianaIdToWindowsId(ianaId);
        m_id = ianaId;
    }

    bool badMonth = false; // Only warn once per zone, if at all.
    if (!m_windowsId.isEmpty()) {
#ifdef QT_USE_REGISTRY_TIMEZONE
        // Open the base TZI for the time zone
        HKEY baseKey = NULL;
        const QString baseKeyPath = QString::fromUtf8(tzRegPath) + QLatin1Char('\\')
                                   + QString::fromUtf8(m_windowsId);
        if (openRegistryKey(baseKeyPath, &baseKey)) {
            //  Load the localized names
            m_displayName = readRegistryString(baseKey, L"Display");
            m_standardName = readRegistryString(baseKey, L"Std");
            m_daylightName = readRegistryString(baseKey, L"Dlt");
            // On Vista and later the optional dynamic key holds historic data
            const QString dynamicKeyPath = baseKeyPath + QLatin1String("\\Dynamic DST");
            HKEY dynamicKey = NULL;
            if (openRegistryKey(dynamicKeyPath, &dynamicKey)) {
                // Find out the start and end years stored, then iterate over them
                int startYear = readRegistryValue(dynamicKey, L"FirstEntry");
                int endYear = readRegistryValue(dynamicKey, L"LastEntry");
                for (int year = startYear; year <= endYear; ++year) {
                    bool ruleOk;
                    QWinTransitionRule rule = readRegistryRule(dynamicKey,
                                                               (LPCWSTR)QString::number(year).utf16(),
                                                               &ruleOk);
                    if (ruleOk
                        // Don't repeat a recurrent rule:
                        && (m_tranRules.isEmpty()
                            || !isSameRule(m_tranRules.last(), rule))) {
                        if (!badMonth
                            && (rule.standardTimeRule.wMonth == 0)
                            != (rule.daylightTimeRule.wMonth == 0)) {
                            badMonth = true;
                            qWarning("MS registry TZ API violated its wMonth constraint;"
                                     "this may cause mistakes for %s from %d",
                                     ianaId.constData(), year);
                        }
                        rule.startYear = m_tranRules.isEmpty() ? MIN_YEAR : year;
                        m_tranRules.append(rule);
                    }
                }
                RegCloseKey(dynamicKey);
            } else {
                // No dynamic data so use the base data
                bool ruleOk;
                QWinTransitionRule rule = readRegistryRule(baseKey, L"TZI", &ruleOk);
                rule.startYear = MIN_YEAR;
                if (ruleOk)
                    m_tranRules.append(rule);
            }
            RegCloseKey(baseKey);
        }
#else // QT_USE_REGISTRY_TIMEZONE
        if (gTimeZones->isEmpty())
            enumerateTimeZones();
        QWinRTTimeZoneHash::const_iterator it = gTimeZones->find(m_windowsId);
        if (it != gTimeZones->constEnd()) {
            m_displayName = it->timezoneName;
            m_standardName = it->standardName;
            m_daylightName = it->daylightName;
            DWORD firstYear = 0;
            DWORD lastYear = 0;
            DYNAMIC_TIME_ZONE_INFORMATION dtzi = dynamicInfoForId(m_windowsId);
            if (GetDynamicTimeZoneInformationEffectiveYears(&dtzi, &firstYear, &lastYear)
                == ERROR_SUCCESS && firstYear < lastYear) {
                for (DWORD year = firstYear; year <= lastYear; ++year) {
                    bool ok = false;
                    QWinTransitionRule rule = readDynamicRule(dtzi, year, &ok);
                    if (ok
                        // Don't repeat a recurrent rule
                        && (m_tranRules.isEmpty()
                            || !isSameRule(m_tranRules.last(), rule))) {
                        if (!badMonth
                            && (rule.standardTimeRule.wMonth == 0)
                            != (rule.daylightTimeRule.wMonth == 0)) {
                            badMonth = true;
                            qWarning("MS dynamic TZ API violated its wMonth constraint;"
                                     "this may cause mistakes for %s from %d",
                                     ianaId.constData(), year);
                        }
                        rule.startYear = m_tranRules.isEmpty() ? MIN_YEAR : year;
                        m_tranRules.append(rule);
                    }
                }
            } else {
                // At least try to get the non-dynamic data:
                dtzi.DynamicDaylightTimeDisabled = false;
                bool ok = false;
                QWinTransitionRule rule = readDynamicRule(dtzi, 1970, &ok);
                if (ok) {
                    rule.startYear = MIN_YEAR;
                    m_tranRules.append(rule);
                }
            }
        }
#endif // QT_USE_REGISTRY_TIMEZONE
    }

    // If there are no rules then we failed to find a windowsId or any tzi info
    if (m_tranRules.size() == 0) {
        m_id.clear();
        m_windowsId.clear();
        m_displayName.clear();
    }
}

QString QWinTimeZonePrivate::comment() const
{
    return m_displayName;
}

QString QWinTimeZonePrivate::displayName(QTimeZone::TimeType timeType,
                                         QTimeZone::NameType nameType,
                                         const QLocale &locale) const
{
    // TODO Registry holds MUI keys, should be able to look up translations?
    Q_UNUSED(locale);

    if (nameType == QTimeZone::OffsetName) {
        const QWinTransitionRule &rule =
            m_tranRules.at(ruleIndexForYear(m_tranRules, QDate::currentDate().year()));
        if (timeType == QTimeZone::DaylightTime)
            return isoOffsetFormat((rule.standardTimeBias + rule.daylightTimeBias) * -60);
        else
            return isoOffsetFormat((rule.standardTimeBias) * -60);
    }

    switch (timeType) {
    case  QTimeZone::DaylightTime :
        return m_daylightName;
    case  QTimeZone::GenericTime :
        return m_displayName;
    case  QTimeZone::StandardTime :
        return m_standardName;
    }
    return m_standardName;
}

QString QWinTimeZonePrivate::abbreviation(qint64 atMSecsSinceEpoch) const
{
    return data(atMSecsSinceEpoch).abbreviation;
}

int QWinTimeZonePrivate::offsetFromUtc(qint64 atMSecsSinceEpoch) const
{
    return data(atMSecsSinceEpoch).offsetFromUtc;
}

int QWinTimeZonePrivate::standardTimeOffset(qint64 atMSecsSinceEpoch) const
{
    return data(atMSecsSinceEpoch).standardTimeOffset;
}

int QWinTimeZonePrivate::daylightTimeOffset(qint64 atMSecsSinceEpoch) const
{
    return data(atMSecsSinceEpoch).daylightTimeOffset;
}

bool QWinTimeZonePrivate::hasDaylightTime() const
{
    return hasTransitions();
}

bool QWinTimeZonePrivate::isDaylightTime(qint64 atMSecsSinceEpoch) const
{
    return (data(atMSecsSinceEpoch).daylightTimeOffset != 0);
}

QTimeZonePrivate::Data QWinTimeZonePrivate::data(qint64 forMSecsSinceEpoch) const
{
    int year = msecsToDate(forMSecsSinceEpoch).year();
    for (int ruleIndex = ruleIndexForYear(m_tranRules, year);
         ruleIndex >= 0; --ruleIndex) {
        const QWinTransitionRule &rule = m_tranRules.at(ruleIndex);
        // Does this rule's period include any transition at all ?
        if (rule.standardTimeRule.wMonth > 0 || rule.daylightTimeRule.wMonth > 0) {
            const int endYear = qMax(rule.startYear, year - 1);
            while (year >= endYear) {
                const TransitionTimePair pair(rule, year);
                bool isDst = false;
                if (pair.std <= forMSecsSinceEpoch) {
                    isDst = pair.std < pair.dst && pair.dst <= forMSecsSinceEpoch;
                } else if (pair.dst <= forMSecsSinceEpoch) {
                    isDst = true;
                } else {
                    --year; // Try an earlier year for this rule (once).
                    continue;
                }
                return ruleToData(rule, forMSecsSinceEpoch,
                                  isDst ? QTimeZone::DaylightTime : QTimeZone::StandardTime);
            }
            // Fell off start of rule, try previous rule.
        } else {
            // No transition, no DST, use the year's standard time.
            return ruleToData(rule, forMSecsSinceEpoch, QTimeZone::StandardTime);
        }
        if (year >= rule.startYear)
            year = rule.startYear - 1; // Seek last transition in new rule.
    }
    // We don't have relevant data :-(
    return invalidData();
}

bool QWinTimeZonePrivate::hasTransitions() const
{
    for (const QWinTransitionRule &rule : m_tranRules) {
        if (rule.standardTimeRule.wMonth > 0 && rule.daylightTimeRule.wMonth > 0)
            return true;
    }
    return false;
}

QTimeZonePrivate::Data QWinTimeZonePrivate::nextTransition(qint64 afterMSecsSinceEpoch) const
{
    int year = msecsToDate(afterMSecsSinceEpoch).year();
    for (int ruleIndex = ruleIndexForYear(m_tranRules, year);
         ruleIndex < m_tranRules.count(); ++ruleIndex) {
        const QWinTransitionRule &rule = m_tranRules.at(ruleIndex);
        // Does this rule's period include any transition at all ?
        if (rule.standardTimeRule.wMonth > 0 || rule.daylightTimeRule.wMonth > 0) {
            if (year < rule.startYear)
                year = rule.startYear; // Seek first transition in this rule.
            const int endYear = ruleIndex + 1 < m_tranRules.count()
                ? qMin(m_tranRules.at(ruleIndex + 1).startYear, year + 2) : (year + 2);
            while (year < endYear) {
                const TransitionTimePair pair(rule, year);
                bool isDst = false;
                if (pair.std > afterMSecsSinceEpoch) {
                    isDst = pair.std > pair.dst && pair.dst > afterMSecsSinceEpoch;
                } else if (pair.dst > afterMSecsSinceEpoch) {
                    isDst = true;
                } else {
                    ++year; // Try a later year for this rule (once).
                    continue;
                }

                if (isDst)
                    return ruleToData(rule, pair.dst, QTimeZone::DaylightTime);
                return ruleToData(rule, pair.std, QTimeZone::StandardTime);
            }
            // Fell off end of rule, try next rule.
        } // else: no transition during rule's period
    }
    // Apparently no transition after the given time:
    return invalidData();
}

QTimeZonePrivate::Data QWinTimeZonePrivate::previousTransition(qint64 beforeMSecsSinceEpoch) const
{
    int year = msecsToDate(beforeMSecsSinceEpoch).year();
    for (int ruleIndex = ruleIndexForYear(m_tranRules, year);
         ruleIndex >= 0; --ruleIndex) {
        const QWinTransitionRule &rule = m_tranRules.at(ruleIndex);
        // Does this rule's period include any transition at all ?
        if (rule.standardTimeRule.wMonth > 0 || rule.daylightTimeRule.wMonth > 0) {
            const int endYear = qMax(rule.startYear, year - 1);
            while (year >= endYear) {
                const TransitionTimePair pair(rule, year);
                bool isDst = false;
                if (pair.std < beforeMSecsSinceEpoch) {
                    isDst = pair.std < pair.dst && pair.dst < beforeMSecsSinceEpoch;
                } else if (pair.dst < beforeMSecsSinceEpoch) {
                    isDst = true;
                } else {
                    --year; // Try an earlier year for this rule (once).
                    continue;
                }
                if (isDst)
                    return ruleToData(rule, pair.dst, QTimeZone::DaylightTime);
                return ruleToData(rule, pair.std, QTimeZone::StandardTime);
            }
            // Fell off start of rule, try previous rule.
        } // else: no transition during rule's period
        if (year >= rule.startYear)
            year = rule.startYear - 1; // Seek last transition in new rule
    }
    // Apparently no transition before the given time:
    return invalidData();
}

QByteArray QWinTimeZonePrivate::systemTimeZoneId() const
{
    const QLocale::Country country = userCountry();
    const QByteArray windowsId = windowsSystemZoneId();
    QByteArray ianaId;
    // If we have a real country, then try get a specific match for that country
    if (country != QLocale::AnyCountry)
        ianaId = windowsIdToDefaultIanaId(windowsId, country);
    // If we don't have a real country, or there wasn't a specific match, try the global default
    if (ianaId.isEmpty()) {
        ianaId = windowsIdToDefaultIanaId(windowsId);
        // If no global default then probably an unknown Windows ID so return UTC
        if (ianaId.isEmpty())
            return utcQByteArray();
    }
    return ianaId;
}

QList<QByteArray> QWinTimeZonePrivate::availableTimeZoneIds() const
{
    QList<QByteArray> result;
    const auto winIds = availableWindowsIds();
    for (const QByteArray &winId : winIds)
        result += windowsIdToIanaIds(winId);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

QTimeZonePrivate::Data QWinTimeZonePrivate::ruleToData(const QWinTransitionRule &rule,
                                                       qint64 atMSecsSinceEpoch,
                                                       QTimeZone::TimeType type) const
{
    Data tran = invalidData();
    tran.atMSecsSinceEpoch = atMSecsSinceEpoch;
    tran.standardTimeOffset = rule.standardTimeBias * -60;
    if (type == QTimeZone::DaylightTime) {
        tran.daylightTimeOffset = rule.daylightTimeBias * -60;
        tran.abbreviation = m_daylightName;
    } else {
        tran.daylightTimeOffset = 0;
        tran.abbreviation = m_standardName;
    }
    tran.offsetFromUtc = tran.standardTimeOffset + tran.daylightTimeOffset;
    return tran;
}

QT_END_NAMESPACE
