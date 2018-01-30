/******************************************************************************
 * Copyright (C) 2017 Kitsune Ral <kitsune-ral@users.sf.net>
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
 */

#pragma once

#include "dialog.h"

namespace QMatrixClient {
    class Connection;
}

class QuaternionRoom;

class QComboBox;
class QLineEdit;
class QPlainTextEdit;
class QCheckBox;
class QPushButton;
class QListWidget;

class RoomSettingsDialog : public Dialog
{
        Q_OBJECT
    public:
        using connections_t = QVector<QMatrixClient::Connection*>;

        RoomSettingsDialog(const connections_t& connections,
                           QWidget* parent = nullptr);
        RoomSettingsDialog(QuaternionRoom* room, QWidget* parent = nullptr);

    public slots:
        void updatePushButtons();

    private slots:
        void load() override;
        void apply() override;

    private:
        RoomSettingsDialog(const QString& title, const QString& applyButtonText,
            QuaternionRoom* r, QWidget* parent, const connections_t& cs = {},
            QDialogButtonBox::StandardButtons extraButtons = QDialogButtonBox::Reset);

        const connections_t connections;
        QuaternionRoom* room;

        QComboBox* account;
        QLabel* avatar;
        QLineEdit* roomName;
        QLineEdit* alias;
        QPlainTextEdit* topic;
        QCheckBox* publishRoom;
        QCheckBox* guestCanJoin;
        QComboBox* nextInvitee;
        QPushButton* inviteButton;
        QListWidget* invitees;
};
