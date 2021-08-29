/*
    SPDX-FileCopyrightText: 2020 Ismael Asensio <isma.af@gmail.com>

    SPDX-License-Identifier: GPL-2.0-only OR GPL-3.0-only OR LicenseRef-KDE-Accepted-GPL
*/

#pragma once

#include "rulebookmodel.h"
#include "rulesmodel.h"

#include <KQuickAddons/ConfigModule>


namespace KWin
{
class RuleSettings;

class KWinRulesDBusAdaptor : public QDBusAbstractAdaptor
{
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.kde.KWin.KCMKWinRules")

public:
    explicit KWinRulesDBusAdaptor(QObject *parent)
        : QDBusAbstractAdaptor(parent){};

signals:
    void argumentsUpdated(const QStringList &arguments);

public slots:
    void updateArguments(const QStringList &arguments);
};

class KCMKWinRules : public KQuickAddons::ConfigModule
{
    Q_OBJECT

    Q_PROPERTY(RuleBookModel *ruleBookModel MEMBER m_ruleBookModel CONSTANT)
    Q_PROPERTY(RulesModel *rulesModel MEMBER m_rulesModel CONSTANT)
    Q_PROPERTY(int editIndex READ editIndex NOTIFY editIndexChanged)

public:
    explicit KCMKWinRules(QObject *parent, const QVariantList &arguments);

    Q_INVOKABLE void setRuleDescription(int index, const QString &description);
    Q_INVOKABLE void editRule(int index);

    Q_INVOKABLE void createRule();
    Q_INVOKABLE void removeRule(int index);
    Q_INVOKABLE void moveRule(int sourceIndex, int destIndex);
    Q_INVOKABLE void duplicateRule(int index);

    Q_INVOKABLE void exportToFile(const QUrl &path, const QList<int> &indexes);
    Q_INVOKABLE void importFromFile(const QUrl &path);

public Q_SLOTS:
    void load() override;
    void save() override;

Q_SIGNALS:
    void editIndexChanged();

private Q_SLOTS:
    void updateNeedsSave();
    void parseArguments(const QStringList &args);

private:
    int editIndex() const;
    void createRuleFromProperties();

    QModelIndex findRuleWithProperties(const QVariantMap &info, bool wholeApp) const;
    void fillSettingsFromProperties(RuleSettings *settings, const QVariantMap &info, bool wholeApp) const;

private:
    RuleBookModel *m_ruleBookModel;
    RulesModel* m_rulesModel;

    QPersistentModelIndex m_editIndex;

    KWinRulesDBusAdaptor *m_dbusAdaptor;
    bool m_alreadyLoaded = false;
    QVariantMap m_winProperties;
    bool m_wholeApp = false;
};

} // namespace
