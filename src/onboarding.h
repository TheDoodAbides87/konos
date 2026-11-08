#pragma once

#include <QWidget>

class QLineEdit;
class QStackedWidget;

// The first time Konos opens (from the redesign video, 19:44): a welcome
// with a media folder to start with, then the theme and accent colour (the
// app behind changes as you pick), then the basic keyboard shortcuts.
class Onboarding : public QWidget {
    Q_OBJECT
public:
    explicit Onboarding(QWidget *window);
    void open();
    void setStep(int step);
    // True the first time Konos runs (no settings yet), until it's finished.
    static bool wanted();

signals:
    void finished();

protected:
    void paintEvent(QPaintEvent *) override;
    bool eventFilter(QObject *obj, QEvent *event) override;

private:
    void finish();
    void layoutCard();
    QWidget *m_card;
    QStackedWidget *m_steps;
    QLineEdit *m_folder;
};
