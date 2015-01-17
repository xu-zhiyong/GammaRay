/*
  signalhistorymodel.cpp

  This file is part of GammaRay, the Qt application inspection and
  manipulation tool.

  Copyright (C) 2013-2014 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Mathias Hasselmann <mathias.hasselmann@kdab.com>

  This program is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 2 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "signalhistorymodel.h"
#include "relativeclock.h"
#include "signalmonitorcommon.h"

#include <core/probeinterface.h>
#include <core/util.h>
#include <core/probe.h>
#include <core/readorwritelocker.h>

#include <QLocale>
#include <QSet>
#include <QThread>

#include <private/qobject_p.h>

using namespace GammaRay;

/// Tries to reuse an already existing instances of \param str by checking
/// a global string pool. If no instance of \param str is interned yet the
/// string will be added to the pool.
template <typename T>
static T internString(const T &str)
{
  static QSet<T> pool;

  // Check if the pool already contains the string...
  const typename QSet<T>::const_iterator it = pool.find(str);

  //  ...and return it if possible.
  if (it != pool.end())
    return *it;

  // Otherwise add the string to the pool.
  pool.insert(str);
  return str;
}

static SignalHistoryModel *s_historyModel = 0;

static void signal_begin_callback(QObject *caller, int method_index, void **argv)
{
  Q_UNUSED(argv);
  if (s_historyModel) {
    const int signalIndex = method_index + 1; // offset 1, so unknown signals end up at 0
    static const QMetaMethod m = s_historyModel->metaObject()->method(s_historyModel->metaObject()->indexOfMethod("onSignalEmitted(QObject*,int)"));
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    Q_ASSERT(m.isValid());
#endif
    m.invoke(s_historyModel, Qt::AutoConnection, Q_ARG(QObject*, caller), Q_ARG(int, signalIndex));
  }
}


SignalHistoryModel::SignalHistoryModel(ProbeInterface *probe, QObject *parent)
  : QAbstractTableModel(parent)
{
  connect(probe->probe(), SIGNAL(objectCreated(QObject*)), this, SLOT(onObjectAdded(QObject*)));
  connect(probe->probe(), SIGNAL(objectDestroyed(QObject*)), this, SLOT(onObjectRemoved(QObject*)));

  QSignalSpyCallbackSet spy = { signal_begin_callback, 0, 0, 0 };
  probe->registerSignalSpyCallbackSet(spy);

  s_historyModel = this;
}

SignalHistoryModel::~SignalHistoryModel()
{
  s_historyModel = 0;
}

int SignalHistoryModel::rowCount(const QModelIndex &parent) const
{
  if (parent.isValid())
    return 0;
  return m_tracedObjects.size();
}

int SignalHistoryModel::columnCount(const QModelIndex &) const
{
  return 3;
}

SignalHistoryModel::Item* SignalHistoryModel::item(const QModelIndex& index) const
{
  if (!index.isValid())
    return 0;
  return m_tracedObjects.at(index.row());
}

QVariant SignalHistoryModel::data(const QModelIndex &index, int role) const
{
  switch (static_cast<ColumnId>(index.column())) {
    case ObjectColumn:
        if (role == Qt::DisplayRole)
          return item(index)->objectName;
        if (role == Qt::ToolTipRole)
          return item(index)->toolTip;
        if (role == Qt::DecorationRole)
          return item(index)->decoration;

        break;

    case TypeColumn:
        if (role == Qt::DisplayRole)
          return item(index)->objectType;
        if (role == Qt::ToolTipRole)
          return item(index)->toolTip;

        break;

    case EventColumn:
        if (role == EventsRole)
          return QVariant::fromValue(item(index)->events);
        if (role == StartTimeRole)
          return item(index)->startTime;
        if (role == EndTimeRole)
          return item(index)->endTime();
        if (role == SignalMapRole)
          return QVariant::fromValue(item(index)->signalNames);

        break;
  }

  return QVariant();
}

QVariant SignalHistoryModel::headerData(int section, Qt::Orientation orientation, int role) const
{
  if (role == Qt::DisplayRole && orientation == Qt::Horizontal) {
    switch (section) {
      case ObjectColumn:
        return tr("Object");
      case TypeColumn:
        return tr("Type");
      case EventColumn:
        return tr("Events");
    }
  }

  return QVariant();
}

QMap< int, QVariant > SignalHistoryModel::itemData(const QModelIndex& index) const
{
  QMap<int, QVariant> d = QAbstractItemModel::itemData(index);
  d.insert(EventsRole, data(index, EventsRole));
  d.insert(StartTimeRole, data(index, StartTimeRole));
  d.insert(EndTimeRole, data(index, EndTimeRole));
  d.insert(SignalMapRole, data(index, SignalMapRole));
  return d;
}

void SignalHistoryModel::onObjectAdded(QObject* object)
{
  Q_ASSERT(thread() == QThread::currentThread());

  // blacklist event dispatchers
  if (qstrncmp(object->metaObject()->className(), "QPAEventDispatcher", 18) == 0
    || qstrncmp(object->metaObject()->className(), "QGuiEventDispatcher", 19) == 0)
    return;

  beginInsertRows(QModelIndex(), m_tracedObjects.size(), m_tracedObjects.size());

  Item *const data = new Item(object);
  m_itemIndex.insert(object, m_tracedObjects.size());
  m_tracedObjects.push_back(data);

  endInsertRows();
}

void SignalHistoryModel::onObjectRemoved(QObject* object)
{
  Q_ASSERT(thread() == QThread::currentThread());

  const auto it = m_itemIndex.find(object);
  if (it == m_itemIndex.end())
    return;
  const int itemIndex = *it;
  m_itemIndex.erase(it);

  Item *data = m_tracedObjects.at(itemIndex);
  Q_ASSERT(data->object == object);
  data->object = 0;
  emit dataChanged(index(itemIndex, EventColumn), index(itemIndex, EventColumn));
}

void SignalHistoryModel::onSignalEmitted(QObject *sender, int signalIndex)
{
  Q_ASSERT(thread() == QThread::currentThread());
  const qint64 timestamp = RelativeClock::sinceAppStart()->mSecs();

  const auto it = m_itemIndex.constFind(sender);
  if (it == m_itemIndex.constEnd())
    return;
  const int itemIndex = *it;

  Item *data = m_tracedObjects.at(itemIndex);
  Q_ASSERT(data->object == sender);
  // ensure the item is known
  if (signalIndex > 0 && !data->signalNames.contains(signalIndex)) {
    // protect dereferencing of sender here
    ReadOrWriteLocker lock(Probe::instance()->objectLock());
    if (!Probe::instance()->isValidObject(sender))
      return;
    const QByteArray signalName = sender->metaObject()->method(signalIndex - 1)
#if QT_VERSION < QT_VERSION_CHECK(5, 0, 0)
      .signature();
#else
      .methodSignature();
#endif
    data->signalNames.insert(signalIndex, internString(signalName));
  }

  data->events.push_back((timestamp << 16) | signalIndex);
  emit dataChanged(index(itemIndex, EventColumn), index(itemIndex, EventColumn));
}


SignalHistoryModel::Item::Item(QObject *obj)
  : object(obj)
  , startTime(RelativeClock::sinceAppStart()->mSecs())
{
  objectName = Util::shortDisplayString(object);
  objectType = internString(QByteArray(obj->metaObject()->className()));
  toolTip = Util::tooltipForObject(object);
  decoration = Util::iconForObject(object).value<QIcon>();
}

qint64 SignalHistoryModel::Item::endTime() const
{
  if (object)
    return -1; // still alive
  if (!events.isEmpty())
    return timestamp(events.size() - 1);

  return startTime;
}
