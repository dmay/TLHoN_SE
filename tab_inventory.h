#ifndef TAB_INVENTORY_H
#define TAB_INVENTORY_H

#include "inventoryproperties.h"
#include <QVariantMap>
#include <QWidget>

namespace Ui { class Tab_Inventory; }

// Catalog grants and owned-instance browser share the protected inventory facade.
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
    void refresh();
    const ItemCatalog::Catalog &equipmentCatalog() const { return catalog; }
    bool verifyEquipmentCatalog(ItemCatalog::Catalog &verified, QString &error) const;

signals:
    void inventoryChanged();
    void catalogChanged();

protected:
    void showEvent(QShowEvent *event) override;

private:
    void renderRows();
    void renderOwnedRows();
    void refreshProperties();
    void renderProperties(const QVariantMap &pending = {});
    void updatePropertyPreview();
    void selectOwned();
    void applySelectedProperties();
    void cancelSelectedProperties();
    bool confirmDiscardDraft();
    bool hasDraft() const;
    QVariantMap originalState() const;
    QString displayName(const Inventory::Instance &item) const;
    QString propertyEditBlocker() const;
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
    Inventory::PropertySelection propertySelection;
    Inventory::PropertyOptions propertyOptions;
    int slotIndex = -1;
    quint64 revision = 0; // invalidate a preview on character/restore or grant configuration change
};
#endif
