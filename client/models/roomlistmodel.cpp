/**************************************************************************
 *                                                                        *
 * Copyright (C) 2016 Felix Rohrbach <kde@fxrh.de>                        *
 *                                                                        *
 * This program is free software; you can redistribute it and/or          *
 * modify it under the terms of the GNU General Public License            *
 * as published by the Free Software Foundation; either version 3         *
 * of the License, or (at your option) any later version.                 *
 *                                                                        *
 * This program is distributed in the hope that it will be useful,        *
 * but WITHOUT ANY WARRANTY; without even the implied warranty of         *
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the          *
 * GNU General Public License for more details.                           *
 *                                                                        *
 * You should have received a copy of the GNU General Public License      *
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.  *
 *                                                                        *
 **************************************************************************/

#include "roomlistmodel.h"

#include "../quaternionroom.h"

#include <user.h>
#include <connection.h>
#include <settings.h>

#include <QtGui/QIcon>
#include <QtCore/QStringBuilder>

#include <functional>

using namespace std::placeholders;

static const auto DirectChat = QStringLiteral("org.qmatrixclient.direct");
static const auto Untagged = QStringLiteral("org.qmatrixclient.none");
static const QStringList DefaultTagsOrder {
    QMatrixClient::FavouriteTag, QStringLiteral("u.*"), DirectChat, Untagged,
    QMatrixClient::LowPriorityTag
};

RoomListModel::RoomListModel(QObject* parent)
    : QAbstractItemModel(parent)
{ }

void RoomListModel::addConnection(QMatrixClient::Connection* connection)
{
    Q_ASSERT(connection);

    using QMatrixClient::Connection;
    using QMatrixClient::Room;
    beginResetModel();
    m_connections.emplace_back(connection, this);
    connect( connection, &Connection::loggedOut,
             this, [=]{ deleteConnection(connection); } );
    connect( connection, &Connection::invitedRoom,
             this, &RoomListModel::replaceRoom);
    connect( connection, &Connection::joinedRoom,
             this, &RoomListModel::replaceRoom);
    connect( connection, &Connection::leftRoom,
             this, &RoomListModel::replaceRoom);
    connect( connection, &Connection::aboutToDeleteRoom,
             this, &RoomListModel::deleteRoom);

    for( auto r: connection->roomMap() )
    {
        insertRoom(r);
        connectRoomSignals(static_cast<QuaternionRoom*>(r));
    }
    endResetModel();
}

void RoomListModel::deleteConnection(QMatrixClient::Connection* connection)
{
    Q_ASSERT(connection);
    const auto connIt =
            find(m_connections.begin(), m_connections.end(), connection);
    if (connIt == m_connections.end())
    {
        Q_ASSERT_X(connIt == m_connections.end(), __FUNCTION__,
                   "Connection is missing in the rooms model");
        return;
    }

    beginResetModel();
    for (auto& group: m_roomGroups)
        group.rooms.erase(
            std::remove_if(group.rooms.begin(), group.rooms.end(),
                [connection](const QuaternionRoom* r) {
                    return r->connection() == connection;
                }), group.rooms.end());
    m_roomGroups.erase(
        std::remove_if(m_roomGroups.begin(), m_roomGroups.end(),
            [=](const RoomGroup& rg) { return rg.rooms.empty(); }),
        m_roomGroups.end());
    m_connections.erase(connIt);
    endResetModel();
}

void RoomListModel::deleteTag(QModelIndex index)
{
    if (!isValidGroupIndex(index))
        return;
    const auto tag = m_roomGroups[index.row()].caption.toString();
    if (tag.isEmpty())
    {
        qCritical() << "RoomListModel: Invalid tag at position" << index.row();
        return;
    }
    if (tag.startsWith("org.qmatrixclient."))
    {
        qWarning() << "RoomListModel: System groups cannot be deleted "
                      "(tried to delete" << tag << "group)";
        return;
    }
    // After the below loop, the respective group will magically disappear from
    // m_roomGroups as well due to tagsChanged() triggered from removeTag()
    for (const auto& c: m_connections)
        for (auto* r: c->roomsWithTag(tag))
            r->removeTag(tag);
}

int RoomListModel::getRoomGroupOffset(QModelIndex index) const
{
    Q_ASSERT(index.isValid()); // Root item shouldn't come here
    // If we're on a room, find its group; otherwise just take the index
    return (index.parent().isValid() ? index.parent() : index).row();
}

RoomListModel::group_iter_t RoomListModel::getRoomGroupFor(QModelIndex index)
{
    return m_roomGroups.begin() + getRoomGroupOffset(index);
}

RoomListModel::group_citer_t RoomListModel::getRoomGroupFor(QModelIndex index) const
{
    return m_roomGroups.cbegin() + getRoomGroupOffset(index);
}

RoomListModel::group_iter_t RoomListModel::lowerBoundGroup(const QVariant& group)
{
    return std::lower_bound(m_roomGroups.begin(), m_roomGroups.end(), group,
                            m_roomOrder.groupLessThan);
}

RoomListModel::group_citer_t RoomListModel::lowerBoundGroup(
        const QVariant& group) const
{
    return std::lower_bound(m_roomGroups.begin(), m_roomGroups.end(), group,
                            m_roomOrder.groupLessThan);
}

RoomListModel::room_iter_t RoomListModel::lowerBoundRoom(
        RoomGroup& group, QuaternionRoom* room)
{
    return std::lower_bound(group.rooms.begin(), group.rooms.end(), room,
                            m_roomOrder.roomLessThanFactory(group.caption));
}

RoomListModel::room_citer_t RoomListModel::lowerBoundRoom(
        const RoomGroup& group, QuaternionRoom* room) const
{
    return std::lower_bound(group.rooms.begin(), group.rooms.end(), room,
                            m_roomOrder.roomLessThanFactory(group.caption));
}

void RoomListModel::visitRoom(QuaternionRoom* room,
                              const std::function<void(QModelIndex)>& visitor)
{
    for (const auto& g: m_roomOrder.groups(room))
    {
        const auto idx = indexOf(g, room);
        if (!isValidGroupIndex(idx.parent()))
        {
            qWarning() << "RoomListModel: Invalid group index for" << g.toString()
                       << "with room" << room->objectName();
            Q_ASSERT(false);
            continue;
        }
        if (!isValidRoomIndex(idx))
        {
            qCritical() << "RoomListModel: the current order lists room"
                        << room->objectName() << "in group" << g.toString()
                        << "but the model doesn't have it";
            Q_ASSERT(false);
            continue;
        }
        visitor(idx);
    }
}

QVariant RoomListModel::roomGroupAt(QModelIndex idx) const
{
    const auto groupIt = getRoomGroupFor(idx);
    return groupIt != m_roomGroups.end() ? groupIt->caption : QVariant();
}

QuaternionRoom* RoomListModel::roomAt(QModelIndex idx) const
{
    return isValidRoomIndex(idx)
            ? m_roomGroups[idx.parent().row()].rooms[idx.row()] : nullptr;
}

QModelIndex RoomListModel::indexOf(const QVariant& group,
                                   QuaternionRoom* room) const
{
    const auto groupIt = lowerBoundGroup(group);
    if (groupIt == m_roomGroups.end() || groupIt->caption != group)
        return {}; // Group not found
    const auto groupIdx = index(groupIt - m_roomGroups.begin(), 0);
    if (!room)
        return groupIdx; // Group caption

    const auto rIt = lowerBoundRoom(*groupIt, room);
    if (rIt == groupIt->rooms.end() || *rIt != room)
        return {}; // Room not found in this group

    return index(rIt - groupIt->rooms.begin(), 0, groupIdx);
}

QModelIndex RoomListModel::index(int row, int column,
                                 const QModelIndex& parent) const
{
    if (!hasIndex(row, column, parent))
        return {};

    // Groups get internalId() == -1, rooms get the group ordinal number
    return createIndex(row, column,
                       quintptr(parent.isValid() ? parent.row() : -1));
}

QModelIndex RoomListModel::parent(const QModelIndex& child) const
{
    const auto parentPos = int(child.internalId());
    return child.isValid() && parentPos != -1
            ? index(parentPos, 0) : QModelIndex();
}

void RoomListModel::replaceRoom(QMatrixClient::Room* room,
                                QMatrixClient::Room* prev)
{
    // There are two cases when this method is called:
    // 1. (prev == nullptr) adding a new room to the room list
    // 2. (prev != nullptr) accepting/rejecting an invitation or inviting to
    //    the previously left room (in both cases prev has the previous state).
    if (prev == room)
    {
        qCritical() << "RoomListModel::updateRoom: room tried to replace itself";
        refresh(static_cast<QuaternionRoom*>(room));
        return;
    }
    if (prev && room->id() != prev->id())
    {
        qCritical() << "RoomListModel::updateRoom: attempt to update room"
                    << prev->id() << "to" << room->id();
        // That doesn't look right but technically we still can do it.
    }
    // Ok, we're through with pre-checks, now for the real thing.
    // TODO: Maybe do better than reset the whole model.
    auto* newRoom = static_cast<QuaternionRoom*>(room);
    connectRoomSignals(newRoom);

    beginResetModel();
    doRebuild();
    endResetModel();
}

void RoomListModel::deleteRoom(QMatrixClient::Room* room)
{
    Q_ASSERT(room);
    visitRoom(static_cast<QuaternionRoom*>(room),
              std::bind(&RoomListModel::doRemoveRoom, this, _1));
}

RoomListModel::group_iter_t RoomListModel::tryInsertGroup(
        const QVariant& caption, bool notify)
{
    Q_ASSERT(!caption.toString().isEmpty());
    auto gIt = lowerBoundGroup(caption);
    if (gIt == m_roomGroups.end() || gIt->caption != caption)
    {
        const auto gPos = gIt - m_roomGroups.begin();
        if (notify)
            beginInsertRows({}, gPos, gPos);
        gIt = m_roomGroups.insert(gIt, {caption, {}});
        if (notify)
        {
            endInsertRows();
            emit groupAdded(gPos);
        }
    }
    return gIt;
}

void RoomListModel::insertRoomToGroups(const QVariantList& groups,
                                       QuaternionRoom* room, bool notify)
{
    for (const auto& g: groups)
    {
        const auto gIt = tryInsertGroup(g, notify);
        const auto rIt = lowerBoundRoom(*gIt, room);
        if (rIt != gIt->rooms.end() && *rIt == room)
        {
            qWarning() << "RoomListModel:" << room->objectName()
                       << "is already listed under group" << g.toString();
            continue;
        }
        const auto rPos = rIt - gIt->rooms.begin();
        if (notify)
            beginInsertRows(index(gIt - m_roomGroups.begin(), 0), rPos, rPos);
        gIt->rooms.insert(rIt, room);
        if (notify)
            endInsertRows();
        qDebug() << "RoomListModel: Added" << room->objectName()
                 << "to group" << gIt->caption.toString();
    }
}

void RoomListModel::insertRoom(QMatrixClient::Room* r, bool notify)
{
    // We can return void from a void function.
    if (auto* qr = static_cast<QuaternionRoom*>(r))
        return insertRoomToGroups(m_roomOrder.groups(qr), qr, notify);

    qCritical() << "Attempt to add nullptr to the room list";
    Q_ASSERT(false);
}

void RoomListModel::connectRoomSignals(QuaternionRoom* room)
{
    connect(room, &QuaternionRoom::displaynameChanged,
            this, [this,room] { displaynameChanged(room); } );
    connect(room, &QuaternionRoom::unreadMessagesChanged,
            this, [this,room] { unreadMessagesChanged(room); } );
    connect(room, &QuaternionRoom::notificationCountChanged,
            this, [this,room] { unreadMessagesChanged(room); } );
    connect(room, &QuaternionRoom::joinStateChanged,
            this, [this,room] { refresh(room); });
    connect(room, &QuaternionRoom::avatarChanged,
            this, [this,room] { refresh(room, { Qt::DecorationRole }); });
    m_roomOrder.connectRoomSignals(room);
}

void RoomListModel::doRemoveRoom(QModelIndex idx)
{
    if (!isValidRoomIndex(idx))
    {
        qCritical() << "Attempt to remove a room at invalid index" << idx;
        Q_ASSERT(false);
        return;
    }
    const auto gPos = idx.parent().row();
    auto& group = m_roomGroups[gPos];
    const auto rIt = group.rooms.begin() + idx.row();
    qDebug() << "RoomListModel: Removing room" << (*rIt)->objectName()
             << "from group" << group.caption;
    beginRemoveRows(idx.parent(), idx.row(), idx.row());
    group.rooms.erase(rIt);
    endRemoveRows();
    if (group.rooms.empty())
    {
        beginRemoveRows({}, gPos, gPos);
        m_roomGroups.remove(gPos);
        endRemoveRows();
    }
}

void RoomListModel::doRebuild()
{
    m_roomGroups.clear();
    for (const auto& c: m_connections)
        for (auto* r: c->roomMap())
            insertRoom(r);
}

int RoomListModel::rowCount(const QModelIndex& parent) const
{
    if (!parent.isValid())
        return m_roomGroups.size();

    if (isValidGroupIndex(parent))
        return m_roomGroups[parent.row()].rooms.size();

    return 0; // Rooms have no children
}

int RoomListModel::totalRooms() const
{
    int result = 0;
    for (const auto& c: m_connections)
        result += c->roomMap().size();
    return result;
}

bool RoomListModel::isValidGroupIndex(QModelIndex i) const
{
    return i.isValid() && !i.parent().isValid() && i.row() < m_roomGroups.size();
}

bool RoomListModel::isValidRoomIndex(QModelIndex i) const
{
    return i.isValid() && isValidGroupIndex(i.parent()) &&
            i.row() < m_roomGroups[i.parent().row()].rooms.size();
}

QStringList initTagsOrder()
{
    static const auto SettingsKey = QStringLiteral("tags_order");
    static QMatrixClient::SettingsGroup sg { "UI/RoomsDock" };
    const auto savedOrder = sg.get<QStringList>(SettingsKey);
    if (savedOrder.isEmpty())
    {
        sg.setValue(SettingsKey, DefaultTagsOrder);
        return DefaultTagsOrder;
    }
    return savedOrder;
}

template <typename LT, typename VT>
inline auto findIndex(const QList<LT>& list, const VT& value)
{
    // Using std::find() instead of indexOf() so that not found keys were
    // naturally sorted after found ones.
    return std::find(list.begin(), list.end(), value) - list.begin();
}

auto findIndexWithWildcards(const QStringList& list, const QString& value)
{
    if (list.empty() || value.isEmpty())
        return list.size();

    auto i = findIndex(list, value);
    // Try namespace groupings (".*" in the list), from right to left
    for (int dotPos = 0;
         i == list.size() && (dotPos = value.lastIndexOf('.', --dotPos)) != -1;)
    {
        i = findIndex(list, value.left(dotPos + 1) + '*');
    }
    return i;
}

void RoomListModel::setOrder(Grouping grouping, Sorting sorting)
{
    Q_ASSERT(grouping == GroupByTag && sorting == SortByName); // Other modes not supported yet

    RoomOrder order
    {
        GroupByTag, sorting,
        [] (const RoomGroup& group, const QVariant& tag) -> bool
        {
            static auto tagsOrder = initTagsOrder();
            const auto& lkey = group.caption.toString();
            const auto& rkey = tag.toString();
            // See above
            auto li = findIndexWithWildcards(tagsOrder, lkey);
            auto ri = findIndexWithWildcards(tagsOrder, rkey);
            return li < ri || (li == ri && lkey < rkey);
        },
        [] (const QVariant& tag) -> RoomOrder::room_lessthan_t
        {
            return [tag=tag.toString()] (const QuaternionRoom* r1,
                                         const QuaternionRoom* r2)
            {
                if (r1 == r2)
                    return false; // Short-circuit

                auto o1 = r1->tag(tag).order;
                auto o2 = r2->tag(tag).order;
                if (o2.omitted())
                    return !o1.omitted() || r1->id() < r2->id(); // FIXME: Use displayName() once the model learns how to move rooms around due to display name changes
                if (o1.omitted()) // && !o2.omitted()
                    return false;

                if (o1.value() < o2.value())
                    return true;

                if (o1.value() > o2.value() || r1->id() == r2->id())
                    return false;

                qWarning() << "RoomListModel:" << tag
                           << "order values aren't strongly ordered:"
                           << r1->objectName() << "with" << o1.value() << "vs."
                           << r2->objectName() << "with" << o2.value();
                return r1->id() < r2->id();
            };
        },
        [] (const QuaternionRoom* r) -> RoomOrder::groups_t
        {
            auto tags = r->tags().keys();
            RoomOrder::groups_t vl; vl.reserve(tags.size());
            std::copy(tags.cbegin(), tags.cend(), std::back_inserter(vl));
            if (r->isDirectChat())
                vl.push_back(DirectChat);
            if (vl.empty())
                vl.push_back(Untagged);
            return vl;
        },
        [this] (QuaternionRoom* r)
        {
            connect(r, &QuaternionRoom::tagsAboutToChange,
                    this, std::bind(&RoomListModel::prepareToUpdateGroups,
                                    this, r));
            connect(r, &QuaternionRoom::tagsChanged,
                    this, std::bind(&RoomListModel::updateGroups, this, r));
        }
    };

    beginResetModel();
    m_roomOrder = order;
    doRebuild();
    endResetModel();
}

QVariant RoomListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid())
        return {};

    if (isValidGroupIndex(index))
    {
        if (role == Qt::DisplayRole)
        {
            static const auto FavouritesLabel = tr("Favourites");
            static const auto LowPriorityLabel = tr("Low priority");
            static const auto DirectChatsLabel = tr("People");
            static const auto UntaggedRoomsLabel = tr("Ungrouped rooms");

            const auto c = roomGroupAt(index);
            return c == Untagged ? UntaggedRoomsLabel :
                   c == DirectChat ? DirectChatsLabel :
                   c == QMatrixClient::FavouriteTag ? FavouritesLabel :
                   c == QMatrixClient::LowPriorityTag ? LowPriorityLabel :
                   c.toString().startsWith("u.") ? c.toString().mid(2) : c;
        }
        return {};
    }

    auto* const room = roomAt(index);
    if (!room)
        return {};
//    if (index.column() == 1)
//        return room->lastUpdated();
//    if (index.column() == 2)
//        return room->lastAttended();

    using QMatrixClient::JoinState;
    switch (role)
    {
        case Qt::DisplayRole:
        {
            const auto unreadCount = room->unreadCount();
            const auto postfix = unreadCount == -1 ? QString() :
                room->readMarker() != room->timelineEdge()
                    ? QStringLiteral(" [%1]").arg(unreadCount)
                    : QStringLiteral(" [%1+]").arg(unreadCount);
            for (const auto& c: m_connections)
            {
                if (c == room->connection())
                    continue;
                if (c->room(room->id(), room->joinState()))
                    return tr("%1 (as %2)").arg(room->displayName(),
                                                room->connection()->userId())
                           + postfix;
            }
            return room->displayName() + postfix;
        }
        case Qt::DecorationRole:
        {
            auto avatar = room->avatar(16, 16);
            if (!avatar.isNull())
                return avatar;
            switch( room->joinState() )
            {
                case JoinState::Join:
                    return QIcon(":/irc-channel-joined.svg");
                case JoinState::Invite:
                    return QIcon(":/irc-channel-invited.svg");
                case JoinState::Leave:
                    return QIcon(":/irc-channel-parted.svg");
            }
        }
        case Qt::ToolTipRole:
        {
            auto result = QStringLiteral("<b>%1</b><br>").arg(room->displayName());
            result += tr("Main alias: %1<br>").arg(room->canonicalAlias());
            result += tr("Members: %1<br>").arg(room->memberCount());

            auto directChatUsers = room->directChatUsers();
            if (!directChatUsers.isEmpty())
            {
                QStringList userNames;
                for (auto* user: directChatUsers)
                    userNames.push_back(user->displayname(room));
                result += tr("Direct chat with %1<br>")
                            .arg(userNames.join(','));
            }

            if (room->usesEncryption())
                result += tr("The room enforces encryption<br>");

            auto unreadCount = room->unreadCount();
            if (unreadCount >= 0)
            {
                const auto unreadLine =
                    room->readMarker() == room->timelineEdge()
                        ? tr("Unread messages: %1+<br>")
                        : tr("Unread messages: %1<br>");
                result += unreadLine.arg(unreadCount);
            }

            auto hlCount = room->highlightCount();
            if (hlCount > 0)
                result += tr("Unread highlights: %1<br>").arg(hlCount);

            result += tr("ID: %1<br>").arg(room->id());
            switch (room->joinState())
            {
                case JoinState::Join:
                    result += tr("You joined this room");
                    break;
                case JoinState::Leave:
                    result += tr("You left this room");
                    break;
                case JoinState::Invite:
                    result += tr("You were invited into this room");
            }
            return result;
        }
        case HasUnreadRole:
            return room->hasUnreadMessages();
        case HighlightCountRole:
            return room->highlightCount();
        case JoinStateRole:
            return toCString(room->joinState()); // FIXME: better make the enum QVariant-convertible (only possible from Qt 5.8, see Q_ENUM_NS)
        case ObjectRole:
            return QVariant::fromValue(room);
        default:
            return {};
    }
}

int RoomListModel::columnCount(const QModelIndex&) const
{
    return 1;
}

void RoomListModel::displaynameChanged(QuaternionRoom* room)
{
    refresh(room);
}

void RoomListModel::unreadMessagesChanged(QuaternionRoom* room)
{
    refresh(room);
}

void RoomListModel::prepareToUpdateGroups(QuaternionRoom* room)
{
    Q_ASSERT(m_roomOrder.grouping == GroupByTag);
    Q_ASSERT(m_roomIdxCache.empty()); // Not in the midst of another update

    const auto& groups = m_roomOrder.groups(room);
    for (const auto& g: groups)
    {
        const auto& rIdx = indexOf(g, room);
        Q_ASSERT(isValidRoomIndex(rIdx));
        m_roomIdxCache.push_back(rIdx);
    }
}

void RoomListModel::updateGroups(QuaternionRoom* room)
{
    if (m_roomOrder.grouping != GroupByTag)
        return;

    auto groups = m_roomOrder.groups(room);
    for (const auto& oldIndex: qAsConst(m_roomIdxCache))
    {
        Q_ASSERT(isValidRoomIndex(oldIndex));
        const auto gIdx = oldIndex.parent();
        auto& group = m_roomGroups[gIdx.row()];
        if (groups.removeOne(group.caption)) // Test and remove at once
        {
            const auto oldIt = group.rooms.begin() + oldIndex.row();
            const auto newIt = lowerBoundRoom(group, room);
            if (newIt == oldIt)
                continue;

            beginMoveRows(gIdx, oldIndex.row(), oldIndex.row(),
                          gIdx, int(newIt - group.rooms.begin()));
            std::move(oldIt, oldIt + 1, newIt);
            endMoveRows();
        } else
            doRemoveRoom(oldIndex); // May invalidate `group`
    }
    m_roomIdxCache.clear();
    insertRoomToGroups(groups, room, true); // Groups the room wasn't before
}

void RoomListModel::refresh(QuaternionRoom* room, const QVector<int>& roles)
{
    // The problem here is that the change might cause the room to change
    // its groups. Assume for now that such changes are processed elsewhere
    // where details about the change are available (e.g. in tagsChanged).
    visitRoom(room,
        [this,&roles] (QModelIndex idx) { emit dataChanged(idx, idx, roles); });
}
