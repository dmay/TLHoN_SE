#include "tab_stats.h"
#include "ui_tab_stats.h"
#include "stylefromfile.h"

#include "tab_settings.h"
#include "tab_inventory.h"
#include "positiveintvalidator.h"

#include <QMessageBox>
#include <QSignalBlocker>
#include <cmath>

enum
{
    DATADIN,
    FORMATTER,
    RESOLUTIONARY,
    SOURCERER,
    RANDOMASTER
};

QString getClass(int c)
{
    switch(c)
    {
    case DATADIN:
        return QObject::tr("Инфорыцарь");
        break;
    case FORMATTER:
        return QObject::tr("Форматировщик");
        break;
    case RESOLUTIONARY:
        return QObject::tr("Разбойник");
        break;
    case SOURCERER:
        return QObject::tr("Чарокодер");
        break;
    case RANDOMASTER:
    default:
        return QObject::tr("Рандомастер");
        break;
    }
}

void disableIfCompleted(QCheckBox* cb, int value, int cond)
{
    const QSignalBlocker blocker(cb);
    if(value >= cond)
    {
        cb->setChecked(true);
        cb->setEnabled(false);
    }
    else
    {
        cb->setChecked(false);
        cb->setEnabled(true);
    }
}

Tab_Stats::Tab_Stats(QWidget *parent) :
    QWidget(parent),
    ui(new Ui::Tab_stats)
{
    ui->setupUi(this);

    setStyleFromFile(this, ":/Resources/StyleSheets/Tab_Stats.qss");

    validator = new PositiveIntValidator;

    ui->le_memory->setValidator(validator);

    for(int i = 0; i < ui->gl_stats->rowCount(); ++i)
        static_cast<QLineEdit*>(ui->gl_stats->itemAtPosition(i, 1)->widget())->setValidator(validator);

    connect(ui->le_memory,          &QLineEdit::editingFinished, [this]() { le_textChanged("memory",           ui->le_memory->text());          });
    connect(ui->le_stat_vitality,   &QLineEdit::editingFinished, [this]() { le_textChanged("_stat_vitality",   ui->le_stat_vitality->text());   });
    connect(ui->le_stat_stamina,    &QLineEdit::editingFinished, [this]() { le_textChanged("_stat_stamina",    ui->le_stat_stamina->text());    });
    connect(ui->le_stat_equipLoad,  &QLineEdit::editingFinished, [this]() { le_textChanged("_stat_equipLoad",  ui->le_stat_equipLoad->text());  });
    connect(ui->le_stat_strength,   &QLineEdit::editingFinished, [this]() { le_textChanged("_stat_strength",   ui->le_stat_strength->text());   });
    connect(ui->le_stat_dexterity,  &QLineEdit::editingFinished, [this]() { le_textChanged("_stat_dexterity",  ui->le_stat_dexterity->text());  });
    connect(ui->le_stat_attunement, &QLineEdit::editingFinished, [this]() { le_textChanged("_stat_attunement", ui->le_stat_attunement->text()); });
    connect(ui->le_stat_luck,       &QLineEdit::editingFinished, [this]() { le_textChanged("_stat_luck",       ui->le_stat_luck->text());       });

    connect(ui->cb_backstabs,  &QCheckBox::toggled, [this](bool) { setCheat("backstabCount",      20); });
    connect(ui->cb_repostes,   &QCheckBox::toggled, [this](bool) { setCheat("reposteCount",       20); });
    connect(ui->cb_rebuffs,    &QCheckBox::toggled, [this](bool) { setCheat("rebuffCount",        20); });
    connect(ui->cb_remembered, &QCheckBox::toggled, [this](bool) { setCheat("itemsRemembered",    75); });
    connect(ui->cb_unburdened, &QCheckBox::toggled, [this](bool) { setCheat("itemsHoovered",      71); });
    connect(ui->cb_DLC,        &QCheckBox::toggled, [this](bool) { setCheat("DLCItemsRemembered", 10); });

    connect(ui->cb_help,       &QCheckBox::toggled, [this](bool) { setCheat("bossFightAssistances", 0, true); });

    connect(ui->cb_all_spell, &QCheckBox::toggled, [this](bool) {
        all_item_cheat(ui->cb_all_spell, Inventory::LegacyGroup::Spells);
    });
    connect(ui->cb_all_rune, &QCheckBox::toggled, [this](bool) {
        all_item_cheat(ui->cb_all_rune, Inventory::LegacyGroup::Runes);
    });

    // Explicit command buttons are wired only when the shared catalog tab is present.
    ui->btn_all_weapon->setEnabled(false);
    ui->btn_all_armor->setEnabled(false);

    ui->btn_cheat->setVisible(false);
    ui->line_3->setVisible(false);

    for(int i = 0; i < ui->vl_cheat_layout->count(); ++i)
        if(auto w = ui->vl_cheat_layout->itemAt(i)->widget(); w)
            w->setVisible(false);

    ui->btn_cheat->setCheckable(true);
    update();
}

void Tab_Stats::initSettings(Tab_Settings *s)
{
    connect(s, &Tab_Settings::characterChanged, this, &Tab_Stats::update);
    connect(s, &Tab_Settings::modeToggled, ui->btn_cheat, &QPushButton::setVisible);
    connect(s, &Tab_Settings::saveRestored, this, &Tab_Stats::update);
    connect(s, &Tab_Settings::editingAvailabilityChanged, this, &Tab_Stats::update);
    connect(s, &Tab_Settings::languageChanged, [this](){ui->retranslateUi(this); });
}

void Tab_Stats::initInventory(Tab_Inventory *inventory)
{
    ui->btn_all_armor->setEnabled(true);
    ui->btn_all_weapon->setEnabled(true);
    connect(ui->btn_all_armor, &QPushButton::clicked, inventory, &Tab_Inventory::addAllArmor);
    connect(ui->btn_all_weapon, &QPushButton::clicked, inventory, &Tab_Inventory::addAllWeapons);
    connect(inventory, &Tab_Inventory::inventoryChanged, this, &Tab_Stats::update);
    connect(this, &Tab_Stats::legacyInventoryChanged, inventory, &Tab_Inventory::refresh);
}

void Tab_Stats::le_textChanged(QString param, QString text)
{
    if (isBlockSignal || !isEnabled() || id < 0 || id >= JsonParser::characters().size()) return;
    if (text.toInt() == JsonParser::characters()[id].character[param].toInt()) return;
    applyMutation([&](SaveCharacter &slot) { slot.character[param] = text.toInt(); });
}

void Tab_Stats::setCheat(QString param, int value, bool info)
{
    if (isBlockSignal || !isEnabled() || id < 0 || id >= JsonParser::characters().size()) return;
    if (!applyMutation([&](SaveCharacter &slot) {
        (info ? slot.info : slot.character)[param] = value;
    }) && info) {
        const QSignalBlocker blocker(ui->cb_help);
        ui->cb_help->setChecked(false);
    }
}

bool Tab_Stats::applyMutation(const std::function<void(SaveCharacter &)> &mutation)
{
    QString error;
    const bool success = JsonParser::mutateCharacter(id, mutation, error);
    update();
    if (!success)
        QMessageBox::warning(this, tr("Save not changed"), error);
    return success;
}

void Tab_Stats::update()
{
    id = Tab_Settings::getCharacterId();
    const auto &characters = JsonParser::characters();
    isBlockSignal = true;
    const bool selected = id >= 0 && id < characters.size();
    setEnabled(selected && JsonParser::canEdit());
    // One-shot action controls are not a live model of the save. Reset them
    // after a character change or restore instead of displaying stale checks.
    for (auto *box : {ui->cb_all_spell, ui->cb_all_rune,
                      ui->cb_restore_quests, ui->cb_help}) {
        const QSignalBlocker blocker(box);
        box->setChecked(false);
    }
    if (!selected) {
        ui->l_nickname->setText(tr("No character"));
        for (auto *line : findChildren<QLineEdit *>()) {
            const QSignalBlocker blocker(line);
            line->clear();
        }
        for (auto *label : {ui->l_class, ui->l_level, ui->l_health, ui->l_stamina, ui->l_required,
                             ui->l_faded, ui->l_remembered, ui->l_unburdened, ui->l_DLC})
            label->clear();
        for (auto *box : {ui->cb_backstabs, ui->cb_repostes, ui->cb_rebuffs,
                          ui->cb_remembered, ui->cb_unburdened, ui->cb_DLC}) {
            const QSignalBlocker blocker(box);
            box->setChecked(false);
        }
        isBlockSignal = false;
        return;
    }
    const auto &json = characters[id].character;

    ui->l_nickname->setText(json["name"].toString());

    ui->l_class->setText(getClass(json["playerClass"].toInt()));
    ui->l_level->setText(QString::number(json["level"].toInt()));
    ui->l_health->setText(QString::number(floor(json["health"].toDouble())));
    ui->l_stamina->setText("[NULL]");

    ui->le_memory->setText(QString::number(json["memory"].toInt()));
    ui->l_required->setText("[NULL]");

    ui->le_stat_vitality->setText(QString::number(json["_stat_vitality"].toInt()));
    ui->le_stat_stamina->setText(QString::number(json["_stat_stamina"].toInt()));
    ui->le_stat_equipLoad->setText(QString::number(json["_stat_equipLoad"].toInt()));
    ui->le_stat_strength->setText(QString::number(json["_stat_strength"].toInt()));
    ui->le_stat_dexterity->setText(QString::number(json["_stat_dexterity"].toInt()));
    ui->le_stat_attunement->setText(QString::number(json["_stat_attunement"].toInt()));
    ui->le_stat_luck->setText(QString::number(json["_stat_luck"].toInt()));

    ui->l_faded->setText("[NULL]");
    ui->l_remembered->setText(QString::number(json["itemsRemembered"].toInt()));
    ui->l_unburdened->setText(QString::number(json["itemsHoovered"].toInt()));
    ui->l_DLC->setText(QString::number(json["DLCItemsRemembered"].toInt()));

    disableIfCompleted(ui->cb_backstabs,   json["backstabCount"].toInt(),        20);
    disableIfCompleted(ui->cb_repostes,    json["reposteCount"].toInt(),         20);
    disableIfCompleted(ui->cb_rebuffs,     json["rebuffCount"].toInt(),          20);
    disableIfCompleted(ui->cb_remembered,  json["itemsRemembered"].toInt(),      75);
    disableIfCompleted(ui->cb_unburdened,  json["itemsHoovered"].toInt(),        71);
    disableIfCompleted(ui->cb_DLC,         json["DLCItemsRemembered"].toInt(),   10);

    isBlockSignal = false;
}

Tab_Stats::~Tab_Stats()
{
    delete ui;
    delete validator;
}

void Tab_Stats::on_btn_cheat_toggled(bool checked)
{
    // qInfo() << this->size();
    if(checked) ui->btn_cheat->setText("<--");
    else ui->btn_cheat->setText("-->");
    ui->line_3->setVisible(checked);

    for(int i = 0; i < ui->vl_cheat_layout->count(); ++i)
        if(auto w = ui->vl_cheat_layout->itemAt(i)->widget(); w)
            w->setVisible(checked);
}

void Tab_Stats::all_item_cheat(QCheckBox* cb, Inventory::LegacyGroup group)
{
    if (isBlockSignal || !isEnabled() || !cb->isChecked() ||
        id < 0 || id >= JsonParser::characters().size()) return;

    QString error;
    qsizetype added = 0;
    const bool success = JsonParser::mutateCharacterChecked(id,
        [&](SaveCharacter &slot, QString &mutationError) {
            return Inventory::addLegacyMissing(slot, group, added, mutationError);
        }, error);
    update();
    if (!success) {
        QMessageBox::warning(this, tr("Save not changed"), error);
    } else if (added == 0) {
        const QSignalBlocker blocker(cb);
        cb->setChecked(true);
        QMessageBox::information(this, QObject::tr("Ты пытался..."),
                                 QObject::tr("У вас уже есть эти предметы."));
    } else {
        emit legacyInventoryChanged();
    }
}

void Tab_Stats::on_cb_restore_quests_toggled(bool checked)
{
    if (!checked || isBlockSignal || !isEnabled() || id < 0 || id >= JsonParser::characters().size()) return;
    if (!applyMutation([](SaveCharacter &slot) {
        for (const auto *key : {"questStage_BrendenMerkle", "questStage_ChainedHero",
                "questStage_Heir", "questStage_Hoover", "questStage_Innocent",
                "questStage_Maiden", "questStage_Nameless", "questStage_Sando",
                "questStage_Sco", "questStage_Shopkeeper", "questStage_TheSmith"})
            slot.quests[QLatin1String(key)] = 10;
    })) {
        const QSignalBlocker blocker(ui->cb_restore_quests);
        ui->cb_restore_quests->setChecked(false);
    }
}
