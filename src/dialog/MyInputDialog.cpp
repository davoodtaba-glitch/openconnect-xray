/*
 * Copyright (C) 2014 Red Hat
 *
 * This file is part of openconnect-gui.
 *
 * openconnect-gui is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "MyInputDialog.h"

MyInputDialog::MyInputDialog(QWidget* w, QString title, QString short_desc, QStringList list)
    : w(w)
    , title(title)
    , short_desc(short_desc)
    , list(list)
    , have_list(true)
{
    mutex.lock();
    this->moveToThread(QApplication::instance()->thread());
}

MyInputDialog::MyInputDialog(QWidget* w, QString title, QString short_desc, QLineEdit::EchoMode type)
    : w(w)
    , title(title)
    , short_desc(short_desc)
    , have_list(false)
    , type(type)
{
    mutex.lock();
    this->moveToThread(QApplication::instance()->thread());
}

MyInputDialog::~MyInputDialog()
{
    mutex.tryLock();
    mutex.unlock();
}

void MyInputDialog::show()
{
    QCoreApplication::postEvent(this, new QEvent(QEvent::User));
}

void MyInputDialog::set_banner(QString banner, QString message)
{
    this->banner = banner.trimmed();
    this->message = message.trimmed();
}

bool MyInputDialog::event(QEvent* ev)
{
    res = false;
    if (ev->type() == QEvent::User) {
        QString to_print;
        if (this->banner.isEmpty() != true) {
            to_print += this->banner + QLatin1String("<br><br>");
        }

        if (this->message.isEmpty() != true) {
            to_print += this->message + QLatin1String("<br><br>");
        }

        to_print += short_desc;
        if (this->have_list) {
            text = QInputDialog::getItem(w, title, to_print, list, 0, false, &res);
        } else {
            text = QInputDialog::getText(w, title, to_print, type, QString(), &res);
        }

        mutex.unlock();
    }
    return res;
}

bool MyInputDialog::result(QString& text)
{
    mutex.lock();
    mutex.unlock();
    text = this->text;
    return res;
}
