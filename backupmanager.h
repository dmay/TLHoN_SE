#ifndef BACKUPMANAGER_H
#define BACKUPMANAGER_H

#include "backupservice.h"
#include <QDialog>

class Tab_Settings;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QTableWidget;

struct BackupCatalogEntry
{
    BackupSnapshot snapshot;
    bool restorable = false;
    QString diagnostic;
};

// UI boundary: tests can supply deterministic catalog and operation results,
// independently of the filesystem, process enumeration and modal restore flow.
class BackupManagerBackend
{
public:
    virtual ~BackupManagerBackend() = default;
    virtual QString storageDirectory() const = 0;
    virtual QString saveDirectory() const = 0;
    virtual bool list(QList<BackupCatalogEntry> &entries, QString &error) = 0;
    virtual bool activeValid(QString &error) = 0;
    virtual bool gameClosed(QString &error) = 0;
    virtual bool pendingRecovery() const = 0;
    virtual bool legacyAvailable() const = 0;
    virtual bool create(const QString &label, bool legacy, BackupSnapshot &snapshot, QString &error) = 0;
    virtual bool rename(const QString &id, const QString &label, QString &error) = 0;
    virtual bool remove(const QString &id, QString &error) = 0;
    virtual bool chooseStorage(const QString &directory, QString &error) = 0;
    virtual QString restore(const QString &id, QWidget *parent) = 0;
    virtual QString reload() = 0;
};

class BackupManager : public QDialog
{
    Q_OBJECT
public:
    explicit BackupManager(BackupManagerBackend &backend, QWidget *parent = nullptr);
    static void open(Tab_Settings *settings);

private:
    void refresh(const QString &preferred = {});
    void updateActions();
    void showSelection();
    void capture(bool legacy);
    void renameSelected();
    void deleteSelected();
    void restoreSelected();
    void chooseFolder();
    void openFolder(const QString &path);
    void feedback(const QString &message);
    bool allowMutation(bool requiresActive);
    const BackupCatalogEntry *selected() const;
    BackupManagerBackend &backend;
    QList<BackupCatalogEntry> entries;
    bool catalogReady = false;
    bool activeReady = false;
    QString activeError;
    QTableWidget *table;
    QPlainTextEdit *details;
    QLabel *folders;
    QLabel *availability;
    QLabel *status;
    QPushButton *createButton;
    QPushButton *legacyButton;
    QPushButton *renameButton;
    QPushButton *deleteButton;
    QPushButton *restoreButton;
};

#endif
