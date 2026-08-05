#ifndef CLIPBOARD_SERVICE_H
#define CLIPBOARD_SERVICE_H

#include <QObject>
#include <QString>

class ClipboardService : public QObject {
    Q_OBJECT

public:
    explicit ClipboardService(QObject* parent = nullptr);
    ~ClipboardService();

    void start();

    void setTextFromClient(const QString& text);
    QString text() const;

    bool isAvailable() const { return available_; }

signals:
    void textChanged(const QString& text);

private slots:
    void onClipboardChanged();

private:
    bool available_ = false;
    QString lastText_;
};

#endif // CLIPBOARD_SERVICE_H
