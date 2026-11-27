#ifndef TAB_INVENTORY_H
#define TAB_INVENTORY_H

#include "inventorymodel.h"
#include <QWidget>

namespace Ui { class Tab_Inventory; }

// Catalog grants only. Per-instance browser/property edits belong to DIG-127.
class Tab_Inventory : public QWidget
{
    Q_OBJECT
public:
    explicit Tab_Inventory(QWidget *parent = nullptr);
    ~Tab_Inventory();
    void initSettings(class Tab_Settings *settings);
    // Also used by the file chooser and Steam discovery; no save writes.
    bool loadCatalog(const ItemCatalog::Source &source, const QString &reviewPath = {});
    void addAllArmor();
    void addAllWeapons();

signals:
    void inventoryChanged();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void refresh();
    void renderRows();
    void updateSelection();
    void retranslate();
    void chooseCatalog();
    void discoverCatalog();
    void chooseReview();
    void addSelected();
    void grant(const QList<ItemCatalog::Category> &categories, const QString &guid = {});
    QString selectedGuid() const;
    QString unavailableReason() const;
    QString characterLabel() const;
    int owned(const ItemCatalog::Entry &entry) const;
    void reject(const QString &error);

    Ui::Tab_Inventory *ui;
    ItemCatalog::Source source;
    QString reviewPath;
    ItemCatalog::Catalog catalog;
    QString catalogError;
    bool catalogLoaded = false;
    QList<Inventory::Instance> instances;
    QString inventoryError;
    int slotIndex = -1;
    quint64 revision = 0; // invalidate a preview on character/restore or grant configuration change
};
#endif
