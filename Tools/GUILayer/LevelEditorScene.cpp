// Copyright 2015 XLGAMES Inc.
//
// Distributed under the MIT License (See
// accompanying file "LICENSE" or the website
// http://www.opensource.org/licenses/mit-license.php)

#include "LevelEditorScene.h"
#include "PlacementsGobInterface.h"
#include "TerrainGobInterface.h"
#include "FlexGobInterface.h"
#include "MarshalString.h"
#include "GUILayerUtil.h"
#include "IOverlaySystem.h"
#include "EditorInterfaceUtils.h"
#include "UITypesBinding.h" // for VisCameraSettings
#include "ExportedNativeTypes.h"
#include "../ToolsRig/PlacementsManipulators.h"     // just needed for destructors referenced in PlacementGobInterface.h
#include "../ToolsRig/VisualisationUtils.h"
#include "../../SceneEngine/PlacementsManager.h"
#include <memory>

namespace GUILayer
{
    // Many level editors work around a structure of objects and attributes.
    // That is, the scene is composed of a hierachy of objects, and each object
    // as some type, and a set of attributes. In the case of the SonyWWS editor,
    // this structure is backed by a xml based DOM.
    // 
    // It's handy because it's simple and flexible. And we can store just about
    // anything like this.
    // 
    // When working with a level editor like this, we need a some kind of dynamic
    // "scene" object. This scene should react to basic commands from the editor:
    //
    //      * Create/destroy object (with given fixed type)
    //      * Set object attribute
    //      * Set object parent
    //
    // We also want to build in a "document" concept. Normally a document should
    // represent a single target file (eg a level file or some settings file).
    // Every object should belong to a single object, and all of the objects in
    // a single document should usually be part of one large hierarchy.
    //
    // For convenience when working with the SonyWWS editor, we want to be able to
    // pre-register common strings (like type names and object property names).
    // It might be ok just to use hash values for all of these cases. It depends on
    // whether we want to validate the names when they are first registered.

///////////////////////////////////////////////////////////////////////////////////////////////////

    EditorScene::EditorScene()
    {
        _placementsManager = std::make_shared<SceneEngine::PlacementsManager>(
            SceneEngine::WorldPlacementsConfig(),
            std::shared_ptr<RenderCore::Assets::IModelFormat>(), Float2(0.f, 0.f));
        _placementsEditor = _placementsManager->CreateEditor();
    }

	EditorScene::~EditorScene()
	{}

///////////////////////////////////////////////////////////////////////////////////////////////////

    using namespace EditorDynamicInterface;

    DocumentId EditorSceneManager::CreateDocument(DocumentTypeId docType) 
        { return _dynInterface->CreateDocument(*_scene.get(), docType, ""); }
    bool EditorSceneManager::DeleteDocument(DocumentId doc, DocumentTypeId docType)
        { return _dynInterface->DeleteDocument(*_scene.get(), doc, docType); }

    ObjectId EditorSceneManager::AssignObjectId(DocumentId doc, ObjectTypeId type)
        { return _dynInterface->AssignObjectId(*_scene.get(), doc, type); }

    bool EditorSceneManager::CreateObject(DocumentId doc, ObjectId obj, ObjectTypeId objType)
        { return _dynInterface->CreateObject(*_scene.get(), doc, obj, objType, ""); }

    bool EditorSceneManager::DeleteObject(DocumentId doc, ObjectId obj, ObjectTypeId objType)
        { return _dynInterface->DeleteObject(*_scene.get(), doc, obj, objType); }

    bool EditorSceneManager::SetProperty(DocumentId doc, ObjectId obj, ObjectTypeId objType, PropertyId prop, const void* src, unsigned elementType, unsigned arrayCount)
        { return _dynInterface->SetProperty(*_scene.get(), doc, obj, objType, prop, src, elementType, arrayCount); }

    bool EditorSceneManager::GetProperty(DocumentId doc, ObjectId obj, ObjectTypeId objType, PropertyId prop, void* dest, size_t* destSize)
        { return _dynInterface->GetProperty(*_scene.get(), doc, obj, objType, prop, dest, destSize); }

    DocumentTypeId EditorSceneManager::GetDocumentTypeId(System::String^ name)                  { return _dynInterface->GetDocumentTypeId(clix::marshalString<clix::E_UTF8>(name).c_str()); }
    ObjectTypeId EditorSceneManager::GetTypeId(System::String^ name)                            { return _dynInterface->GetTypeId(clix::marshalString<clix::E_UTF8>(name).c_str()); }
    PropertyId EditorSceneManager::GetPropertyId(ObjectTypeId type, System::String^ name)       { return _dynInterface->GetPropertyId(type, clix::marshalString<clix::E_UTF8>(name).c_str()); }
    ChildListId EditorSceneManager::GetChildListId(ObjectTypeId type, System::String^ name)     { return _dynInterface->GetChildListId(type, clix::marshalString<clix::E_UTF8>(name).c_str()); }

    bool EditorSceneManager::SetObjectParent(DocumentId doc, 
            ObjectId childId, ObjectTypeId childTypeId, 
            ObjectId parentId, ObjectTypeId parentTypeId, int insertionPosition)
    {
        return _dynInterface->SetParent(
            *_scene.get(), doc, childId, childTypeId,
            parentId, parentTypeId, insertionPosition);
    }

    IManipulatorSet^ EditorSceneManager::CreateTerrainManipulators() 
    { 
        if (_scene->_terrainGob && _scene->_terrainGob->_terrainManager) {
            return gcnew TerrainManipulators(_scene->_terrainGob->_terrainManager);
        } else {
            return nullptr;
        }
    }

    IManipulatorSet^ EditorSceneManager::CreatePlacementManipulators(
        IPlacementManipulatorSettingsLayer^ context)
    {
        if (_scene->_placementsEditor) {
            return gcnew PlacementManipulators(context->GetNative(), _scene->_placementsEditor);
        } else {
            return nullptr;
        }
    }

	IntersectionTestSceneWrapper^ EditorSceneManager::GetIntersectionScene()
	{
		return gcnew IntersectionTestSceneWrapper(
            (_scene->_terrainGob) ? _scene->_terrainGob->_terrainManager : nullptr,
            _scene->_placementsEditor);
    }

    PlacementsEditorWrapper^ EditorSceneManager::GetPlacementsEditor()
	{
		return gcnew PlacementsEditorWrapper(_scene->_placementsEditor);
    }

    EditorSceneManager::EditorSceneManager()
    {
        _scene = std::make_shared<EditorScene>();
        _dynInterface = std::make_shared<EditorDynamicInterface::RegisteredTypes>();
        _dynInterface->RegisterType(std::make_shared<EditorDynamicInterface::PlacementObjectType>());
        _dynInterface->RegisterType(std::make_shared<EditorDynamicInterface::TerrainObjectType>());

        _flexGobInterface = std::make_shared<EditorDynamicInterface::FlexObjectType>();
        Internal::RegisterTerrainFlexObjects(*_flexGobInterface.get());
        _dynInterface->RegisterType(_flexGobInterface.GetNativePtr());

        _selection = gcnew ObjectSet;
    }

    EditorSceneManager::~EditorSceneManager()
    {
        _scene.reset();
        _dynInterface.reset();
    }

    EditorSceneManager::!EditorSceneManager()
    {
        _scene.reset();
        _dynInterface.reset();
    }

    namespace Internal
    {
        IOverlaySystem^ CreateOverlaySystem(
            std::shared_ptr<EditorScene> scene, 
            std::shared_ptr<EditorDynamicInterface::FlexObjectType> flexGobInterface,
            ObjectSet^ selection, 
            std::shared_ptr<ToolsRig::VisCameraSettings> camera, 
            EditorSceneRenderSettings^ renderSettings);
    }

    IOverlaySystem^ EditorSceneManager::CreateOverlaySystem(VisCameraSettings^ camera, EditorSceneRenderSettings^ renderSettings)
    {
        return Internal::CreateOverlaySystem(
            _scene.GetNativePtr(), _flexGobInterface.GetNativePtr(),
            _selection, camera->GetUnderlying(), renderSettings);
    }

    void EditorSceneManager::SetSelection(ObjectSet^ objectSet)
    {
        *_selection->_nativePlacements = *objectSet->_nativePlacements;
        if (!!_scene->_placementsEditor) {
            _selection->DoFixup(*_scene->_placementsEditor);
        }
    }
}