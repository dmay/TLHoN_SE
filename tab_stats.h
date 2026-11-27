#ifndef TAB_STATS_H
#define TAB_STATS_H

#include "jsonparser.h"
#include "inventorymodel.h"

#include <QWidget>

namespace Ui {
class Tab_stats;
}

class Tab_Stats : public QWidget
{
    Q_OBJECT

    int id = -1;
    bool isBlockSignal = false;
public:
    explicit Tab_Stats(QWidget *parent = nullptr);
    void initSettings(class Tab_Settings* s);
    ~Tab_Stats();

private slots:
    void on_btn_cheat_toggled(bool checked);
    // void translate();
    void on_cb_restore_quests_toggled(bool);

private:
    void update();
    void all_item_cheat(class QCheckBox* cb, Inventory::LegacyGroup group);
    void le_textChanged(QString param, QString text);
    void setCheat(QString param, int value, bool info = false);
    bool applyMutation(const std::function<void(SaveCharacter &)> &mutation);

private:
    Ui::Tab_stats *ui;
    class PositiveIntValidator* validator;
};

#endif // TAB_STATS_H
