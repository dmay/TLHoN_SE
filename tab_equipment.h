#ifndef TAB_EQUIPMENT_H
#define TAB_EQUIPMENT_H

#include "equipmentservice.h"
#include <QWidget>

namespace Ui { class Tab_Equipment; }
class QComboBox;
class Tab_Inventory;
class Tab_Settings;

class Tab_Equipment : public QWidget
{
    Q_OBJECT

public:
    explicit Tab_Equipment(QWidget *parent = nullptr);
    void initSettings(Tab_Settings *settings, Tab_Inventory *inventory);
    void refresh();
    ~Tab_Equipment();

private:
    struct Binding {
        Equipment::Position position;
        QComboBox *box;
    };
    void populate(const Equipment::Changes &pending = {});
    void updatePreview();
    void apply();
    void retranslate();
    Equipment::Changes draft() const;
    bool hasDraft() const;
    bool confirmDiscardDraft();
    QString choiceText(const Equipment::Slot &slot) const;
    QString choiceDetails(const Equipment::Slot &slot) const;

    Ui::Tab_Equipment *ui;
    Tab_Inventory *inventory = nullptr;
    QList<Binding> bindings;
    Equipment::Model model;
    QJsonObject baseline;
    int characterIndex = -1;
    bool valid = false;
    QString readError;
};

#endif
