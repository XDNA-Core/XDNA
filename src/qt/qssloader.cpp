#include "qssloader.h"
#include <QApplication>
#include <QFile>
#include <QKeyEvent>
#include <QDebug>
#include <QMessageBox>

void QssLoader::attach(const QString &filename, QKeySequence key)
{
    QssLoader * loader = new QssLoader(qApp, filename, key);
    qApp->installEventFilter(loader);
    loader->setAppStyleSheet();
}

bool QssLoader::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::KeyPress)
    {
       QKeyEvent *keyEvent = static_cast<QKeyEvent *>(event);
       if(m_key == QKeySequence(keyEvent->key()))
       {
         setAppStyleSheet();
         return true;
       }
    }
      return QObject::eventFilter(obj, event);
}

void QssLoader::setAppStyleSheet()
{
    QFile file(m_filename);
    if(!file.open(QIODevice::ReadOnly))
    {
        qDebug() << "Cannot open qss file " << m_filename;
        return;
    }
    QString stylesheet = QString::fromUtf8(file.readAll());
    qApp->setStyleSheet(stylesheet);
}

QString QssLoader::defaultStyleFile()
{
    return QApplication::applicationDirPath() + "/default.qss";
}


QssLoader::QssLoader(QObject *parent, const QString& filename, const QKeySequence &key):
    QObject(parent),
    m_filename(filename),
    m_key(key)
{

}
