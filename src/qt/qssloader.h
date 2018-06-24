#ifndef QSSLOADER_H
#define QSSLOADER_H

#include <QObject>
#include <QKeySequence>
class QssLoader: public QObject
{
    Q_OBJECT
public:
    static void attach(const QString& filename = defaultStyleFile(),
                       QKeySequence key = QKeySequence("F6"));

    bool eventFilter(QObject *obj, QEvent *event);
private:
    QssLoader(QObject * parent, const QString& filename, const QKeySequence& key);
    void setAppStyleSheet();
    static QString defaultStyleFile();
    QString m_filename;
    QKeySequence m_key;

};
#endif