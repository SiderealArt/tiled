/*
 * propertybrowser.cpp
 * Copyright 2013-2021, Thorbjørn Lindeijer <thorbjorn@lindeijer.nl>
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

#include "propertybrowser.h"

#include "changeimagelayerproperties.h"
#include "changelayer.h"
#include "changemapobject.h"
#include "changemapproperty.h"
#include "changeobjectgroupproperties.h"
#include "changeproperties.h"
#include "changetile.h"
#include "changetileimagesource.h"
#include "changetileprobability.h"
#include "changewangcolordata.h"
#include "changewangsetdata.h"
#include "documentmanager.h"
#include "flipmapobjects.h"
#include "grouplayer.h"
#include "imagelayer.h"
#include "map.h"
#include "mapdocument.h"
#include "mapobject.h"
#include "movemapobject.h"
#include "objectgroup.h"
#include "objecttemplate.h"
#include "preferences.h"
#include "properties.h"
#include "replacetileset.h"
#include "resizemapobject.h"
#include "rotatemapobject.h"
#include "tile.h"
#include "tilelayer.h"
#include "tilesetchanges.h"
#include "tilesetdocument.h"
#include "tilesetformat.h"
#include "tilesetmanager.h"
#include "tilesetwangsetmodel.h"
#include "utils.h"
#include "varianteditorfactory.h"
#include "variantpropertymanager.h"
#include "wangcolormodel.h"
#include "wangoverlay.h"
#include "wangset.h"

#include <QtGroupPropertyManager>

#include <QCoreApplication>
#include <QDebug>
#include <QKeyEvent>
#include <QMessageBox>
#include <QScopedValueRollback>

#include <algorithm>

namespace Tiled {

namespace {

/**
 * A helper class instantiated while properties are updated.
 *
 * Makes sure the resize mode is set to Fixed during its lifetime. Used to work
 * around performance issues caused by the view continuously making sure its
 * name column is adjusted to the contents.
 *
 * Also restores the scroll position, which gets reset when properties are
 * removed and then added back.
 */
class UpdatingProperties
{
public:
    UpdatingProperties(QtTreePropertyBrowser *browser, bool &isUpdating, bool force = false)
        : mBrowser(browser)
        , mForced(force)
        , mWasUpdating(isUpdating)
        , mIsUpdating(isUpdating)
    {
        if (!isUpdating || force) {
            isUpdating = true;
            mPreviousResizeMode = browser->resizeMode();
            mPreviousScrollPosition = browser->scrollPosition();
            mBrowser->setResizeMode(QtTreePropertyBrowser::Fixed);
        }
    }

    ~UpdatingProperties()
    {
        if (!mWasUpdating || mForced) {
            mBrowser->setResizeMode(mPreviousResizeMode);
            mBrowser->setScrollPosition(mPreviousScrollPosition);
            mIsUpdating = mWasUpdating;
        }
    }

private:
    QtTreePropertyBrowser * const mBrowser;
    bool const mForced;
    bool const mWasUpdating;
    bool &mIsUpdating;
    QtTreePropertyBrowser::ResizeMode mPreviousResizeMode;
    int mPreviousScrollPosition;
};

} // anonymous namespace

PropertyBrowser::PropertyBrowser(QWidget *parent)
    : QtTreePropertyBrowser(parent)
    , mVariantManager(new VariantPropertyManager(this))
    , mGroupManager(new QtGroupPropertyManager(this))
    , mCustomPropertiesGroup(nullptr)
    , mCustomPropertiesHelper(mVariantManager)
{
    VariantEditorFactory *variantEditorFactory = new VariantEditorFactory(this);

    setFactoryForManager(mVariantManager, variantEditorFactory);
    setResizeMode(ResizeToContents);
    setRootIsDecorated(false);
    setPropertiesWithoutValueMarked(true);
    setAllowMultiSelection(true);

    retranslateUi();

    mWangSetIcons.insert(WangSet::Corner, wangSetIcon(WangSet::Corner));
    mWangSetIcons.insert(WangSet::Edge, wangSetIcon(WangSet::Edge));
    mWangSetIcons.insert(WangSet::Mixed, wangSetIcon(WangSet::Mixed));

    connect(mVariantManager, &QtVariantPropertyManager::valueChanged,
            this, &PropertyBrowser::valueChanged);

    connect(variantEditorFactory, &VariantEditorFactory::resetProperty,
            this, &PropertyBrowser::resetProperty);

    connect(Preferences::instance(), &Preferences::objectTypesChanged,
            this, &PropertyBrowser::objectTypesChanged);
}

/**
 * Sets the \a object for which to display the properties.
 */
void PropertyBrowser::setObject(Object *object)
{
    if (mObject == object)
        return;

    UpdatingProperties updatingProperties(this, mUpdating);
    removeProperties();
    mObject = object;
    addProperties();
}

/**
 * Sets the \a document, used for keeping track of changes and for
 * undo/redo support.
 */
void PropertyBrowser::setDocument(Document *document)
{
    MapDocument *mapDocument = qobject_cast<MapDocument*>(document);
    TilesetDocument *tilesetDocument = qobject_cast<TilesetDocument*>(document);

    if (mDocument == document)
        return;

    if (mDocument) {
        mDocument->disconnect(this);
        if (mTilesetDocument) {
            mTilesetDocument->wangSetModel()->disconnect(this);
        }
    }

    mDocument = document;
    mMapDocument = mapDocument;
    mTilesetDocument = tilesetDocument;
    mCustomPropertiesHelper.setMapDocument(mapDocument);

    if (mapDocument) {
        connect(mapDocument, &MapDocument::mapChanged,
                this, &PropertyBrowser::mapChanged);
        connect(mapDocument, &MapDocument::imageLayerChanged,
                this, &PropertyBrowser::imageLayerChanged);
        connect(mapDocument, &MapDocument::tileTypeChanged,
                this, &PropertyBrowser::tileTypeChanged);

        connect(mapDocument, &MapDocument::selectedObjectsChanged,
                this, &PropertyBrowser::selectedObjectsChanged);
        connect(mapDocument, &MapDocument::selectedLayersChanged,
                this, &PropertyBrowser::selectedLayersChanged);
    }

    if (tilesetDocument) {
        connect(tilesetDocument, &TilesetDocument::tilesetNameChanged,
                this, &PropertyBrowser::tilesetChanged);
        connect(tilesetDocument, &TilesetDocument::tilesetTileOffsetChanged,
                this, &PropertyBrowser::tilesetChanged);
        connect(tilesetDocument, &TilesetDocument::tilesetObjectAlignmentChanged,
                this, &PropertyBrowser::tilesetChanged);
        connect(tilesetDocument, &TilesetDocument::tilesetChanged,
                this, &PropertyBrowser::tilesetChanged);

        connect(tilesetDocument, &TilesetDocument::tileProbabilityChanged,
                this, &PropertyBrowser::tileChanged);
        connect(tilesetDocument, &TilesetDocument::tileImageSourceChanged,
                this, &PropertyBrowser::tileChanged);
        connect(tilesetDocument, &TilesetDocument::tileTypeChanged,
                this, &PropertyBrowser::tileTypeChanged);

        connect(tilesetDocument, &TilesetDocument::selectedTilesChanged,
                this, &PropertyBrowser::selectedTilesChanged);

        TilesetWangSetModel *wangSetModel = tilesetDocument->wangSetModel();
        connect(wangSetModel, &TilesetWangSetModel::wangSetChanged,
                this, &PropertyBrowser::wangSetChanged);
    }

    if (document) {
        connect(document, &Document::changed,
                this, &PropertyBrowser::documentChanged);

        // For custom properties:
        connect(document, &Document::propertyAdded,
                this, &PropertyBrowser::propertyAdded);
        connect(document, &Document::propertyRemoved,
                this, &PropertyBrowser::propertyRemoved);
        connect(document, &Document::propertyChanged,
                this, &PropertyBrowser::propertyChanged);
        connect(document, &Document::propertiesChanged,
                this, &PropertyBrowser::propertiesChanged);
    }
}

/**
 * Returns whether the given \a item displays a custom property.
 */
bool PropertyBrowser::isCustomPropertyItem(const QtBrowserItem *item) const
{
    return mCustomPropertiesHelper.hasProperty(item->property());
}

/**
 * Returns whether the given list of \a items are all custom properties.
 */
bool PropertyBrowser::allCustomPropertyItems(const QList<QtBrowserItem *> &items) const
{
    return std::all_of(items.begin(), items.end(), [this] (QtBrowserItem *item) {
        return isCustomPropertyItem(item);
    });
}

/**
 * Selects the custom property with the given \a name, if it exists.
 */
void PropertyBrowser::selectCustomProperty(const QString &name)
{
    QtVariantProperty *property = mCustomPropertiesHelper.property(name);
    if (!property)
        return;

    const QList<QtBrowserItem*> propertyItems = items(property);
    if (!propertyItems.isEmpty())
        setCurrentItem(propertyItems.first());
}

/**
 * Makes the custom property with the \a name the currently edited one,
 * if it exists.
 */
void PropertyBrowser::editCustomProperty(const QString &name)
{
    QtVariantProperty *property = mCustomPropertiesHelper.property(name);
    if (!property)
        return;

    const QList<QtBrowserItem*> propertyItems = items(property);
    if (!propertyItems.isEmpty())
        editItem(propertyItems.first());
}

QSize PropertyBrowser::sizeHint() const
{
    return Utils::dpiScaled(QSize(260, 100));
}

bool PropertyBrowser::event(QEvent *event)
{
    if (event->type() == QEvent::LanguageChange)
        retranslateUi();

    if (event->type() == QEvent::ShortcutOverride) {
        if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Tab) {
            if (editedItem()) {
                event->accept();
                return true;
            }
        }
    }

    return QtTreePropertyBrowser::event(event);
}

void PropertyBrowser::documentChanged(const ChangeEvent &change)
{
    switch (change.type) {
    case ChangeEvent::LayerChanged:
    case ChangeEvent::TileLayerChanged:
        if (mObject == static_cast<const LayerChangeEvent&>(change).layer)
            updateProperties();
        break;
    case ChangeEvent::MapObjectsChanged:
        objectsChanged(static_cast<const MapObjectsChangeEvent&>(change));
        break;
    case ChangeEvent::ObjectGroupChanged:
        if (mObject == static_cast<const ObjectGroupChangeEvent&>(change).objectGroup)
            updateProperties();
        break;
    case ChangeEvent::WangSetChanged:
        if (mObject == static_cast<const WangSetChangeEvent&>(change).wangSet)
            updateProperties();
        break;
    default:
        break;
    }
}

void PropertyBrowser::mapChanged()
{
    if (mObject == mMapDocument->map())
        updateProperties();
}

void PropertyBrowser::objectsChanged(const MapObjectsChangeEvent &mapObjectsChange)
{
    if (!mObject || mObject->typeId() != Object::MapObjectType)
        return;
    if (!mapObjectsChange.mapObjects.contains(static_cast<MapObject*>(mObject)))
        return;

    updateProperties();

    if (mapObjectsChange.properties & (MapObject::CustomProperties | MapObject::TypeProperty))
        updateCustomProperties();
}

void PropertyBrowser::imageLayerChanged(ImageLayer *imageLayer)
{
    if (mObject == imageLayer)
        updateProperties();
}

void PropertyBrowser::tilesetChanged(Tileset *tileset)
{
    if (mObject == tileset) {
        updateProperties();
        updateCustomProperties();   // Tileset may have been swapped
    }
}

void PropertyBrowser::tileChanged(Tile *tile)
{
    if (mObject == tile)
        updateProperties();
}

void PropertyBrowser::tileTypeChanged(Tile *tile)
{
    if (mObject == tile) {
        updateProperties();
        updateCustomProperties();
    } else if (mObject && mObject->typeId() == Object::MapObjectType) {
        auto mapObject = static_cast<MapObject*>(mObject);
        if (mapObject->cell().tile() == tile && mapObject->type().isEmpty())
            updateProperties();
    }
}

void PropertyBrowser::wangSetChanged(Tileset *tileset, int index)
{
    if (mObject == tileset->wangSet(index))
        updateProperties();
}

static bool anyObjectHasProperty(const QList<Object*> &objects, const QString &name)
{
    for (Object *obj : objects) {
        if (obj->hasProperty(name))
            return true;
    }
    return false;
}

static bool propertyValueAffected(Object *currentObject,
                                  Object *changedObject,
                                  const QString &propertyName)
{
    if (currentObject == changedObject)
        return true;

    // Changed property may be inherited
    if (currentObject && currentObject->typeId() == Object::MapObjectType && changedObject->typeId() == Object::TileType) {
        auto tile = static_cast<MapObject*>(currentObject)->cell().tile();
        if (tile == changedObject && !currentObject->hasProperty(propertyName))
            return true;
    }

    return false;
}

static bool objectPropertiesRelevant(Document *document, Object *object)
{
    auto currentObject = document->currentObject();
    if (!currentObject)
        return false;

    if (currentObject == object)
        return true;

    if (currentObject->typeId() == Object::MapObjectType)
        if (static_cast<MapObject*>(currentObject)->cell().tile() == object)
            return true;

    if (document->currentObjects().contains(object))
        return true;

    return false;
}

void PropertyBrowser::propertyAdded(Object *object, const QString &name)
{
    if (!objectPropertiesRelevant(mDocument, object))
        return;
    if (QtVariantProperty *property = mCustomPropertiesHelper.property(name)) {
        if (propertyValueAffected(mObject, object, name))
            setCustomPropertyValue(property, object->property(name));
    } else {
        const QVariant value = mObject->resolvedProperty(name);
        addCustomProperty(name, value);
    }
    updateCustomPropertyColor(name);
}

void PropertyBrowser::propertyRemoved(Object *object, const QString &name)
{
    auto property = mCustomPropertiesHelper.property(name);
    if (!property)
        return;
    if (!objectPropertiesRelevant(mDocument, object))
        return;

    const QVariant resolvedValue = mObject->resolvedProperty(name);

    if (!resolvedValue.isValid() &&
            !anyObjectHasProperty(mDocument->currentObjects(), name)) {
        // It's not a predefined property and no selected object has this
        // property, so delete it.

        // First move up or down the currently selected item
        QtBrowserItem *item = currentItem();
        if (item && item->property() == property) {
            const QList<QtBrowserItem *> siblings = item->parent()->children();
            if (siblings.count() > 1) {
                int currentItemIndex = siblings.indexOf(item);
                if (item == siblings.last()) {
                    setCurrentItem(siblings.at(currentItemIndex - 1));
                } else {
                    setCurrentItem(siblings.at(currentItemIndex + 1));
                }
            }
        }

        deleteCustomProperty(property);
        return;
    }

    if (propertyValueAffected(mObject, object, name)) {
        // Property deleted from the current object, so reset the value.
        setCustomPropertyValue(property, resolvedValue);
    }

    updateCustomPropertyColor(name);
}

void PropertyBrowser::propertyChanged(Object *object, const QString &name)
{
    auto property = mCustomPropertiesHelper.property(name);
    if (!property)
        return;

    if (propertyValueAffected(mObject, object, name))
        setCustomPropertyValue(property, object->property(name));

    if (mDocument->currentObjects().contains(object))
        updateCustomPropertyColor(name);
}

void PropertyBrowser::propertiesChanged(Object *object)
{
    if (objectPropertiesRelevant(mDocument, object))
        updateCustomProperties();
}

void PropertyBrowser::selectedObjectsChanged()
{
    updateCustomProperties();
}

void PropertyBrowser::selectedLayersChanged()
{
    updateCustomProperties();
}

void PropertyBrowser::selectedTilesChanged()
{
    updateCustomProperties();
}

void PropertyBrowser::objectTypesChanged()
{
    if (mObject && mObject->typeId() == Object::MapObjectType)
        updateCustomProperties();
}

void PropertyBrowser::valueChanged(QtProperty *property, const QVariant &val)
{
    if (mUpdating)
        return;
    if (!mObject || !mDocument)
        return;

    if (mCustomPropertiesHelper.hasProperty(property)) {
        QUndoStack *undoStack = mDocument->undoStack();
        undoStack->push(new SetProperty(mDocument,
                                        mDocument->currentObjects(),
                                        property->propertyName(),
                                        fromDisplayValue(property, val)));
        return;
    }

    if (!mPropertyToId.contains(property))
        return;

    const PropertyId id = mPropertyToId.value(property);

    switch (mObject->typeId()) {
    case Object::MapType:               applyMapValue(id, val); break;
    case Object::MapObjectType:         applyMapObjectValue(id, val); break;
    case Object::LayerType:             applyLayerValue(id, val); break;
    case Object::TilesetType:           applyTilesetValue(id, val); break;
    case Object::TileType:              applyTileValue(id, val); break;
    case Object::WangSetType:           applyWangSetValue(id, val); break;
    case Object::WangColorType:         applyWangColorValue(id, val); break;
    case Object::ObjectTemplateType:    break;
    }
}

void PropertyBrowser::resetProperty(QtProperty *property)
{
    auto typeId = mVariantManager->propertyType(property);
    if (typeId == QMetaType::QColor)
        mVariantManager->setValue(property, QColor());
    else if (typeId == VariantPropertyManager::displayObjectRefTypeId()) {
        mVariantManager->setValue(property, toDisplayValue(QVariant::fromValue(ObjectRef())));
    } else
        qWarning() << "Resetting of property type not supported right now";
}

void PropertyBrowser::addMapProperties()
{
    QtProperty *groupProperty = mGroupManager->addProperty(tr("Map"));

    QtVariantProperty *orientationProperty =
            addProperty(OrientationProperty,
                        QtVariantPropertyManager::enumTypeId(),
                        tr("Orientation"),
                        groupProperty);

    orientationProperty->setAttribute(QLatin1String("enumNames"), mOrientationNames);

    addProperty(WidthProperty, QMetaType::Int, tr("Width"), groupProperty)->setEnabled(false);
    addProperty(HeightProperty, QMetaType::Int, tr("Height"), groupProperty)->setEnabled(false);
    auto tileWidthProperty = addProperty(TileWidthProperty, QMetaType::Int, tr("Tile Width"), groupProperty);
    auto tileHeightProperty = addProperty(TileHeightProperty, QMetaType::Int, tr("Tile Height"), groupProperty);
    addProperty(InfiniteProperty, QMetaType::Bool, tr("Infinite"), groupProperty);

    tileWidthProperty->setAttribute(QStringLiteral("minimum"), 1);
    tileHeightProperty->setAttribute(QStringLiteral("minimum"), 1);

    addProperty(HexSideLengthProperty, QMetaType::Int, tr("Tile Side Length (Hex)"), groupProperty);

    QtVariantProperty *staggerAxisProperty =
            addProperty(StaggerAxisProperty,
                        QtVariantPropertyManager::enumTypeId(),
                        tr("Stagger Axis"),
                        groupProperty);

    staggerAxisProperty->setAttribute(QLatin1String("enumNames"), mStaggerAxisNames);

    QtVariantProperty *staggerIndexProperty =
            addProperty(StaggerIndexProperty,
                        QtVariantPropertyManager::enumTypeId(),
                        tr("Stagger Index"),
                        groupProperty);

    staggerIndexProperty->setAttribute(QLatin1String("enumNames"), mStaggerIndexNames);

    QtVariantProperty *layerFormatProperty =
            addProperty(LayerFormatProperty,
                        QtVariantPropertyManager::enumTypeId(),
                        tr("Tile Layer Format"),
                        groupProperty);

    layerFormatProperty->setAttribute(QLatin1String("enumNames"), mLayerFormatNames);

    QtVariantProperty *chunkWidthProperty = addProperty(ChunkWidthProperty, QMetaType::Int, tr("Output Chunk Width"), groupProperty);
    QtVariantProperty *chunkHeightProperty = addProperty(ChunkHeightProperty, QMetaType::Int, tr("Output Chunk Height"), groupProperty);

    chunkWidthProperty->setAttribute(QLatin1String("minimum"), CHUNK_SIZE_MIN);
    chunkHeightProperty->setAttribute(QLatin1String("minimum"), CHUNK_SIZE_MIN);

    QtVariantProperty *renderOrderProperty =
            addProperty(RenderOrderProperty,
                        QtVariantPropertyManager::enumTypeId(),
                        tr("Tile Render Order"),
                        groupProperty);

    addProperty(CompressionLevelProperty, QMetaType::Int, tr("Compression Level"), groupProperty);

    renderOrderProperty->setAttribute(QLatin1String("enumNames"), mRenderOrderNames);

    addProperty(BackgroundColorProperty, QMetaType::QColor, tr("Background Color"), groupProperty);
    addProperty(groupProperty);
}

static QStringList objectTypeNames()
{
    QStringList names;
    for (const ObjectType &type : Object::objectTypes())
        names.append(type.name);
    return names;
}

enum MapObjectFlags {
    ObjectHasDimensions = 0x1,
    ObjectHasTile = 0x2,
    ObjectIsText = 0x4
};

static int mapObjectFlags(const MapObject *mapObject)
{
    int flags = 0;
    if (mapObject->hasDimensions())
        flags |= ObjectHasDimensions;
    if (!mapObject->cell().isEmpty())
        flags |= ObjectHasTile;
    if (mapObject->shape() == MapObject::Text)
        flags |= ObjectIsText;
    return flags;
}

void PropertyBrowser::addMapObjectProperties()
{
    QtProperty *groupProperty = mGroupManager->addProperty(tr("Object"));

    addProperty(IdProperty, QMetaType::Int, tr("ID"), groupProperty)->setEnabled(false);
    addProperty(TemplateProperty, filePathTypeId(), tr("Template"), groupProperty)->setEnabled(false);
    addProperty(NameProperty, QMetaType::QString, tr("Name"), groupProperty);

    QtVariantProperty *typeProperty =
            addProperty(TypeProperty, QMetaType::QString, tr("Type"), groupProperty);
    typeProperty->setAttribute(QLatin1String("suggestions"), objectTypeNames());

    if (mMapDocument->allowHidingObjects())
        addProperty(VisibleProperty, QMetaType::Bool, tr("Visible"), groupProperty);

    addProperty(XProperty, QMetaType::Double, tr("X"), groupProperty);
    addProperty(YProperty, QMetaType::Double, tr("Y"), groupProperty);

    auto mapObject = static_cast<const MapObject*>(mObject);
    mMapObjectFlags = mapObjectFlags(mapObject);

    if (mMapObjectFlags & ObjectHasDimensions) {
        addProperty(WidthProperty, QMetaType::Double, tr("Width"), groupProperty);
        addProperty(HeightProperty, QMetaType::Double, tr("Height"), groupProperty);
    }

    bool isPoint = mapObject->shape() == MapObject::Point;
    addProperty(RotationProperty, QMetaType::Double, tr("Rotation"), groupProperty)->setEnabled(!isPoint);

    if (mMapObjectFlags & ObjectHasTile) {
        QtVariantProperty *flippingProperty =
                addProperty(FlippingProperty, VariantPropertyManager::flagTypeId(),
                               tr("Flipping"), groupProperty);

        flippingProperty->setAttribute(QLatin1String("flagNames"), mFlippingFlagNames);
    }

    if (mMapObjectFlags & ObjectIsText) {
        addProperty(TextProperty, QMetaType::QString, tr("Text"), groupProperty)->setAttribute(QLatin1String("multiline"), true);
        addProperty(TextAlignmentProperty, VariantPropertyManager::alignmentTypeId(), tr("Alignment"), groupProperty);
        addProperty(FontProperty, QMetaType::QFont, tr("Font"), groupProperty);
        addProperty(WordWrapProperty, QMetaType::Bool, tr("Word Wrap"), groupProperty);
        addProperty(ColorProperty, QMetaType::QColor, tr("Color"), groupProperty);
    }

    addProperty(groupProperty);
}

void PropertyBrowser::addLayerProperties(QtProperty *parent)
{
    addProperty(IdProperty, QMetaType::Int, tr("ID"), parent)->setEnabled(false);
    addProperty(NameProperty, QMetaType::QString, tr("Name"), parent);
    addProperty(VisibleProperty, QMetaType::Bool, tr("Visible"), parent);
    addProperty(LockedProperty, QMetaType::Bool, tr("Locked"), parent);

    QtVariantProperty *opacityProperty =
            addProperty(OpacityProperty, QMetaType::Double, tr("Opacity"), parent);
    opacityProperty->setAttribute(QLatin1String("minimum"), 0.0);
    opacityProperty->setAttribute(QLatin1String("maximum"), 1.0);
    opacityProperty->setAttribute(QLatin1String("singleStep"), 0.1);
    addProperty(TintColorProperty, QMetaType::QColor, tr("Tint Color"), parent);

    addProperty(OffsetXProperty, QMetaType::Double, tr("Horizontal Offset"), parent);
    addProperty(OffsetYProperty, QMetaType::Double, tr("Vertical Offset"), parent);

    addProperty(ParallaxFactorProperty, QMetaType::QPointF, tr("Parallax Factor"), parent);
}

void PropertyBrowser::addTileLayerProperties()
{
    QtProperty *groupProperty = mGroupManager->addProperty(tr("Tile Layer"));
    addLayerProperties(groupProperty);
    addProperty(groupProperty);
}

void PropertyBrowser::addObjectGroupProperties()
{
    QtProperty *groupProperty = mGroupManager->addProperty(tr("Object Layer"));
    addLayerProperties(groupProperty);

    addProperty(ColorProperty, QMetaType::QColor, tr("Color"), groupProperty);

    QtVariantProperty *drawOrderProperty =
            addProperty(DrawOrderProperty,
                        QtVariantPropertyManager::enumTypeId(),
                        tr("Drawing Order"),
                        groupProperty);

    drawOrderProperty->setAttribute(QLatin1String("enumNames"), mDrawOrderNames);

    addProperty(groupProperty);
}

void PropertyBrowser::addImageLayerProperties()
{
    QtProperty *groupProperty = mGroupManager->addProperty(tr("Image Layer"));
    addLayerProperties(groupProperty);

    QtVariantProperty *imageSourceProperty = addProperty(ImageSourceProperty,
                                                         filePathTypeId(),
                                                         tr("Image"), groupProperty);

    imageSourceProperty->setAttribute(QLatin1String("filter"),
                                      Utils::readableImageFormatsFilter());

    addProperty(ColorProperty, QMetaType::QColor, tr("Transparent Color"), groupProperty);

    addProperty(groupProperty);
}

void PropertyBrowser::addGroupLayerProperties()
{
    QtProperty *groupProperty = mGroupManager->addProperty(tr("Group Layer"));
    addLayerProperties(groupProperty);
    addProperty(groupProperty);
}

void PropertyBrowser::addTilesetProperties()
{
    const Tileset *tileset = static_cast<const Tileset*>(mObject);

    QtProperty *groupProperty = mGroupManager->addProperty(tr("Tileset"));

    if (mMapDocument) {
        auto property = addProperty(FileNameProperty, filePathTypeId(), tr("Filename"), groupProperty);

        QString filter = QCoreApplication::translate("MainWindow", "All Files (*)");
        FormatHelper<TilesetFormat> helper(FileFormat::Read, filter);

        property->setAttribute(QStringLiteral("filter"), helper.filter());
    }

    QtVariantProperty *nameProperty = addProperty(NameProperty, QMetaType::QString, tr("Name"), groupProperty);
    nameProperty->setEnabled(mTilesetDocument);

    QtVariantProperty *alignmentProperty =
            addProperty(ObjectAlignmentProperty,
                        QtVariantPropertyManager::enumTypeId(),
                        tr("Object Alignment"),
                        groupProperty);

    alignmentProperty->setAttribute(QLatin1String("enumNames"), mAlignmentNames);

    QtVariantProperty *tileOffsetProperty = addProperty(TileOffsetProperty, QMetaType::QPoint, tr("Drawing Offset"), groupProperty);
    tileOffsetProperty->setEnabled(mTilesetDocument);

    QtVariantProperty *backgroundProperty = addProperty(BackgroundColorProperty, QMetaType::QColor, tr("Background Color"), groupProperty);
    backgroundProperty->setEnabled(mTilesetDocument);

    QtVariantProperty *orientationProperty =
            addProperty(OrientationProperty,
                        QtVariantPropertyManager::enumTypeId(),
                        tr("Orientation"),
                        groupProperty);

    orientationProperty->setAttribute(QLatin1String("enumNames"), mTilesetOrientationNames);

    QtVariantProperty *gridWidthProperty = addProperty(GridWidthProperty, QMetaType::Int, tr("Grid Width"), groupProperty);
    gridWidthProperty->setEnabled(mTilesetDocument);
    gridWidthProperty->setAttribute(QLatin1String("minimum"), 1);
    QtVariantProperty *gridHeightProperty = addProperty(GridHeightProperty, QMetaType::Int, tr("Grid Height"), groupProperty);
    gridHeightProperty->setEnabled(mTilesetDocument);
    gridHeightProperty->setAttribute(QLatin1String("minimum"), 1);

    QtVariantProperty *columnsProperty = addProperty(ColumnCountProperty, QMetaType::Int, tr("Columns"), groupProperty);
    columnsProperty->setAttribute(QLatin1String("minimum"), 1);

    QtVariantProperty *transformationsGroupProperty = mVariantManager->addProperty(VariantPropertyManager::unstyledGroupTypeId(), tr("Allowed Transformations"));

    QtVariantProperty *flipHorizontallyProperty = addProperty(AllowFlipHorizontallyProperty, QMetaType::Bool, tr("Flip Horizontally"), transformationsGroupProperty);
    QtVariantProperty *flipVerticallyProperty = addProperty(AllowFlipVerticallyProperty, QMetaType::Bool, tr("Flip Vertically"), transformationsGroupProperty);
    QtVariantProperty *rotateProperty = addProperty(AllowRotateProperty, QMetaType::Bool, tr("Rotate"), transformationsGroupProperty);
    QtVariantProperty *randomProperty = addProperty(PreferUntransformedProperty, QMetaType::Bool, tr("Prefer Untransformed Tiles"), transformationsGroupProperty);
    flipHorizontallyProperty->setEnabled(mTilesetDocument);
    flipVerticallyProperty->setEnabled(mTilesetDocument);
    rotateProperty->setEnabled(mTilesetDocument);
    randomProperty->setEnabled(mTilesetDocument);

    groupProperty->addSubProperty(transformationsGroupProperty);

    // Next properties we should add only for non 'Collection of Images' tilesets
    if (!tileset->isCollection()) {
        QtVariantProperty *parametersProperty =
                addProperty(TilesetImageParametersProperty, VariantPropertyManager::tilesetParametersTypeId(), tr("Image"), groupProperty);

        parametersProperty->setEnabled(mTilesetDocument);

        QtVariantProperty *imageSourceProperty = addProperty(ImageSourceProperty, QMetaType::QString, tr("Source"), parametersProperty);
        QtVariantProperty *tileWidthProperty = addProperty(TileWidthProperty, QMetaType::Int, tr("Tile Width"), parametersProperty);
        QtVariantProperty *tileHeightProperty = addProperty(TileHeightProperty, QMetaType::Int, tr("Tile Height"), parametersProperty);
        QtVariantProperty *marginProperty = addProperty(MarginProperty, QMetaType::Int, tr("Margin"), parametersProperty);
        QtVariantProperty *spacingProperty = addProperty(SpacingProperty, QMetaType::Int, tr("Spacing"), parametersProperty);
        QtVariantProperty *colorProperty = addProperty(ColorProperty, QMetaType::QColor, tr("Transparent Color"), parametersProperty);

        // These properties can't be directly edited. To change the parameters,
        // the TilesetParametersEdit is used.
        imageSourceProperty->setEnabled(false);
        tileWidthProperty->setEnabled(false);
        tileHeightProperty->setEnabled(false);
        marginProperty->setEnabled(false);
        spacingProperty->setEnabled(false);
        colorProperty->setEnabled(false);
    }
    addProperty(groupProperty);
}

void PropertyBrowser::addTileProperties()
{
    QtProperty *groupProperty = mGroupManager->addProperty(tr("Tile"));
    addProperty(IdProperty, QMetaType::Int, tr("ID"), groupProperty)->setEnabled(false);

    QtVariantProperty *typeProperty =
            addProperty(TypeProperty, QMetaType::QString, tr("Type"), groupProperty);
    typeProperty->setAttribute(QLatin1String("suggestions"), objectTypeNames());
    typeProperty->setEnabled(mTilesetDocument);

    addProperty(WidthProperty, QMetaType::Int, tr("Width"), groupProperty)->setEnabled(false);
    addProperty(HeightProperty, QMetaType::Int, tr("Height"), groupProperty)->setEnabled(false);

    QtVariantProperty *probabilityProperty = addProperty(TileProbabilityProperty,
                                                         QMetaType::Double,
                                                         tr("Probability"),
                                                         groupProperty);
    probabilityProperty->setAttribute(QLatin1String("decimals"), 3);
    probabilityProperty->setToolTip(tr("Relative chance this tile will be picked"));
    probabilityProperty->setEnabled(mTilesetDocument);

    const Tile *tile = static_cast<const Tile*>(mObject);
    if (!tile->imageSource().isEmpty()) {
        QtVariantProperty *imageSourceProperty = addProperty(ImageSourceProperty,
                                                             filePathTypeId(),
                                                             tr("Image"), groupProperty);

        imageSourceProperty->setAttribute(QLatin1String("filter"),
                                          Utils::readableImageFormatsFilter());
        imageSourceProperty->setEnabled(mTilesetDocument);
    }

    addProperty(groupProperty);
}

void PropertyBrowser::addWangSetProperties()
{
    QtProperty *groupProperty = mGroupManager->addProperty(tr("Terrain Set"));
    QtVariantProperty *nameProperty = addProperty(NameProperty, QMetaType::QString, tr("Name"), groupProperty);
    QtVariantProperty *typeProperty = addProperty(WangSetTypeProperty,
                                                  QtVariantPropertyManager::enumTypeId(),
                                                  tr("Type"),
                                                  groupProperty);
    QtVariantProperty *colorCountProperty = addProperty(ColorCountProperty, QMetaType::Int, tr("Terrain Count"), groupProperty);

    typeProperty->setAttribute(QLatin1String("enumNames"), mWangSetTypeNames);
    typeProperty->setAttribute(QLatin1String("enumIcons"), QVariant::fromValue(mWangSetIcons));

    colorCountProperty->setAttribute(QLatin1String("minimum"), 0);
    colorCountProperty->setAttribute(QLatin1String("maximum"), WangId::MAX_COLOR_COUNT);

    nameProperty->setEnabled(mTilesetDocument);
    typeProperty->setEnabled(mTilesetDocument);
    colorCountProperty->setEnabled(mTilesetDocument);

    addProperty(groupProperty);
}

void PropertyBrowser::addWangColorProperties()
{
    QtProperty *groupProperty = mGroupManager->addProperty(tr("Terrain"));
    QtVariantProperty *nameProperty = addProperty(NameProperty,
                                                  QMetaType::QString,
                                                  tr("Name"),
                                                  groupProperty);
    QtVariantProperty *colorProperty = addProperty(ColorProperty,
                                                   QMetaType::QColor,
                                                   tr("Color"),
                                                   groupProperty);
    QtVariantProperty *probabilityProperty = addProperty(WangColorProbabilityProperty,
                                                         QMetaType::Double,
                                                         tr("Probability"),
                                                         groupProperty);

    probabilityProperty->setAttribute(QLatin1String("minimum"), 0.01);

    nameProperty->setEnabled(mTilesetDocument);
    colorProperty->setEnabled(mTilesetDocument);
    probabilityProperty->setEnabled(mTilesetDocument);

    addProperty(groupProperty);
}

void PropertyBrowser::applyMapValue(PropertyId id, const QVariant &val)
{
    QUndoCommand *command = nullptr;

    switch (id) {
    case TileWidthProperty:
        command = new ChangeMapProperty(mMapDocument, Map::TileWidthProperty,
                                        val.toInt());
        break;
    case TileHeightProperty:
        command = new ChangeMapProperty(mMapDocument, Map::TileHeightProperty,
                                        val.toInt());
        break;
    case InfiniteProperty: {
        bool infinite = val.toInt();

        QUndoStack *undoStack = mDocument->undoStack();
        undoStack->beginMacro(tr("Change Infinite Property"));

        if (!infinite) {
            QRect mapBounds(QPoint(0, 0), mMapDocument->map()->size());

            LayerIterator iterator(mMapDocument->map());
            while (Layer *layer = iterator.next()) {
                if (TileLayer *tileLayer = dynamic_cast<TileLayer*>(layer))
                    mapBounds = mapBounds.united(tileLayer->region().boundingRect());
            }

            if (mapBounds.size() == QSize(0, 0))
                mapBounds.setSize(QSize(1, 1));

            mMapDocument->resizeMap(mapBounds.size(), -mapBounds.topLeft(), false);
        }

        undoStack->push(new ChangeMapProperty(mMapDocument, Map::InfiniteProperty,
                                              val.toInt()));
        undoStack->endMacro();
        break;
    }
    case OrientationProperty: {
        Map::Orientation orientation = static_cast<Map::Orientation>(val.toInt() + 1);
        command = new ChangeMapProperty(mMapDocument, orientation);
        break;
    }
    case HexSideLengthProperty: {
        command = new ChangeMapProperty(mMapDocument, Map::HexSideLengthProperty,
                                        val.toInt());
        break;
    }
    case StaggerAxisProperty: {
        Map::StaggerAxis staggerAxis = static_cast<Map::StaggerAxis>(val.toInt());
        command = new ChangeMapProperty(mMapDocument, staggerAxis);
        break;
    }
    case StaggerIndexProperty: {
        Map::StaggerIndex staggerIndex = static_cast<Map::StaggerIndex>(val.toInt());
        command = new ChangeMapProperty(mMapDocument, staggerIndex);
        break;
    }
    case LayerFormatProperty: {
        Map::LayerDataFormat format = mLayerFormatValues.at(val.toInt());
        command = new ChangeMapProperty(mMapDocument, format);
        break;
    }
    case RenderOrderProperty: {
        Map::RenderOrder renderOrder = static_cast<Map::RenderOrder>(val.toInt());
        command = new ChangeMapProperty(mMapDocument, renderOrder);
        break;
    }
    case BackgroundColorProperty:
        command = new ChangeMapProperty(mMapDocument, val.value<QColor>());
        break;
    case CompressionLevelProperty:
        command = new ChangeMapProperty(mMapDocument, Map::CompressionLevelProperty, val.toInt());
        break;
    case ChunkWidthProperty: {
        QSize chunkSize = mMapDocument->map()->chunkSize();
        chunkSize.setWidth(val.toInt());
        command = new ChangeMapProperty(mMapDocument, chunkSize);
        break;
    }
    case ChunkHeightProperty: {
        QSize chunkSize = mMapDocument->map()->chunkSize();
        chunkSize.setHeight(val.toInt());
        command = new ChangeMapProperty(mMapDocument, chunkSize);
        break;
    }
    default:
        break;
    }

    if (command)
        mDocument->undoStack()->push(command);
}

QUndoCommand *PropertyBrowser::applyMapObjectValueTo(PropertyId id, const QVariant &val, MapObject *mapObject)
{
    QUndoCommand *command = nullptr;

    switch (id) {
    default: {
        MapObject::Property property;

        switch (id) {
        case NameProperty:          property = MapObject::NameProperty; break;
        case TypeProperty:          property = MapObject::TypeProperty; break;
        case VisibleProperty:       property = MapObject::VisibleProperty; break;
        case TextProperty:          property = MapObject::TextProperty; break;
        case FontProperty:          property = MapObject::TextFontProperty; break;
        case TextAlignmentProperty: property = MapObject::TextAlignmentProperty; break;
        case WordWrapProperty:      property = MapObject::TextWordWrapProperty; break;
        case ColorProperty:         property = MapObject::TextColorProperty; break;
        default:
            return nullptr; // unrecognized property
        }

        command = new ChangeMapObject(mDocument, mapObject, property, val);
        break;
    }
    case XProperty: {
        const QPointF oldPos = mapObject->position();
        const QPointF newPos(val.toReal(), oldPos.y());
        command = new MoveMapObject(mDocument, mapObject, newPos, oldPos);
        break;
    }
    case YProperty: {
        const QPointF oldPos = mapObject->position();
        const QPointF newPos(oldPos.x(), val.toReal());
        command = new MoveMapObject(mDocument, mapObject, newPos, oldPos);
        break;
    }
    case WidthProperty: {
        const QSizeF oldSize = mapObject->size();
        const QSizeF newSize(val.toReal(), oldSize.height());
        command = new ResizeMapObject(mDocument, mapObject, newSize, oldSize);
        break;
    }
    case HeightProperty: {
        const QSizeF oldSize = mapObject->size();
        const QSizeF newSize(oldSize.width(), val.toReal());
        command = new ResizeMapObject(mDocument, mapObject, newSize, oldSize);
        break;
    }
    case RotationProperty:
        if (mapObject->canRotate()) {
            const qreal newRotation = val.toDouble();
            const qreal oldRotation = mapObject->rotation();
            command = new RotateMapObject(mDocument, mapObject, newRotation, oldRotation);
        }
        break;
    case FlippingProperty: {
        const int flippingFlags = val.toInt();

        MapObjectCell mapObjectCell;
        mapObjectCell.object = mapObject;
        mapObjectCell.cell = mapObject->cell();
        mapObjectCell.cell.setFlippedHorizontally(flippingFlags & 1);
        mapObjectCell.cell.setFlippedVertically(flippingFlags & 2);

        command = new ChangeMapObjectCells(mDocument, { mapObjectCell });

        command->setText(QCoreApplication::translate("Undo Commands",
                                                     "Flip %n Object(s)",
                                                     nullptr,
                                                     mMapDocument->selectedObjects().size()));
        break;
    }
    }

    return command;
}

void PropertyBrowser::applyMapObjectValue(PropertyId id, const QVariant &val)
{
    MapObject *mapObject = static_cast<MapObject*>(mObject);

    QUndoCommand *command = applyMapObjectValueTo(id, val, mapObject);
    if (!command)
        return;

    mDocument->undoStack()->beginMacro(command->text());
    mDocument->undoStack()->push(command);

    for (MapObject *obj : mMapDocument->selectedObjects()) {
        if (obj != mapObject) {
            if (QUndoCommand *cmd = applyMapObjectValueTo(id, val, obj))
                mDocument->undoStack()->push(cmd);
        }
    }

    mDocument->undoStack()->endMacro();
}

void PropertyBrowser::applyLayerValue(PropertyId id, const QVariant &val)
{
    Layer *currentLayer = static_cast<Layer*>(mObject);

    QUndoCommand *command = applyLayerValueTo(id, val, currentLayer);
    if (!command)
        return;

    mDocument->undoStack()->beginMacro(command->text());
    mDocument->undoStack()->push(command);

    for (Layer *layer : mMapDocument->selectedLayers()) {
        if (layer != currentLayer) {
            if (QUndoCommand *cmd = applyLayerValueTo(id, val, layer))
                mDocument->undoStack()->push(cmd);
        }
    }

    mDocument->undoStack()->endMacro();
}

QUndoCommand *PropertyBrowser::applyLayerValueTo(PropertyBrowser::PropertyId id, const QVariant &val, Layer *layer)
{
    QUndoCommand *command = nullptr;

    switch (id) {
    case NameProperty:
        command = new SetLayerName(mMapDocument, layer, val.toString());
        break;
    case VisibleProperty:
        command = new SetLayerVisible(mMapDocument, layer, val.toBool());
        break;
    case LockedProperty:
        command = new SetLayerLocked(mMapDocument, layer, val.toBool());
        break;
    case OpacityProperty:
        command = new SetLayerOpacity(mMapDocument, layer, val.toDouble());
        break;
    case TintColorProperty:
        command = new SetLayerTintColor(mMapDocument, layer, val.value<QColor>());
        break;
    case OffsetXProperty:
    case OffsetYProperty: {
        QPointF offset = layer->offset();

        if (id == OffsetXProperty)
            offset.setX(val.toDouble());
        else
            offset.setY(val.toDouble());

        command = new SetLayerOffset(mMapDocument, layer, offset);
        break;
    }
    case ParallaxFactorProperty:
        command = new SetLayerParallaxFactor(mMapDocument, layer, val.toPointF());
        break;
    default:
        switch (layer->layerType()) {
        case Layer::TileLayerType:
            command = applyTileLayerValueTo(id, val, static_cast<TileLayer*>(layer));
            break;
        case Layer::ObjectGroupType:
            command = applyObjectGroupValueTo(id, val, static_cast<ObjectGroup*>(layer));
            break;
        case Layer::ImageLayerType:
            command = applyImageLayerValueTo(id, val, static_cast<ImageLayer*>(layer));
            break;
        case Layer::GroupLayerType:
            command = applyGroupLayerValueTo(id, val, static_cast<GroupLayer*>(layer));
            break;
        }
        break;
    }

    return command;
}

QUndoCommand *PropertyBrowser::applyTileLayerValueTo(PropertyId id, const QVariant &val, TileLayer *tileLayer)
{
    Q_UNUSED(id)
    Q_UNUSED(val)
    Q_UNUSED(tileLayer)

    return nullptr;
}

QUndoCommand *PropertyBrowser::applyObjectGroupValueTo(PropertyId id, const QVariant &val, ObjectGroup *objectGroup)
{
    QUndoCommand *command = nullptr;

    switch (id) {
    case ColorProperty: {
        const QColor color = val.value<QColor>();
        command = new ChangeObjectGroupProperties(mMapDocument,
                                                  objectGroup,
                                                  color,
                                                  objectGroup->drawOrder());
        break;
    }
    case DrawOrderProperty: {
        ObjectGroup::DrawOrder drawOrder = static_cast<ObjectGroup::DrawOrder>(val.toInt());
        command = new ChangeObjectGroupProperties(mMapDocument,
                                                  objectGroup,
                                                  objectGroup->color(),
                                                  drawOrder);
        break;
    }
    default:
        break;
    }

    return command;
}

QUndoCommand *PropertyBrowser::applyImageLayerValueTo(PropertyId id, const QVariant &val, ImageLayer *imageLayer)
{
    QUndoCommand *command = nullptr;

    switch (id) {
    case ImageSourceProperty: {
        const FilePath imageSource = val.value<FilePath>();
        const QColor &color = imageLayer->transparentColor();
        command = new ChangeImageLayerProperties(mMapDocument,
                                                 imageLayer,
                                                 color,
                                                 imageSource.url);
        break;
    }
    case ColorProperty: {
        const QColor color = val.value<QColor>();
        const QUrl &imageSource = imageLayer->imageSource();
        command = new ChangeImageLayerProperties(mMapDocument,
                                                 imageLayer,
                                                 color,
                                                 imageSource);
        break;
    }
    default:
        break;
    }

    return command;
}

QUndoCommand *PropertyBrowser::applyGroupLayerValueTo(PropertyId id, const QVariant &val, GroupLayer *groupLayer)
{
    Q_UNUSED(id)
    Q_UNUSED(val)
    Q_UNUSED(groupLayer)

    return nullptr;
}

void PropertyBrowser::applyTilesetValue(PropertyId id, const QVariant &val)
{
    Tileset *tileset = static_cast<Tileset*>(mObject);
    QUndoStack *undoStack = mDocument->undoStack();

    switch (id) {
    case FileNameProperty: {
        FilePath filePath = val.value<FilePath>();
        QString error;
        SharedTileset newTileset = TilesetManager::instance()->loadTileset(filePath.url.toLocalFile(), &error);
        if (!newTileset) {
            QMessageBox::critical(window(), tr("Error Reading Tileset"), error);
            return;
        }

        int index = mMapDocument->map()->tilesets().indexOf(tileset->sharedPointer());
        if (index != -1)
            undoStack->push(new ReplaceTileset(mMapDocument, index, newTileset));

        break;
    }
    case NameProperty:
        Q_ASSERT(mTilesetDocument);
        undoStack->push(new RenameTileset(mTilesetDocument, val.toString()));
        break;
    case ObjectAlignmentProperty: {
        Q_ASSERT(mTilesetDocument);
        auto objectAlignment = static_cast<Alignment>(val.toInt());
        undoStack->push(new ChangeTilesetObjectAlignment(mTilesetDocument,
                                                         objectAlignment));
        break;
    }
    case TileOffsetProperty:
        Q_ASSERT(mTilesetDocument);
        undoStack->push(new ChangeTilesetTileOffset(mTilesetDocument,
                                                    val.toPoint()));
        break;
    case OrientationProperty: {
        Q_ASSERT(mTilesetDocument);
        auto orientation = static_cast<Tileset::Orientation>(val.toInt());
        undoStack->push(new ChangeTilesetOrientation(mTilesetDocument,
                                                     orientation));
        break;
    }
    case GridWidthProperty: {
        Q_ASSERT(mTilesetDocument);
        QSize gridSize = tileset->gridSize();
        gridSize.setWidth(val.toInt());
        undoStack->push(new ChangeTilesetGridSize(mTilesetDocument,
                                                  gridSize));
        break;
    }
    case GridHeightProperty: {
        Q_ASSERT(mTilesetDocument);
        QSize gridSize = tileset->gridSize();
        gridSize.setHeight(val.toInt());
        undoStack->push(new ChangeTilesetGridSize(mTilesetDocument,
                                                  gridSize));
        break;
    }
    case ColumnCountProperty:
        Q_ASSERT(mTilesetDocument);
        undoStack->push(new ChangeTilesetColumnCount(mTilesetDocument,
                                                     val.toInt()));
        break;
    case BackgroundColorProperty:
        Q_ASSERT(mTilesetDocument);
        undoStack->push(new ChangeTilesetBackgroundColor(mTilesetDocument,
                                                         val.value<QColor>()));
        break;
    case AllowFlipHorizontallyProperty:
    case AllowFlipVerticallyProperty:
    case AllowRotateProperty:
    case PreferUntransformedProperty: {
        Q_ASSERT(mTilesetDocument);

        Tileset::TransformationFlag flag = Tileset::NoTransformation;
        switch (id) {
        case AllowFlipHorizontallyProperty:
            flag = Tileset::AllowFlipHorizontally;
            break;
        case AllowFlipVerticallyProperty:
            flag = Tileset::AllowFlipVertically;
            break;
        case AllowRotateProperty:
            flag = Tileset::AllowRotate;
            break;
        case PreferUntransformedProperty:
            flag = Tileset::PreferUntransformed;
            break;
        default:
            return;
        }

        auto flags = tileset->transformationFlags();

#if QT_VERSION >= 0x050700
        flags.setFlag(flag, val.toBool());
#else
        if (val.toBool())
            flags |= flag;
        else
            flags &= ~flag;
#endif

        undoStack->push(new ChangeTilesetTransformationFlags(mTilesetDocument, flags));
        break;
    }
    default:
        break;
    }
}

void PropertyBrowser::applyTileValue(PropertyId id, const QVariant &val)
{
    Q_ASSERT(mTilesetDocument);

    Tile *tile = static_cast<Tile*>(mObject);
    QUndoStack *undoStack = mDocument->undoStack();

    switch (id) {
    case TypeProperty:
        undoStack->push(new ChangeTileType(mTilesetDocument,
                                           mTilesetDocument->selectedTiles(),
                                           val.toString()));
        break;
    case TileProbabilityProperty:
        undoStack->push(new ChangeTileProbability(mTilesetDocument,
                                                  mTilesetDocument->selectedTiles(),
                                                  val.toFloat()));
        break;
    case ImageSourceProperty: {
        const FilePath filePath = val.value<FilePath>();
        undoStack->push(new ChangeTileImageSource(mTilesetDocument,
                                                  tile, filePath.url));
        break;
    }
    default:
        break;
    }
}

void PropertyBrowser::applyWangSetValue(PropertyId id, const QVariant &val)
{
    Q_ASSERT(mTilesetDocument);

    WangSet *wangSet = static_cast<WangSet*>(mObject);

    switch (id) {
    case NameProperty:
        mDocument->undoStack()->push(new RenameWangSet(mTilesetDocument,
                                                       wangSet,
                                                       val.toString()));
        break;
    case WangSetTypeProperty: {
        auto type = static_cast<WangSet::Type>(val.toInt());
        mDocument->undoStack()->push(new ChangeWangSetType(mTilesetDocument,
                                                           wangSet,
                                                           type));
        break;
    }
    case ColorCountProperty:
        mDocument->undoStack()->push(new ChangeWangSetColorCount(mTilesetDocument,
                                                                 wangSet,
                                                                 val.toInt()));
        break;
    default:
        break;
    }
}

void PropertyBrowser::applyWangColorValue(PropertyId id, const QVariant &val)
{
    Q_ASSERT(mTilesetDocument);

    WangColor *wangColor = static_cast<WangColor*>(mObject);

    switch (id) {
    case NameProperty:
        mDocument->undoStack()->push(new ChangeWangColorName(mTilesetDocument,
                                                             wangColor,
                                                             val.toString()));
        break;
    case ColorProperty:
        mDocument->undoStack()->push(new ChangeWangColorColor(mTilesetDocument,
                                                              wangColor,
                                                              val.value<QColor>()));
        break;
    case WangColorProbabilityProperty:
        mDocument->undoStack()->push(new ChangeWangColorProbability(mTilesetDocument,
                                                                    wangColor,
                                                                    val.toDouble()));
        break;
    default:
        break;
    }
}

/**
 * @warning This function does not add the property to the view.
 */
QtVariantProperty *PropertyBrowser::createProperty(PropertyId id, int type,
                                                   const QString &name)
{
    Q_ASSERT(!mIdToProperty.contains(id));

    QtVariantProperty *property = mVariantManager->addProperty(type, name);
    if (!property) {
        // fall back to string property for unsupported property types
        property = mVariantManager->addProperty(QMetaType::QString, name);
    }

    if (type == QMetaType::Bool)
        property->setAttribute(QLatin1String("textVisible"), false);

    mPropertyToId.insert(property, id);
    mIdToProperty.insert(id, property);

    return property;
}

QtVariantProperty *PropertyBrowser::createCustomProperty(const QString &name,
                                                         const QVariant &value)
{
    Q_ASSERT(mObject);

    QtVariantProperty *property = mCustomPropertiesHelper.createProperty(name, value);

    if (mObject->isPartOfTileset())
        property->setEnabled(mTilesetDocument);

    return property;
}

QtVariantProperty *PropertyBrowser::addProperty(PropertyId id, int type,
                                                const QString &name,
                                                QtProperty *parent)
{
    QtVariantProperty *property = createProperty(id, type, name);
    parent->addSubProperty(property);
    return property;
}

QtVariantProperty *PropertyBrowser::addCustomProperty(const QString &name, const QVariant &value)
{
    // Determine the property preceding the new property, if any
    const QList<QtProperty *> properties = mCustomPropertiesGroup->subProperties();
    QtProperty *precedingProperty = nullptr;
    for (int i = 0; i < properties.size(); ++i) {
        if (properties.at(i)->propertyName() < name)
            precedingProperty = properties.at(i);
        else
            break;
    }

    QScopedValueRollback<bool> updating(mUpdating, true);
    QtVariantProperty *property = createCustomProperty(name, value);
    mCustomPropertiesGroup->insertSubProperty(property, precedingProperty);

    // Collapse custom color properties, to save space
    if (value.userType() == QMetaType::QColor)
        setExpanded(items(property).constFirst(), false);

    return property;
}

void PropertyBrowser::deleteCustomProperty(QtVariantProperty *property)
{
    mCustomPropertiesHelper.deleteProperty(property);
}

void PropertyBrowser::setCustomPropertyValue(QtVariantProperty *property,
                                             const QVariant &value)
{
    const QVariant displayValue = toDisplayValue(value);

    if (displayValue.userType() != property->valueType()) {
        // Re-creating the property is necessary to change its type
        const QString name = property->propertyName();
        const bool wasCurrent = currentItem() && currentItem()->property() == property;

        deleteCustomProperty(property);
        property = addCustomProperty(name, value);
        updateCustomPropertyColor(name);

        if (wasCurrent)
            setCurrentItem(items(property).constFirst());
    } else {
        QScopedValueRollback<bool> updating(mUpdating, true);
        property->setValue(displayValue);
    }
}

void PropertyBrowser::addProperties()
{
    if (!mObject)
        return;

    Q_ASSERT(mUpdating);

    // Add the built-in properties for each object type
    switch (mObject->typeId()) {
    case Object::MapType:               addMapProperties(); break;
    case Object::MapObjectType:         addMapObjectProperties(); break;
    case Object::LayerType:
        switch (static_cast<Layer*>(mObject)->layerType()) {
        case Layer::TileLayerType:      addTileLayerProperties();   break;
        case Layer::ObjectGroupType:    addObjectGroupProperties(); break;
        case Layer::ImageLayerType:     addImageLayerProperties();  break;
        case Layer::GroupLayerType:     addGroupLayerProperties();  break;
        }
        break;
    case Object::TilesetType:           addTilesetProperties(); break;
    case Object::TileType:              addTileProperties(); break;
    case Object::WangSetType:           addWangSetProperties(); break;
    case Object::WangColorType:         addWangColorProperties(); break;
    case Object::ObjectTemplateType:    break;
    }

    // Make sure the color and font properties are collapsed, to save space
    if (QtProperty *colorProperty = mIdToProperty.value(ColorProperty))
        setExpanded(items(colorProperty).constFirst(), false);
    if (QtProperty *colorProperty = mIdToProperty.value(BackgroundColorProperty))
        setExpanded(items(colorProperty).constFirst(), false);
    if (QtProperty *fontProperty = mIdToProperty.value(FontProperty))
        setExpanded(items(fontProperty).constFirst(), false);
    if (QtProperty *tintColorProperty = mIdToProperty.value(TintColorProperty))
        setExpanded(items(tintColorProperty).constFirst(), false);

    // Add a node for the custom properties
    mCustomPropertiesGroup = mGroupManager->addProperty(tr("Custom Properties"));
    addProperty(mCustomPropertiesGroup);

    updateProperties();
    updateCustomProperties();
}

void PropertyBrowser::removeProperties()
{
    Q_ASSERT(mUpdating);

    mCustomPropertiesHelper.clear();
    mVariantManager->clear();
    mGroupManager->clear();
    mPropertyToId.clear();
    mIdToProperty.clear();
    mCustomPropertiesGroup = nullptr;
}

void PropertyBrowser::updateProperties()
{
    Q_ASSERT(mObject);

    QScopedValueRollback<bool> updating(mUpdating, true);

    switch (mObject->typeId()) {
    case Object::MapType: {
        const Map *map = static_cast<const Map*>(mObject);
        mIdToProperty[WidthProperty]->setValue(map->width());
        mIdToProperty[HeightProperty]->setValue(map->height());
        mIdToProperty[TileWidthProperty]->setValue(map->tileWidth());
        mIdToProperty[TileHeightProperty]->setValue(map->tileHeight());
        mIdToProperty[InfiniteProperty]->setValue(map->infinite());
        mIdToProperty[OrientationProperty]->setValue(map->orientation() - 1);
        mIdToProperty[HexSideLengthProperty]->setValue(map->hexSideLength());
        mIdToProperty[StaggerAxisProperty]->setValue(map->staggerAxis());
        mIdToProperty[StaggerIndexProperty]->setValue(map->staggerIndex());
        mIdToProperty[LayerFormatProperty]->setValue(mLayerFormatValues.indexOf(map->layerDataFormat()));
        mIdToProperty[CompressionLevelProperty]->setValue(map->compressionLevel());
        mIdToProperty[RenderOrderProperty]->setValue(map->renderOrder());
        mIdToProperty[BackgroundColorProperty]->setValue(map->backgroundColor());
        mIdToProperty[ChunkWidthProperty]->setValue(map->chunkSize().width());
        mIdToProperty[ChunkHeightProperty]->setValue(map->chunkSize().height());
        break;
    }
    case Object::MapObjectType: {
        const MapObject *mapObject = static_cast<const MapObject*>(mObject);
        const int flags = mapObjectFlags(mapObject);

        if (mMapObjectFlags != flags) {
            UpdatingProperties updatingProperties(this, mUpdating, true);
            removeProperties();
            addProperties();
            return;
        }

        const QString &type = mapObject->effectiveType();
        const auto typeColorGroup = mapObject->type().isEmpty() ? QPalette::Disabled
                                                                : QPalette::Active;

        FilePath templateFilePath;
        if (auto objectTemplate = mapObject->objectTemplate())
            templateFilePath.url = QUrl::fromLocalFile(objectTemplate->fileName());

        mIdToProperty[IdProperty]->setValue(mapObject->id());
        mIdToProperty[TemplateProperty]->setValue(QVariant::fromValue(templateFilePath));
        mIdToProperty[NameProperty]->setValue(mapObject->name());
        mIdToProperty[TypeProperty]->setValue(type);
        mIdToProperty[TypeProperty]->setValueColor(palette().color(typeColorGroup, QPalette::WindowText));
        if (auto visibleProperty = mIdToProperty[VisibleProperty])
            visibleProperty->setValue(mapObject->isVisible());
        mIdToProperty[XProperty]->setValue(mapObject->x());
        mIdToProperty[YProperty]->setValue(mapObject->y());

        if (flags & ObjectHasDimensions) {
            mIdToProperty[WidthProperty]->setValue(mapObject->width());
            mIdToProperty[HeightProperty]->setValue(mapObject->height());
        }

        mIdToProperty[RotationProperty]->setValue(mapObject->rotation());

        if (flags & ObjectHasTile) {
            int flippingFlags = 0;
            if (mapObject->cell().flippedHorizontally())
                flippingFlags |= 1;
            if (mapObject->cell().flippedVertically())
                flippingFlags |= 2;
            mIdToProperty[FlippingProperty]->setValue(flippingFlags);
        }

        if (flags & ObjectIsText) {
            const auto& textData = mapObject->textData();
            mIdToProperty[TextProperty]->setValue(textData.text);
            mIdToProperty[FontProperty]->setValue(textData.font);
            mIdToProperty[TextAlignmentProperty]->setValue(QVariant::fromValue(textData.alignment));
            mIdToProperty[WordWrapProperty]->setValue(textData.wordWrap);
            mIdToProperty[ColorProperty]->setValue(textData.color);
        }
        break;
    }
    case Object::LayerType: {
        const Layer *layer = static_cast<const Layer*>(mObject);

        mIdToProperty[IdProperty]->setValue(layer->id());
        mIdToProperty[NameProperty]->setValue(layer->name());
        mIdToProperty[VisibleProperty]->setValue(layer->isVisible());
        mIdToProperty[LockedProperty]->setValue(layer->isLocked());
        mIdToProperty[OpacityProperty]->setValue(layer->opacity());
        mIdToProperty[TintColorProperty]->setValue(layer->tintColor());
        mIdToProperty[OffsetXProperty]->setValue(layer->offset().x());
        mIdToProperty[OffsetYProperty]->setValue(layer->offset().y());
        mIdToProperty[ParallaxFactorProperty]->setValue(layer->parallaxFactor());

        switch (layer->layerType()) {
        case Layer::TileLayerType:
            break;
        case Layer::ObjectGroupType: {
            const ObjectGroup *objectGroup = static_cast<const ObjectGroup*>(layer);
            const QColor color = objectGroup->color();
            mIdToProperty[ColorProperty]->setValue(color);
            mIdToProperty[DrawOrderProperty]->setValue(objectGroup->drawOrder());
            break;
        }
        case Layer::ImageLayerType: {
            const ImageLayer *imageLayer = static_cast<const ImageLayer*>(layer);
            mIdToProperty[ImageSourceProperty]->setValue(QVariant::fromValue(FilePath { imageLayer->imageSource() }));
            mIdToProperty[ColorProperty]->setValue(imageLayer->transparentColor());
            break;
        }
        case Layer::GroupLayerType:
            break;
        }
        break;
    }
    case Object::TilesetType: {
        Tileset *tileset = static_cast<Tileset*>(mObject);

        if (QtVariantProperty *fileNameProperty = mIdToProperty.value(FileNameProperty))
            fileNameProperty->setValue(QVariant::fromValue(FilePath { QUrl::fromLocalFile(tileset->fileName()) }));

        mIdToProperty[BackgroundColorProperty]->setValue(tileset->backgroundColor());

        mIdToProperty[NameProperty]->setValue(tileset->name());
        mIdToProperty[ObjectAlignmentProperty]->setValue(tileset->objectAlignment());
        mIdToProperty[TileOffsetProperty]->setValue(tileset->tileOffset());
        mIdToProperty[OrientationProperty]->setValue(tileset->orientation());
        mIdToProperty[GridWidthProperty]->setValue(tileset->gridSize().width());
        mIdToProperty[GridHeightProperty]->setValue(tileset->gridSize().height());
        mIdToProperty[ColumnCountProperty]->setValue(tileset->columnCount());
        mIdToProperty[ColumnCountProperty]->setEnabled(mTilesetDocument && tileset->isCollection());

        if (!tileset->isCollection()) {
            mIdToProperty[TilesetImageParametersProperty]->setValue(QVariant::fromValue(mTilesetDocument));
            mIdToProperty[ImageSourceProperty]->setValue(tileset->imageSource().toString(QUrl::PreferLocalFile));
            mIdToProperty[TileWidthProperty]->setValue(tileset->tileWidth());
            mIdToProperty[TileHeightProperty]->setValue(tileset->tileHeight());
            mIdToProperty[MarginProperty]->setValue(tileset->margin());
            mIdToProperty[SpacingProperty]->setValue(tileset->tileSpacing());
            mIdToProperty[ColorProperty]->setValue(tileset->transparentColor());
        }

        const auto flags = tileset->transformationFlags();
        mIdToProperty[AllowFlipHorizontallyProperty]->setValue(flags.testFlag(Tileset::AllowFlipHorizontally));
        mIdToProperty[AllowFlipVerticallyProperty]->setValue(flags.testFlag(Tileset::AllowFlipVertically));
        mIdToProperty[AllowRotateProperty]->setValue(flags.testFlag(Tileset::AllowRotate));
        mIdToProperty[PreferUntransformedProperty]->setValue(flags.testFlag(Tileset::PreferUntransformed));
        break;
    }
    case Object::TileType: {
        const Tile *tile = static_cast<const Tile*>(mObject);
        const QSize tileSize = tile->size();
        mIdToProperty[IdProperty]->setValue(tile->id());
        mIdToProperty[TypeProperty]->setValue(tile->type());
        mIdToProperty[WidthProperty]->setValue(tileSize.width());
        mIdToProperty[HeightProperty]->setValue(tileSize.height());
        mIdToProperty[TileProbabilityProperty]->setValue(tile->probability());
        if (QtVariantProperty *imageSourceProperty = mIdToProperty.value(ImageSourceProperty))
            imageSourceProperty->setValue(QVariant::fromValue(FilePath { tile->imageSource() }));
        break;
    }
    case Object::WangSetType: {
        const WangSet *wangSet = static_cast<const WangSet*>(mObject);
        mIdToProperty[NameProperty]->setValue(wangSet->name());
        mIdToProperty[WangSetTypeProperty]->setValue(wangSet->type());
        mIdToProperty[ColorCountProperty]->setValue(wangSet->colorCount());
        break;
    }
    case Object::WangColorType: {
        const WangColor *wangColor = static_cast<const WangColor*>(mObject);
        mIdToProperty[NameProperty]->setValue(wangColor->name());
        mIdToProperty[ColorProperty]->setValue(wangColor->color());
        mIdToProperty[WangColorProbabilityProperty]->setValue(wangColor->probability());
        break;
    }
    case Object::ObjectTemplateType:
        break;
    }
}

void PropertyBrowser::updateCustomProperties()
{
    if (!mObject)
        return;

    UpdatingProperties updatingProperties(this, mUpdating);

    mCustomPropertiesHelper.clear();

    mCombinedProperties = mObject->properties();
    // Add properties from selected objects which mObject does not contain to mCombinedProperties.
    const auto currentObjects = mDocument->currentObjects();
    for (Object *obj : currentObjects) {
        if (obj == mObject)
            continue;

        QMapIterator<QString,QVariant> it(obj->properties());

        while (it.hasNext()) {
            it.next();
            if (!mCombinedProperties.contains(it.key()))
                mCombinedProperties.insert(it.key(), QString());
        }
    }

    QString objectType;

    switch (mObject->typeId()) {
    case Object::TileType:
        objectType = static_cast<Tile*>(mObject)->type();
        break;
    case Object::MapObjectType: {
        auto mapObject = static_cast<MapObject*>(mObject);
        objectType = mapObject->type();

        // Inherit properties from the template
        if (const MapObject *templateObject = mapObject->templateObject()) {
            QMapIterator<QString,QVariant> it(templateObject->properties());
            while (it.hasNext()) {
                it.next();
                if (!mCombinedProperties.contains(it.key()))
                    mCombinedProperties.insert(it.key(), it.value());
            }
        }

        if (Tile *tile = mapObject->cell().tile()) {
            if (objectType.isEmpty())
                objectType = tile->type();

            // Inherit properties from the tile
            QMapIterator<QString,QVariant> it(tile->properties());
            while (it.hasNext()) {
                it.next();
                if (!mCombinedProperties.contains(it.key()))
                    mCombinedProperties.insert(it.key(), it.value());
            }
        }
        break;
    }
    case Object::LayerType:
    case Object::MapType:
    case Object::TilesetType:
    case Object::WangSetType:
    case Object::WangColorType:
    case Object::ObjectTemplateType:
        break;
    }

    if (!objectType.isEmpty()) {
        // Inherit properties from the object type
        for (const ObjectType &type : Object::objectTypes()) {
            if (type.name == objectType) {
                QMapIterator<QString,QVariant> it(type.defaultProperties);
                while (it.hasNext()) {
                    it.next();
                    if (!mCombinedProperties.contains(it.key()))
                        mCombinedProperties.insert(it.key(), it.value());
                }
            }
        }
    }

    QMapIterator<QString,QVariant> it(mCombinedProperties);

    while (it.hasNext()) {
        it.next();

        QtVariantProperty *property = createCustomProperty(it.key(), it.value());
        mCustomPropertiesGroup->addSubProperty(property);

        // Collapse custom color properties, to save space
        if (property->valueType() == QMetaType::QColor)
            setExpanded(items(property).constFirst(), false);

        updateCustomPropertyColor(it.key());
    }
}

// If there are other objects selected check if their properties are equal. If not give them a gray color.
void PropertyBrowser::updateCustomPropertyColor(const QString &name)
{
    QtVariantProperty *property = mCustomPropertiesHelper.property(name);
    if (!property)
        return;
    if (!property->isEnabled())
        return;

    QString propertyName = property->propertyName();
    QString propertyValue = property->valueText();

    const auto &objects = mDocument->currentObjects();

    QColor textColor = palette().color(QPalette::Active, QPalette::WindowText);
    QColor disabledTextColor = palette().color(QPalette::Disabled, QPalette::WindowText);

    // If one of the objects doesn't have this property then gray out the name and value.
    for (Object *obj : objects) {
        if (!obj->hasProperty(propertyName)) {
            property->setNameColor(disabledTextColor);
            property->setValueColor(disabledTextColor);
            return;
        }
    }

    // If one of the objects doesn't have the same property value then gray out the value.
    for (Object *obj : objects) {
        if (obj == mObject)
            continue;
        if (obj->property(propertyName) != propertyValue) {
            property->setNameColor(textColor);
            property->setValueColor(disabledTextColor);
            return;
        }
    }

    property->setNameColor(textColor);
    property->setValueColor(textColor);
}

QVariant PropertyBrowser::toDisplayValue(QVariant value) const
{
    return mCustomPropertiesHelper.toDisplayValue(value);
}

QVariant PropertyBrowser::fromDisplayValue(QtProperty *property, QVariant value) const
{
    return mCustomPropertiesHelper.fromDisplayValue(property, value);
}

void PropertyBrowser::retranslateUi()
{
    mStaggerAxisNames.clear();
    mStaggerAxisNames.append(tr("X"));
    mStaggerAxisNames.append(tr("Y"));

    mStaggerIndexNames.clear();
    mStaggerIndexNames.append(tr("Odd"));
    mStaggerIndexNames.append(tr("Even"));

    mOrientationNames.clear();
    mOrientationNames.append(QCoreApplication::translate("Tiled::NewMapDialog", "Orthogonal"));
    mOrientationNames.append(QCoreApplication::translate("Tiled::NewMapDialog", "Isometric"));
    mOrientationNames.append(QCoreApplication::translate("Tiled::NewMapDialog", "Isometric (Staggered)"));
    mOrientationNames.append(QCoreApplication::translate("Tiled::NewMapDialog", "Hexagonal (Staggered)"));

    mTilesetOrientationNames.clear();
    mTilesetOrientationNames.append(mOrientationNames.at(0));
    mTilesetOrientationNames.append(mOrientationNames.at(1));

    mLayerFormatNames.clear();
    mLayerFormatValues.clear();

    mLayerFormatNames.append(QCoreApplication::translate("PreferencesDialog", "XML (deprecated)"));
    mLayerFormatNames.append(QCoreApplication::translate("PreferencesDialog", "Base64 (uncompressed)"));
    mLayerFormatNames.append(QCoreApplication::translate("PreferencesDialog", "Base64 (gzip compressed)"));
    mLayerFormatNames.append(QCoreApplication::translate("PreferencesDialog", "Base64 (zlib compressed)"));
#ifdef TILED_ZSTD_SUPPORT
    mLayerFormatNames.append(QCoreApplication::translate("PreferencesDialog", "Base64 (Zstandard compressed)"));
#endif
    mLayerFormatNames.append(QCoreApplication::translate("PreferencesDialog", "CSV"));

    mLayerFormatValues.append(Map::XML);
    mLayerFormatValues.append(Map::Base64);
    mLayerFormatValues.append(Map::Base64Gzip);
    mLayerFormatValues.append(Map::Base64Zlib);
#ifdef TILED_ZSTD_SUPPORT
    mLayerFormatValues.append(Map::Base64Zstandard);
#endif
    mLayerFormatValues.append(Map::CSV);

    mRenderOrderNames.clear();
    mRenderOrderNames.append(QCoreApplication::translate("PreferencesDialog", "Right Down"));
    mRenderOrderNames.append(QCoreApplication::translate("PreferencesDialog", "Right Up"));
    mRenderOrderNames.append(QCoreApplication::translate("PreferencesDialog", "Left Down"));
    mRenderOrderNames.append(QCoreApplication::translate("PreferencesDialog", "Left Up"));

    mAlignmentNames.clear();
    mAlignmentNames.append(tr("Unspecified"));
    mAlignmentNames.append(tr("Top Left"));
    mAlignmentNames.append(tr("Top"));
    mAlignmentNames.append(tr("Top Right"));
    mAlignmentNames.append(tr("Left"));
    mAlignmentNames.append(tr("Center"));
    mAlignmentNames.append(tr("Right"));
    mAlignmentNames.append(tr("Bottom Left"));
    mAlignmentNames.append(tr("Bottom"));
    mAlignmentNames.append(tr("Bottom Right"));

    mFlippingFlagNames.clear();
    mFlippingFlagNames.append(tr("Horizontal"));
    mFlippingFlagNames.append(tr("Vertical"));

    mDrawOrderNames.clear();
    mDrawOrderNames.append(tr("Top Down"));
    mDrawOrderNames.append(tr("Manual"));

    mWangSetTypeNames.clear();
    mWangSetTypeNames.append(tr("Corner"));
    mWangSetTypeNames.append(tr("Edge"));
    mWangSetTypeNames.append(tr("Mixed"));

    UpdatingProperties updatingProperties(this, mUpdating);
    removeProperties();
    addProperties();
}

} // namespace Tiled

#include "moc_propertybrowser.cpp"
