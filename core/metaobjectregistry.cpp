/*
  metaobjecttreemodel.cpp

  This file is part of GammaRay, the Qt application inspection and
  manipulation tool.

  Copyright (C) 2012-2017 Klarälvdalens Datakonsult AB, a KDAB Group company, info@kdab.com
  Author: Kevin Funk <kevin.funk@kdab.com>

  Licensees holding valid commercial KDAB GammaRay licenses may use this file in
  accordance with GammaRay Commercial License Agreement provided with the Software.

  Contact info@kdab.com if any conditions of this licensing are not clear to you.

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

#include "metaobjectregistry.h"

#include <core/execution.h>
#include <core/probe.h>
#include <core/qmetaobjectvalidator.h>

#include <common/metatypedeclarations.h>
#include <common/tools/metaobjectbrowser/qmetaobjectmodel.h>

#include <QDebug>
#include <QThread>
#include <QTimer>

#include <assert.h>

using namespace GammaRay;

namespace GammaRay {
/**
 * Open QObject for access to protected data members
 */
class UnprotectedQObject : public QObject
{
public:
    inline QObjectData *data() const { return d_ptr.data(); }
};
}

/**
 * Return true in case the object has a dynamic meta object
 *
 * If you look at the code generated by moc you'll see this:
 * @code
 * const QMetaObject *GammaRay::MessageModel::metaObject() const
 * {
 *     return QObject::d_ptr->metaObject ? QObject::d_ptr->dynamicMetaObject() : &staticMetaObject;
 * }
 * @endcode
 *
 * QtQuick uses dynamic meta objects (subclasses of QAbstractDynamicMetaObject, defined in qobject_h.p)
 * for QML types. It's possible that these meta objects get destroyed
 * at runtime, so we need to protect against this.
 *
 * @note We cannot say if a specific QMetaObject* is dynamic or not
 * (QMetaObject is non-polymorphic, so we cannot dynamic_cast to QAbstractDynamicMetaObject*),
 * we can just judge by looking at QObjectData of QObject*
 * -- hence the QObject* parameter in hasDynamicMetaObject.
 *
 * @return Return true in case metaObject() does not point to staticMetaObject.
 */
static inline bool hasDynamicMetaObject(const QObject *object)
{
    return reinterpret_cast<const UnprotectedQObject *>(object)->data()->metaObject != nullptr;
}

MetaObjectRegistry::MetaObjectRegistry(QObject *parent)
    : QObject(parent)
{
    qRegisterMetaType<const QMetaObject *>();
    scanMetaTypes();
}

MetaObjectRegistry::~MetaObjectRegistry()
{
}

QVariant MetaObjectRegistry::data(const QMetaObject *object, MetaObjectData type) const
{
    switch (type) {
    case ClassName:
        return m_metaObjectInfoMap.value(object).className;
    case Valid:
        return isValid(object);
    case SelfCount:
        if (inheritsQObject(object))
            return m_metaObjectInfoMap.value(object).selfCount;
        return QStringLiteral("-");
    case InclusiveCount:
        if (inheritsQObject(object))
            return m_metaObjectInfoMap.value(object).inclusiveCount;
        return QStringLiteral("-");
    case SelfAliveCount:
        if (inheritsQObject(object))
            return m_metaObjectInfoMap.value(object).selfAliveCount;
        return QStringLiteral("-");
    case InclusiveAliveCount:
        if (inheritsQObject(object))
            return m_metaObjectInfoMap.value(object).inclusiveAliveCount;
        return QStringLiteral("-");
    }
    return QVariant();
}

bool MetaObjectRegistry::isValid(const QMetaObject *mo) const
{
    return !m_metaObjectInfoMap[mo].invalid;
}

const QMetaObject *MetaObjectRegistry::parentOf(const QMetaObject *mo) const
{
    return m_childParentMap.value(mo);
}

QVector<const QMetaObject *> MetaObjectRegistry::childrenOf(const QMetaObject *mo) const
{
    return m_parentChildMap.value(mo);
}

bool MetaObjectRegistry::inheritsQObject(const QMetaObject *mo) const
{
    while (mo) {
        if (mo == &QObject::staticMetaObject)
            return true;
        mo = m_childParentMap.value(mo);
    }

    return false;
}

void MetaObjectRegistry::objectAdded(QObject *obj)
{
    // Probe::objectFullyConstructed calls us and ensures this already
    Q_ASSERT(thread() == QThread::currentThread());
    Q_ASSERT(Probe::instance()->isValidObject(obj));

    Q_ASSERT(!obj->parent() || Probe::instance()->isValidObject(obj->parent()));

    const QMetaObject *metaObject = obj->metaObject();

    if (hasDynamicMetaObject(obj)) {
        // ideally we would clone the meta object here
        // for now we just move up to the first known static parent meta object, and work with that
        while (metaObject && !isKnownMetaObject(metaObject))
            metaObject = metaObject->superClass();
        if (!metaObject) // the QML engines actually manages to hit this case, with QObject-ified gadgets...
            return;
    }

    addMetaObject(metaObject);

    /*
     * This will increase these values:
     * - selfCount for that particular @p metaObject
     * - inclusiveCount for @p metaObject and *all* ancestors
     *
     * Complexity-wise the inclusive count calculation should be okay,
     * since the number of ancestors should be rather small
     * (QMetaObject class hierarchy is rather a broad than a deep tree structure)
     *
     * If this yields some performance issues, we might need to remove the inclusive
     * costs calculation altogether (a calculate-on-request pattern should be even slower)
     */
    m_metaObjectMap.insert(obj, metaObject);
    auto &info = m_metaObjectInfoMap[metaObject];
    ++info.selfCount;
    ++info.selfAliveCount;

    // increase inclusive counts
    const QMetaObject *current = metaObject;
    while (current) {
        auto &info = m_metaObjectInfoMap[current];
        ++info.inclusiveCount;
        ++info.inclusiveAliveCount;
        info.invalid = false;
        emit dataChanged(current);
        current = current->superClass();
    }
}

void MetaObjectRegistry::scanMetaTypes()
{
#if QT_VERSION >= QT_VERSION_CHECK(5, 0, 0)
    for (int mtId = 0; mtId <= QMetaType::User || QMetaType::isRegistered(mtId); ++mtId) {
        if (!QMetaType::isRegistered(mtId))
            continue;
        const auto *mt = QMetaType::metaObjectForType(mtId);
        if (mt)
            addMetaObject(mt);
    }
#endif
    addMetaObject(&staticQtMetaObject);
}

void MetaObjectRegistry::addMetaObject(const QMetaObject *metaObject)
{
    if (isKnownMetaObject(metaObject))
        return;

    const QMetaObject *parentMetaObject = metaObject->superClass();
    if (parentMetaObject && !isKnownMetaObject(parentMetaObject)) {
        // add parent first
        addMetaObject(metaObject->superClass());
    }

    auto &info = m_metaObjectInfoMap[metaObject];
    info.className = metaObject->className();
    info.isStatic = Execution::isReadOnlyData(metaObject);
    // make the parent immediately retrieveable, so that slots connected to
    // beforeMetaObjectAdded() can use parentOf().
    m_childParentMap.insert(metaObject, parentMetaObject);

    QVector<const QMetaObject *> &children = m_parentChildMap[ parentMetaObject ];

    emit beforeMetaObjectAdded(metaObject);
    children.push_back(metaObject);
    emit afterMetaObjectAdded(metaObject);
}

void MetaObjectRegistry::objectRemoved(QObject *obj)
{
    Q_ASSERT(thread() == QThread::currentThread());

    // decrease counter
    const QMetaObject *metaObject = m_metaObjectMap.take(obj);
    if (!metaObject)
        return;

    assert(m_metaObjectInfoMap.contains(metaObject));
    if (m_metaObjectInfoMap[metaObject].selfAliveCount == 0) {
        // something went wrong, but let's just ignore this event in case of assert
        return;
    }

    --m_metaObjectInfoMap[metaObject].selfAliveCount;
    assert(m_metaObjectInfoMap[metaObject].selfAliveCount >= 0);

    // decrease inclusive counts
    const QMetaObject *current = metaObject;
    while (current) {
        MetaObjectInfo &info = m_metaObjectInfoMap[current];
        --info.inclusiveAliveCount;
        assert(info.inclusiveAliveCount >= 0);
        emit dataChanged(current);
        const QMetaObject *parent = m_childParentMap.value(current);
        // there is no way to detect when a QMetaObject is getting actually destroyed,
        // so mark them as invalid when there are no objects if that type alive anymore.
        if (info.inclusiveAliveCount == 0 && !info.isStatic) {
            info.invalid = true;
        }
        current = parent;
    }
}

bool MetaObjectRegistry::isKnownMetaObject(const QMetaObject *metaObject) const
{
    return m_childParentMap.contains(metaObject);
}