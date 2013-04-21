/*

Copyright 2012-2013 Adam Reichold
Copyright 2012 Michał Trybus

This file is part of qpdfview.

qpdfview is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

qpdfview is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with qpdfview.  If not, see <http://www.gnu.org/licenses/>.

*/

#include <iostream>

#include <QApplication>
#include <QDebug>
#include <QDir>
#include <QMessageBox>
#include <QScopedPointer>
#include <QTranslator>

#ifdef WITH_DBUS

#include <QDBusInterface>
#include <QDBusReply>

#endif // WITH_DBUS

#ifdef WITH_SYNCTEX

#include <synctex_parser.h>

#endif // WITH_SYNCTEX

#include "documentview.h"
#include "mainwindow.h"

#ifdef WITH_SIGNALS

#include "signalhandler.h"

#endif // WITH_SIGNALS

struct File
{
    QString filePath;
    int page;

    QString sourceName;
    int sourceLine;
    int sourceColumn;
    QRectF enclosingBox;

    File() : filePath(), page(-1), sourceName(), sourceLine(-1), sourceColumn(-1), enclosingBox() {}

};

int main(int argc, char** argv)
{
    qRegisterMetaType< QList< QRectF > >("QList<QRectF>");
    qRegisterMetaType< Rotation >("Rotation");

    QApplication application(argc, argv);

    QApplication::setOrganizationDomain("local.qpdfview");
    QApplication::setOrganizationName("qpdfview");
    QApplication::setApplicationName("qpdfview");

    QApplication::setApplicationVersion(APPLICATION_VERSION);

    QApplication::setWindowIcon(QIcon(":icons/qpdfview.svg"));

    QTranslator translator;

#if QT_VERSION >= QT_VERSION_CHECK(4,8,0)

    if(translator.load(QLocale::system(), "qpdfview", "_", QDir(QApplication::applicationDirPath()).filePath("data"))) { application.installTranslator(&translator); }
    else if(translator.load(QLocale::system(), "qpdfview", "_", DATA_INSTALL_PATH)) { application.installTranslator(&translator); }

#else

    if(translator.load("qpdfview_" + QLocale::system().name(), QDir(QApplication::applicationDirPath()).filePath("data"))) { application.installTranslator(&translator); }
    else if(translator.load("qpdfview_" + QLocale::system().name(), DATA_INSTALL_PATH)) { application.installTranslator(&translator); }

#endif // QT_VERSION

    bool unique = false;
    bool quiet = false;

    bool instanceNameIsNext = false;
    QString instanceName = "";

    bool searchTextIsNext = false;
    QString searchText = "";

    QList< File > files;

    {
        // command-line arguments

        QRegExp regExp1("(.+)#(\\d+)");
        QRegExp regExp2("(.+)#src:(.+):(\\d+):(\\d+)");

        QStringList arguments = QApplication::arguments();

        if(!arguments.isEmpty())
        {
            arguments.removeFirst();
        }

        foreach(QString argument, arguments)
        {
            if(instanceNameIsNext)
            {
                if(argument.isEmpty())
                {
                    qCritical() << QObject::tr("An empty instance name is not allowed.");
                    return 1;
                }

                instanceNameIsNext = false;
                instanceName = argument;
            }
            else if(searchTextIsNext)
            {
                if(argument.isEmpty())
                {
                    qCritical() << QObject::tr("An empty search text is not allowed.");
                    return 1;
                }

                searchTextIsNext = false;
                searchText = argument;
            }
            else if(argument == "--unique")
            {
                unique = true;
            }
            else if(argument == "--quiet")
            {
                quiet = true;
            }
            else if(argument == "--instance")
            {
                instanceNameIsNext = true;
            }
            else if(argument == "--search")
            {
                searchTextIsNext = true;
            }
            else if(argument == "--help")
            {
                std::cout << "Usage: qpdfview [options] [file[#page]] [file[#src:name:line:column]] ..." << std::endl
                          << std::endl
                          << "Available options:" << std::endl
                          << "  --help                      Show this information" << std::endl
                          << "  --quiet                     Suppress warning messages when opening files" << std::endl
                          << "  --search text               Search for text in the current tab" << std::endl
                          << "  --unique                    Open files as tabs in unique window" << std::endl
                          << "  --unique --instance name    Open files as tabs in named instance" << std::endl
                          << std::endl
                          << "Please report bugs at \"https://launchpad.net/qpdfview\"." << std::endl;

                return 0;
            }
            else
            {
                File file;

                if(regExp1.exactMatch(argument))
                {
                    file.filePath = QFileInfo(regExp1.cap(1)).absoluteFilePath();
                    file.page = regExp1.cap(2).toInt();
                }
                else if(regExp2.exactMatch(argument))
                {
                    file.filePath = QFileInfo(regExp2.cap(1)).absoluteFilePath();
                    file.sourceName = regExp2.cap(2);
                    file.sourceLine = regExp2.cap(3).toInt();
                    file.sourceColumn = regExp2.cap(4).toInt();
                }
                else
                {
                    file.filePath = QFileInfo(argument).absoluteFilePath();
                }

                files.append(file);
            }
        }

        if(instanceNameIsNext)
        {
            qCritical() << QObject::tr("Using '--instance' requires an instance name.");
            return 1;
        }

        if(!unique && !instanceName.isEmpty())
        {
            qCritical() << QObject::tr("Using '--instance' is not allowed without using '--unique'.");
            return 1;
        }

        if(searchTextIsNext)
        {
            qCritical() << QObject::tr("Using '--search' requires a search text.");
            return 1;
        }
    }

#ifdef WITH_SYNCTEX

    {
        // SyncTeX

        for(int index = 0; index < files.count(); ++index)
        {
            File& file = files[index];

            if(!file.sourceName.isNull())
            {
                synctex_scanner_t scanner = synctex_scanner_new_with_output_file(file.filePath.toLocal8Bit(), 0, 1);

                if(scanner != 0)
                {
                    if(synctex_display_query(scanner, file.sourceName.toLocal8Bit(), file.sourceLine, file.sourceColumn) > 0)
                    {
                        for(synctex_node_t node = synctex_next_result(scanner); node != 0; node = synctex_next_result(scanner))
                        {
                            int page = synctex_node_page(node);
                            QRectF enclosingBox(synctex_node_box_visible_h(node), synctex_node_box_visible_v(node), synctex_node_box_visible_width(node), synctex_node_box_visible_height(node));

                            if(file.page != page)
                            {
                                file.page = page;
                                file.enclosingBox = enclosingBox;
                            }
                            else
                            {
                                file.enclosingBox = file.enclosingBox.united(enclosingBox);
                            }
                        }
                    }

                    synctex_scanner_free(scanner);
                }
                else
                {
                    qWarning() << QObject::tr("SyncTeX data for '%1' could not be found.").arg(file.filePath);
                }
            }
        }
    }

#endif // WITH_SYNCTEX

#ifdef WITH_DBUS

    MainWindow* mainWindow = 0;

    {
        // D-Bus

        QString serviceName = QApplication::organizationDomain();

        if(!instanceName.isEmpty())
        {
            serviceName.append("." + instanceName);
        }

        if(unique)
        {
            QScopedPointer< QDBusInterface > interface(new QDBusInterface(serviceName, "/MainWindow", "local.qpdfview.MainWindow", QDBusConnection::sessionBus()));

            if(interface->isValid())
            {
                interface->call("raiseAndActivate");

                foreach(File file, files)
                {
                    QDBusReply< bool > reply = interface->call("jumpToPageOrOpenInNewTab", file.filePath, file.page, true, file.enclosingBox, quiet);

                    if(!reply.isValid())
                    {
                        qCritical() << QDBusConnection::sessionBus().lastError().message();

                        return 1;
                    }
                }

                if(!searchText.isEmpty())
                {
                    interface->call("startSearch", searchText);
                }

                return 0;
            }
            else
            {
                mainWindow = new MainWindow(instanceName);

                new MainWindowAdaptor(mainWindow);

                if(!QDBusConnection::sessionBus().registerService(serviceName))
                {
                    qCritical() << QDBusConnection::sessionBus().lastError().message();

                    delete mainWindow;
                    return 1;
                }

                if(!QDBusConnection::sessionBus().registerObject("/MainWindow", mainWindow))
                {
                    qCritical() << QDBusConnection::sessionBus().lastError().message();

                    delete mainWindow;
                    return 1;
                }
            }
        }
        else
        {
            mainWindow = new MainWindow();
        }
    }

#else

    MainWindow* mainWindow = new MainWindow();

#endif // WITH_DBUS

#ifdef WITH_SIGNALS

    if(SignalHandler::prepareSignals())
    {
        SignalHandler* signalHandler = new SignalHandler(mainWindow);

        QObject::connect(signalHandler, SIGNAL(sigIntReceived()), mainWindow, SLOT(close()));
        QObject::connect(signalHandler, SIGNAL(sigTermReceived()), mainWindow, SLOT(close()));
    }
    else
    {
        qWarning() << QObject::tr("Could not prepare signal handler.");
    }

#endif // WITH_SIGNALS

    mainWindow->show();
    mainWindow->setAttribute(Qt::WA_DeleteOnClose);

    foreach(File file, files)
    {
        mainWindow->openInNewTab(file.filePath, file.page, file.enclosingBox, quiet);
    }

    if(!searchText.isEmpty())
    {
        mainWindow->startSearch(searchText);
    }
    
    return application.exec();
}
