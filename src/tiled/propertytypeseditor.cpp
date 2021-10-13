/*
 * propertytypeseditor.cpp
 * Copyright 2016-2021, Thorbjørn Lindeijer <bjorn@lindeijer.nl>>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "propertytypeseditor.h"
#include "ui_propertytypeseditor.h"

#include "custompropertieshelper.h"
#include "object.h"
#include "preferences.h"
#include "project.h"
#include "projectmanager.h"
#include "propertytypesmodel.h"
#include "utils.h"
#include "varianteditorfactory.h"
#include "variantpropertymanager.h"

#include <QCloseEvent>
#include <QScopedValueRollback>
#include <QStackedLayout>
#include <QStringListModel>
#include <QStyledItemDelegate>
#include <QToolBar>

#include <QtTreePropertyBrowser>

#include "qtcompat_p.h"

namespace Tiled {

PropertyTypesEditor::PropertyTypesEditor(QWidget *parent)
    : QDialog(parent)
    , mUi(new Ui::PropertyTypesEditor)
    , mPropertyTypesModel(new PropertyTypesModel(this))
    , mValuesView(new QTreeView)
    , mValuesModel(new QStringListModel(this))
    , mMembersView(new QtTreePropertyBrowser)
    , mPropertiesHelper(new CustomPropertiesHelper(mMembersView, this))
    , mValuesAndMembersStack(new QStackedLayout)
    , mValuesOrMembersLabel(new QLabel(tr("Values")))
{
    mUi->setupUi(this);

    mValuesView->setRootIsDecorated(false);
    mValuesView->setUniformRowHeights(true);
    mValuesView->setHeaderHidden(true);

    mValuesAndMembersStack->addWidget(mValuesView);
    mValuesAndMembersStack->addWidget(mMembersView);

    auto valuesAndMembersWithToolBarLayout = new QVBoxLayout;
    valuesAndMembersWithToolBarLayout->addItem(mValuesAndMembersStack);

    mUi->formLayout->addRow(mValuesOrMembersLabel, valuesAndMembersWithToolBarLayout);

    resize(Utils::dpiScaled(size()));

    mUi->propertyTypesView->setModel(mPropertyTypesModel);

    mValuesView->setModel(mValuesModel);
    mValuesView->setSelectionMode(QAbstractItemView::ExtendedSelection);

    mAddEnumPropertyTypeAction = new QAction(this);
    mAddClassPropertyTypeAction = new QAction(this);
    mRemovePropertyTypeAction = new QAction(this);
    mAddValueAction = new QAction(this);
    mRemoveValueAction = new QAction(this);

    mRemovePropertyTypeAction->setEnabled(false);
    mAddValueAction->setEnabled(false);
    mRemoveValueAction->setEnabled(false);
    mRemoveValueAction->setPriority(QAction::LowPriority);

    QIcon addIcon(QLatin1String(":/images/22/add.png"));
    QIcon removeIcon(QLatin1String(":/images/22/remove.png"));

    mAddEnumPropertyTypeAction->setIcon(addIcon);
    mAddClassPropertyTypeAction->setIcon(addIcon);
    mRemovePropertyTypeAction->setIcon(removeIcon);
    mRemovePropertyTypeAction->setPriority(QAction::LowPriority);
    mAddValueAction->setIcon(addIcon);
    mRemoveValueAction->setIcon(removeIcon);

    Utils::setThemeIcon(mAddEnumPropertyTypeAction, "add");
    Utils::setThemeIcon(mAddClassPropertyTypeAction, "add");
    Utils::setThemeIcon(mRemovePropertyTypeAction, "remove");
    Utils::setThemeIcon(mAddValueAction, "add");
    Utils::setThemeIcon(mRemoveValueAction, "remove");

    auto stretch = new QWidget;
    stretch->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Minimum);

    QToolBar *propertyTypesToolBar = new QToolBar(this);
    propertyTypesToolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    propertyTypesToolBar->setIconSize(Utils::smallIconSize());
    propertyTypesToolBar->addAction(mAddEnumPropertyTypeAction);
    propertyTypesToolBar->addAction(mAddClassPropertyTypeAction);
    propertyTypesToolBar->addAction(mRemovePropertyTypeAction);

    QToolBar *propertiesToolBar = new QToolBar(this);
    propertiesToolBar->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    propertiesToolBar->setIconSize(Utils::smallIconSize());
    propertiesToolBar->addAction(mAddValueAction);
    propertiesToolBar->addAction(mRemoveValueAction);

    mUi->propertyTypesLayout->addWidget(propertyTypesToolBar);
    valuesAndMembersWithToolBarLayout->addWidget(propertiesToolBar);

    connect(mUi->propertyTypesView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &PropertyTypesEditor::selectedPropertyTypesChanged);
    connect(mValuesView->selectionModel(), &QItemSelectionModel::selectionChanged,
            this, &PropertyTypesEditor::updateActions);
    connect(mPropertyTypesModel, &PropertyTypesModel::modelReset,
            this, &PropertyTypesEditor::selectFirstPropertyType);

    connect(mAddEnumPropertyTypeAction, &QAction::triggered,
            this, [this] { addPropertyType(PropertyType::PT_Enum); });
    connect(mAddClassPropertyTypeAction, &QAction::triggered,
            this, [this] { addPropertyType(PropertyType::PT_Class); });
    connect(mRemovePropertyTypeAction, &QAction::triggered,
            this, &PropertyTypesEditor::removeSelectedPropertyTypes);

    connect(mAddValueAction, &QAction::triggered,
                this, &PropertyTypesEditor::addValue);
    connect(mRemoveValueAction, &QAction::triggered,
            this, &PropertyTypesEditor::removeValues);

    connect(mPropertyTypesModel, &PropertyTypesModel::nameChanged,
            this, &PropertyTypesEditor::propertyTypeNameChanged);
    connect(mPropertyTypesModel, &QAbstractItemModel::dataChanged,
            this, &PropertyTypesEditor::applyPropertyTypes);
    connect(mPropertyTypesModel, &QAbstractItemModel::rowsInserted,
            this, &PropertyTypesEditor::applyPropertyTypes);
    connect(mPropertyTypesModel, &QAbstractItemModel::rowsRemoved,
            this, &PropertyTypesEditor::applyPropertyTypes);

    connect(mValuesModel, &QAbstractItemModel::dataChanged,
            this, &PropertyTypesEditor::valuesChanged);
    connect(mValuesModel, &QAbstractItemModel::rowsInserted,
            this, &PropertyTypesEditor::valuesChanged);
    connect(mValuesModel, &QAbstractItemModel::rowsRemoved,
            this, &PropertyTypesEditor::valuesChanged);
    connect(mUi->nameEdit, &QLineEdit::textEdited,
            this, &PropertyTypesEditor::nameChanged);

    Preferences *prefs = Preferences::instance();

    auto &project = ProjectManager::instance()->project();
    mPropertyTypesModel->setPropertyTypes(project.propertyTypes());

    connect(prefs, &Preferences::propertyTypesChanged,
            this, &PropertyTypesEditor::propertyTypesChanged);
    retranslateUi();
}

PropertyTypesEditor::~PropertyTypesEditor()
{
    delete mUi;
}

void PropertyTypesEditor::closeEvent(QCloseEvent *event)
{
    QWidget::closeEvent(event);
    if (event->isAccepted())
        emit closed();
}

void PropertyTypesEditor::changeEvent(QEvent *e)
{
    QWidget::changeEvent(e);
    switch (e->type()) {
    case QEvent::LanguageChange:
        mUi->retranslateUi(this);
        retranslateUi();
        break;
    default:
        break;
    }
}

void PropertyTypesEditor::retranslateUi()
{
    mAddEnumPropertyTypeAction->setText(tr("Add Enum"));
    mAddClassPropertyTypeAction->setText(tr("Add Class"));
    mRemovePropertyTypeAction->setText(tr("Remove Type"));

    mAddValueAction->setText(tr("Add Value"));
    mRemoveValueAction->setText(tr("Remove Value"));
}

void PropertyTypesEditor::addPropertyType(PropertyType::Type type)
{
    const QModelIndex newIndex = mPropertyTypesModel->addNewPropertyType(type);
    if (!newIndex.isValid())
        return;

    // Select and focus the new row and ensure it is visible
    QItemSelectionModel *sm = mUi->propertyTypesView->selectionModel();
    sm->select(newIndex,
               QItemSelectionModel::ClearAndSelect |
               QItemSelectionModel::Rows);
    sm->setCurrentIndex(newIndex, QItemSelectionModel::Current);
    mUi->propertyTypesView->edit(newIndex);
}

void PropertyTypesEditor::selectedPropertyTypesChanged()
{
    const QItemSelectionModel *sm = mUi->propertyTypesView->selectionModel();
    mRemovePropertyTypeAction->setEnabled(sm->hasSelection());
    updateValues();
}

void PropertyTypesEditor::removeSelectedPropertyTypes()
{
    // Cancel potential editor first, since letting it apply can cause
    // reordering of the types in setData, which would cause the wrong types to
    // get removed.
    mUi->propertyTypesView->closePersistentEditor(mUi->propertyTypesView->currentIndex());

    const QItemSelectionModel *sm = mUi->propertyTypesView->selectionModel();
    mPropertyTypesModel->removePropertyTypes(sm->selectedRows());
}

/**
 * Returns the index of the currently selected property type, or an invalid
 * index if no or multiple property types are selected.
 */
QModelIndex PropertyTypesEditor::selectedPropertyTypeIndex() const
{
    const auto selectionModel = mUi->propertyTypesView->selectionModel();
    const QModelIndexList selectedRows = selectionModel->selectedRows();
    return selectedRows.size() == 1 ? selectedRows.first() : QModelIndex();
}

void PropertyTypesEditor::propertyTypeNameChanged(const QModelIndex &index, const PropertyType &type)
{
    if (mSettingName)
        return;

    if (index == selectedPropertyTypeIndex())
        mUi->nameEdit->setText(type.name);
}

void PropertyTypesEditor::applyPropertyTypes()
{
    QScopedValueRollback<bool> settingPrefPropertyTypes(mSettingPrefPropertyTypes, true);
    emit Preferences::instance()->propertyTypesChanged();

    Project &project = ProjectManager::instance()->project();
    project.save();
}

void PropertyTypesEditor::propertyTypesChanged()
{
    // ignore signal if we caused it
    if (mSettingPrefPropertyTypes)
        return;

    auto &project = ProjectManager::instance()->project();
    mPropertyTypesModel->setPropertyTypes(project.propertyTypes());

    updateValues();
}

static QString nextValueText(const EnumPropertyType &propertyType)
{
    auto baseText = propertyType.name;
    if (!baseText.isEmpty())
        baseText.append(QLatin1Char(' '));

    // Search for a unique value, starting from the current count
    int number = propertyType.values.count();
    QString valueText;
    do {
        valueText = baseText + QString::number(number++);
    } while (propertyType.values.contains(valueText));

    return valueText;
}

void PropertyTypesEditor::addValue()
{
    const auto selectedTypeIndex = selectedPropertyTypeIndex();
    if (!selectedTypeIndex.isValid())
        return;

    const PropertyType *propertyType = mPropertyTypesModel->propertyTypeAt(selectedTypeIndex);
    if (!propertyType || propertyType->type != PropertyType::PT_Enum)
        return;

    const int row = mValuesModel->rowCount();
    if (!mValuesModel->insertRow(row))
        return;

    const QString valueText = nextValueText(*static_cast<const EnumPropertyType*>(propertyType));

    const auto valueIndex = mValuesModel->index(row);
    mValuesView->setCurrentIndex(valueIndex);
    mValuesModel->setData(valueIndex, valueText, Qt::DisplayRole);
    mValuesView->edit(valueIndex);
}

void PropertyTypesEditor::removeValues()
{
    const QItemSelection selection = mValuesView->selectionModel()->selection();
    for (const QItemSelectionRange &range : selection)
        mValuesModel->removeRows(range.top(), range.height());
}

void PropertyTypesEditor::updateValues()
{
    const auto selectedTypeIndex = selectedPropertyTypeIndex();
    const PropertyType *propertyType = mPropertyTypesModel->propertyTypeAt(selectedTypeIndex);
    if (!propertyType)
        return;

    if (propertyType->type == PropertyType::PT_Enum) {
        const auto &enumType = *static_cast<const EnumPropertyType*>(propertyType);

        QScopedValueRollback<bool> touchingValues(mUpdatingValues, true);
        mValuesModel->setStringList(enumType.values);
    }

    mUi->nameEdit->setText(propertyType->name);
    mUi->nameEdit->setEnabled(selectedTypeIndex.isValid());

    updateActions();
}

void PropertyTypesEditor::updateActions()
{
    const auto selectedTypeIndex = selectedPropertyTypeIndex();
    const auto valuesSelectionModel = mValuesView->selectionModel();
    const auto selectedValues = valuesSelectionModel->selectedRows();

    mAddValueAction->setEnabled(selectedTypeIndex.isValid());
    mRemoveValueAction->setEnabled(!selectedValues.isEmpty());
}

void PropertyTypesEditor::selectFirstPropertyType()
{
    const QModelIndex firstIndex = mPropertyTypesModel->index(0, 0);
    if (firstIndex.isValid()) {
        mUi->propertyTypesView->selectionModel()->select(firstIndex,
                                                         QItemSelectionModel::ClearAndSelect |
                                                         QItemSelectionModel::Rows);
    } else {
        // make sure the properties view is empty
        updateValues();
    }
}

void PropertyTypesEditor::valuesChanged()
{
    if (mUpdatingValues)
        return;

    const auto index = selectedPropertyTypeIndex();
    if (!index.isValid())
        return;

    const QStringList newValues = mValuesModel->stringList();
    mPropertyTypesModel->setPropertyTypeValues(index.row(), newValues);

    applyPropertyTypes();
}

void PropertyTypesEditor::nameChanged(const QString &name)
{
    const auto index = selectedPropertyTypeIndex();
    if (!index.isValid())
        return;

    QScopedValueRollback<bool> settingName(mSettingName, true);
    mPropertyTypesModel->setPropertyTypeName(index.row(), name);
}

} // namespace Tiled

#include "moc_propertytypeseditor.cpp"
