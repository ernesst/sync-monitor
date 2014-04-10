/*
 * Copyright 2014 Canonical Ltd.
 *
 * This file is part of sync-monitor.
 *
 * sync-monitor is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; version 3.
 *
 * contact-service-app is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "eds-helper-mock.h"
#include "config.h"

#include <gmock/gmock.h>

#include <QObject>
#include <QtTest>
#include <QDebug>

#include <QtContacts/QContact>
#include <QtContacts/QContactName>
#include <QtContacts/QContactSyncTarget>

#include <QtOrganizer/QOrganizerEvent>

using namespace QtContacts;
using namespace QtOrganizer;

class EdsHelperTest : public QObject
{
    Q_OBJECT

private Q_SLOTS:
    void testCreateAContact()
    {
        EdsHelperMock mock;
        QSignalSpy spy(&mock, SIGNAL(dataChanged(QString,QString)));
        QContact c;

        QContactName name;
        name.setFirstName("Foo");
        name.setLastName("Bar");
        c.saveDetail(&name);

        QContactSyncTarget syncTarget;
        syncTarget.setSyncTarget("address-book-test");
        c.saveDetail(&syncTarget);

        mock.contactEngine()->saveContact(&c);

        // check if the signal dataChanged was fired with contacts
        QCOMPARE(spy.count() , 1);
        QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args[0].toString(), QStringLiteral(CONTACTS_SERVICE_NAME));
        QCOMPARE(args[1].toString(), QStringLiteral("address-book-test"));
    }

    void testCreateACalendarEvent()
    {
        EdsHelperMock mock;
        QSignalSpy spy(&mock, SIGNAL(dataChanged(QString,QString)));
        QOrganizerEvent ev;

        ev.setDescription("test");
        ev.setDisplayLabel("display test");
        ev.setStartDateTime(QDateTime::currentDateTime());
        mock.trackCollectionFromItem(&ev);

        mock.organizerEngine()->saveItem(&ev);

        // check if the signal dataChanged was fired with contacts
        QTRY_COMPARE(spy.count(), 1);
        QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args[0].toString(), QStringLiteral(CALENDAR_SERVICE_NAME));
        QCOMPARE(args[1].toString(), QStringLiteral("Default Collection"));
    }

    void testFreezeNotify()
    {
        EdsHelperMock mock;
        mock.freezeNotify();
        QSignalSpy spy(&mock, SIGNAL(dataChanged(QString,QString)));

        // create contact
        QContact c;
        QContactName name;
        name.setFirstName("Foo");
        name.setLastName("Bar");
        c.saveDetail(&name);

        QContactSyncTarget syncTarget;
        syncTarget.setSyncTarget("address-book-test");
        c.saveDetail(&syncTarget);

        mock.contactEngine()->saveContact(&c);
        QCOMPARE(spy.count(), 0);

        // create a event
        QOrganizerEvent ev;

        ev.setDescription("test");
        ev.setDisplayLabel("display test");
        ev.setStartDateTime(QDateTime::currentDateTime());
        mock.trackCollectionFromItem(&ev);

        mock.organizerEngine()->saveItem(&ev);
        QCOMPARE(spy.count(), 0);

        // flush all pending events
        mock.flush();
        QTRY_COMPARE(spy.count(), 2);

        QList<QVariant> args = spy.takeFirst();
        QCOMPARE(args[0].toString(), QStringLiteral(CONTACTS_SERVICE_NAME));
        QCOMPARE(args[1].toString(), QStringLiteral("address-book-test"));

        args = spy.takeFirst();
        QCOMPARE(args[0].toString(), QStringLiteral(CALENDAR_SERVICE_NAME));
        QCOMPARE(args[1].toString(), QStringLiteral("Default Collection"));
    }
};

int main(int argc, char *argv[])
{
    // The following line causes Google Mock to throw an exception on failure,
    // which will be interpreted by your testing framework as a test failure.
    ::testing::GTEST_FLAG(throw_on_failure) = true;
    ::testing::InitGoogleMock(&argc, argv);

    QCoreApplication app(argc, argv);
    app.setAttribute(Qt::AA_Use96Dpi, true);
    EdsHelperTest tc;
    return QTest::qExec(&tc, argc, argv);
}

#include "eds-helper-test.moc"

